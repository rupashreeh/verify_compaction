// wal.cc: algorithms WAL1 to WAL9. Each function names the algorithm it implements,
// and the comments follow the numbered steps of the pseudocode.
#include "wal.h"

#include <algorithm>
#include <cstdio>

#include "ghost.h"
#include "lsm_tree.h"

namespace lsm {

const char* LogStateName(LogState s) {
  switch (s) {
    case LogState::kActive: return "Active";
    case LogState::kClosed: return "Closed";
    case LogState::kObsolete: return "Obsolete";
    case LogState::kArchived: return "Archived";
    case LogState::kRecycled: return "Recycled";
    case LogState::kDeleted: return "Deleted";
  }
  return "?";
}

// ============================================================ log record format
std::string EncodeLogRecord(LogRecordType t, const std::string& payload) {
  std::string body(1, static_cast<char>(t));
  body += payload;
  std::string rec;
  PutFixed32(&rec, Crc32(body));
  PutFixed32(&rec, static_cast<uint32_t>(payload.size()));
  rec += body;
  return rec;
}

LogRecord LogReader::Read() {
  LogRecord x;
  if (pos_ >= d_.size()) return x;  // kCleanEOF
  if (d_.size() - pos_ < 9) {
    x.kind = LogRecord::kIncomplete;
    pos_ = d_.size();
    return x;
  }
  uint32_t crc, len;
  std::memcpy(&crc, d_.data() + pos_, 4);
  std::memcpy(&len, d_.data() + pos_ + 4, 4);
  if (d_.size() - pos_ - 9 < len) {  // the record runs past the end of the file
    x.kind = LogRecord::kIncomplete;
    pos_ = d_.size();
    return x;
  }
  size_t start = pos_;
  pos_ += 9 + len;
  if (Crc32(d_.data() + start + 8, 1 + len) != crc) {
    x.kind = LogRecord::kCorrupt;
    return x;
  }
  x.kind = LogRecord::kRecord;
  x.type = static_cast<LogRecordType>(d_[start + 8]);
  Reader r(d_, start + 9, start + 9 + len);
  if (x.type == LogRecordType::kHeader) {
    uint8_t flag = 0;
    if (!r.Fixed64(&x.headerNum) || !r.Fixed64(&x.predLast) || !r.Byte(&flag) || !r.Fixed64(&x.recoveryCut))
      x.kind = LogRecord::kCorrupt;
    x.afterRecovery = flag != 0;
    return x;
  }
  if (x.type != LogRecordType::kGroup) {
    x.kind = LogRecord::kCorrupt;
    return x;
  }
  uint32_t m;
  if (!r.Fixed64(&x.firstSeq) || !r.Fixed32(&m)) {
    x.kind = LogRecord::kCorrupt;
    return x;
  }
  for (uint32_t i = 0; i < m; i++) {
    uint32_t c;
    uint8_t t;
    Entry e;
    if (!r.Fixed32(&c) || !r.Byte(&t) || !r.Str(&e.key) || !r.Str(&e.value)) {
      x.kind = LogRecord::kCorrupt;
      return x;
    }
    e.type = static_cast<ValueType>(t);
    e.seq = x.firstSeq + i;
    x.records.push_back({c, std::move(e)});
  }
  return x;
}

// ============================================================ queries
LogNum Wal::MinLogToKeep() const {
  // WAL8a: the minimum over the durable N_c of the live CFs. A dropped CF stops
  // pinning logs at once (CF2). It is never above the active log.
  LogNum m = a ? a->num : UINT64_MAX;
  for (CfId c : T_->CFs()) m = std::min(m, T_->LogNumber(c));
  return m;
}

uint64_t Wal::AccountedWALBytes() const {
  uint64_t sum = 0;
  for (auto& l : L)
    if (l->state == LogState::kActive || l->state == LogState::kClosed) sum += l->accountedBytes;
  return sum;
}

uint64_t Wal::EffectiveWALLimit() const {
  if (opts_.max_total_wal_size > 0) return opts_.max_total_wal_size;
  return 4 * opts_.write_buffer_size * std::max<size_t>(1, T_->CFs().size());
}

bool Wal::PendingInserts() const {
  for (auto& G : Q)
    for (auto& b : G->batches)
      if ((b->stage == BatchStage::kLogged || b->stage == BatchStage::kUnlogged) && !b->AllInserted())
        return true;
  return false;
}

bool Wal::PendingFor(CfId c) const {
  for (auto& G : Q)
    for (auto& b : G->batches)
      if ((b->stage == BatchStage::kLogged || b->stage == BatchStage::kUnlogged) &&
          b->Cfs().count(c) && !b->insertedCfs.count(c))
        return true;
  return false;
}

bool Wal::UnpublishedFor(CfId c) const {
  for (auto& G : Q)
    for (auto& b : G->batches)
      if (b->stage >= BatchStage::kSequenced && b->stage < BatchStage::kPublished && b->Cfs().count(c))
        return true;
  return false;
}

bool Wal::AnyUnpublished() const {
  for (auto& G : Q)
    if (G->appended && !G->published) return true;
  return false;
}

LogFile* Wal::Find(LogNum n) const {
  for (auto& l : L)
    if (l->num == n) return l.get();
  return nullptr;
}

void Wal::Forget(const WriteGroup& G) {
  Q.erase(std::remove_if(Q.begin(), Q.end(), [&](auto& g) { return g.get() == &G; }), Q.end());
}

void Wal::SetBgError(const Status& s, bool soft) {
  if (bgError && !bgErrorSoft) return;  // keep the first hard error
  bgError = true;
  bgErrorSoft = soft;
  bgStatus = s;
}

void Wal::AddRequest(CfId c, FlushCause cause, FlushReqState st) {
  for (auto& r : F)
    if (r.c == c && r.cause == cause && r.st == st) return;  // an equivalent request is pending
  F.push_back({c, cause, st});
}

// ============================================================ WAL1 Admit
Status Wal::Admit(WriteGroup& G) {
  // when at the head of the write queue, before anything in G is logged
  if (readOnly) return Status::InvalidArgument("read-only DB");
  if (G.wbmStalled) return WbmWait(G);  // the leader is blocked in WBM5; it does not start over
  // WBM5 stops new writers of this DB from joining while its leader is stalled
  if (stall && stall->state == WbmStall::kBlocked) return Status::Busy("waits behind a stalled leader");
  if (bgError) {
    for (auto& b : G.batches) { b->stage = BatchStage::kFailed; b->result = bgStatus; }
    return bgStatus;
  }
  // WAL5: may force a flush now
  Status s = CheckWALPressure();
  if (!s.ok()) return s;
  // memory pressure: the write buffer manager (WBM4)
  if (B && B->ShouldFlush()) {
    auto victims = T_->PickWbmVictims();
    if (!victims.empty()) {
      s = RequestFlush(victims, FlushCause::kWriteBufferManager);  // WAL6b
      if (!s.ok()) return s;
      B->wbmFlushes++;
    }
  }
  // memtables marked full by WAL4a
  std::vector<CfId> marked;
  for (auto& r : F)
    if (r.cause == FlushCause::kMemtableFull && r.st == FlushReqState::kMarked) marked.push_back(r.c);
  for (CfId c : marked) {
    if (!T_->IsLive(c)) continue;
    s = RequestFlush({c}, FlushCause::kMemtableFull);  // WAL6b
    if (!s.ok()) return s;
  }
  // write stalls
  StallState v = T_->GetStallState();
  if (v == StallState::kStop) {
    bool any = false;
    for (auto& b : G.batches) {
      if (b->stage != BatchStage::kQueued) continue;
      if (b->o.noSlowdown) {
        b->stage = BatchStage::kFailed;
        b->result = Status::Incomplete("write stall");
      } else {
        any = true;
      }
    }
    // A real leader waits here until flushes finish. The step engine returns Busy
    // instead, and the caller runs background work and calls Admit again.
    return any ? Status::Busy("write stall") : Status::Incomplete("write stall");
  }
  return WbmStallWrites(G);  // WBM5
}

// ---------------------------------------------------------------- WBM5 StallWrites
Status Wal::WbmStallWrites(WriteGroup& G) {
  if (B && B->ShouldStall()) {
    bool any = false;
    for (auto& b : G.batches) {
      if (b->stage != BatchStage::kQueued) continue;
      if (b->o.noSlowdown) {
        b->stage = BatchStage::kFailed;
        b->result = Status::Incomplete("write buffer manager stall");
      } else {
        any = true;
      }
    }
    if (!any) return Status::Incomplete("write buffer manager stall");
    // stop new writers of this DB from joining the write queue: G stays at its head
    stall->state = WbmStall::kBlocked;
    G.wbmStalled = true;
    B->BeginWriteStall(stall);  // WBM6
    if (stall->state == WbmStall::kBlocked) return Status::Busy("write buffer manager stall");
    G.wbmStalled = false;  // WBM6 signaled it at once: the stall had ended
  }
  for (auto& b : G.batches)
    if (b->stage == BatchStage::kQueued) b->stage = BatchStage::kAdmitted;
  return Status::OK();
}

// The leader waits for its signal. A real leader blocks; the step engine returns
// Busy and the caller calls Admit again later. With kFlushLargestAcrossDBs each
// retry stands for one 20 ms wake-up of WBM9.
Status Wal::WbmWait(WriteGroup& G) {
  if (stall->state == WbmStall::kBlocked && B->FlushAcrossDBs(initiator)) {
    // WBM9 local retry: B removed this DB from its queue; flush here (WBM4), then WBM5 again
    G.wbmStalled = false;
    if (B->ShouldStall()) {
      auto victims = T_->PickWbmVictims();
      if (!victims.empty()) {
        Status s = RequestFlush(victims, FlushCause::kWriteBufferManager);  // WAL6b
        if (!s.ok() && !s.IsBusy()) return s;
        if (s.ok()) B->wbmFlushes++;
      }
    }
    return WbmStallWrites(G);
  }
  if (stall->state == WbmStall::kBlocked) return Status::Busy("write buffer manager stall");
  // signaled: admit the rest of G without testing again (the limit is soft)
  G.wbmStalled = false;
  for (auto& b : G.batches)
    if (b->stage == BatchStage::kQueued) b->stage = BatchStage::kAdmitted;
  return Status::OK();
}

// ============================================================ WAL2 AppendGroup
Status Wal::AppendGroup(WriteGroup& G) {
  if (G.appended) return Status::InvalidArgument("group already appended");
  if (a == nullptr) return Status::InvalidArgument("no active log");
  // assign consecutive sequence numbers to all records of G, starting at the next unused one
  SeqNo next = T_->lastAllocated + 1;
  G.firstSeq = next;
  std::vector<BatchState*> GL;  // batches that go to the log
  for (auto& b : G.batches) {
    if (b->stage != BatchStage::kAdmitted) continue;
    b->firstSeq = next;
    next += b->batch.ops.size();
    b->stage = BatchStage::kSequenced;
    if (!b->o.disableWAL) GL.push_back(b.get());
  }
  G.lastSeq = next - 1;
  T_->lastAllocated = G.lastSeq;
  G.appended = true;
  if (!GL.empty()) {
    // one physical log record for the whole group
    std::string payload;
    uint32_t m = 0;
    for (auto* b : GL) m += b->batch.ops.size();
    PutFixed64(&payload, GL.front()->firstSeq);
    PutFixed32(&payload, m);
    for (auto* b : GL)
      for (auto& op : b->batch.ops) {
        PutFixed32(&payload, op.cf);
        payload.push_back(static_cast<char>(op.type));
        PutStr(&payload, op.key);
        PutStr(&payload, op.value);
      }
    std::string rec = EncodeLogRecord(LogRecordType::kGroup, payload);
    a->buffer += rec;
    a->appendedPos += rec.size();
    a->accountedBytes += rec.size();
    a->numRecords++;
    a->lastSeq = G.lastSeq;
    bool anySync = false;
    for (auto* b : GL) {
      b->stage = BatchStage::kLogged;
      b->log = a->num;
      b->endPos = a->appendedPos;
      anySync |= b->o.sync;
    }
    if (opts_.ghost)
      for (auto* b : GL) opts_.ghost->OnAppend(*b);
    if (!opts_.manual_wal_flush || anySync) {  // WAL3a (a sync write flushes first)
      Status s = FlushWAL(*a);
      if (!s.ok()) return s;
    }
    if (anySync) {  // WAL3b; the group waits for it
      Status s = SyncWALs();
      if (!s.ok()) {
        for (auto* b : GL) b->result = s;
        return s;
      }
    }
  }
  for (auto& b : G.batches)
    if (b->stage == BatchStage::kSequenced) {
      b->stage = BatchStage::kUnlogged;
      if (opts_.ghost) opts_.ghost->OnAppend(*b);
    }
  return Status::OK();
}

// ============================================================ WAL3a FlushWAL
Status Wal::FlushWAL(LogFile& l) {
  if (l.buffer.empty()) return Status::OK();
  Status s = env_->Append(l.path, l.buffer);  // survives a process crash, not a power failure
  if (!s.ok()) {
    SetBgError(s, false);
    return s;
  }
  l.buffer.clear();
  l.flushedPos = l.appendedPos;
  return Status::OK();
}

// Syncs one log (used by WAL3b and WAL7a).
Status Wal::SyncLog(LogFile& l) {
  Status s = FlushWAL(l);
  if (!s.ok()) return s;
  uint64_t p = l.flushedPos;  // position at the time the sync is issued
  if (l.syncedPos < p) {
    s = env_->Sync(l.path);
    if (!s.ok()) {
      SetBgError(s, false);  // never retried as success
      return s;
    }
  }
  if (!l.dirSynced && opts_.fix.FixDirSync) {  // C2
    env_->SyncDir();
    l.dirSynced = true;
  }
  l.syncedPos = p;
  return Status::OK();
}

// ============================================================ WAL3b SyncWALs
Status Wal::SyncWALs() {
  // for each live log with syncedPos < flushedPos, in increasing number (C1);
  // without FixSyncAllLogs only the active log is synced
  for (auto& l : L) {
    bool target = opts_.fix.FixSyncAllLogs
                      ? ((l->state == LogState::kActive || l->state == LogState::kClosed) && Live(*l))
                      : l.get() == a;
    if (!target) continue;
    if (l->syncedPos < l->appendedPos || !l->dirSynced) {
      Status s = SyncLog(*l);
      if (!s.ok()) return s;
    }
  }
  // mark synced every logged batch whose record lies at or before its log's syncedPos
  for (auto& G : Q)
    for (auto& b : G->batches)
      if (b->stage >= BatchStage::kLogged && b->log != 0 && !b->synced) {
        LogFile* l = Find(b->log);
        if (l && l->syncedPos >= b->endPos) b->synced = true;
      }
  return Status::OK();
}

// ============================================================ WAL4a InsertGroup
Status Wal::InsertBatchCf(WriteGroup& G, size_t i, CfId c) {
  auto& b = G.batches.at(i);
  if (b->stage != BatchStage::kLogged && b->stage != BatchStage::kUnlogged)
    return Status::InvalidArgument("batch is not logged");  // write-ahead, I1
  if (b->insertedCfs.count(c)) return Status::OK();
  MemTable* M = T_->Mutable(c);  // current memtable of the CF
  if (M == nullptr) return Status::InvalidArgument("no such column family");
  for (size_t j = 0; j < b->batch.ops.size(); j++) {
    auto& op = b->batch.ops[j];
    if (op.cf != c) continue;
    M->Insert(Entry{op.key, b->firstSeq + j, op.type, op.value}, b->stage == BatchStage::kLogged);  // MT1
  }
  b->insertedCfs.insert(c);
  if (M->ShouldFlush()) AddRequest(c, FlushCause::kMemtableFull, FlushReqState::kMarked);  // MT3
  if (opts_.ghost) opts_.ghost->OnInsert(*b, c);
  return Status::OK();
}

Status Wal::InsertGroup(WriteGroup& G) {
  for (size_t i = 0; i < G.batches.size(); i++) {
    auto& b = G.batches[i];
    if (b->stage != BatchStage::kLogged && b->stage != BatchStage::kUnlogged) continue;
    for (CfId c : b->Cfs()) {
      Status s = InsertBatchCf(G, i, c);
      if (!s.ok()) return s;
    }
  }
  return Status::OK();
}

// ============================================================ WAL4b PublishGroup
Status Wal::PublishGroup(WriteGroup& G) {
  if (!G.appended || G.published) return Status::InvalidArgument("group not appended or already published");
  // every earlier group is published
  if (G.firstSeq > T_->lastPublished + 1) return Status::Busy("an earlier group is not yet published");
  // every batch of G is inserted
  if (opts_.fix.FixPublishAfterInserts && !G.AllInserted()) return Status::Busy("inserts pending");
  T_->lastPublished = std::max(T_->lastPublished, G.lastSeq);  // readers see G from now on
  G.published = true;
  if (opts_.ghost) opts_.ghost->OnPublish(G);
  for (auto& b : G.batches) {
    if (b->stage == BatchStage::kFailed) continue;
    b->stage = BatchStage::kPublished;
    if (!b->o.sync || b->synced) {
      b->result = Status::OK();  // return success to the client
      if (opts_.ghost) opts_.ghost->OnAck(*b);
    } else if (b->result.ok()) {
      b->result = Status::IOError("sync did not complete");
    }
  }
  Forget(G);
  return Status::OK();
}

// ============================================================ WAL5 CheckWALPressure
Status Wal::CheckWALPressure() {
  if (T_->CFs().size() <= 1 || AccountedWALBytes() <= EffectiveWALLimit())
    return Status::OK();  // single-CF stores never take this path
  LogFile* l0 = nullptr;  // the oldest log with state Active or Closed
  for (auto& l : L)
    if (l->state == LogState::kActive || l->state == LogState::kClosed) { l0 = l.get(); break; }
  if (l0 == nullptr || gettingFlushed.count(l0->num)) return Status::OK();  // fire once per log
  std::set<CfId> S;  // CFs that still need l0
  for (CfId c : T_->CFs())
    if (T_->LogNumber(c) <= l0->num) S.insert(c);
  // wait until no logged batch is waiting to be inserted (I3)
  if (opts_.fix.FixWaitPendingInserts && PendingInserts()) return Status::Busy("WAL5 waits for pending inserts");
  gettingFlushed.insert(l0->num);
  Status s = SwitchMemtable(S);  // WAL6a
  if (!s.ok()) return s;
  for (CfId c : S) {
    AddRequest(c, FlushCause::kWALPressure, FlushReqState::kRequested);
    T_->ScheduleFlush(c, FlushCause::kWALPressure);
  }
  return Status::OK();
}

// ============================================================ WAL6a SwitchMemtable
Status Wal::NewLog(LogNum num, bool syncDir, bool afterRecovery, SeqNo recoveryCut) {
  auto l = std::make_shared<LogFile>();
  l->num = num;
  l->path = NumberedName("wal", num, "log");
  l->state = LogState::kActive;
  if (!recyclePool.empty()) {  // reuse a recycled file
    std::string old = recyclePool.back();
    recyclePool.pop_back();
    env_->Rename(old, l->path);
    env_->Truncate(l->path, 0);  // simplification: RocksDB overwrites instead
  }
  if (env_->Exists(l->path)) env_->Delete(l->path);  // a stale file from before a crash
  std::string payload;
  PutFixed64(&payload, num);
  // if verifyPredecessor: write the last sequence number in a into the header
  l->predLast = (opts_.track_and_verify_wals && a != nullptr) ? a->lastSeq : 0;
  PutFixed64(&payload, l->predLast);
  payload.push_back(afterRecovery ? 1 : 0);
  PutFixed64(&payload, recoveryCut);
  std::string hdr = EncodeLogRecord(LogRecordType::kHeader, payload);
  l->buffer = hdr;
  l->appendedPos = hdr.size();
  Status s = FlushWAL(*l);
  if (!s.ok()) return s;
  if (syncDir) {
    s = env_->Sync(l->path);
    if (!s.ok()) return s;
    l->syncedPos = l->flushedPos;
    env_->SyncDir();
    l->dirSynced = true;
  }
  if (a != nullptr && a->state == LogState::kActive) a->state = LogState::kClosed;
  L.push_back(l);
  a = l.get();
  return Status::OK();
}

Status Wal::SwitchMemtable(const std::set<CfId>& S) {
  // pre: no batch is between AppendGroup and InsertGroup (I3)
  if (opts_.fix.FixWaitPendingInserts && PendingInserts()) return Status::Busy("WAL6a waits for pending inserts");
  // This is the only caller of T.SealMutable (K1).
  if (a->numRecords > 0) {  // rotate only a non-empty log
    Status s = NewLog(a->num + 1, false);
    if (!s.ok()) return s;
  }
  for (CfId c : S) {
    MemTable* m = T_->Mutable(c);
    if (m != nullptr && T_->IsLive(c) && !m->IsEmpty())
      T_->SealMutable(c, a->num);  // its logged writes all lie in logs < num(a)
  }
  // idle CFs stop holding old logs
  std::vector<CfId> idle;
  for (CfId c : T_->CFs())
    if (!T_->HasUnflushed(c) && !PendingFor(c) && T_->LogNumber(c) < a->num) idle.push_back(c);
  if (!idle.empty()) return T_->AdvanceIdle(idle, a->num);
  return Status::OK();
}

// ============================================================ WAL6b RequestFlush
Status Wal::RequestFlush(const std::set<CfId>& S, FlushCause cause) {
  Status s = SwitchMemtable(S);  // WAL6a
  if (!s.ok()) return s;
  for (CfId c : S) {
    F.erase(std::remove_if(F.begin(), F.end(),
                           [&](const FlushRequest& r) {
                             return r.c == c && r.cause == cause && r.st == FlushReqState::kMarked;
                           }),
            F.end());
    AddRequest(c, cause, FlushReqState::kRequested);
    T_->ScheduleFlush(c, cause);
  }
  return Status::OK();
}

// ============================================================ WAL7a BeforeFlushInstall
Status Wal::BeforeFlushInstall(const std::set<CfId>& S) {
  (void)S;
  // called by T just before the MANIFEST update that installs the flush (K3)
  if (T_->CFs().size() > 1 && opts_.fix.FixSyncClosedOnFlush) {
    for (auto& l : L) {
      if (l->state == LogState::kClosed && Live(*l)) {
        Status s = SyncLog(*l);  // to its appended position, and its directory entry if needed
        if (!s.ok()) return s;
      }
    }
  }
  return Status::OK();
}

// ============================================================ WAL7b AfterFlushInstall
void Wal::AfterFlushInstall(const std::set<CfId>& S, const std::vector<std::shared_ptr<MemTable>>& mems) {
  // called by T just after the atomic MANIFEST update (K2, K3, K4)
  if (opts_.ghost) opts_.ghost->OnFlushInstalled(S, mems);  // D: bookkeeping for the proofs only
  // mark the requests for c in S whose memtables are now all flushed Committed
  for (CfId c : S) {
    const ColumnFamily* cf = T_->Cf(c);
    if (cf && cf->imm.empty())
      F.erase(std::remove_if(F.begin(), F.end(),
                             [&](const FlushRequest& r) { return r.c == c && r.st == FlushReqState::kRequested; }),
              F.end());
  }
  MarkObsolete();  // WAL8a
}

// ============================================================ WAL8a MarkObsolete
void Wal::MarkObsolete() {
  LogNum minKeep = MinLogToKeep();  // durable values from the MANIFEST
  for (auto& l : L)
    if (l->state == LogState::kClosed && l->num < minKeep) l->state = LogState::kObsolete;
}

// ============================================================ WAL8b DisposeLog
Status Wal::DisposeLog(LogFile& l) {
  if (l.state != LogState::kObsolete) return Status::InvalidArgument("log is not obsolete");
  if (opts_.archive_wal) {  // kept for replication or tailing
    env_->Rename(l.path, NumberedName("archive", l.num, "log"));
    l.state = LogState::kArchived;
  } else if (recyclePool.size() < opts_.recycle_log_file_num) {
    std::string p = NumberedName("recycle", l.num, "log");
    env_->Rename(l.path, p);
    recyclePool.push_back(p);
    l.state = LogState::kRecycled;
  } else {
    Status s = env_->Delete(l.path);
    if (!s.ok()) return s;
    l.state = LogState::kDeleted;
  }
  return Status::OK();
}

Status Wal::PurgeObsoleteLogs() {
  for (auto& l : L)
    if (l->state == LogState::kObsolete) {
      Status s = DisposeLog(*l);
      if (!s.ok()) return s;
    }
  // archived, recycled and deleted logs are no longer tracked in L
  L.erase(std::remove_if(L.begin(), L.end(),
                         [](auto& l) {
                           return l->state == LogState::kDeleted || l->state == LogState::kArchived ||
                                  l->state == LogState::kRecycled;
                         }),
          L.end());
  return Status::OK();
}

// ============================================================ WAL9 Recover
Status Wal::Recover(bool ro) {
  readOnly = ro;
  Status s = T_->OpenManifest(ro);
  if (!s.ok()) return s;
  LogNum minKeep = UINT64_MAX;
  for (CfId c : T_->CFs()) minKeep = std::min(minKeep, T_->LogNumber(c));

  // the log files on disk, sorted by number
  std::vector<std::pair<LogNum, std::string>> found;
  for (auto& p : env_->List("wal")) {
    unsigned long long n = 0;
    if (std::sscanf(p.c_str(), "wal/%llu.log", &n) == 1) found.push_back({n, p});
  }
  std::sort(found.begin(), found.end());
  recyclePool = env_->List("recycle");
  L.clear();
  a = nullptr;

  SeqNo next = T_->manifestLastSeq + 1;
  bool stop = false, flushedMid = false;
  SeqNo cut = 0, prev = 0;
  LogNum badLog = 0;
  size_t replayed = 0;
  VersionEdit E;
  const auto mode = opts_.wal_recovery_mode;
  LogNum lastFound = found.empty() ? 0 : found.back().first;

  for (auto& [num, path] : found) {
    std::string data;
    env_->Read(path, &data);
    auto lf = std::make_shared<LogFile>();
    lf->num = num;
    lf->path = path;
    lf->appendedPos = lf->flushedPos = lf->syncedPos = data.size();  // what survived is on disk
    lf->accountedBytes = data.size();
    lf->dirSynced = true;
    lf->state = num < minKeep ? LogState::kObsolete : LogState::kClosed;
    L.push_back(lf);
    if (num < minKeep) continue;  // L <- log files with num >= MinLogToKeep

    LogReader rd(data);
    bool headerSeen = false;
    while (true) {
      LogRecord x = rd.Read();
      if (x.kind == LogRecord::kCleanEOF) break;
      if (x.kind == LogRecord::kCorrupt || x.kind == LogRecord::kIncomplete) {
        // handled by the recovery mode
        if (mode == WALRecoveryMode::kSkipAnyCorruptedRecords) {
          if (x.kind == LogRecord::kIncomplete) break;
          continue;
        } else if (mode == WALRecoveryMode::kTolerateCorruptedTailRecords && num == lastFound &&
                   x.kind == LogRecord::kIncomplete) {
          break;  // damage at the tail of the last log
        } else if (mode == WALRecoveryMode::kPointInTimeRecovery) {
          if (!stop) {
            stop = true;
            cut = next - 1;
            badLog = num;
          }
          break;
        } else {
          return Status::Corruption("damaged log record in " + path);  // Absolute, or damage before the tail
        }
      }
      if (x.type == LogRecordType::kHeader) {
        headerSeen = true;
        lf->predLast = x.predLast;
        // Addition to WAL9: a log started by an earlier recovery that stopped at this
        // same point continues the history, even if its first record does not carry
        // the next sequence number (an unlogged write or an ingestion took it).
        if (opts_.fix.FixResumeAtRecoveryLog && stop && x.afterRecovery && x.recoveryCut == cut) {
          stop = false;
          continue;
        }
        // WAL-hole check: the predecessor is live and its recorded last sequence
        // number is not the last one read
        if (opts_.track_and_verify_wals && x.predLast != 0 && num - 1 >= minKeep && !stop &&
            prev != x.predLast) {
          stop = true;
          cut = next - 1;
          badLog = num;
        }
        continue;
      }
      (void)headerSeen;
      // a group record with first sequence number s0 and m records
      SeqNo s0 = x.firstSeq;
      SeqNo m = x.records.size();
      lf->numRecords++;
      lf->lastSeq = s0 + m - 1;
      if (stop) {
        if (opts_.fix.FixResumeRule && s0 == next) {
          stop = false;  // resume: written after an earlier recovery
        } else {
          prev = s0 + m - 1;
          continue;  // drop the abandoned suffix
        }
      }
      for (auto& [c, r] : x.records) {
        if (!T_->IsLive(c) || num < T_->LogNumber(c)) continue;  // already flushed, or dropped
        T_->Mutable(c)->Insert(r);  // not logged again
        replayed++;
      }
      prev = s0 + m - 1;
      next = s0 + m;
      if (!ro)
        for (CfId c : T_->CFs())
          if (T_->Mutable(c)->ShouldFlush()) {  // mid-recovery flush
            s = T_->FlushForRecovery(c, &E);
            if (!s.ok()) return s;
            flushedMid = true;
          }
    }
  }
  lastRecovery = RecoveryInfo{stop, cut, badLog, replayed, flushedMid};

  // an SST holds data beyond the stopping point
  if (stop) {
    bool bad = opts_.approx_pit_check ? T_->ApproxCheckFails(badLog) : T_->AnySstAbove(cut);
    if (bad) return Status::Corruption("an SST holds data beyond the point where replay stopped");
  }
  if (ro) {
    T_->lastAllocated = T_->lastPublished = std::max(T_->manifestLastSeq, next - 1);
    return Status::OK();
  }

  LogNum nstar = found.empty() ? 0 : found.back().first;  // the largest num in L
  LogNum newA = nstar + 1;
  for (CfId c : T_->CFs()) newA = std::max(newA, T_->LogNumber(c));
  for (CfId c : T_->CFs()) {
    MemTable* M = T_->Mutable(c);
    if (!M->IsEmpty() && (flushedMid || !opts_.avoid_flush_during_recovery)) {
      s = T_->FlushForRecovery(c, &E);  // final flush
      if (!s.ok()) return s;
    }
    if (T_->Mutable(c)->IsEmpty()) {
      E.logNumber.push_back({c, newA});  // stage LogNumber(c) <- n* + 1
      T_->Mutable(c)->memLog = newA;
    }
  }
  // What survived is on disk (WAL_EndToEnd.tla sets synced to the full length). After
  // a process crash the recovered bytes may still sit in the OS cache, so they are
  // synced before anything built from them is installed.
  for (auto& l : L)
    if (l->state == LogState::kClosed && opts_.fix.FixSyncRecoveredLogs) {
      s = env_->Sync(l->path);
      if (!s.ok()) return s;
    }
  s = T_->InstallRecoveryEdits(E, std::max(T_->manifestLastSeq, next - 1));  // atomic
  if (!s.ok()) return s;
  for (auto& l : L)
    if (l->state == LogState::kActive) l->state = LogState::kClosed;
  // a <- NewLog() with SyncDir; the recovered logs stay Closed. If replay stopped and
  // the old logs are kept, the header records where it stopped.
  s = NewLog(newA, true, stop, stop ? cut : 0);
  if (!s.ok()) return s;
  MarkObsolete();  // WAL8a
  return Status::OK();
}

}  // namespace lsm
