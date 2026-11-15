// wal.h: the write-ahead log, algorithms WAL1 to WAL9.
//
// The WAL owns only its own state W = (L, a, Q, D, F, theta):
//   L  the log files, in increasing number          -> Wal::L
//   a  the active log file                          -> Wal::a
//   Q  the writes in flight, with their stage       -> Wal::Q (write groups)
//   D  the writes safely in SSTs (proofs only)      -> kept by the Ghost, not here
//   F  the flush requests                           -> Wal::F
//   theta the configuration                         -> Options
// Everything else (memtables, SSTs, N_c) belongs to the LSM tree and is reached
// through the LsmTree interface (contracts K1 to K4).
#pragma once
#include <deque>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common.h"
#include "memtable.h"
#include "options.h"
#include "sim_env.h"

namespace lsm {

class LsmTree;

// ---- log files -------------------------------------------------------------
enum class LogState { kActive, kClosed, kObsolete, kArchived, kRecycled, kDeleted };
const char* LogStateName(LogState s);

struct LogFile {
  LogNum num = 0;
  std::string path;
  LogState state = LogState::kActive;
  uint64_t appendedPos = 0;  // bytes appended by the store
  uint64_t flushedPos = 0;   // bytes handed to the OS (WAL3a)
  uint64_t syncedPos = 0;    // bytes forced onto disk (WAL3b)
  bool dirSynced = false;    // the file's directory entry is on disk
  uint64_t accountedBytes = 0;  // counted toward the WAL size limit while Active or Closed
  uint64_t numRecords = 0;      // group records (not counting the header)
  SeqNo lastSeq = 0;            // last sequence number written to this log
  SeqNo predLast = 0;           // header: last sequence number of the predecessor log
  std::string buffer;           // appended but not yet handed to the OS
};

// ---- writes ----------------------------------------------------------------
struct WriteOptions {
  bool sync = false;
  bool disableWAL = false;
  bool noSlowdown = false;
};

struct WriteBatch {
  struct Op {
    CfId cf;
    ValueType type;
    std::string key, value;
  };
  std::vector<Op> ops;
  void Put(CfId c, const std::string& k, const std::string& v) { ops.push_back({c, ValueType::kPut, k, v}); }
  void Delete(CfId c, const std::string& k) { ops.push_back({c, ValueType::kDelete, k, ""}); }
};

// The stages of Q, in order.
enum class BatchStage { kQueued, kAdmitted, kSequenced, kLogged, kUnlogged, kInserted, kPublished, kFailed };

struct BatchState {
  uint64_t id = 0;
  WriteBatch batch;
  WriteOptions o;
  BatchStage stage = BatchStage::kQueued;
  SeqNo firstSeq = 0;   // op i has sequence number firstSeq + i
  LogNum log = 0;       // the log that holds it (0 if unlogged)
  uint64_t endPos = 0;  // end of its record in that log
  bool synced = false;
  std::set<CfId> insertedCfs;
  Status result;
  std::set<CfId> Cfs() const {
    std::set<CfId> s;
    for (auto& op : batch.ops) s.insert(op.cf);
    return s;
  }
  bool AllInserted() const { return insertedCfs == Cfs(); }
};

// A write group: batches that are logged as one record. All batches of a group
// agree on disableWAL (the leader groups only like with like).
struct WriteGroup {
  uint64_t id = 0;
  std::vector<std::shared_ptr<BatchState>> batches;
  SeqNo firstSeq = 0, lastSeq = 0;
  bool appended = false, published = false;
  bool AllInserted() const {
    for (auto& b : batches)
      if (b->stage != BatchStage::kFailed && !b->AllInserted()) return false;
    return true;
  }
};

// ---- flush requests (F) ----------------------------------------------------
enum class FlushCause { kMemtableFull, kWriteBufferManager, kWALPressure, kManual, kIngest, kRecovery };
enum class FlushReqState { kMarked, kRequested, kCommitted };
struct FlushRequest {
  CfId c;
  FlushCause cause;
  FlushReqState st;
};

// ---- the WAL ---------------------------------------------------------------
class Wal {
 public:
  Wal(SimEnv* env, const Options& opts, LsmTree* T) : env_(env), opts_(opts), T_(T) {}

  Status Admit(WriteGroup& G);                       // WAL1
  Status AppendGroup(WriteGroup& G);                 // WAL2
  Status FlushWAL(LogFile& l);                       // WAL3a
  Status SyncWALs();                                 // WAL3b
  Status InsertGroup(WriteGroup& G);                 // WAL4a, the whole group
  Status InsertBatchCf(WriteGroup& G, size_t i, CfId c);  // WAL4a, one batch into one CF
  Status PublishGroup(WriteGroup& G);                // WAL4b
  Status CheckWALPressure();                         // WAL5
  Status SwitchMemtable(const std::set<CfId>& S);    // WAL6a
  Status RequestFlush(const std::set<CfId>& S, FlushCause cause);  // WAL6b
  Status BeforeFlushInstall(const std::set<CfId>& S);              // WAL7a
  void AfterFlushInstall(const std::set<CfId>& S,
                         const std::vector<std::shared_ptr<MemTable>>& mems);  // WAL7b
  void MarkObsolete();                               // WAL8a
  Status DisposeLog(LogFile& l);                     // WAL8b
  Status PurgeObsoleteLogs();                        // WAL8b over every obsolete log
  Status Recover(bool readOnly);                     // WAL9

  // ---- queries ----
  LogNum MinLogToKeep() const;  // min N_c over live CFs, and never above the active log
  bool Live(const LogFile& l) const { return l.num >= MinLogToKeep(); }
  uint64_t AccountedWALBytes() const;
  uint64_t EffectiveWALLimit() const;
  bool PendingInserts() const;     // some sequenced batch is not yet inserted (I3)
  bool PendingFor(CfId c) const;   // ... for CF c
  bool UnpublishedFor(CfId c) const;
  bool AnyUnpublished() const;
  LogFile* Find(LogNum n) const;
  LogNum ActiveNum() const { return a ? a->num : 0; }
  void Forget(const WriteGroup& G);  // removes G from Q

  // ---- state ----
  std::vector<std::shared_ptr<LogFile>> L;  // increasing num; deleted logs are removed
  LogFile* a = nullptr;
  std::deque<std::shared_ptr<WriteGroup>> Q;
  std::vector<FlushRequest> F;
  std::set<LogNum> gettingFlushed;  // logs already targeted by WAL5
  bool bgError = false;
  bool bgErrorSoft = false;  // a flush failure, cleared by Resume; WAL I/O errors are not
  Status bgStatus;
  std::vector<std::string> recyclePool;  // paths of recycled log files
  bool readOnly = false;

  // results of the last WAL9 run, for tests
  struct RecoveryInfo {
    bool stopped = false;
    SeqNo cut = 0;
    LogNum badLog = 0;
    size_t recordsReplayed = 0;
    bool flushedMid = false;
  } lastRecovery;

 private:
  Status NewLog(LogNum num, bool syncDir, bool afterRecovery = false, SeqNo recoveryCut = 0);
  Status SyncLog(LogFile& l);
  void SetBgError(const Status& s, bool soft);
  void AddRequest(CfId c, FlushCause cause, FlushReqState st);

  SimEnv* env_;
  const Options& opts_;
  LsmTree* T_;
};

// Physical log records: crc32 | len | type | payload, crc over type and payload.
enum class LogRecordType : uint8_t { kHeader = 1, kGroup = 2 };
struct LogRecord {
  enum Kind { kRecord, kCleanEOF, kIncomplete, kCorrupt } kind = kCleanEOF;
  LogRecordType type = LogRecordType::kGroup;
  LogNum headerNum = 0;
  SeqNo predLast = 0;              // header
  bool afterRecovery = false;      // header: the log was started by WAL9 ...
  SeqNo recoveryCut = 0;           // ... whose replay stopped after this sequence number
  SeqNo firstSeq = 0;              // group
  std::vector<std::pair<CfId, Entry>> records;  // group, with sequence numbers filled in
};
class LogReader {
 public:
  explicit LogReader(const std::string& data) : d_(data) {}
  LogRecord Read();
  size_t pos() const { return pos_; }
  bool AtEnd() const { return pos_ >= d_.size(); }

 private:
  const std::string& d_;
  size_t pos_ = 0;
};
std::string EncodeLogRecord(LogRecordType t, const std::string& payload);
// header record: framing (9) + num (8) + predLast (8) + afterRecovery (1) + recoveryCut (8)
constexpr size_t kLogHeaderSize = 9 + 25;

}  // namespace lsm
