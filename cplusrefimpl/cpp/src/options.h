// options.h: the configuration theta, plus the Fix* switches from the TLA+ models.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lsm {

class Ghost;
class WriteBufferManager;
class BlockCache;

enum class WALRecoveryMode {
  kTolerateCorruptedTailRecords,
  kAbsoluteConsistency,
  kPointInTimeRecovery,  // the default, and the mode the TLA+ model checks
  kSkipAnyCorruptedRecords,
};

// Each switch turns one correctness mechanism on (true, the default) or off.
// The names are the TLA+ constants. With every switch on, every invariant holds.
// Turning one off lets the tests reproduce the counterexample TLC found.
struct Fixes {
  // ---- WAL_EndToEnd.tla ----
  bool FixSyncAllLogs = true;         // C1, WAL3b: a sync covers every live log, not only the active one
  bool FixDirSync = true;             // C2, WAL3b: the first sync of a new log also syncs its directory
  bool FixWaitPendingInserts = true;  // F4, WAL6a/MT4: a switch waits for logged-but-not-inserted
                                      //     writes (MemSST.tla calls this FixSealWaitsInserts)
  bool FixSyncClosedOnFlush = true;   // F7, WAL7a: a multi-CF flush first syncs closed logs
  bool FixResumeRule = true;          // F8, WAL9: PIT replay resumes when a later log continues the sequence
  // ---- MemSST.tla ----
  bool FixInstallInOrder = true;      // MT6: flush results are installed in seal order
  bool FixFlushOverlap = true;        // SST2 (3): flush memtables that overlap an ingested file first
  bool FixGlobalSeqno = true;         // SST3: a file that overlaps existing data gets seqno last+1
  bool FixLevelAbove = true;          // SST3: never place a file below a level it overlaps
  bool FixBlockWrites = true;         // SST2 (2): stop writes and wait for pending ones before ingesting
  bool FixAtomicMultiIngest = true;   // SST4: every CF of one ingestion is installed in one MANIFEST edit
  bool FixPublishAfterInserts = true; // WAL4b: a group is published only after all its inserts
  bool FixDeferDropDelete = true;     // CF2/CF3: a dropped CF's data stays until its last handle goes
  // ---- found by the C++ random simulator (not in the TLA+ models) ----
  bool FixKeepUnpublishedShadowed = true;  // MT5: keep a version shadowed only by an unpublished one
  bool FixSyncRecoveredLogs = true;    // WAL9: sync the replayed logs before installing what was built from them
  bool FixResumeAtRecoveryLog = true;  // WAL9: resume at a log that an earlier recovery started at this cut
  // ---- WBM.tla (write buffer manager) ----
  // Read from Options::fix (memtable side):
  bool FixScheduleFreeOnSeal = true;  // MT4 calls B.ScheduleFreeMem (WBM2): a sealed memtable leaves active
  bool FixFreeAfterUnpin = true;      // WBM2 FreeMem runs when the memtable is destroyed, not at MT6
  // Read from WriteBufferManager::fix (inside B):
  bool FixEndStallOnFree = true;      // WBM2 FreeMem calls MaybeEndWriteStall (WBM7)
  bool FixEndStallOnResize = true;    // WBM8 SetBufferSize calls MaybeEndWriteStall
  bool FixRecheckUnderLock = true;    // WBM6 tests ShouldStall again under the mutex
  bool FixWakeAll = true;             // WBM7 signals every queued DB, not only the first
  bool FixCacheRoundUp = true;        // WBM3 shrinks the reservation to a multiple of D that is >= used
  // ---- BlockCache.tla (block cache) ----
  // Read from Options::fix (table reader side). The switches inside the cache are
  // in BlockCache::fix (CacheFixes).
  bool FixUniqueKey = true;           // BC10: cache keys start with the DB session id, not only the file number
  bool FixCompactionNoFill = true;    // BC9: compaction reads with fill_cache = false
  bool FixReleaseOnClose = true;      // BC9: a block holder releases its pin when it goes away
  // ---- LSMTree.tla (compaction) ----
  bool FixL0Overlap = true;           // LSM3/LSM5: an L0 pick takes every L0 file it overlaps
  bool FixCleanCut = true;            // LSM3: a pick in Ln (n >= 1) takes the files that share a user key
  bool FixOutputOverlap = true;       // LSM5: a pick takes every file of the output level it overlaps
  bool FixSkipBusy = true;            // LSM5/LSM6: never pick a busy file or a range a compaction writes
  bool FixSnapshotKeep = true;        // LSM7: keep the newest version in each snapshot stripe
  bool FixTombstoneBottom = true;     // LSM7: drop a deletion only if no older version can be below
  bool FixAdjacentRuns = true;        // LSM6: universal merges only runs next to each other in age
  bool FixIngestBehindKeep = true;    // LSM7: with allow_ingest_behind, never drop a deletion
  bool FixVersionRef = true;          // LSM10: delete an obsolete file only when no version uses it
  bool FixIngestSkipCompacting = true;  // SST3: an ingested file stays above a range a compaction writes
  // ---- RocksDBStore.tla (the whole store) ----
  bool FixAtomicEdit = true;          // DB7: a compaction is one MANIFEST record (delete inputs, add outputs)
  bool FixSyncBeforeApply = true;     // DB7: the record is synced before the new version is used
  bool FixProtectOutputs = true;      // DB8: a full purge keeps every number >= the oldest pending output
  bool FixRecordLastSeq = true;       // DB9: the ingestion's record carries the last sequence number
  bool FixSyncWalBeforeIngest = true; // DB9: sync the WAL before installing an ingested file
                                      //      (RocksDB does not; see rocksdb_store/README.md)
  bool FixStallNeedsCompaction = true;  // DB10: stop writes on L0 or debt only if auto compaction is on
  bool FixSanitizeTriggers = true;    // DB1: stop >= slowdown >= level0_file_num_compaction_trigger
  bool FixBumpFileNumber = true;      // DB1: recovery sets the next file number past every file on disk
};

// RocksDB: compaction_style. Only leveled and universal are in scope.
enum class CompactionStyle { kLevel, kUniversal };
// RocksDB: compaction_pri (two of its orders).
enum class CompactionPri { kMinOverlappingRatio, kRoundRobin };

// RocksDB: CompactionOptionsUniversal (LSM6).
struct CompactionOptionsUniversal {
  int size_ratio = 1;                       // percent
  int min_merge_width = 2;
  int max_merge_width = 1 << 30;
  int max_size_amplification_percent = 200;
  bool allow_trivial_move = false;
};

// RocksDB: BlockBasedTableOptions::prepopulate_block_cache.
enum class PrepopulateBlockCache { kDisable, kFlushOnly };

// The options of BlockBasedTableOptions that concern the block cache (BC9 to BC11).
struct BlockBasedTableOptions {
  // The cache. If null and no_block_cache is false, the DB makes its own 32 MB
  // LRU cache, as RocksDB does. One cache may be shared by many DBs.
  std::shared_ptr<BlockCache> block_cache;
  bool no_block_cache = false;
  size_t block_entries = 4;  // entries per data block (stands in for block_size)
  bool cache_index_and_filter_blocks = false;
  bool cache_index_and_filter_blocks_with_high_priority = true;
  bool pin_l0_filter_and_index_blocks_in_cache = false;
  PrepopulateBlockCache prepopulate_block_cache = PrepopulateBlockCache::kDisable;
};

struct Options {
  // ---- opening ----
  bool create_if_missing = true;
  bool create_missing_column_families = false;

  // ---- memtables (MT3, WAL1 stall rule) ----
  size_t write_buffer_size = 4096;      // bytes; MT3 says "full" at this size
  int max_write_buffer_number = 3;      // writes stop when a CF has this many - 1 sealed memtables (MaxImm)
  int max_write_buffer_number_to_merge = 1;  // MT5 picks at most this many sealed memtables per job
  size_t db_write_buffer_size = 0;      // the DB makes its own WriteBufferManager with this limit; 0 = off
  // A WriteBufferManager shared with other DBs (WBM1 to WBM9). If set, it is used
  // instead of db_write_buffer_size, as in RocksDB.
  std::shared_ptr<WriteBufferManager> write_buffer_manager;
  bool flush_drop_shadowed = true;      // MT5 drops versions no snapshot can see

  // ---- WAL (WAL2, WAL3a, WAL5, WAL8b) ----
  uint64_t max_total_wal_size = 0;      // 0 means 4 x (sum of write_buffer_size over live CFs)
  bool manual_wal_flush = false;        // WAL3a runs only when the user calls FlushWAL
  bool track_and_verify_wals = true;    // FixPredecessorCheck. RocksDB's default is false; the TLA+
                                        // model shows the prefix guarantee needs it.
  bool archive_wal = false;             // WAL8b: archive obsolete logs instead of deleting them
  size_t recycle_log_file_num = 0;      // WAL8b: size of the recycle pool

  // ---- recovery (WAL9) ----
  WALRecoveryMode wal_recovery_mode = WALRecoveryMode::kPointInTimeRecovery;
  bool avoid_flush_during_recovery = false;  // AvoidRecoveryFlush
  bool approx_pit_check = false;             // ApproxPITCheck: RocksDB's approximate consistency check

  // ---- LSM tree ----
  int num_levels = 3;                   // L0, L1, ..., L(num_levels-1); the last is the bottommost
  int level0_stop_writes_trigger = 36;  // WAL1 stall: writes stop at this many L0 files
  bool allow_ingest_behind = false;     // reserves the bottommost level for ingest_behind files

  // ---- compaction (LSM4 to LSM12) ----
  CompactionStyle compaction_style = CompactionStyle::kLevel;
  // RocksDB's default is false. It is true here so that the earlier tests keep the
  // shapes they build by hand; the compaction tests turn it off.
  bool disable_auto_compactions = true;
  int level0_file_num_compaction_trigger = 4;  // LSM4 (leveled: L0 files; universal: sorted runs)
  int level0_slowdown_writes_trigger = 20;     // LSM12
  uint64_t max_bytes_for_level_base = 16384;   // LSM4, bytes
  int max_bytes_for_level_multiplier = 10;     // LSM4
  bool level_compaction_dynamic_level_bytes = true;  // LSM4 (RocksDB's default since 8.4)
  uint64_t target_file_size_base = 4096;       // LSM8, bytes
  int target_file_size_multiplier = 1;         // LSM8
  CompactionPri compaction_pri = CompactionPri::kMinOverlappingRatio;  // LSM5
  CompactionOptionsUniversal compaction_options_universal;             // LSM6
  uint64_t soft_pending_compaction_bytes_limit = 0;  // LSM12; 0 = off
  uint64_t hard_pending_compaction_bytes_limit = 0;  // LSM12; 0 = off
  // Test only (the model's CutAnywhere): LSM8 may end a file between two versions
  // of one user key, so that two files of a level share a user key.
  bool compaction_split_user_keys = false;

  // ---- block cache (BC1 to BC11) ----
  BlockBasedTableOptions table_options;
  uint32_t uncache_aggressiveness = 0;  // > 0: erase the blocks of files that compaction made obsolete

  Fixes fix;

  // ---- checking ----
  // The ghost records the history the invariants are stated over. It must outlive
  // the DB, because it is carried across crashes (see ghost.h).
  Ghost* ghost = nullptr;
  bool check_invariants = false;   // check every invariant after every step
  bool abort_on_violation = false; // abort() at the first violation (assert-like)

  // When true, the convenience calls (Put, Write, Flush, Ingest) run scheduled flush
  // jobs and log purges right away, like a background thread that never lags. Tests
  // that want to interleave steps by hand set it to false.
  bool inline_background = true;
};

}  // namespace lsm
