# WAL, MemTable, and SST Files in C++

## What this is

This is a C++17 implementation of three parts of an LSM-tree key–value store, modeled on RocksDB:

- the **write-ahead log** (WAL), algorithms WAL1 to WAL9;
- **memtables and column families**, algorithms MT1 to MT7 and CF1 to CF5;
- **SST files**: flushing memtables to disk, and ingesting files built outside the store, algorithms
  SST1 to SST4.

The code follows two TLA+ specifications. `WAL_EndToEnd.tla` describes the WAL. `MemSST.tla`
describes memtables, column families, and SST files. Every C++ function names the algorithm step it
implements, and the code keeps the names the specifications use (`mem`, `imm`, `memLog`, `N[c]`,
`MinLogToKeep`, the flush requests `F`, and so on). Every safety invariant of both specifications is
checked at run time, after every step. Each correctness mechanism can be switched off, and a test
shows that the matching invariant then fails, just as TLC found.

The goal is a faithful, testable reference, not a fast store. It runs in one thread, keeps
everything in memory, and writes to a simulated disk that can crash.

## Background in brief

A key–value store built as an LSM tree keeps new writes in memory, in a sorted buffer called a
**memtable**. When the memtable is full, it is **sealed** (frozen, but still readable) and a fresh one
takes its place. A background **flush** writes the sealed memtable to an **SST**, a sorted file on disk
that never changes. New SSTs go to level 0 (L0). **Compaction** later merges them into deeper levels.

Every write gets a **sequence number** that says how new it is. A read checks the memtable, then the
sealed memtables (newest first), then L0 (newest first), then each deeper level, and returns the first
version it finds. So newer data must always sit above older data.

A crash would lose the memtables. The **WAL** prevents that. Each write is appended to a log file
before it goes into a memtable. After a crash, recovery replays the logs.

A **column family** (CF) is a separate key space with its own memtables and SSTs. All CFs share one
WAL and one sequence of numbers. Each CF c remembers its **log number N[c]**: the oldest log file it
still needs. Log files older than the smallest N[c] (`MinLogToKeep`) are no longer needed and can be
deleted. Sealing any memtable starts a new log file, so log file boundaries match memtable
boundaries. If the logs grow past a size limit, the store flushes the CFs that hold the oldest log
(**WAL pressure**).

**Ingestion** brings in an SST built outside the store, where every key has sequence number 0. The
store stops writes, flushes any memtable that overlaps the file, picks a level, gives the file a new
sequence number if it overlaps existing data, and installs it in one atomic MANIFEST update. The
**MANIFEST** is the durable record of which CFs and SSTs exist, each N[c], and the last sequence number.

## Files

| File | Contents |
|---|---|
| `src/common.h` | Types (`SeqNo`, `LogNum`, `CfId`, `Entry`), `Status`, encoding, CRC32, a seeded RNG |
| `src/options.h` | The configuration θ, and the `Fixes` switches named after the TLA+ constants |
| `src/sim_env.h/.cc` | The simulated disk: OS cache, synced prefix, directory entries, crashes, fault injection |
| `src/memtable.h/.cc` | `MemTable` (MT1 to MT3), `SstFile`, the SST file format, `SstFileWriter` (SST1) |
| `src/manifest.h/.cc` | `VersionEdit` and the MANIFEST: one checksummed, synced record per edit |
| `src/wal.h/.cc` | `Wal`: WAL1 to WAL9, log files, write groups, flush requests, the log record format |
| `src/lsm_tree.h/.cc` | `LsmTree`: MT4 to MT7, CF1 to CF5, SST2 to SST4, compaction, the MANIFEST state |
| `src/db.h/.cc` | `DB`: the public API; opens the store (CF4) and checks invariants after every step |
| `src/ghost.h/.cc` | Ghost state and `CheckInvariants` (WAL invariants, MS1 to MS7, recovery) |
| `tests/test_basic.cc` | Normal operation: reads, flushes, triggers, stalls, CFs, iterators, ingestion |
| `tests/test_crash.cc` | Crashes and recovery, including 900 random power failures |
| `tests/test_bugs.cc` | One test per correctness mechanism: on, no violation; off, the expected violation |
| `tests/test_random.cc` | A random simulator, like TLC's simulation mode |
| `results.txt` | Output of the last test run |

## Building and running

```
make test          # or: cmake -B build . && cmake --build build && ctest --test-dir build
```

Any C++17 compiler works. The tests were run with g++ 13.3, and also with AddressSanitizer and
UndefinedBehaviorSanitizer (`make CXXFLAGS="-std=c++17 -g -fsanitize=address,undefined"`). Set `SEEDS`
to change how many random runs `test_random` makes (150 by default for part 1, 1000 for part 2).

A small example:

```cpp
lsm::SimEnv env;
lsm::Ghost ghost;                      // optional: enables the invariant checks
lsm::Options opts;
opts.ghost = &ghost;
opts.check_invariants = true;
std::unique_ptr<lsm::DB> db;
lsm::DB::Open(&env, opts, &db);

lsm::WriteOptions sync;
sync.sync = true;
db->Put(sync, lsm::kDefaultCf, "k", "v");  // WAL1 to WAL4b
db->Flush(lsm::kDefaultCf);                // WAL6b, WAL6a, MT5, MT6, WAL7a, WAL7b

db.reset();                                // a process crash
env.MachineCrashDropUnsynced();            // then a power failure
ghost.OnCrash();
lsm::DB::Open(&env, opts, &db);            // WAL9 recovery
// ghost.violations is empty if every invariant held
```

## How it is built

**One thread, explicit steps.** The TLA+ models describe concurrency as a choice of which atomic
step runs next. The C++ code does the same. Each algorithm step is a separate call, for example
`Admit`, `AppendGroup`, `InsertBatchCf` (one batch into one CF), `PublishGroup`, `FlushStart`,
`FlushFinish`, and `InstallFlush`. A test or the random simulator picks the order. Where a real store
would block, the step returns `Status::Busy` and changes nothing; the caller runs other steps and
tries again. The convenience calls (`Put`, `Write`, `Flush`, `IngestExternalFile`) run the steps back
to back. With `inline_background` on (the default), they also run scheduled flushes and log purges
right away.

**Two modules with a narrow interface.** `Wal` owns only the WAL state W = (L, a, Q, D, F, θ).
`LsmTree` owns the memtables, SSTs, column families, and the MANIFEST. The WAL reaches the tree only
through the operations the WAL specification lists: `Mutable`, `SealMutable`, `HasUnflushed`,
`AdvanceIdle`, `LogNumber`, `ScheduleFlush`, `StallState`, and the recovery helpers. The four
contracts hold by construction:

- **K1.** Only `Wal::SwitchMemtable` (WAL6a) calls `LsmTree::SealMutable` (MT4), and it rotates the log.
- **K2.** `LsmTree::InstallFlush` (MT6) sets N[c] in the same MANIFEST edit that adds the L0 files.
- **K3.** `InstallFlush` calls `Wal::BeforeFlushInstall` (WAL7a) just before that edit and
  `Wal::AfterFlushInstall` (WAL7b) just after it.
- **K4.** Every MANIFEST edit is one checksummed record, synced before it is applied. Every SST is
  written and synced, with its directory entry, before an edit names it.

**The simulated disk.** `SimEnv` keeps, for each file, the bytes handed to the OS, the length of the
synced prefix, and whether its directory entry is durable. Destroying a `DB` without `Close()` is a
process crash: log bytes still in the store's buffer are lost, and bytes in the OS survive.
`MachineCrash(rng)` is a power failure, as in the `Crash` and `SettleLog` actions of
`WAL_EndToEnd.tla`. Each file keeps its synced prefix plus a random part of the rest, which may end
in garbage (a torn record). A file whose directory entry was never synced may vanish.
`MachineCrashDropUnsynced()` is the worst case. `InjectFault` makes appends or syncs fail.

**Ghost state.** The invariants speak about history the store does not keep: which writes were
logged, acknowledged, or published, and what a read should return. The `Ghost` records it through
hooks in the WAL and the tree. It lives outside the `DB`, so it survives crashes. After each recovery
it checks the recovery invariants and then keeps only what survived, as the TLA+ model does
(`issued' = present`).

## Algorithms and functions

| Algorithm | Function |
|---|---|
| WAL1 Admit | `Wal::Admit` |
| WAL2 AppendGroup | `Wal::AppendGroup` |
| WAL3a FlushWAL, WAL3b SyncWALs | `Wal::FlushWAL`, `Wal::SyncWALs` |
| WAL4a InsertGroup | `Wal::InsertGroup`, `Wal::InsertBatchCf` |
| WAL4b PublishGroup | `Wal::PublishGroup` |
| WAL5 CheckWALPressure | `Wal::CheckWALPressure` |
| WAL6a SwitchMemtable, WAL6b RequestFlush | `Wal::SwitchMemtable`, `Wal::RequestFlush` |
| WAL7a BeforeFlushInstall, WAL7b AfterFlushInstall | `Wal::BeforeFlushInstall`, `Wal::AfterFlushInstall` |
| WAL8a MarkObsolete, WAL8b DisposeLog | `Wal::MarkObsolete`, `Wal::DisposeLog`, `Wal::PurgeObsoleteLogs` |
| WAL9 Recover | `Wal::Recover` |
| MT1 Insert, MT2 Get, MT3 ShouldFlush and IsEmpty | `MemTable::Insert`, `MemTable::Get`, `MemTable::ShouldFlush`, `MemTable::IsEmpty` |
| MT4 SealMutable | `LsmTree::SealMutable` |
| MT5 FlushJob | `LsmTree::FlushStart` and `LsmTree::FlushFinish` (together `FlushJob`) |
| MT6 InstallFlush | `LsmTree::InstallFlush` |
| MT7 Get | `LsmTree::Get` |
| CF1, CF2, CF3 | `LsmTree::CreateColumnFamily`, `DropColumnFamily`, `ReleaseHandle` |
| CF4 Open | `DB::Open` |
| CF5 NewIterators | `LsmTree::NewIterators` |
| SST1 SstFileWriter | `SstFileWriter` |
| SST2, SST3, SST4 | `LsmTree::IngestExternalFiles`, `AssignLevelAndSeqno`, `CommitIngestion` |

The comments inside each function follow the numbered lines of its pseudocode.

## Spec names in the code

| Spec | C++ |
|---|---|
| `mem[c]`, `imm[c]` | `ColumnFamily::mem`, `ColumnFamily::imm` (oldest first) |
| `memLog[c]`; the sealed memtable's `next` (WAL) or `nextLog` (MT4) | `MemTable::memLog`, `MemTable::nextLog` |
| `N[c]`, `MinLogToKeep` | `ColumnFamily::N` (`LsmTree::LogNumber`), `Wal::MinLogToKeep` |
| L, `active`, `lstate` | `Wal::L`, `Wal::a`, `LogFile::state` |
| `synced`, `dirSynced`, `predLast` | `LogFile::syncedPos`, `LogFile::dirSynced`, `LogFile::predLast` |
| Q, `pending` | `Wal::Q` (write groups with a stage per batch), `Wal::PendingInserts` |
| F, `gettingFlushed` | `Wal::F`, `Wal::gettingFlushed` |
| `lastSeq` (MANIFEST), `lastAllocated`, `visSeq` / `lastPublished` | `LsmTree::manifestLastSeq`, `lastAllocated`, `lastPublished` |
| `Stalled` | `LsmTree::GetStallState` |
| `l0`, `lv` | `ColumnFamily::l0` (newest first), `ColumnFamily::levels` |
| `cfstate` | `ColumnFamily::state` (`kLive`, `kDropped`, `kGone`) |
| D, `issued`, `logged`, `ackedSync`, `hist`, `snaps`, `jobs` | fields of `Ghost` |

## Invariants checked at run time

`CheckInvariants` runs after every step when `check_invariants` is on. A violation is recorded in
`Ghost::violations` with the step that caused it. With `abort_on_violation` it stops the program,
like an assert.

From `WAL_EndToEnd.tla`:

- **Coverage (I4).** A write of CF c in a log below N[c] is in c's SSTs.
- **Lifecycle (I5).** Only logs below `MinLogToKeep` are obsolete, archived, recycled, or deleted.
- **NeededImpliesLive (Lemma 1).** A log that holds an unflushed write is live.
- **ActiveLive.** The active log is live.
- **Alignment (I3).** Every logged write in a sealed memtable lies in a log before the one its seal
  started.
- **WriteAhead (I1).** A logged write in a memtable is in a live log.
- **NoLostAckWhileUp.** A running store holds every acknowledged sync write.
- **RecoveryOpens, RecoveryPrefix, RecoveryDurable.** Checked once per recovery: it opens; the
  logged writes that survive are a prefix in sequence order; every acknowledged sync write survives.

From `MemSST.tla`:

- **MS1 ReadLatest.** A read returns the latest committed value.
- **MS2 SnapshotRead.** A snapshot keeps showing what was committed when it was taken.
- **MS3 NewerShadowsOlder.** Along the read path, a later place never holds a newer version of a key.
- **MS4 BatchAtomic.** A published batch is in every CF it writes.
- **MS5 IngestAllOrNothing.** An ingestion lands in all of its CFs or in none.
- **MS6 LevelsDisjoint.** Files in L1 and deeper never overlap within a level.
- **MS7 LogNumberSafe.** N[c] never passes the log in which an unflushed memtable of c began.

A flush may drop a version that no reader can see. So "a write is in an SST" means the SST holds that
version or a newer version of the same key.

## Correctness mechanisms and what breaks without them

Each mechanism is a field of `Options::fix`, on by default. `tests/test_bugs.cc` replays the TLC
counterexample for each one. With the mechanism on, no invariant fails. With it off, the invariant
TLC reported fails, or one that catches the same fault earlier.

| Switch | Algorithm | Invariant that fails when off |
|---|---|---|
| `FixSyncAllLogs` (C1) | WAL3b | RecoveryDurable |
| `FixDirSync` (C2) | WAL3b | RecoveryDurable |
| `FixWaitPendingInserts` (F4; `FixSealWaitsInserts` in MemSST) | WAL6a, MT4 | Coverage; MS3 |
| `FixSyncClosedOnFlush` (F7) | WAL7a | RecoveryPrefix |
| `FixResumeRule` (F8) | WAL9 | RecoveryDurable (with `FixResumeAtRecoveryLog` also off) |
| `track_and_verify_wals` (`FixPredecessorCheck`) | WAL6a, WAL9 | RecoveryPrefix |
| `approx_pit_check` on (`ApproxPITCheck`) | WAL9 | RecoveryOpens |
| `FixInstallInOrder` | MT6 | MS7 |
| `FixPublishAfterInserts` | WAL4b | MS1 |
| `FixDeferDropDelete` | CF2, CF3 | MS1 |
| `FixFlushOverlap` | SST2 | MS1 |
| `FixGlobalSeqno` | SST3 | MS3 at once, MS1 after compaction |
| `FixLevelAbove` | SST3 | MS1 |
| `FixBlockWrites` | SST2 | MS4 |
| `FixAtomicMultiIngest` | SST4 | MS5 |
| `IngestOptions::snapshot_consistency` off | SST3 | MS2 (RocksDB documents this) |
| `FixKeepUnpublishedShadowed` (new) | MT5 | MS1 |
| `FixSyncRecoveredLogs` (new) | WAL9 | RecoveryPrefix |
| `FixResumeAtRecoveryLog` (new) | WAL9 | RecoveryDurable or RecoveryOpens |

`track_and_verify_wals` is on by default here. RocksDB's default is off, but the TLA+ model showed
that the prefix guarantee needs it.

## What the C++ tests found beyond the TLA+ models

The random simulator in `tests/test_random.cc` runs random interleavings of every step, with random
crashes, and checks every invariant after every step. With every mechanism on, 3,000 runs of 400
steps in each of five configurations found no violation. Getting there turned up four problems that
the TLA+ models cannot show, because of what they abstract away. Each fix is now a switch, and each
has a test in `test_bugs.cc`.

1. **A flush dropped a version that readers could still see** (`FixKeepUnpublishedShadowed`, MT5).
   WAL6a waits for pending inserts, but not for publication. So a sealed memtable can hold a version
   that is inserted but not yet published. MT5 then dropped the older version below it, and a read
   at `lastPublished` found nothing. The fix treats every read point from `lastPublished` on as a
   snapshot. `MemSST.tla` keeps every version, so it cannot see this.
2. **Recovery trusted log bytes that were only in the OS cache** (`FixSyncRecoveredLogs`, WAL9).
   `WAL_EndToEnd.tla` assumes that whatever recovery reads is on disk. After a process crash, it may
   not be. A later power failure then lost an early write while a later one survived. The fix syncs
   the replayed logs before recovery installs anything built from them.
3. **The resume rule missed a gap in sequence numbers** (`FixResumeAtRecoveryLog`, WAL9). After a
   recovery that stops early and keeps the old logs, the next recovery must resume at the first log
   written afterwards. The rule "resume when a record carries the next sequence number" fails if that
   number went to a write that skipped the WAL, or to an ingested file. The fix records, in the
   header of the log that recovery creates, where that recovery stopped. A later replay that stops at
   the same point resumes at that log.
4. **The consistency check counted ingested files** (WAL9). The pseudocode refuses to open if any
   SST holds a sequence number past the point where replay stopped. Ingested files take sequence
   numbers that never appear in a log, so this refused consistent stores. The model checks only
   logged writes, so the code now does the same. Each SST records the largest sequence number that
   came through the WAL (`largestLoggedSeq`, kept in the MANIFEST).

These are findings about the specifications, checked only against this implementation. Whether
RocksDB has the same gaps should be checked against its source.

## Test results

All 58 tests pass (see `results.txt`). In part 2 of the random test, the simulator found 16 of the
18 disabled mechanisms on its own, within at most 711 runs. It did not find `FixResumeRule`, because
the new header rule also resumes in every case it covers. It did not find `FixInstallInOrder`, because
it rarely finishes a newer flush job before an older one and then installs. The targeted tests in
`test_bugs.cc` cover both.

## Simplifications

- The disk is simulated in memory. There is no POSIX backend.
- Everything runs in one thread. A blocked step returns `Busy` instead of waiting.
- Only puts and deletes are supported: no merge operator, range deletions, in-place updates, or
  Bloom filters.
- Each operation in a batch gets its own sequence number, as in RocksDB. `MemSST.tla` gives a whole
  batch one number. Publication is still all at once, so batches stay atomic.
- Compaction is a simple model outside the specifications: it merges L0 (or one file of a level) into
  the next level and keeps every version.
- The stall rule covers too many sealed memtables and too many L0 files. There is no slowdown state.
- Recycled log files are truncated and reused. RocksDB instead overwrites them and tags records with
  the log number.
- When an idle CF's N[c] advances at a switch, the change is written to the MANIFEST at once.
- `max_write_buffer_size_to_maintain`, atomic flush, two-phase commit, and `two_write_queues` are not
  implemented.
- CF4 creates missing CFs right after WAL9 rather than before it. Nothing can observe the difference.
  A read-only open replays the logs into memory, as RocksDB does, but writes nothing.
