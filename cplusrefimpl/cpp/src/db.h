// db.h: the public API. DB owns the WAL and the LSM tree and wires them together.
//
// The store is single-threaded and deterministic. Concurrency is modelled the way
// the TLA+ specs model it: each algorithm step is a separate call, and a caller (a
// test, or the random simulator) chooses the interleaving. The convenience calls
// (Put, Write, Flush, ...) run the steps back to back.
//
// Destroying a DB object without Close() is a process crash: buffered log bytes
// are lost, and nothing else is written. Call SimEnv::MachineCrash* afterwards to
// model a power failure, then DB::Open again to run recovery (WAL9).
#pragma once
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "lsm_tree.h"
#include "options.h"
#include "sim_env.h"
#include "wal.h"

namespace lsm {

struct Snapshot {
  SeqNo seq;
};

// A reader's hold on a CF's memtables (RocksDB: a referenced super version, as an
// iterator keeps). While it lives, the memtables are not destroyed, so B.FreeMem
// (WBM2) waits for it.
struct MemtablePin {
  std::vector<std::shared_ptr<MemTable>> mems;
};

class DB : public FlushInitiator {
 public:
  // CF4 Open. cfNames must list every CF in the MANIFEST unless readOnly; a missing
  // name is created if create_missing_column_families is set. handles[i] is the id of cfNames[i].
  static Status Open(SimEnv* env, const Options& opts, const std::vector<std::string>& cfNames,
                     std::unique_ptr<DB>* out, std::vector<CfId>* handles = nullptr, bool readOnly = false);
  static Status Open(SimEnv* env, const Options& opts, std::unique_ptr<DB>* out) {
    return Open(env, opts, {"default"}, out);
  }
  DB(SimEnv* env, const Options& opts);
  ~DB() override;

  Status Close();  // clean shutdown: hand buffered log bytes to the OS

  // ---- writes (WAL1 to WAL4b back to back) ----
  Status Put(const WriteOptions& o, CfId c, const std::string& k, const std::string& v);
  Status Delete(const WriteOptions& o, CfId c, const std::string& k);
  Status Write(const WriteOptions& o, const WriteBatch& b);

  // ---- writes, one step at a time ----
  std::shared_ptr<WriteGroup> NewGroup(const std::vector<std::pair<WriteBatch, WriteOptions>>& batches,
                                       Status* s);
  Status Admit(WriteGroup& G);                            // WAL1
  Status AppendGroup(WriteGroup& G);                      // WAL2 (+ WAL3a, WAL3b)
  Status InsertGroup(WriteGroup& G);                      // WAL4a
  Status InsertBatchCf(WriteGroup& G, size_t i, CfId c);  // WAL4a, one batch into one CF
  Status PublishGroup(WriteGroup& G);                     // WAL4b
  void AbandonGroup(WriteGroup& G) {  // a stalled leader that gives up leaves B's queue (WBM8)
    if (G.wbmStalled && wbmStall_.state == WbmStall::kBlocked) tree.wbm->RemoveDBFromQueue(&wbmStall_);
    wal.Forget(G);
  }

  // ---- the write buffer manager (WBM9 hand-offs come in through FlushInitiator) ----
  WriteBufferManager& Wbm() { return *tree.wbm; }
  std::shared_ptr<MemtablePin> PinMemtables(CfId c);
  size_t FlushableMemory() const override { return tree.FlushableMemory(); }
  bool WritersStalled() const override { return wbmStall_.state == WbmStall::kBlocked; }
  bool FlushLargestCf() override;
  WbmStall* Stall() override { return &wbmStall_; }

  // ---- reads ----
  Status Get(CfId c, const std::string& k, std::string* v, const Snapshot* snap = nullptr);  // MT7
  const Snapshot* GetSnapshot();
  void ReleaseSnapshot(const Snapshot* s);
  std::vector<std::vector<std::pair<std::string, std::string>>> NewIterators(
      const std::vector<CfId>& cs, const Snapshot* snap = nullptr);  // CF5

  // ---- flushes and background work ----
  Status Flush(CfId c);                 // WAL6b with cause Manual, then the flush job if inline
  Status FlushWAL(bool sync);           // WAL3a (and WAL3b if sync), for manual_wal_flush
  Status RunBackgroundWork();           // scheduled flush jobs (MT5, MT6) and log purges (WAL8b)
  uint64_t FlushStart(CfId c);          // MT5, first half
  Status FlushFinish(uint64_t job);     // MT5, second half
  Status InstallFlush(CfId c);          // MT6
  Status PurgeObsoleteLogs();           // WAL8b
  Status Resume();                      // clears a flush error (not a WAL I/O error)

  // ---- column families ----
  Status CreateColumnFamily(const std::string& name, CfId* c);  // CF1
  Status DropColumnFamily(CfId c);                              // CF2
  Status ReleaseHandle(CfId c);                                 // CF3

  // ---- ingestion and compaction ----
  Status IngestExternalFile(CfId c, const std::vector<std::string>& files, const IngestOptions& o = {});
  Status IngestExternalFiles(const std::vector<IngestArg>& args);  // SST2
  Status CompactLevel(CfId c, int from);  // manual: level from into from + 1, start to finish

  // ---- compaction, one step at a time (LSM5/LSM6 then LSM7 to LSM9) ----
  uint64_t CompactStart(CfId c);        // pick by the style's policy; 0 if nothing is due
  Status CompactFinish(uint64_t id);
  Status MaybeCompact();                // LSM11: run every due compaction now

  // ---- versions (LSM10) ----
  std::shared_ptr<VersionPin> PinVersion(CfId c);       // an iterator's hold on the files
  void ReleaseVersion(std::shared_ptr<VersionPin>* p);  // Unref, then purge obsolete files
  Status ReadPinned(const VersionPin& p, const std::string& k, std::string* v);

  // ---- the RocksDBStore steps (rocksdb_store/rocksdb_store_algorithm.pdf) ----
  Status CompactWrite(uint64_t id);    // DB6: merge and write the outputs
  Status CompactInstall(uint64_t id);  // DB7: one MANIFEST record, then the new version
  Status SyncManifest();               // only without FixSyncBeforeApply: write the applied records
  Status PurgeAllObsoleteFiles();      // DB8: full scan, keeping pending outputs
  void UpdateWriteStall();             // DB10: accepting <- not stopped
  void UpdatePersisted();              // persistedThrough <- what a crash keeps for sure
  // RocksDBStore.persistedThrough: every logged write and every ingestion with a
  // sequence number up to here survives any crash.
  SeqNo persistedThrough = 0;
  // RocksDBStore.accepting: writes may go on (WAL1 would not stop them).
  bool accepting = true;

  // ---- checking ----
  std::vector<std::string> CheckInvariants() const;  // every invariant, now
  std::vector<std::string> CheckStore() const;       // ST2 to ST5 (rocksdb_store/)
  void Step(const char* name);  // called after every step; checks if opts.check_invariants

  SimEnv* env;
  Options opts;
  LsmTree tree;
  Wal wal;
  bool readOnly = false;

 private:
  Status WriteGroupNow(std::shared_ptr<WriteGroup> G);
  WbmStall wbmStall_;
  std::list<Snapshot> snaps_;
  uint64_t nextBatchId_ = 1, nextGroupId_ = 1;
};

}  // namespace lsm
