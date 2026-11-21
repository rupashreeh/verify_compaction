#include "db.h"

#include <cstdio>
#include <cstdlib>

#include "ghost.h"

namespace lsm {

DB::DB(SimEnv* e, const Options& o) : env(e), opts(o), tree(e, opts), wal(e, opts, &tree) {
  // DB1 SanitizeOptions: stop >= slowdown >= level0_file_num_compaction_trigger, so
  // that an L0 stop always leaves a compaction due (RocksDB does the same).
  if (opts.fix.FixSanitizeTriggers) {
    opts.level0_slowdown_writes_trigger =
        std::max(opts.level0_slowdown_writes_trigger, opts.level0_file_num_compaction_trigger);
    opts.level0_stop_writes_trigger = std::max(opts.level0_stop_writes_trigger, opts.level0_slowdown_writes_trigger);
  }
  tree.wal = &wal;
  tree.ghost = opts.ghost;
  tree.stepHook = [this](const char* name) { Step(name); };
  // B: the shared write buffer manager, or one of the DB's own (RocksDB does the same)
  tree.wbm = opts.write_buffer_manager ? opts.write_buffer_manager
                                       : std::make_shared<WriteBufferManager>(opts.db_write_buffer_size);
  wal.B = tree.wbm.get();
  // the block cache: the one given, or a 32 MB LRU cache of the DB's own (BC1)
  if (!opts.table_options.no_block_cache)
    tree.blockCache = opts.table_options.block_cache ? opts.table_options.block_cache
                                                     : std::make_shared<BlockCache>(32 << 20);
  tree.sessionId = BlockCache::NewSessionId();
  wal.stall = &wbmStall_;
  wal.initiator = this;
  tree.wbm->RegisterFlushInitiator(this);
}

DB::~DB() {
  // RemoveDBFromQueue (WBM8). The memtables are destroyed after this, with the tree,
  // and each one calls B.FreeMem (WBM2).
  tree.wbm->DeregisterFlushInitiator(this);
}

std::shared_ptr<MemtablePin> DB::PinMemtables(CfId c) {
  auto p = std::make_shared<MemtablePin>();
  if (auto* cf = tree.Cf(c)) {
    p->mems.push_back(cf->mem);
    for (auto& m : cf->imm) p->mems.push_back(m);
  }
  return p;
}

// WBM9 hand-off: B asks this DB to flush its largest CF for another DB's writer.
bool DB::FlushLargestCf() {
  CfId c = 0;
  if (readOnly || tree.FlushableMemory(&c) == 0) return false;
  Status s = wal.RequestFlush({c}, FlushCause::kWriteBufferManager);  // WAL6b; Busy while inserts are pending
  Step("WBM9 FlushAcrossDBs");
  if (!s.ok()) return false;
  if (opts.inline_background) RunBackgroundWork();  // B's executor runs the flush
  return true;
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
  db->persistedThrough = db->tree.lastAllocated;  // everything recovered is on disk
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
  UpdatePersisted();
  UpdateWriteStall();
  if (!opts.check_invariants || !opts.ghost) return;
  auto all = CheckInvariants();
  for (auto& v : CheckStore()) all.push_back(v);  // ST2 to ST5
  for (auto& v : tree.wbm->CheckInvariants()) all.push_back(v);  // BM1, BM2, BM4, BM5
  if (tree.blockCache)
    for (auto& v : tree.blockCache->CheckInvariants()) all.push_back(v);  // CH1 to CH7
  for (auto& v : all) {
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
    // a caller that stops waiting leaves B's queue (WBM8 RemoveDBFromQueue)
    if (G->wbmStalled && wbmStall_.state == WbmStall::kBlocked) tree.wbm->RemoveDBFromQueue(&wbmStall_);
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
  Status st;
  LookupResult x = tree.Read(c, k, s, &st);  // through the block cache (BC9)
  if (!st.ok()) return st;
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
  if (!opts.disable_auto_compactions) {
    s = tree.MaybeScheduleCompaction();  // LSM11
    Step("LSM11 compactions");
    if (!s.ok()) return s;
  }
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

namespace lsm {

uint64_t DB::CompactStart(CfId c) {
  uint64_t id = tree.CompactStart(c);
  Step("LSM5/LSM6 pick (start)");
  return id;
}
Status DB::CompactFinish(uint64_t id) {
  Status s = tree.CompactFinish(id);
  Step("LSM7-LSM9 compaction (finish)");
  return s;
}
Status DB::MaybeCompact() {
  bool was = opts.disable_auto_compactions;  // tree.opts refers to opts
  opts.disable_auto_compactions = false;
  Status s = tree.MaybeScheduleCompaction();
  opts.disable_auto_compactions = was;
  Step("LSM11 compactions");
  return s;
}
std::shared_ptr<VersionPin> DB::PinVersion(CfId c) {
  auto p = tree.PinVersion(c);
  Step("LSM10 Ref");
  return p;
}
void DB::ReleaseVersion(std::shared_ptr<VersionPin>* p) {
  p->reset();
  Status s = tree.PurgeObsoleteFiles();
  (void)s;
  Step("LSM10 Unref + PurgeObsoleteFiles");
}
Status DB::ReadPinned(const VersionPin& p, const std::string& k, std::string* v) {
  Status st;
  LookupResult x = tree.ReadPinned(p, k, &st);
  if (!st.ok()) return st;
  if (x.kind != LookupResult::kFound) return Status::NotFound();
  *v = x.value;
  return Status::OK();
}

}  // namespace lsm

// ============================================================ RocksDBStore (DB1 to DB11)
namespace lsm {

Status DB::CompactWrite(uint64_t id) {
  Status s = tree.CompactWrite(id);
  Step("DB6 CompactWrite");
  return s;
}
Status DB::CompactInstall(uint64_t id) {
  Status s = tree.CompactInstall(id);
  Step("DB7 CompactInstall");
  return s;
}
Status DB::SyncManifest() {
  Status s = tree.SyncManifest();
  Step("DB7 SyncManifest");
  return s;
}
Status DB::PurgeAllObsoleteFiles() {
  Status s = tree.PurgeAllObsoleteFiles();
  Step("DB8 PurgeAllObsoleteFiles");
  return s;
}

// DB10: accepting is false while WAL1 would stop a write group.
void DB::UpdateWriteStall() {
  accepting = !readOnly && tree.GetStallState() != StallState::kStop && !(wal.bgError && !wal.bgErrorSoft);
}

// persistedThrough only grows while the store is up. It reaches lastAllocated
// when every log that holds records is synced, and the largest logged sequence
// number in any SST file (a flush made it durable; WAL7a synced the closed logs of
// the other CFs, K3). Writes with disableWAL are not covered: nothing promises them.
void DB::UpdatePersisted() {
  SeqNo p = persistedThrough;
  bool allSynced = true;
  for (auto& l : wal.L) {
    if (l->state != LogState::kActive && l->state != LogState::kClosed) continue;
    if (l->numRecords > 0 && (l->syncedPos < l->appendedPos || !l->dirSynced)) allSynced = false;
  }
  if (allSynced) p = std::max(p, tree.lastAllocated);
  for (auto& [id, cf] : tree.cfs) {
    for (auto& f : cf.l0) p = std::max(p, f->largestLoggedSeq);
    for (auto& lv : cf.levels)
      for (auto& f : lv) p = std::max(p, f->largestLoggedSeq);
  }
  if (p != persistedThrough) {
    persistedThrough = p;
    if (opts.ghost) opts.ghost->OnPersisted(p);
  }
}

std::vector<std::string> DB::CheckStore() const {
  std::vector<std::string> v;
  // ST2 ManifestLive: every file the MANIFEST on disk lists is on disk.
  for (auto& [c, files] : tree.durableFiles)
    for (uint64_t n : files)
      if (!env->Exists(NumberedName("sst", n, "sst")))
        v.push_back("ST2 ManifestLive: the MANIFEST lists file " + std::to_string(n) + ", which is not on disk");
  // ST3 OutputsLive: what a running job wrote stays until it is installed.
  for (auto& [id, C] : tree.compactions)
    for (auto& f : C.outs)
      if (!env->Exists(f->path))
        v.push_back("ST3 OutputsLive: output " + std::to_string(f->number) + " of compaction " +
                    std::to_string(id) + " was deleted before install");
  for (auto& [id, cf] : tree.cfs)
    for (auto& M : cf.imm)
      if (M->state == MemState::kBuilt && M->output && !env->Exists(M->output->path))
        v.push_back("ST3 OutputsLive: flush output " + std::to_string(M->output->number) + " was deleted before install");
  // ST4 FreshSeq: no version in the store is newer than lastAllocated.
  auto fresh = [&](SeqNo s, const std::string& where) {
    if (s > tree.lastAllocated)
      v.push_back("ST4 FreshSeq: " + where + " holds sequence number " + std::to_string(s) +
                  " but the next one to give out is " + std::to_string(tree.lastAllocated + 1));
  };
  for (auto& [id, cf] : tree.cfs) {
    if (cf.state == CfState::kGone) continue;
    fresh(cf.mem->largestSeq, "the memtable");
    for (auto& M : cf.imm) fresh(M->largestSeq, "a sealed memtable");
    auto files = [&](const std::vector<std::shared_ptr<SstFile>>& fs) {
      for (auto& f : fs)
        for (auto& e : f->entries) fresh(e.seq, "file " + std::to_string(f->number));
    };
    files(cf.l0);
    for (auto& lv : cf.levels) files(lv);
  }
  // ST5 StallEnds: if writes are stopped, a background job can end the stop.
  if (!readOnly && tree.GetStallState() == StallState::kStop) {
    bool memStop = false, due = !tree.compactions.empty();
    for (auto& [id, cf] : tree.cfs) {
      if (cf.state != CfState::kLive) continue;
      if (static_cast<int>(cf.imm.size()) >= opts.max_write_buffer_number - 1) memStop = true;
      if (!opts.disable_auto_compactions && tree.NeedsCompaction(id)) due = true;
    }
    if (!memStop && !due)
      v.push_back(std::string("ST5 StallEnds: writes are stopped on L0 or compaction debt, and no compaction ") +
                  (opts.disable_auto_compactions ? "can run (auto compaction is off)" : "is due"));
  }
  return v;
}

}  // namespace lsm
