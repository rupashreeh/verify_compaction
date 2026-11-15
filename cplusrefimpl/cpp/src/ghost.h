// ghost.h: ghost state and the runtime invariant checks.
//
// The TLA+ models state their invariants over history the real store does not
// keep: which writes were issued, logged, acknowledged; which value a read should
// return; which writes a crash may lose. The Ghost records that history from hooks
// in the WAL and the LSM tree. It lives outside the DB, so it survives a crash.
// CheckInvariants compares it with the store after every step.
//
// WAL invariants (WAL_EndToEnd.tla):  Coverage (I4), Lifecycle (I5), NeededImpliesLive
//   (Lemma 1), ActiveLive, Alignment (I3), WriteAhead (I1), NoLostAckWhileUp, and at
//   recovery RecoveryOpens, RecoveryPrefix, RecoveryDurable.
// MemTable/SST invariants (MemSST.tla): MS1 ReadLatest, MS2 SnapshotRead,
//   MS3 NewerShadowsOlder, MS4 BatchAtomic, MS5 IngestAllOrNothing,
//   MS6 LevelsDisjoint, MS7 LogNumberSafe.
#pragma once
#include <map>
#include <set>
#include <string>
#include <vector>

#include "lsm_tree.h"
#include "wal.h"

namespace lsm {

class DB;
struct Snapshot;

class Ghost {
 public:
  struct Write {
    SeqNo seq;
    CfId cf;
    std::string key;
    ValueType type;
    std::string value;
    uint64_t batch;
    bool logged = false;
    LogNum log = 0;
    bool syncReq = false;
    bool acked = false;
    bool published = false;
  };
  struct Hist {  // a committed write or ingested key, in commit order
    CfId cf;
    std::string key;
    bool del;
    std::string value;
    uint64_t t;
    SeqNo seq;
    bool ingest;
  };
  struct Batch {
    std::set<CfId> cfs, inserted;
    SeqNo first = 0, last = 0;
  };
  struct Job {
    std::set<CfId> cfs, done;
  };
  struct RecResult {
    bool ran = false, failed = false, prefixOK = true, durableOK = true;
    std::string detail;
  };

  // ---- hooks ----
  void OnAppend(const BatchState& b);
  void OnInsert(const BatchState& b, CfId c);
  void OnPublish(const WriteGroup& G);
  void OnAck(const BatchState& b);
  void OnFlushInstalled(const std::set<CfId>& S, const std::vector<std::shared_ptr<MemTable>>& mems);
  void OnIngestPrepare(uint64_t job, const std::set<CfId>& cfs);
  void OnIngestCommit(uint64_t job, const std::vector<LsmTree::Placement>& placed, const std::set<CfId>& done);
  void OnIngestFail(uint64_t job);
  void OnSnapshot(const Snapshot* s);
  void OnReleaseSnapshot(const Snapshot* s);
  void OnCrash();                            // call before re-opening after a crash
  void OnRecovered(const DB& db);            // called by DB::Open after WAL9
  void OnRecoveryFailed(const Status& s);    // called by DB::Open when WAL9 fails

  // What a read at logical time t must return (NotFound if absent or deleted).
  bool Expected(CfId c, const std::string& k, uint64_t t, std::string* v) const;

  std::map<SeqNo, Write> writes;  // issued writes the store still claims to hold
  std::vector<Hist> hist;
  uint64_t clock = 0;
  std::map<uint64_t, Batch> batches;
  std::map<uint64_t, Job> jobs;
  std::map<const Snapshot*, std::pair<SeqNo, uint64_t>> snaps;  // (s, t)
  RecResult rec;
  uint64_t flushedRecords = 0;  // |D|
  std::vector<std::string> violations;  // every violation seen so far, with the step
};

std::vector<std::string> CheckInvariants(const DB& db, const Ghost& g);

}  // namespace lsm
