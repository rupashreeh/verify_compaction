#include "db.h"

#include <cstdio>
#include <cstdlib>

#include "ghost.h"

namespace lsm {

DB::DB(SimEnv* e, const Options& o) : env(e), opts(o), tree(e, opts), wal(e, opts, &tree) {
  tree.wal = &wal;
  tree.ghost = opts.ghost;
  tree.stepHook = [this](const char* name) { Step(name); };
}

// ============================================================ CF4 Open
Status DB::Open(SimEnv* env, const Options& opts, const std::vector<std::string>& cfNames,
                std::unique_ptr<DB>* out, std::vector<CfId>* handles, bool readOnly) {
  auto db = std::make_unique<DB>(env, opts);
  db->readOnly = readOnly;
  // C_M <- the CFs recorded in the MANIFEST; a read-write open must name every one.
  // (The check needs the MANIFEST, so it runs right after WAL9 has read it and
  // before anything is written for the new names.)
  Status s = db->wal.Recover(readOnly);  // WAL9 replays each CF from its N_c
  if (!s.ok()) {
    if (opts.ghost) opts.ghost->OnRecoveryFailed(s);
    return s;
  }
  std::map<std::string, CfId> byName;
  for (CfId c : db->tree.CFs()) byName[db->tree.Cf(c)->name] = c;
  if (!readOnly)
    for (auto& [name, c] : byName)
      if (std::find(cfNames.begin(), cfNames.end(), name) == cfNames.end())
        return Status::InvalidArgument("read-write open must name every CF; missing " + name);
  std::vector<CfId> hs;
  for (auto& name : cfNames) {
    auto it = byName.find(name);
    if (it != byName.end()) {
      db->tree.Cf(it->second)->refs++;
      hs.push_back(it->second);
      continue;
    }
    if (!opts.create_missing_column_families || readOnly)
      return Status::InvalidArgument("unknown column family " + name);
    CfId c;
    s = db->tree.CreateColumnFamily(name, &c);  // CF1
    if (!s.ok()) return s;
    hs.push_back(c);
  }
  if (!readOnly) {
    db->tree.DeleteOrphanFiles();
    db->wal.PurgeObsoleteLogs();
  }
  if (opts.ghost) opts.ghost->OnRecovered(*db);
  db->Step("CF4 Open / WAL9 Recover");
  if (handles) *handles = hs;
  *out = std::move(db);
  return Status::OK();
}

Status DB::Close() {
  for (auto& l : wal.L) {
    Status s = wal.FlushWAL(*l);
    if (!s.ok()) return s;
  }
  return Status::OK();
}

// ============================================================ checking
std::vector<std::string> DB::CheckInvariants() const {
  if (!opts.ghost) return {};
  return lsm::CheckInvariants(*this, *opts.ghost);
}

void DB::Step(const char* name) {
  if (!opts.check_invariants || !opts.ghost) return;
  for (auto& v : CheckInvariants()) {
    std::string msg = std::string(name) + ": " + v;
    auto& seen = opts.ghost->violations;
    if (std::find(seen.begin(), seen.end(), msg) == seen.end()) seen.push_back(msg);
    if (opts.abort_on_violation) {
      std::fprintf(stderr, "invariant violated after %s\n", msg.c_str());
      std::abort();
    }
  }
}

// ============================================================ writes
std::shared_ptr<WriteGroup> DB::NewGroup(const std::vector<std::pair<WriteBatch, WriteOptions>>& batches,
                                         Status* s) {
  *s = Status::OK();
  if (readOnly) {
    *s = Status::InvalidArgument("read-only DB");
    return nullptr;
  }
  auto G = std::make_shared<WriteGroup>();
  G->id = nextGroupId_++;
  for (auto& [b, o] : batches) {
    if (b.ops.empty()) continue;
    if (o.disableWAL != batches.front().second.disableWAL) {
      *s = Status::InvalidArgument("a group mixes disableWAL settings");
      return nullptr;
    }
    for (auto& op : b.ops)
      if (!tree.IsLive(op.cf)) {
        *s = Status::InvalidArgument("column family is not live");
        return nullptr;
      }
    auto bs = std::make_shared<BatchState>();
    bs->id = nextBatchId_++;
    bs->batch = b;
    bs->o = o;
    G->batches.push_back(bs);
  }
  wal.Q.push_back(G);
  return G;
}

Status DB::Admit(WriteGroup& G) {
  Status s = wal.Admit(G);
  Step("WAL1 Admit");
  return s;
}
Status DB::AppendGroup(WriteGroup& G) {
  Status s = wal.AppendGroup(G);
  Step("WAL2 AppendGroup");
  return s;
}
Status DB::InsertGroup(WriteGroup& G) {
  Status s = wal.InsertGroup(G);
  Step("WAL4a InsertGroup");
  return s;
}
Status DB::InsertBatchCf(WriteGroup& G, size_t i, CfId c) {
  Status s = wal.InsertBatchCf(G, i, c);
  Step("WAL4a InsertGroup (one CF)");
  return s;
}
Status DB::PublishGroup(WriteGroup& G) {
  Status s = wal.PublishGroup(G);
  Step("WAL4b PublishGroup");
  return s;
}

Status DB::WriteGroupNow(std::shared_ptr<WriteGroup> G) {
  Status s;
  for (int tries = 0;; tries++) {
    s = Admit(*G);
    if (!s.IsBusy() || !opts.inline_background || tries > 8) break;
    Status bg = RunBackgroundWork();  // let the flushes that end the stall run
    if (!bg.ok()) { s = bg; break; }
  }
  if (!s.ok()) {
    wal.Forget(*G);
    return s;
  }
  s = AppendGroup(*G);
  if (s.ok()) s = InsertGroup(*G);
  if (s.ok()) s = PublishGroup(*G);
  if (!s.ok()) {
    wal.Forget(*G);
    return s;
  }
  if (opts.inline_background) {
    Status bg = RunBackgroundWork();
    (void)bg;  // a flush error is kept as the background error
  }
  for (auto& b : G->batches)
    if (!b->result.ok()) return b->result;
  return Status::OK();
}

Status DB::Write(const WriteOptions& o, const WriteBatch& b) {
  Status s;
  auto G = NewGroup({{b, o}}, &s);
  if (!s.ok()) return s;
  return WriteGroupNow(G);
}

Status DB::Put(const WriteOptions& o, CfId c, const std::string& k, const std::string& v) {
  WriteBatch b;
  b.Put(c, k, v);
  return Write(o, b);
}

Status DB::Delete(const WriteOptions& o, CfId c, const std::string& k) {
  WriteBatch b;
  b.Delete(c, k);
  return Write(o, b);
}

// ============================================================ reads
Status DB::Get(CfId c, const std::string& k, std::string* v, const Snapshot* snap) {
  if (!tree.IsReadable(c)) return Status::InvalidArgument("column family is not readable");
  SeqNo s = snap ? snap->seq : tree.lastPublished;
  LookupResult x = tree.Get(c, k, s);
  if (x.kind != LookupResult::kFound) return Status::NotFound();
  *v = x.value;
  return Status::OK();
}

const Snapshot* DB::GetSnapshot() {
  snaps_.push_back(Snapshot{tree.lastPublished});
  const Snapshot* s = &snaps_.back();
  tree.snapshots.insert(s->seq);
  if (opts.ghost) opts.ghost->OnSnapshot(s);
  return s;
}

void DB::ReleaseSnapshot(const Snapshot* s) {
  for (auto it = snaps_.begin(); it != snaps_.end(); ++it)
    if (&*it == s) {
      tree.snapshots.erase(tree.snapshots.find(s->seq));
      if (opts.ghost) opts.ghost->OnReleaseSnapshot(s);
      snaps_.erase(it);
      return;
    }
}

std::vector<std::vector<std::pair<std::string, std::string>>> DB::NewIterators(const std::vector<CfId>& cs,
                                                                               const Snapshot* snap) {
  SeqNo s = snap ? snap->seq : tree.lastPublished;  // if no snapshot is given, s <- lastPublished
  return tree.NewIterators(cs, s);
}

// ============================================================ flushes
Status DB::Flush(CfId c) {
  if (!tree.IsLive(c)) return Status::InvalidArgument("column family is not live");
  Status s = wal.RequestFlush({c}, FlushCause::kManual);  // WAL6b
  Step("WAL6b RequestFlush");
  if (!s.ok()) return s;
  if (opts.inline_background) s = RunBackgroundWork();
  return s;
}

Status DB::FlushWAL(bool sync) {
  Status s;
  for (auto& l : wal.L)
    if (s.ok()) s = wal.FlushWAL(*l);
  if (s.ok() && sync) s = wal.SyncWALs();
  Step("WAL3a/WAL3b FlushWAL");
  return s;
}

Status DB::RunBackgroundWork() {
  Status s = tree.RunScheduledFlushes();
  Step("MT5/MT6 flush jobs");
  if (!s.ok()) return s;
  s = wal.PurgeObsoleteLogs();
  Step("WAL8b DisposeLog");
  return s;
}

uint64_t DB::FlushStart(CfId c) {
  uint64_t id = tree.FlushStart(c);
  Step("MT5 FlushJob (start)");
  return id;
}
Status DB::FlushFinish(uint64_t job) {
  Status s = tree.FlushFinish(job);
  Step("MT5 FlushJob (finish)");
  return s;
}
Status DB::InstallFlush(CfId c) {
  Status s = tree.InstallFlush(c);
  Step("MT6 InstallFlush");
  return s;
}
Status DB::PurgeObsoleteLogs() {
  Status s = wal.PurgeObsoleteLogs();
  Step("WAL8b DisposeLog");
  return s;
}

Status DB::Resume() {
  if (wal.bgError && !wal.bgErrorSoft) return wal.bgStatus;  // needs a reopen
  wal.bgError = false;
  wal.bgStatus = Status::OK();
  return Status::OK();
}

// ============================================================ column families
Status DB::CreateColumnFamily(const std::string& name, CfId* c) {
  if (readOnly) return Status::InvalidArgument("read-only DB");
  Status s = tree.CreateColumnFamily(name, c);
  Step("CF1 CreateColumnFamily");
  return s;
}
Status DB::DropColumnFamily(CfId c) {
  Status s = tree.DropColumnFamily(c);
  Step("CF2 DropColumnFamily");
  return s;
}
Status DB::ReleaseHandle(CfId c) {
  Status s = tree.ReleaseHandle(c);
  Step("CF3 ReleaseHandle");
  return s;
}

// ============================================================ ingestion and compaction
Status DB::IngestExternalFile(CfId c, const std::vector<std::string>& files, const IngestOptions& o) {
  return IngestExternalFiles({IngestArg{c, files, o}});
}
Status DB::IngestExternalFiles(const std::vector<IngestArg>& args) {
  if (readOnly) return Status::InvalidArgument("read-only DB");
  Status s = tree.IngestExternalFiles(args);
  Step("SST2 IngestExternalFiles");
  if (s.ok() && opts.inline_background) wal.PurgeObsoleteLogs();
  return s;
}
Status DB::CompactLevel(CfId c, int from) {
  Status s = tree.Compact(c, from);
  Step("Compaction");
  return s;
}

}  // namespace lsm
