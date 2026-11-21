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
#include "table_reader.h"
#include "wal.h"
#include "write_buffer_manager.h"

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

// A running compaction (LSM5, LSM6). Its inputs are marked beingCompacted.
struct Compaction {
  uint64_t id = 0;
  CfId c = 0;
  int startLevel = 0;  // L_b (leveled); the newest run's level (universal)
  int outLevel = 0;    // L_o
  std::vector<std::pair<int, std::shared_ptr<SstFile>>> inputs;  // (level, file)
  std::string lo, hi;  // user-key range of the inputs
  uint64_t outEpoch = 0;  // an L0 output takes the smallest epoch of its inputs
  bool trivialMove = false;
  std::string reason;
  // DB6: the job's state after CompactWrite. minOut is the file number captured in
  // pendingOutputs when the job started (RocksDB: CaptureCurrentFileNumberInPendingOutputs).
  uint64_t minOut = 0;
  bool written = false;
  std::vector<std::shared_ptr<SstFile>> outs;
  std::vector<std::pair<std::string, SeqNo>> dropped;
};

// A reader's hold on a version of a CF (LSM10): the files it may read. While a
// pin lives, PurgeObsoleteFiles keeps its files on disk (K13).
struct VersionPin {
  CfId c = 0;
  SeqNo seq = 0;  // the read point
  std::vector<std::pair<int, std::shared_ptr<SstFile>>> files;  // (level, file), read order
};

// LSM4: level targets and scores.
struct LevelInfo {
  int baseLevel = 1;
  std::vector<double> target;  // [level]
  std::vector<double> score;   // [level]
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
  std::set<CfId> PickWbmVictims() const;            // WBM4: the CF B's policy picks
  size_t FlushableMemory(CfId* largest = nullptr) const;  // WBM9: bytes of the largest flushable CF

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
  // Get reads the files directly; the checks (ghost.cc) use it as the reference.
  // Read is the same lookup through the table readers and the block cache (BC9):
  // the user's read path. It fails only if a strict cache refuses a block.
  LookupResult Get(CfId c, const std::string& k, SeqNo s) const;
  LookupResult Read(CfId c, const std::string& k, SeqNo s, Status* st) const;

  // ---- block cache (BC9 to BC11) ----
  TableReader* Reader(const std::shared_ptr<SstFile>& f, bool inL0) const;
  void CloseReader(uint64_t number, bool obsolete);  // the file left the LSM tree
  BlockCache* Cache() const { return blockCache.get(); }

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

  // ---- compaction (LSM2 to LSM12, lsm_tree/lsm_tree_algorithm.pdf) ----
  // Manual compaction of level `from` into from + 1 (all of L0, or the first file
  // of Ln by priority), start to finish. Busy if a running compaction is in the way.
  Status Compact(CfId c, int from);
  std::vector<std::vector<std::pair<int, std::shared_ptr<SstFile>>>> SortedRuns(CfId c) const;  // LSM2
  std::vector<std::shared_ptr<SstFile>> Overlapping(CfId c, int level, const std::string& lo,
                                                    const std::string& hi) const;            // LSM3
  void ExpandToCleanCut(CfId c, int level, std::vector<std::shared_ptr<SstFile>>* I) const;  // LSM3
  LevelInfo LevelTargets(CfId c) const;                                                      // LSM4
  bool PickLeveled(CfId c, Compaction* out, int manualLevel = -1);                           // LSM5
  bool PickUniversal(CfId c, Compaction* out);                                               // LSM6
  uint64_t CompactStart(CfId c, int manualLevel = -1);  // pick and mark busy; 0 if nothing to do
  Status CompactFinish(uint64_t id);                    // LSM7, LSM8, LSM9 (= CompactWrite + CompactInstall)
  Status CompactWrite(uint64_t id);                     // DB6: merge and write the outputs (LSM7, LSM8)
  Status CompactInstall(uint64_t id);                   // DB7: the MANIFEST record and the new version (LSM9)
  // dropped: the deletions the merge dropped, with everything below them (for the ghost)
  Status CompactionMerge(const Compaction& C, std::vector<Entry>* out,
                         std::vector<std::pair<std::string, SeqNo>>* dropped = nullptr) const;  // LSM7
  bool KeyNotExistsBeyond(const Compaction& C, const std::string& k) const;  // LSM7
  bool NeedsCompaction(CfId c) const;
  Status MaybeScheduleCompaction();  // LSM11: run due compactions (inline background)
  uint64_t PendingCompactionBytes(CfId c) const;  // LSM12
  int LastOutputLevel() const { return opts.allow_ingest_behind ? Lmax() - 1 : Lmax(); }
  bool RangeBeingCompacted(CfId c, int level, const std::string& lo, const std::string& hi) const;

  // ---- versions (LSM10) ----
  std::shared_ptr<VersionPin> PinVersion(CfId c);
  LookupResult ReadPinned(const VersionPin& p, const std::string& k, Status* st) const;
  Status PurgeObsoleteFiles();
  Status PurgeAllObsoleteFiles();  // DB8: full scan of the directory (RocksDB: FindObsoleteFiles(force))
  std::multiset<uint64_t> pendingOutputs;  // DB8: numbers captured by running flush and compaction jobs
  uint64_t CapturePendingOutput() { pendingOutputs.insert(nextFileNumber); return nextFileNumber; }
  void ReleasePendingOutput(uint64_t n) {
    auto it = pendingOutputs.find(n);
    if (it != pendingOutputs.end()) pendingOutputs.erase(it);
  }
  std::map<uint64_t, Compaction> compactions;  // running
  std::vector<std::weak_ptr<VersionPin>> pins;
  std::vector<std::shared_ptr<SstFile>> obsoleteFiles;  // out of the current version, maybe on disk
  std::map<std::pair<CfId, int>, std::string> compactCursor;  // kRoundRobin (LSM1's cursor)
  uint64_t nextCompactionId = 1;
  uint64_t compactionsDone = 0, trivialMoves = 0;

  // ---- MANIFEST ----
  Status LogAndApply(VersionEdit& e);  // atomic and durable (K4), then applied in memory
  Status OpenManifest(bool readOnly);
  // Without FixSyncBeforeApply, a compaction's record is applied first and kept
  // here until SyncManifest writes it (the bug of MC_Bug_ApplyBeforeSync).
  std::vector<VersionEdit> pendingEdits;
  Status SyncManifest();
  // The files the MANIFEST on disk lists, per CF (ST2 ManifestLive checks them).
  std::map<CfId, std::set<uint64_t>> durableFiles;
  void NoteDurable(const VersionEdit& e);

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
    return std::make_shared<MemTable>(nextMemId++, memLog, opts.write_buffer_size, wbm);
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
  std::shared_ptr<WriteBufferManager> wbm;  // B; every memtable charges it
  std::shared_ptr<BlockCache> blockCache;   // null with no_block_cache
  std::string sessionId;                    // new at every open (BC10)
  mutable TableStats tableStats;
  std::function<void(const char*)> stepHook;  // lets the DB check invariants mid-algorithm
  SimEnv* env;
  const Options& opts;

 private:
  void Apply(const VersionEdit& e);
  std::shared_ptr<SstFile> BuildFromMemtables(const std::vector<std::shared_ptr<MemTable>>& P,
                                              bool useSnapshots, Status* s);
  void ClearData(ColumnFamily& cf);
  template <class FileGet>
  LookupResult GetWith(CfId c, const std::string& k, SeqNo s, FileGet fileGet) const;
  bool recovering_ = false;
  // declared last, so the readers release their pins before the cache can go away
  mutable std::map<uint64_t, std::unique_ptr<TableReader>> readers_;
};

}  // namespace lsm
