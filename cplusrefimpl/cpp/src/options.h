// options.h: the configuration theta, plus the Fix* switches from the TLA+ models.
#pragma once
#include <cstddef>
#include <cstdint>

namespace lsm {

class Ghost;

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
};

struct Options {
  // ---- opening ----
  bool create_if_missing = true;
  bool create_missing_column_families = false;

  // ---- memtables (MT3, WAL1 stall rule) ----
  size_t write_buffer_size = 4096;      // bytes; MT3 says "full" at this size
  int max_write_buffer_number = 3;      // writes stop when a CF has this many - 1 sealed memtables (MaxImm)
  int max_write_buffer_number_to_merge = 1;  // MT5 picks at most this many sealed memtables per job
  size_t db_write_buffer_size = 0;      // WriteBufferManager limit over all mutable memtables; 0 = off
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
