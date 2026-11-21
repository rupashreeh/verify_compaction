// lsm_tree.cc: MT4 to MT7, CF1 to CF5, SST2 to SST4, and the MANIFEST.
#include "lsm_tree.h"

#include <algorithm>
#include <cstdio>

#include "ghost.h"

namespace lsm {

ColumnFamily* LsmTree::Cf(CfId c) {
  auto it = cfs.find(c);
  return it == cfs.end() ? nullptr : &it->second;
}
const ColumnFamily* LsmTree::Cf(CfId c) const {
  auto it = cfs.find(c);
  return it == cfs.end() ? nullptr : &it->second;
}

// ============================================================ interface used by the WAL
std::vector<CfId> LsmTree::CFs() const {
  std::vector<CfId> out;
  for (auto& [id, cf] : cfs)
    if (cf.state == CfState::kLive) out.push_back(id);
  return out;
}

MemTable* LsmTree::Mutable(CfId c) {
  auto* cf = Cf(c);
  return cf ? cf->mem.get() : nullptr;
}

// ---------------------------------------------------------------- MT4 SealMutable
void LsmTree::SealMutable(CfId c, LogNum n) {
  // pre: no write group is between WAL2 and the end of WAL4a (WAL6a waits)
  ColumnFamily& cf = *Cf(c);
  std::shared_ptr<MemTable> M = cf.mem;
  M->nextLog = n;  // its logged data lies in logs < n
  M->state = MemState::kSealed;
  if (opts.fix.FixScheduleFreeOnSeal) M->MarkImmutable();  // B.ScheduleFreeMem (WBM2)
  cf.imm.push_back(M);  // oldest first
  cf.mem = NewMem(n);   // a new empty memtable with memLog = n
}

bool LsmTree::HasUnflushed(CfId c) const {
  const ColumnFamily* cf = Cf(c);
  return cf && (!cf->mem->IsEmpty() || !cf->imm.empty());
}

Status LsmTree::AdvanceIdle(const std::vector<CfId>& cs, LogNum n) {
  VersionEdit e;
  for (CfId c : cs)
    if (LogNumber(c) < n) e.logNumber.push_back({c, n});
  if (e.empty()) return Status::OK();
  Status s = LogAndApply(e);
  if (s.ok())
    for (CfId c : cs) Cf(c)->mem->memLog = std::max(Cf(c)->mem->memLog, n);
  return s;
}

LogNum LsmTree::LogNumber(CfId c) const {
  const ColumnFamily* cf = Cf(c);
  return cf ? cf->N : 0;
}

void LsmTree::ScheduleFlush(CfId c, FlushCause) { flushQueue.insert(c); }

// LSM12 (DB10): compaction debt slows writes down, then stops them. With
// FixStallNeedsCompaction, as in RocksDB's GetWriteStallConditionAndCause, the L0
// and debt conditions apply only when auto compaction is on: otherwise nothing
// would ever end the stop.
StallState LsmTree::GetStallState() const {
  StallState v = StallState::kGo;
  const bool l0Stalls = !opts.disable_auto_compactions || !opts.fix.FixStallNeedsCompaction;
  for (auto& [id, cf] : cfs) {
    if (cf.state != CfState::kLive) continue;
    if (static_cast<int>(cf.imm.size()) >= opts.max_write_buffer_number - 1) return StallState::kStop;
    if (!l0Stalls) continue;
    int n0 = opts.compaction_style == CompactionStyle::kUniversal ? static_cast<int>(SortedRuns(id).size())
                                                                 : static_cast<int>(cf.l0.size());
    uint64_t debt = PendingCompactionBytes(id);
    if (n0 >= opts.level0_stop_writes_trigger) return StallState::kStop;
    if (opts.hard_pending_compaction_bytes_limit && debt >= opts.hard_pending_compaction_bytes_limit)
      return StallState::kStop;
    if (n0 >= opts.level0_slowdown_writes_trigger ||
        (opts.soft_pending_compaction_bytes_limit && debt >= opts.soft_pending_compaction_bytes_limit))
      v = StallState::kDelay;
  }
  return v;
}

// ---------------------------------------------------------------- WBM4 PickVictims
std::set<CfId> LsmTree::PickWbmVictims() const {
  // cand <- live CFs with a non-empty mutable memtable and no flush pending or running.
  // Every sealed memtable here has a flush scheduled, so "pending or running" is
  // "imm is not empty".
  const ColumnFamily* best = nullptr;
  bool oldest = !wbm || wbm->Policy() == WriteBufferFlushPolicy::kFlushOldest;
  for (auto& [id, cf] : cfs) {
    if (cf.state != CfState::kLive || cf.mem->IsEmpty() || !cf.imm.empty()) continue;
    if (!best) { best = &cf; continue; }
    if (oldest ? cf.mem->id < best->mem->id           // created first
               : cf.mem->bytes > best->mem->bytes)    // largest
      best = &cf;
  }
  if (!best) return {};  // nothing in this DB can help
  return {best->id};
}

size_t LsmTree::FlushableMemory(CfId* largest) const {
  size_t m = 0;
  for (auto& [id, cf] : cfs)
    if (cf.state == CfState::kLive && cf.imm.empty() && cf.mem->bytes > m) {
      m = cf.mem->bytes;
      if (largest) *largest = id;
    }
  return m;
}

// ============================================================ MT5 FlushJob
std::shared_ptr<SstFile> LsmTree::BuildFromMemtables(const std::vector<std::shared_ptr<MemTable>>& P,
                                                     bool useSnapshots, Status* s) {
  // merge of the memtables in P, by key ascending and sequence number descending
  std::map<IKey, std::pair<ValueType, std::string>, IKeyCmp> merged;
  for (auto& M : P)
    for (auto& [k, v] : M->table) merged[k] = v;
  // drop each version that is shadowed by a newer version of the same key with no
  // snapshot in between
  std::vector<Entry> out;
  std::string curKey;
  SeqNo newerSeq = 0;
  bool have = false;
  for (auto& [ik, tv] : merged) {
    bool keep = true;
    if (have && ik.key == curKey && opts.flush_drop_shadowed) {
      // a snapshot s with ik.seq <= s < newerSeq still sees this version
      keep = false;
      if (useSnapshots) {
        auto it = snapshots.lower_bound(ik.seq);
        if (it != snapshots.end() && *it < newerSeq) keep = true;
        // Every read point from lastPublished on is a future snapshot. WAL6a only
        // waits for pending inserts, not for publication, so a sealed memtable can
        // hold an inserted but unpublished version. The version below it must stay:
        // readers will see it until the newer one is published.
        if (opts.fix.FixKeepUnpublishedShadowed && newerSeq > lastPublished) keep = true;
      }
    }
    if (!have || ik.key != curKey) {
      curKey = ik.key;
      have = true;
    }
    if (keep) {
      out.push_back(Entry{ik.key, ik.seq, tv.first, tv.second});
      newerSeq = ik.seq;
    }
  }
  auto f = std::make_shared<SstFile>();
  f->number = nextFileNumber++;
  f->path = NumberedName("sst", f->number, "sst");
  f->entries = std::move(out);
  f->ComputeBounds();
  for (auto& M : P) f->largestLoggedSeq = std::max(f->largestLoggedSeq, M->largestLoggedSeq);
  // write it with the SST builder (real sequence numbers) and sync the file
  *s = env->WriteFileDurably(f->path, EncodeSst(f->entries, 0));
  return f;
}

uint64_t LsmTree::FlushStart(CfId c) {
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return 0;  // dropped CFs are not flushed
  // P <- every Sealed memtable, oldest first, up to max_write_buffer_number_to_merge
  FlushJobInfo job{nextJobId, c, {}};
  for (auto& M : cf->imm) {
    if (M->state != MemState::kSealed) continue;
    if (static_cast<int>(job.P.size()) >= std::max(1, opts.max_write_buffer_number_to_merge)) break;
    job.P.push_back(M);
  }
  if (job.P.empty()) return 0;
  for (auto& M : job.P) {
    M->state = MemState::kBuilding;
    M->jobId = job.id;
    if (M->pendingOut) ReleasePendingOutput(M->pendingOut);  // a retry captures again
    M->pendingOut = 0;
  }
  job.P.front()->pendingOut = CapturePendingOutput();  // DB8: protect the output until MT6
  jobs[job.id] = job;
  return nextJobId++;
}

Status LsmTree::FlushFinish(uint64_t id) {
  auto it = jobs.find(id);
  if (it == jobs.end()) return Status::InvalidArgument("no such flush job");
  FlushJobInfo job = it->second;
  jobs.erase(it);
  Status s;
  auto f = BuildFromMemtables(job.P, true, &s);
  if (!s.ok()) {
    for (auto& M : job.P) M->state = MemState::kSealed;  // the flush is retried
    if (wal) {
      wal->bgError = true;
      wal->bgErrorSoft = true;
      wal->bgStatus = s;
    }
    return s;
  }
  if (blockCache && opts.table_options.prepopulate_block_cache == PrepopulateBlockCache::kFlushOnly)
    Reader(f, true)->Prepopulate();  // BC11: warm the cache with the new file's data blocks
  for (auto& M : job.P) {
    M->state = MemState::kBuilt;
    M->output = f;
  }
  return Status::OK();
}

Status LsmTree::FlushJob(CfId c) {
  uint64_t id = FlushStart(c);
  return id == 0 ? Status::OK() : FlushFinish(id);
}

// ============================================================ MT6 InstallFlush
Status LsmTree::InstallFlush(CfId c) {
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return Status::OK();
  std::vector<std::shared_ptr<MemTable>> P;
  if (opts.fix.FixInstallInOrder) {
    // P <- the longest prefix of Imm_c whose memtables are all Built
    for (auto& M : cf->imm) {
      if (M->state != MemState::kBuilt) break;
      P.push_back(M);
    }
    if (P.empty()) return Status::Busy("the oldest sealed memtable is not built");
  } else {
    // bug variant: install the first built result, even if an older one is not built
    for (auto& M : cf->imm)
      if (M->state == MemState::kBuilt) { P.push_back(M); break; }
    if (P.empty()) return Status::Busy("nothing is built");
  }
  Status s = wal->BeforeFlushInstall({c});  // WAL7a: sync closed logs (K3)
  if (!s.ok()) return s;
  // e <- {add each output file of P to L0 of c; N_c <- nextLog of the newest memtable in P (K2)}
  VersionEdit e;
  std::set<uint64_t> added;
  SeqNo largest = manifestLastSeq;
  for (auto& M : P) {
    largest = std::max(largest, M->largestSeq);
    if (added.insert(M->output->number).second)
      e.addFile.push_back(FileAdd{c, 0, M->output->number, 0, nextEpoch++, M->output->largestLoggedSeq, M->output});
  }
  e.logNumber.push_back({c, P.back()->nextLog});
  e.lastSequence = largest;
  e.nextFileNumber = nextFileNumber;
  s = LogAndApply(e);  // one atomic, durable MANIFEST edit, files synced first (K4)
  if (!s.ok()) return s;
  // Imm_c <- Imm_c without P; install a new super version
  for (auto& M : P) {
    cf->imm.erase(std::find(cf->imm.begin(), cf->imm.end(), M));
    if (M->pendingOut) ReleasePendingOutput(M->pendingOut);
    M->pendingOut = 0;
  }
  // Without FixFreeAfterUnpin, B.FreeMem runs here, even if a reader still holds M.
  if (!opts.fix.FixFreeAfterUnpin)
    for (auto& M : P) M->FreeNow();
  wal->AfterFlushInstall({c}, P);  // WAL7b: record the flush and retire logs (K3)
  // max_write_buffer_size_to_maintain is not modelled: P is freed when no reader holds it
  return Status::OK();
}

Status LsmTree::RunScheduledFlushes() {
  std::set<CfId> q;
  q.swap(flushQueue);
  for (CfId c : q) {
    ColumnFamily* cf = Cf(c);
    if (cf == nullptr || cf->state != CfState::kLive) continue;
    while (!cf->imm.empty()) {
      bool progress = false;
      if (cf->imm.front()->state == MemState::kSealed) {
        Status s = FlushJob(c);
        if (!s.ok()) return s;
        progress = true;
      }
      if (cf->imm.front()->state == MemState::kBuilt) {
        Status s = InstallFlush(c);
        if (!s.ok()) return s;
        progress = true;
      }
      if (!progress) break;  // another job is building it
    }
  }
  return Status::OK();
}

// ============================================================ MT7 Get
template <class FileGet>
LookupResult LsmTree::GetWith(CfId c, const std::string& k, SeqNo s, FileGet fileGet) const {
  const ColumnFamily* cf = Cf(c);
  LookupResult x;
  if (cf == nullptr) return x;
  // memtables hold the newest data: Mut_c, then Imm_c newest first
  x = cf->mem->Get(k, s);  // MT2
  if (x.kind != LookupResult::kNotFound) return x;
  for (auto it = cf->imm.rbegin(); it != cf->imm.rend(); ++it) {
    x = (*it)->Get(k, s);
    if (x.kind != LookupResult::kNotFound) return x;
  }
  bool failed = false;
  for (auto& F : cf->l0) {  // each L0 file, newest first
    x = fileGet(F, true, &failed);
    if (failed) return LookupResult{};
    if (x.kind != LookupResult::kNotFound) return x;
  }
  for (int l = 1; l <= Lmax(); l++)  // each level is sorted with disjoint files
    for (auto& F : cf->levels[l]) {
      if (k < F->lo || k > F->hi) continue;
      x = fileGet(F, false, &failed);
      if (failed) return LookupResult{};
      if (x.kind != LookupResult::kNotFound) return x;
    }
  return LookupResult{};
}

LookupResult LsmTree::Get(CfId c, const std::string& k, SeqNo s) const {
  return GetWith(c, k, s, [&](const std::shared_ptr<SstFile>& F, bool, bool*) { return F->Get(k, s); });
}

LookupResult LsmTree::Read(CfId c, const std::string& k, SeqNo s, Status* st) const {
  *st = Status::OK();
  return GetWith(c, k, s, [&](const std::shared_ptr<SstFile>& F, bool inL0, bool* failed) {
    if (!blockCache) return F->Get(k, s);  // no_block_cache: blocks are read and dropped
    LookupResult r;
    *st = Reader(F, inL0)->Get(k, s, BlockReadOptions{}, &r);  // BC9
    if (!st->ok()) *failed = true;
    return r;
  });
}

// ============================================================ BC9 to BC11 table readers
TableReader* LsmTree::Reader(const std::shared_ptr<SstFile>& f, bool inL0) const {
  auto& r = readers_[f->number];
  if (!r || r->file() != f) {
    // BC10: the cache key prefix. With the fix it holds this open's session id, so
    // the keys of a file are new even if its number was used before.
    std::string prefix = opts.fix.FixUniqueKey ? sessionId + "/" + std::to_string(f->number)
                                               : "file/" + std::to_string(f->number);
    r.reset();
    r = std::make_unique<TableReader>(f, prefix, opts, blockCache.get(), &tableStats, inL0);
  }
  return r.get();
}

void LsmTree::CloseReader(uint64_t number, bool obsolete) {
  auto it = readers_.find(number);
  if (it == readers_.end()) return;
  if (obsolete && opts.uncache_aggressiveness > 0) it->second->Uncache();  // BC11
  readers_.erase(it);  // releases the pinned index and filter blocks (K9)
}

// ============================================================ CF1 CreateColumnFamily
Status LsmTree::CreateColumnFamily(const std::string& name, CfId* out) {
  for (auto& [id, cf] : cfs)
    if (cf.state == CfState::kLive && cf.name == name) return Status::InvalidArgument("CF exists: " + name);
  CfId c = nextCfId;
  LogNum n = wal && wal->a ? wal->a->num : 1;
  VersionEdit e;
  e.addCf.push_back({c, name});
  e.logNumber.push_back({c, n});  // it needs no log older than the active one
  e.nextCfId = c + 1;
  Status s = LogAndApply(e);  // atomic and durable (K4)
  if (!s.ok()) return s;
  Cf(c)->refs = 1;
  *out = c;
  return Status::OK();
}

// ============================================================ CF2 DropColumnFamily
Status LsmTree::DropColumnFamily(CfId c) {
  if (c == kDefaultCf) return Status::InvalidArgument("cannot drop the default CF");
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return Status::InvalidArgument("CF is not live");
  // enter the write queue alone and wait for pending writes to c (WAL4b)
  if (wal->UnpublishedFor(c)) return Status::Busy("writes to the CF are pending");
  VersionEdit e;
  e.dropCf.push_back(c);
  Status s = LogAndApply(e);
  if (!s.ok()) return s;
  cf->state = CfState::kDropped;
  flushQueue.erase(c);
  if (!opts.fix.FixDeferDropDelete) ClearData(*cf);  // bug variant: delete at once
  wal->MarkObsolete();  // WAL8a: MinLogToKeep now ignores c
  // the data of c stays readable through open handles until CF3 releases the last one
  return Status::OK();
}

void LsmTree::ClearData(ColumnFamily& cf) {
  cf.mem = NewMem(cf.mem->memLog);
  cf.imm.clear();
  // The SSTs go the way of compaction inputs (LSM10): an iterator that still pins a
  // version of this CF keeps its files until it lets go.
  for (auto& f : cf.l0) {
    CloseReader(f->number, true);
    obsoleteFiles.push_back(f);
  }
  cf.l0.clear();
  for (auto& lv : cf.levels) {
    for (auto& f : lv) {
      CloseReader(f->number, true);
      obsoleteFiles.push_back(f);
    }
    lv.clear();
  }
  PurgeObsoleteFiles();
}

// ============================================================ CF3 ReleaseHandle
Status LsmTree::ReleaseHandle(CfId c) {
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr) return Status::InvalidArgument("no such CF");
  cf->refs--;
  if (cf->refs <= 0 && cf->state == CfState::kDropped) {
    ClearData(*cf);  // free Mut_c and Imm_c; delete every SST of c
    cf->state = CfState::kGone;
  }
  return Status::OK();
}

// ============================================================ CF5 NewIterators
std::vector<std::vector<std::pair<std::string, std::string>>> LsmTree::NewIterators(
    const std::vector<CfId>& cs, SeqNo s) const {
  // Every iterator reads at the same sequence number s. A batch over several CFs is
  // published at once (WAL4b), so every iterator sees all of it or none of it.
  std::vector<std::vector<std::pair<std::string, std::string>>> out;
  for (CfId c : cs) {
    std::set<std::string> keys;
    const ColumnFamily* cf = Cf(c);
    if (cf) {
      for (auto& [ik, v] : cf->mem->table) keys.insert(ik.key);
      for (auto& M : cf->imm)
        for (auto& [ik, v] : M->table) keys.insert(ik.key);
      for (auto& f : cf->l0)
        for (auto& e : f->entries) keys.insert(e.key);
      for (auto& lv : cf->levels)
        for (auto& f : lv)
          for (auto& e : f->entries) keys.insert(e.key);
    }
    std::vector<std::pair<std::string, std::string>> rows;
    for (auto& k : keys) {
      Status st;
      LookupResult x = Read(c, k, s, &st);  // skips versions newer than s
      if (x.kind == LookupResult::kFound) rows.push_back({k, x.value});
    }
    out.push_back(std::move(rows));
  }
  return out;
}

// ============================================================ SST2 IngestExternalFiles
bool LsmTree::MemKeyInRange(const MemTable& m, const std::string& lo, const std::string& hi) {
  auto it = m.table.lower_bound(IKey{lo, UINT64_MAX});
  return it != m.table.end() && it->first.key <= hi;
}

bool LsmTree::DataOverlap(CfId c, int level, const SstFile& F) const {
  const ColumnFamily* cf = Cf(c);
  auto keyIn = [&](const SstFile& g) {
    if (!g.Overlaps(F.lo, F.hi)) return false;
    auto it = std::lower_bound(g.entries.begin(), g.entries.end(), F.lo,
                               [](const Entry& e, const std::string& k) { return e.key < k; });
    return it != g.entries.end() && it->key <= F.hi;
  };
  const auto& files = level == 0 ? cf->l0 : cf->levels[level];
  for (auto& g : files)
    if (keyIn(*g)) return true;
  return false;
}

bool LsmTree::FitsInLevel(CfId c, int level, const SstFile& F) const {
  if (level == 0) return true;
  for (auto& g : Cf(c)->levels[level])
    if (g->Overlaps(F.lo, F.hi)) return false;
  return true;
}

Status LsmTree::IngestExternalFiles(const std::vector<IngestArg>& args) {
  uint64_t jobId = nextIngestId++;
  struct Prepared {
    CfId c;
    std::shared_ptr<SstFile> f;
    const IngestOptions* o;
  };
  std::vector<Prepared> prep;
  auto cleanup = [&](Status s) {  // an error deletes the copies and installs nothing
    for (auto& p : prep) env->Delete(p.f->path);
    if (ghost) ghost->OnIngestFail(jobId);
    return s;
  };
  // ---- (1) Prepare, without blocking writes ----
  std::set<CfId> S;
  for (auto& arg : args) {
    if (!IsLive(arg.cf)) return cleanup(Status::InvalidArgument("CF is not live"));
    S.insert(arg.cf);
    for (auto& path : arg.files) {
      std::shared_ptr<SstFile> ext;
      Status s = LoadSst(env, path, 0, &ext);
      if (!s.ok()) return cleanup(s);
      for (auto& e : ext->entries)  // check that F came from SST1
        if (e.seq != 0) return cleanup(Status::InvalidArgument("external file has non-zero seqno"));
      uint64_t num = nextFileNumber++;
      std::string dst = NumberedName("sst", num, "sst");
      std::string data;
      env->Read(path, &data);
      if (arg.opts.move_files) {  // hard-link (modelled as a rename)
        s = env->Rename(path, dst);
        if (s.ok()) s = env->SyncDir();
      } else {
        s = env->WriteFileDurably(dst, data);  // copy; sync the file and the directory
      }
      if (!s.ok()) return cleanup(s);
      ext->number = num;
      ext->path = dst;
      prep.push_back({arg.cf, ext, &arg.opts});
    }
  }
  for (size_t i = 0; i < prep.size(); i++)
    for (size_t j = i + 1; j < prep.size(); j++)
      if (prep[i].c == prep[j].c && prep[i].f->Overlaps(prep[j].f->lo, prep[j].f->hi))
        return cleanup(Status::InvalidArgument("two files of the same CF overlap"));
  if (ghost) ghost->OnIngestPrepare(jobId, S);
  // ---- (2) Stop writes ----
  // enter the write queue alone and wait until lastPublished = lastAllocated
  if (opts.fix.FixBlockWrites && (wal->AnyUnpublished() || lastPublished != lastAllocated))
    return cleanup(Status::Busy("ingestion waits for writes in progress"));
  // ---- (3) Flush memtables that overlap the files ----
  if (opts.fix.FixFlushOverlap) {
    for (CfId c : S) {
      ColumnFamily* cf = Cf(c);
      bool overlap = false;
      for (auto& p : prep) {
        if (p.c != c) continue;
        if (MemKeyInRange(*cf->mem, p.f->lo, p.f->hi)) overlap = true;
        for (auto& M : cf->imm)
          if (MemKeyInRange(*M, p.f->lo, p.f->hi)) overlap = true;
      }
      if (!overlap) continue;
      bool allowBlocking = true;
      for (auto& p : prep)
        if (p.c == c) allowBlocking &= p.o->allow_blocking_flush;
      if (!allowBlocking) return cleanup(Status::InvalidArgument("a memtable overlaps and blocking flush is off"));
      Status s = wal->RequestFlush({c}, FlushCause::kIngest);  // WAL6b
      if (!s.ok()) return cleanup(s);
      // wait until MT6 has installed every sealed memtable of c
      while (!cf->imm.empty()) {
        if (cf->imm.front()->state == MemState::kSealed) {
          s = FlushJob(c);
          if (!s.ok()) return cleanup(s);
        }
        s = InstallFlush(c);
        if (!s.ok()) return cleanup(s);
      }
      flushQueue.erase(c);
    }
  }
  // DB9: sync the WAL, so that a crash cannot keep the file and lose an older write
  if (opts.fix.FixSyncWalBeforeIngest && wal) {
    for (auto& l : wal->L) {
      Status s = wal->FlushWAL(*l);
      if (!s.ok()) return cleanup(s);
    }
    Status s = wal->SyncWALs();
    if (!s.ok()) return cleanup(s);
  }
  // ---- (4) Place every file ----
  std::vector<Placement> placed;
  SeqNo snext = lastAllocated + 1;
  bool writeGlobal = true;
  for (auto& p : prep) {
    int level;
    SeqNo s;
    Status st = AssignLevelAndSeqno(p.c, *p.f, snext, *p.o, &level, &s);  // SST3
    if (!st.ok()) return cleanup(st);
    placed.push_back({p.c, p.f, level, s, p.o->ingest_behind});
    writeGlobal &= p.o->write_global_seqno;
  }
  // ---- (5) Commit ----
  Status s = CommitIngestion(placed, jobId, writeGlobal);  // SST4; all or nothing
  if (!s.ok()) return cleanup(s);
  return Status::OK();  // resume writes
}

// ============================================================ SST3 AssignLevelAndSeqno
Status LsmTree::AssignLevelAndSeqno(CfId c, const SstFile& F, SeqNo snext, const IngestOptions& o,
                                    int* level, SeqNo* s) const {
  if (o.ingest_behind) {
    if (!opts.allow_ingest_behind) return Status::InvalidArgument("allow_ingest_behind is off");
    if (!FitsInLevel(c, Lmax(), F)) return Status::InvalidArgument("file does not fit in the bottommost level");
    *level = Lmax();
    *s = 0;  // older than everything; existing versions of a key win
    return Status::OK();
  }
  int l = 0;
  bool overlap = false;
  for (int i = 0; i <= Lmax(); i++) {
    if (opts.allow_ingest_behind && i == Lmax()) continue;  // reserved for ingest-behind
    if (opts.fix.FixIngestSkipCompacting && RangeBeingCompacted(c, i, F.lo, F.hi)) {
      overlap = true;  // a running compaction writes this range of L_i: stay above it,
      break;           // with a new sequence number (RocksDB: RangeOverlapWithCompaction)
    }
    if (DataOverlap(c, i, F)) {
      overlap = true;
      if (opts.fix.FixLevelAbove) break;  // F must stay above this level
    }
    if (FitsInLevel(c, i, F)) l = i;  // go as deep as possible
  }
  bool memOverlap = false;  // only possible when FixFlushOverlap is off
  if (const ColumnFamily* cf = Cf(c)) {
    memOverlap = MemKeyInRange(*cf->mem, F.lo, F.hi);
    for (auto& M : cf->imm) memOverlap |= MemKeyInRange(*M, F.lo, F.hi);
  }
  bool need = (opts.fix.FixGlobalSeqno && (overlap || memOverlap)) ||
              (o.snapshot_consistency && !snapshots.empty()) || o.force_global_seqno;
  *level = l;
  if (need) {
    if (!o.allow_global_seqno) return Status::InvalidArgument("file needs a global seqno");
    *s = snext;  // newer than every existing version
  } else {
    *s = 0;  // no key of F exists yet, and no snapshot can tell
  }
  return Status::OK();
}

// ============================================================ SST4 CommitIngestion
Status LsmTree::CommitIngestion(std::vector<Placement>& placed, uint64_t jobId, bool writeGlobalSeqno) {
  SeqNo sstar = lastAllocated;
  for (auto& p : placed) sstar = std::max(sstar, p.s);
  for (auto& p : placed) {
    if (p.s == 0) continue;
    if (writeGlobalSeqno) {  // write s into the global seqno field of F and sync F
      std::string data;
      env->Read(p.f->path, &data);
      std::memcpy(&data[8], &p.s, 8);
      Status s = env->WriteFileDurably(p.f->path, data);
      if (!s.ok()) return s;
    }  // otherwise s is kept only in the MANIFEST
    p.f->ApplyGlobalSeqno(p.s);
  }
  auto addTo = [&](VersionEdit& e, const Placement& p) {
    p.f->epoch = nextEpoch++;
    e.addFile.push_back(FileAdd{p.c, p.level, p.f->number, p.s, p.f->epoch, 0, p.f});
  };
  if (opts.fix.FixAtomicMultiIngest) {
    // one atomic edit across all CFs (K4)
    VersionEdit e;
    for (auto& p : placed) addTo(e, p);
    if (opts.fix.FixRecordLastSeq) e.lastSequence = std::max(manifestLastSeq, sstar);
    e.nextFileNumber = nextFileNumber;
    Status s = LogAndApply(e);
    if (!s.ok()) return s;
    lastAllocated = std::max(lastAllocated, sstar);
    lastPublished = std::max(lastPublished, sstar);  // readers see every file at once
    if (ghost) {
      std::set<CfId> done;
      for (auto& p : placed) done.insert(p.c);
      ghost->OnIngestCommit(jobId, placed, done);
    }
  } else {
    // bug variant: each CF's files go in their own edit
    std::set<CfId> cs;
    for (auto& p : placed) cs.insert(p.c);
    for (CfId c : cs) {
      VersionEdit e;
      std::vector<Placement> mine;
      for (auto& p : placed)
        if (p.c == c) { addTo(e, p); mine.push_back(p); }
      e.lastSequence = std::max(manifestLastSeq, sstar);
      e.nextFileNumber = nextFileNumber;
      Status s = LogAndApply(e);
      if (!s.ok()) return s;
      lastAllocated = std::max(lastAllocated, sstar);
      lastPublished = std::max(lastPublished, sstar);
      if (ghost) ghost->OnIngestCommit(jobId, mine, {c});
      if (stepHook) stepHook("SST4 (one CF of a non-atomic ingestion)");
    }
  }
  return Status::OK();
}

// ============================================================ MANIFEST
Status LsmTree::LogAndApply(VersionEdit& e) {
  Status s = SyncManifest();  // records go to the MANIFEST in order
  if (!s.ok()) return s;
  s = ManifestAppend(env, e);
  if (!s.ok()) return s;
  NoteDurable(e);
  Apply(e);
  return Status::OK();
}

Status LsmTree::SyncManifest() {
  while (!pendingEdits.empty()) {
    Status s = ManifestAppend(env, pendingEdits.front());
    if (!s.ok()) return s;
    NoteDurable(pendingEdits.front());
    pendingEdits.erase(pendingEdits.begin());
  }
  return Status::OK();
}

void LsmTree::NoteDurable(const VersionEdit& e) {
  for (auto& [c, name] : e.addCf) durableFiles[c];
  for (CfId c : e.dropCf) durableFiles.erase(c);
  for (auto& [c, level, num] : e.delFile) durableFiles[c].erase(num);
  for (auto& fa : e.addFile) durableFiles[fa.cf].insert(fa.number);
}

void LsmTree::Apply(const VersionEdit& e) {
  for (auto& [c, name] : e.addCf) {
    ColumnFamily cf;
    cf.id = c;
    cf.name = name;
    cf.state = CfState::kLive;
    cf.levels.resize(Lmax() + 1);
    cf.mem = NewMem(wal && wal->a ? wal->a->num : 1);
    cfs[c] = std::move(cf);
    nextCfId = std::max(nextCfId, c + 1);
  }
  for (CfId c : e.dropCf) {
    if (recovering_) cfs.erase(c);  // a dropped CF is gone after a restart
    else if (auto* cf = Cf(c)) cf->state = CfState::kDropped;
  }
  for (auto& [c, n] : e.logNumber)
    if (auto* cf = Cf(c)) cf->N = n;
  std::set<uint64_t> moved;  // a trivial move deletes and adds the same file (LSM9)
  for (auto& fa : e.addFile) moved.insert(fa.number);
  for (auto& [c, level, num] : e.delFile) {
    CloseReader(num, !moved.count(num));  // the file is obsolete (BC11), unless it only moved
    auto* cf = Cf(c);
    if (!cf) continue;
    auto& v = level == 0 ? cf->l0 : cf->levels[level];
    v.erase(std::remove_if(v.begin(), v.end(), [&](auto& f) { return f->number == num; }), v.end());
  }
  for (auto& fa : e.addFile) {
    auto* cf = Cf(fa.cf);
    if (!cf) continue;
    std::shared_ptr<SstFile> f = fa.file;
    if (!f) {
      Status s = LoadSst(env, NumberedName("sst", fa.number, "sst"), fa.number, &f);
      if (!s.ok()) continue;  // a missing file is reported by the invariant checks
    }
    if (fa.globalSeqno > 0) f->ApplyGlobalSeqno(fa.globalSeqno);
    f->epoch = fa.epoch;
    f->largestLoggedSeq = fa.largestLoggedSeq;
    nextEpoch = std::max(nextEpoch, fa.epoch + 1);
    if (fa.level == 0) {
      cf->l0.push_back(f);
      std::sort(cf->l0.begin(), cf->l0.end(), [](auto& x, auto& y) { return x->epoch > y->epoch; });
    } else {
      cf->levels[fa.level].push_back(f);
      std::sort(cf->levels[fa.level].begin(), cf->levels[fa.level].end(),
                [](auto& x, auto& y) {  // by key; two files may share a boundary user key
                  if (x->lo != y->lo) return x->lo < y->lo;
                  SeqNo sx = x->entries.empty() ? 0 : x->entries.front().seq;
                  SeqNo sy = y->entries.empty() ? 0 : y->entries.front().seq;
                  return sx > sy;
                });
    }
  }
  if (e.lastSequence) manifestLastSeq = std::max(manifestLastSeq, *e.lastSequence);
  if (e.nextFileNumber) nextFileNumber = std::max(nextFileNumber, *e.nextFileNumber);
  if (e.nextCfId) nextCfId = std::max(nextCfId, *e.nextCfId);
}

Status LsmTree::OpenManifest(bool readOnly) {
  cfs.clear();
  snapshots.clear();
  jobs.clear();
  flushQueue.clear();
  manifestLastSeq = 0;
  if (!env->Exists(kManifestPath)) {
    if (!opts.create_if_missing || readOnly) return Status::InvalidArgument("no MANIFEST");
    VersionEdit e;
    e.addCf.push_back({kDefaultCf, "default"});
    e.logNumber.push_back({kDefaultCf, 1});
    e.nextCfId = 1;
    e.lastSequence = 0;
    e.nextFileNumber = 1;
    Status s = ManifestAppend(env, e);
    if (!s.ok()) return s;
  }
  std::vector<VersionEdit> edits;
  Status s = ManifestRead(env, &edits);
  if (!s.ok()) return s;
  recovering_ = true;
  for (auto& e : edits) Apply(e);
  recovering_ = false;
  for (auto& [id, cf] : cfs) cf.mem = NewMem(cf.N);
  durableFiles.clear();
  for (auto& [id, cf] : cfs) {
    auto& d = durableFiles[id];
    for (auto& f : cf.l0) d.insert(f->number);
    for (auto& lv : cf.levels)
      for (auto& f : lv) d.insert(f->number);
  }
  pendingEdits.clear();
  pendingOutputs.clear();
  // DB1: never reuse the number of a file left behind by a crash
  if (opts.fix.FixBumpFileNumber)
  for (auto& p : env->List("sst")) {
    unsigned long long n = 0;
    if (std::sscanf(p.c_str(), "sst/%llu.sst", &n) == 1) nextFileNumber = std::max<uint64_t>(nextFileNumber, n + 1);
  }
  lastAllocated = lastPublished = manifestLastSeq;
  return Status::OK();
}

// ============================================================ recovery helpers
Status LsmTree::FlushForRecovery(CfId c, VersionEdit* E) {
  ColumnFamily* cf = Cf(c);
  if (cf->mem->IsEmpty()) return Status::OK();
  Status s;
  auto f = BuildFromMemtables({cf->mem}, false, &s);
  if (!s.ok()) return s;
  E->addFile.push_back(FileAdd{c, 0, f->number, 0, nextEpoch++, f->largestLoggedSeq, f});
  E->nextFileNumber = nextFileNumber;
  cf->mem = NewMem(cf->mem->memLog);
  return Status::OK();
}

Status LsmTree::InstallRecoveryEdits(VersionEdit& E, SeqNo last) {
  E.lastSequence = last;
  Status s = LogAndApply(E);
  if (!s.ok()) return s;
  lastAllocated = lastPublished = std::max(manifestLastSeq, last);
  return Status::OK();
}

bool LsmTree::AnySstAbove(SeqNo cut) const {
  // Only data that came through the WAL counts, as in WAL_EndToEnd.tla (ingested
  // files take sequence numbers that never appear in a log).
  for (auto& [id, cf] : cfs) {
    for (auto& f : cf.l0)
      if (f->largestLoggedSeq > cut) return true;
    for (auto& lv : cf.levels)
      for (auto& f : lv)
        if (f->largestLoggedSeq > cut) return true;
  }
  return false;
}

bool LsmTree::ApproxCheckFails(LogNum badLog) const {
  // RocksDB: some CF has N_c past the damaged log and has live SSTs
  for (auto& [id, cf] : cfs) {
    bool hasSst = !cf.l0.empty();
    for (auto& lv : cf.levels) hasSst |= !lv.empty();
    if (cf.N > badLog && hasSst) return true;
  }
  return false;
}

void LsmTree::DeleteOrphanFiles() {
  std::set<std::string> live;
  for (auto& [id, cf] : cfs) {
    for (auto& f : cf.l0) live.insert(f->path);
    for (auto& lv : cf.levels)
      for (auto& f : lv) live.insert(f->path);
  }
  for (auto& p : env->List("sst"))
    if (!live.count(p)) env->Delete(p);
}

}  // namespace lsm
