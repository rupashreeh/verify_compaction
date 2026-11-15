// lsm_tree.h: the LSM tree T. MemTable life cycle (MT4 to MT7), column families
// (CF1 to CF5), and SST ingestion (SST2 to SST4). The WAL reaches it only through
// the "interface used by the WAL" section below.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "manifest.h"
#include "memtable.h"
#include "options.h"
#include "sim_env.h"
#include "wal.h"

namespace lsm {

class Ghost;

enum class CfState { kNone, kLive, kDropped, kGone };

struct ColumnFamily {
  CfId id = 0;
  std::string name;
  CfState state = CfState::kLive;
  int refs = 0;                                    // open handles (CF3)
  std::shared_ptr<MemTable> mem;                   // Mut_c
  std::vector<std::shared_ptr<MemTable>> imm;      // Imm_c, oldest first
  std::vector<std::shared_ptr<SstFile>> l0;        // newest first
  std::vector<std::vector<std::shared_ptr<SstFile>>> levels;  // levels[1..Lmax]; [0] unused
  LogNum N = 1;                                    // N_c, as in the MANIFEST
};

enum class StallState { kGo, kDelay, kStop };

struct IngestOptions {
  bool move_files = false;
  bool snapshot_consistency = true;
  bool allow_global_seqno = true;
  bool allow_blocking_flush = true;
  bool ingest_behind = false;
  bool write_global_seqno = true;
  bool force_global_seqno = false;
};
struct IngestArg {
  CfId cf;
  std::vector<std::string> files;
  IngestOptions opts;
};

struct FlushJobInfo {
  uint64_t id;
  CfId c;
  std::vector<std::shared_ptr<MemTable>> P;
};

class LsmTree {
 public:
  LsmTree(SimEnv* env, const Options& opts) : env(env), opts(opts) {}

  // ---- interface used by the WAL (contracts K1 to K4) ----
  std::vector<CfId> CFs() const;                    // live column families
  MemTable* Mutable(CfId c);
  void SealMutable(CfId c, LogNum n);               // MT4; only WAL6a calls it (K1)
  bool HasUnflushed(CfId c) const;
  Status AdvanceIdle(const std::vector<CfId>& cs, LogNum n);
  LogNum LogNumber(CfId c) const;
  void ScheduleFlush(CfId c, FlushCause cause);
  StallState GetStallState() const;
  size_t MutableMemoryUsage() const;                // seen by the WriteBufferManager
  std::set<CfId> PickWbmVictims() const;            // the CF with the oldest mutable memtable

  // ---- sequence numbers ----
  SeqNo lastAllocated = 0;    // next unused is lastAllocated + 1 (WAL2)
  SeqNo lastPublished = 0;    // readers see up to here (WAL4b)
  SeqNo manifestLastSeq = 0;  // LastSequence in the MANIFEST

  // ---- MT5, MT6 ----
  uint64_t FlushStart(CfId c);        // MT5 first half: pick and mark Building; 0 if nothing
  Status FlushFinish(uint64_t job);   // MT5 second half: write the SST; Built or Sealed again
  Status FlushJob(CfId c);            // MT5 whole
  Status InstallFlush(CfId c);        // MT6
  Status RunScheduledFlushes();

  // ---- MT7 ----
  LookupResult Get(CfId c, const std::string& k, SeqNo s) const;

  // ---- CF1 to CF5 ----
  Status CreateColumnFamily(const std::string& name, CfId* out);  // CF1
  Status DropColumnFamily(CfId c);                                // CF2
  Status ReleaseHandle(CfId c);                                   // CF3
  std::vector<std::vector<std::pair<std::string, std::string>>> NewIterators(
      const std::vector<CfId>& cs, SeqNo s) const;                // CF5

  // ---- SST2 to SST4 ----
  struct Placement {
    CfId c;
    std::shared_ptr<SstFile> f;
    int level;
    SeqNo s;
    bool behind;
  };
  Status IngestExternalFiles(const std::vector<IngestArg>& args);  // SST2
  Status AssignLevelAndSeqno(CfId c, const SstFile& F, SeqNo snext, const IngestOptions& o,
                             int* level, SeqNo* s) const;           // SST3
  Status CommitIngestion(std::vector<Placement>& placed, uint64_t jobId,
                         bool writeGlobalSeqno);                     // SST4

  // ---- compaction (outside the spec; keeps every version, like MemSST.tla) ----
  Status Compact(CfId c, int from);

  // ---- MANIFEST ----
  Status LogAndApply(VersionEdit& e);  // atomic and durable (K4), then applied in memory
  Status OpenManifest(bool readOnly);

  // ---- recovery helpers used by WAL9 ----
  Status FlushForRecovery(CfId c, VersionEdit* E);
  Status InstallRecoveryEdits(VersionEdit& E, SeqNo last);
  bool AnySstAbove(SeqNo cut) const;
  bool ApproxCheckFails(LogNum badLog) const;
  void DeleteOrphanFiles();

  // ---- helpers ----
  ColumnFamily* Cf(CfId c);
  const ColumnFamily* Cf(CfId c) const;
  bool IsLive(CfId c) const { auto* f = Cf(c); return f && f->state == CfState::kLive; }
  bool IsReadable(CfId c) const {
    auto* f = Cf(c);
    return f && (f->state == CfState::kLive || f->state == CfState::kDropped);
  }
  int Lmax() const { return opts.num_levels - 1; }
  static bool MemKeyInRange(const MemTable& m, const std::string& lo, const std::string& hi);
  bool DataOverlap(CfId c, int level, const SstFile& F) const;
  bool FitsInLevel(CfId c, int level, const SstFile& F) const;
  std::shared_ptr<MemTable> NewMem(LogNum memLog) {
    return std::make_shared<MemTable>(nextMemId++, memLog, opts.write_buffer_size);
  }

  // ---- state ----
  std::map<CfId, ColumnFamily> cfs;
  std::multiset<SeqNo> snapshots;
  std::map<uint64_t, FlushJobInfo> jobs;
  std::set<CfId> flushQueue;
  uint64_t nextFileNumber = 1, nextMemId = 1, nextJobId = 1, nextEpoch = 1, nextIngestId = 1;
  CfId nextCfId = 1;

  Wal* wal = nullptr;
  Ghost* ghost = nullptr;
  std::function<void(const char*)> stepHook;  // lets the DB check invariants mid-algorithm
  SimEnv* env;
  const Options& opts;

 private:
  void Apply(const VersionEdit& e);
  std::shared_ptr<SstFile> BuildFromMemtables(const std::vector<std::shared_ptr<MemTable>>& P,
                                              bool useSnapshots, Status* s);
  void ClearData(ColumnFamily& cf);
  bool recovering_ = false;
};

}  // namespace lsm
