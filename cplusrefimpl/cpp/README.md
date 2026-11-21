# WAL, MemTable, SST Files, the Write Buffer Manager, the Block Cache, Compaction, and the Whole Store in C++

## What this is

This is a C++17 implementation of the main parts of an LSM-tree key–value store, modeled on RocksDB:

- the **write-ahead log** (WAL), algorithms WAL1 to WAL9;
- **memtables and column families**, algorithms MT1 to MT7 and CF1 to CF5;
- **SST files**: flushing memtables to disk, and ingesting files built outside the store, algorithms
  SST1 to SST4;
- the **write buffer manager**, which limits memtable memory across column families and stores,
  algorithms WBM1 to WBM9;
- the **block cache**, which keeps SST blocks in memory for reads, algorithms BC1 to BC11;
- **compaction** of the LSM tree, in both the leveled and the universal style, algorithms LSM2 to
  LSM12;
- the **whole store** (`RocksDBStore`) that composes all of them: recovery, the compaction job in
  three steps, file deletion, ingestion, and write stalls, algorithms DB1 to DB11.

The code follows six TLA+ specifications. `WAL_EndToEnd.tla` describes the WAL. `MemSST.tla`
describes memtables, column families, and SST files. `WBM.tla` describes the write buffer manager.
`BlockCache.tla` describes the block cache. `LSMTree.tla` describes compaction. `RocksDBStore.tla`
describes the store that composes them. Every C++ function names the algorithm step it
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

The **write buffer manager** (B) is one object that may be shared by many CFs and many stores. It
counts the memory of all their memtables in two counters: `used`, every memtable not yet freed, and
`active`, the memtables that still take writes. Before each write group, WAL1 asks it whether to flush
a memtable early (`active` over 7/8 of the limit, or `used` at the limit with at least half of it
active) and whether writers must stall (`used` at the limit, if stalls are allowed). It can also
charge memtable memory to the block cache with empty dummy entries.

An SST file is read in **blocks**: data blocks with the key–value pairs, an index block that says
which data block holds a key, and a filter block that says whether a key can be in the file. The
**block cache** keeps recently read blocks in memory, up to a capacity. A reader that uses a block
**pins** it, and a pinned block is never freed. Unpinned blocks wait on an LRU list, and the oldest is
evicted first. Point reads and iterators fill the cache. Compaction uses cached blocks but does not
add the blocks it reads, since it reads each one only once.

## Files

| File | Contents |
|---|---|
| `src/common.h` | Types (`SeqNo`, `LogNum`, `CfId`, `Entry`), `Status`, encoding, CRC32, a seeded RNG |
| `src/options.h` | The configuration θ, and the `Fixes` switches named after the TLA+ constants |
| `src/sim_env.h/.cc` | The simulated disk: OS cache, synced prefix, directory entries, crashes, fault injection |
| `src/memtable.h/.cc` | `MemTable` (MT1 to MT3), `SstFile`, the SST file format, `SstFileWriter` (SST1) |
| `src/manifest.h/.cc` | `VersionEdit` and the MANIFEST: one checksummed, synced record per edit |
| `src/wal.h/.cc` | `Wal`: WAL1 to WAL9, log files, write groups, flush requests, the log record format |
| `src/lsm_tree.h/.cc` | `LsmTree`: MT4 to MT7, CF1 to CF5, SST2 to SST4, the MANIFEST state, write stalls (LSM12) |
| `src/compaction.cc` | `LsmTree` compaction: LSM2 to LSM11, both styles, version pins and file deletion |
| `src/db.h/.cc` | `DB`: the public API; opens the store (CF4, DB1), keeps `persistedThrough` and `accepting`, and checks invariants after every step |
| `src/rocksdb_store.h` | `RocksDBStore` is `DB`: which member holds each field of the data structure, and which function runs each of DB1 to DB11 |
| `src/write_buffer_manager.h/.cc` | `WriteBufferManager` (WBM1 to WBM9), its stall handles, and the BM checks |
| `src/block_cache.h/.cc` | `BlockCache` (BC1 to BC8): the sharded LRU cache, its `CacheFixes`, and the CH checks |
| `src/table_reader.h/.cc` | `TableReader` (BC9 to BC11): blocks of an SST file read through the cache; `BlockHolder` |
| `src/ghost.h/.cc` | Ghost state and `CheckInvariants` (WAL invariants, MS1 to MS7, recovery) |
| `tests/test_basic.cc` | Normal operation: reads, flushes, triggers, stalls, CFs, iterators, ingestion |
| `tests/test_crash.cc` | Crashes and recovery, including 900 random power failures |
| `tests/test_bugs.cc` | One test per correctness mechanism: on, no violation; off, the expected violation |
| `tests/test_random.cc` | A random simulator, like TLC's simulation mode |
| `tests/test_wbm.cc` | The write buffer manager: accounting, triggers, stalls, the cache, both liveness gaps, and one test per mechanism |
| `tests/test_cache.cc` | The block cache: LRU order, pins, strict limit, pool, shards, reads, compaction, file life, two stores, the write buffer manager, one test per mechanism, and a random simulator |
| `tests/test_compaction.cc` | Compaction: triggers, targets, trivial moves, snapshot stripes, deletions, ingest-behind, clean cut, concurrent jobs, pinned versions, stalls, universal, and one test per mechanism |
| `tests/test_store.cc` | The whole store: `persistedThrough`, a crash at every step of a compaction job, the full purge, the stall rules, ingestion and the WAL, and one test per mechanism of `RocksDBStore.tla` |
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
| WBM1 ReserveMem | `WriteBufferManager::ReserveMem`, called from `MemTable::Insert` (MT1) |
| WBM2 ScheduleFreeMem, FreeMem | `WriteBufferManager::ScheduleFreeMem` (from `MemTable::MarkImmutable`, called by MT4) and `FreeMem` (from `~MemTable`) |
| WBM3 UpdateCacheReservation | `WriteBufferManager::UpdateCacheReservation` |
| WBM4 ShouldFlush, PickVictims | `WriteBufferManager::ShouldFlush`, `LsmTree::PickWbmVictims`, called in `Wal::Admit` |
| WBM5 ShouldStall, StallWrites | `WriteBufferManager::ShouldStall`, `Wal::WbmStallWrites`, `Wal::WbmWait` |
| WBM6 BeginWriteStall, WBM7 MaybeEndWriteStall | `WriteBufferManager::BeginWriteStall`, `MaybeEndWriteStall` |
| WBM8 | `WriteBufferManager::SetBufferSize`, `SetAllowStall`, `RemoveDBFromQueue` |
| WBM9 FlushAcrossDBs | `WriteBufferManager::FlushAcrossDBs`; the hand-off target is `DB::FlushLargestCf` |
| BC1 NewLRUCache, Shard | `BlockCache::BlockCache`, `DefaultShardBits`, `ShardOf` |
| BC2 Lookup, BC3 Insert, BC4 Release, BC5 Erase | `LRUCacheShard::Lookup`, `Insert`, `Release`, `Erase`, `EraseUnRefEntries` |
| BC6 LruInsert, LruRemove, MaintainPoolSize; BC7 EvictFromLRU | `LRUCacheShard::LruInsert`, `LruRemove`, `MaintainPoolSize`, `EvictFromLRU` |
| BC8 SetCapacity, SetStrictCapacityLimit | `BlockCache::SetCapacity`, `SetStrictCapacityLimit` |
| BC9 RetrieveBlock, Get | `TableReader::RetrieveBlock`, `TableReader::Get`, `ReadAll` (compaction); `LsmTree::Read` |
| BC10 CacheKey | `TableReader::CacheKey`; the prefix is built in `LsmTree::Reader` from `BlockCache::NewSessionId` |
| BC11 OpenTable, OnFlushOutput, OnObsolete | `TableReader::TableReader`, `Prepopulate` (from `FlushFinish`), `LsmTree::CloseReader` and `Uncache` |
| LSM2 SortedRuns | `LsmTree::SortedRuns`; reads walk the runs in `LsmTree::Read` |
| LSM3 Overlapping, ExpandToCleanCut | `LsmTree::Overlapping`, `ExpandToCleanCut` |
| LSM4 LevelTargets, Scores | `LsmTree::LevelTargets` (`LevelInfo`) |
| LSM5 PickLeveled | `LsmTree::PickLeveled` |
| LSM6 PickUniversal | `LsmTree::PickUniversal` |
| LSM7 CompactionIterator | `LsmTree::CompactionMerge`, `KeyNotExistsBeyond` |
| LSM8 WriteOutputs, LSM9 InstallCompaction | `LsmTree::CompactFinish` (with `CompactStart`, the job) |
| LSM10 Ref, Unref, PurgeObsoleteFiles | `LsmTree::PinVersion` (`VersionPin`), `DB::ReleaseVersion`, `LsmTree::PurgeObsoleteFiles` |
| LSM11 MaybeSchedule | `LsmTree::NeedsCompaction`, `MaybeScheduleCompaction` (from `DB::RunBackgroundWork`) |
| LSM12 StallCondition | `LsmTree::GetStallState`, `PendingCompactionBytes` |

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
| `bufSize`, `used`, `active`, `resv` (`WBM.tla`) | `WriteBufferManager::BufferSize()`, `MemoryUsage()`, `MutableMemtableMemoryUsage()`, `DummyEntriesInCacheUsage()` |
| `stallActive`, `queue`, `wst[d] = "blocked"` | `WriteBufferManager::StallActive()`, its queue, `WbmStall::kBlocked` |
| `retired` with `held` | a memtable kept alive by a `MemtablePin` (`DB::PinMemtables`) |
| `ent`, `usage`, `highUsage`, `lowL`, `highL` (`BlockCache.tla`) | `LRUHandle`, `LRUCacheShard::Usage()`, `HighPriPoolUsage()`, the shard's LRU list split at `lruLowPri_` |
| `rd[r] = "hold"` | a `BlockHolder` with a handle |
| `Key(f, b)` | `TableReader::CacheKey` |
| `V`, `exists`, `comps`, `Busy` (`LSMTree.tla`) | the levels of `ColumnFamily`, the files on `SimEnv`, `LsmTree::compactions`, `SstFile::beingCompacted` |
| `pinned[r]` | a `VersionPin` from `DB::PinVersion` |
| `fd[f].ep` | `SstFile::epoch` |

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

From `WBM.tla` (checked by `WriteBufferManager::CheckInvariants`, called from `DB::Step`):

- **BM1 ExactAccounting.** `used` equals the memory of every memtable that still exists, and `active`
  that of the mutable ones.
- **BM2 NoLostWakeup.** A blocked store waits only while stalls are on and `used` is at the limit.
  (The code runs in one thread, so no WBM7 call can be in flight.)
- **BM4 CacheCovers.** The dummy entries cover `used`, in whole entries, and shrink only below 3/4.
- **BM5 QueueConsistent.** The queue holds exactly the blocked stores, and is empty when no stall is on.

BM3, the memory bound, is not checked: the race that breaks it in RocksDB needs a second thread.
BM6, that every write finishes, is a liveness property; `test_wbm.cc` shows both ways it fails with
FlushOldest and that FlushLargestAcrossDBs fixes them.

From `BlockCache.tla` (checked by `BlockCache::CheckInvariants`, called from `DB::Step`; tests that
use the cache alone call it directly):

- **CH1 ExactAccounting.** Each shard's usage, LRU usage, and pool usage equal the charges they stand
  for, and the LRU list holds exactly the unpinned entries in the table.
- **CH2 PinSafety.** No pinned entry is freed. Reported when it happens (only a disabled mechanism can
  cause it); the entry is then kept as a "zombie" so the program stays memory safe.
- **CH3 CapacityBound.** A shard over capacity has an empty LRU list; a strict shard is never over
  capacity unless `SetCapacity` lowered it.
- **CH4 NoStaleBlock.** Every block that comes from the cache equals the block of the file the reader
  asked for. Checked at each hit, when `check_invariants` is on.
- **CH5 CompactionNoFill.** No entry in the cache was inserted by a compaction read.
- **CH6 NoLeakedPin.** Each entry's pin count equals the number of `BlockHolder`s (and dummy entries)
  that hold it.
- **CH7 PoolBound.** The pool is within its share. It fails after `SetCapacity` lowers the capacity,
  as in RocksDB, until the next high-priority insert.

CH8 (the write buffer manager's dummy entries are all in the cache) is the cache side of BM4.

From `LSMTree.tla`:

- **LS1 LevelsDisjoint.** Files in L1 and deeper never overlap within a level, in internal-key order
  (MS6, extended). Two neighbors may share a user key, but never a version.
- **LS2 NewerAbove** and **LS3 ReadsCorrect** are MS3, MS1, and MS2, which now cover compaction.
- **LS4 Exclusive.** No file is an input of two running compactions, and two compactions that write
  one level cover disjoint ranges.
- **LS5 PinnedLive.** Every file of a pinned version is on disk.
- **LS6 CurrentLive.** Every file of the current version is on disk, and every input of a running
  compaction is in it.
- **LS7 NoDuplicate.** A version (key and sequence number) is in one file of the current version.

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
| `FixScheduleFreeOnSeal` | MT4, WBM2 | BM1 |
| `FixFreeAfterUnpin` | MT6, WBM2 | BM1 |
| `FixEndStallOnFree` (in `WriteBufferManager::fix`) | WBM2 | BM2 |
| `FixEndStallOnResize` (in `WriteBufferManager::fix`) | WBM8 | BM2 |
| `FixRecheckUnderLock` (in `WriteBufferManager::fix`) | WBM6 | BM2 |
| `FixWakeAll` (in `WriteBufferManager::fix`) | WBM7 | BM2 |
| `FixCacheRoundUp` (in `WriteBufferManager::fix`) | WBM3 | BM4 |
| `FixUniqueKey` | BC10 | CH4 |
| `FixCompactionNoFill` | BC9 | CH5 |
| `FixReleaseOnClose` | BC9 (`BlockHolder`) | CH6 |
| `FixLookupUnpins` (in `BlockCache::fix`) | BC2 | CH2 (CH1 fails first) |
| `FixDetachOnReplace` (in `BlockCache::fix`) | BC3 | CH2 |
| `FixEraseOverCap` (in `BlockCache::fix`) | BC4 | CH3 |
| `FixStrictRefuse` (in `BlockCache::fix`) | BC3 | CH3 |
| `FixMaintainPool` (in `BlockCache::fix`) | BC6 | CH7 |
| `FixL0Overlap` | LSM3, LSM5 | MS3 (LS2) |
| `FixCleanCut` | LSM3, LSM5 | MS3 (LS2) |
| `FixOutputOverlap` | LSM5 | LS1 |
| `FixSkipBusy` | LSM5, LSM6 | LS4 |
| `FixSnapshotKeep` | LSM7 | MS2 (LS3) |
| `FixTombstoneBottom` | LSM7 | MS1 (LS3) |
| `FixAdjacentRuns` | LSM6 | MS3 (LS2) |
| `FixIngestBehindKeep` | LSM7 | MS1 (LS3) |
| `FixVersionRef` | LSM10 | LS5 |
| `FixIngestSkipCompacting` | SST3 | LS1 |
| `FixAtomicEdit` | DB7 | MS3 (LS2) |
| `FixSyncBeforeApply` | DB7 | ST2 |
| `FixProtectOutputs` | DB8 | ST3 |
| `FixRecordLastSeq` | DB9 (SST4) | ST4 |
| `FixSyncWalBeforeIngest` | DB9 | ST1 |
| `FixStallNeedsCompaction` | DB10 | ST5 |
| `FixSanitizeTriggers` | DB1 | ST5 |
| `FixBumpFileNumber` | DB1 | none: a second line of defense |

The write buffer manager's switches live in two places. The two on the memtable side are in
`Options::fix`, like the others. The five inside B are in `WriteBufferManager::fix`, because one B
may serve several stores. Their tests are in `tests/test_wbm.cc`. The block cache's switches are split
the same way: the three on the table reader side are in `Options::fix`, and the five inside the
cache are in `BlockCache::fix` (`CacheFixes`). Their tests are in `tests/test_cache.cc`. The
compaction switches are in `Options::fix`; their tests are in `tests/test_compaction.cc`. The
store's switches are in `Options::fix` too; their tests are in `tests/test_store.cc`.

`track_and_verify_wals` is on by default here. RocksDB's default is off, but the TLA+ model showed
that the prefix guarantee needs it.

## What the C++ tests found beyond the TLA+ models

The random simulator in `tests/test_random.cc` runs random interleavings of every step, with random
crashes, and checks every invariant after every step. With every mechanism on, no run found a
violation: 3,000 runs of 400 steps in each of the five original configurations, and 150 runs (the
default) in each of the two write buffer manager configurations. Getting there turned up four problems that
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

Compaction added three more:

5. **Universal compaction rewrote the reserved level** (LSM6). With `allow_ingest_behind`, the
   files in Lmax are the oldest sorted run. A size amplification compaction took that run and wrote
   its output back to Lmax, so the next ingest-behind file landed beside newer data (MS3).
   `LSMTree.tla` allows one ingest-behind file, so it could not show this. Universal now leaves that
   run out, and the model forbids it too.
6. **A dropped column family deleted files that a version still pinned** (CF3). Its files now go
   through `PurgeObsoleteFiles` (LSM10), like compaction inputs (LS5).
7. **The reference ordered versions by sequence number** (ghost). Once compaction drops every
   version of a key, an ingested file holding the key overlaps nothing, so it legally keeps sequence
   number 0, yet it is newer. The ghost history now orders versions by commit order.

These are findings about the specifications, checked only against this implementation. Whether
RocksDB has the same gaps should be checked against its source.

## The write buffer manager in this code

`Options::write_buffer_manager` is a `std::shared_ptr<WriteBufferManager>` that several stores may
share. If it is not set, each store makes its own from `db_write_buffer_size`, as RocksDB does. Every
memtable holds a pointer to B and acts as RocksDB's `AllocTracker`: `MemTable::Insert` charges B
(WBM1), `MarkImmutable` tells it about the seal (WBM2, called by MT4), and the destructor frees the
memory (WBM2). A memtable is destroyed when the last `shared_ptr` to it goes, so a reader that wants
to hold memtables takes a `MemtablePin` from `DB::PinMemtables`.

A stall cannot block in one thread. When WAL1 stalls a group, the store's `WbmStall` handle becomes
`kBlocked`, and `Admit` returns `Busy` until B signals it. A blocked group does not run the flush checks
again, as in RocksDB; it is admitted as soon as it is signaled. Other groups of the same store wait
behind it. With FlushLargestAcrossDBs, each retry of `Admit` stands for one 20 ms wake-up of WBM9:
B first asks another store with flushable memory to flush (`DB::FlushLargestCf`), and otherwise lets
the waiting store flush locally.

B charges the real block cache (`BlockCache`, below). Each dummy entry has a key of its own, is
inserted with a handle and low priority, and stays pinned until B releases it with
`eraseIfLastRef`. The dummy entry size is a constructor argument (256 KB by default) so that tests
can use small sizes.

## The block cache in this code

`BlockBasedTableOptions::block_cache` is a `std::shared_ptr<BlockCache>` that several stores may
share. If it is not set, each store makes its own 32 MB cache, as RocksDB does;
`no_block_cache` turns the cache off. The cache is RocksDB's `LRUCache`: `2^b` shards (by default
shards of at least 512 KB, at most 64), each with a hash table, a capacity, and an intrusive LRU list
split into a low-priority part and a high-priority pool (`high_pri_pool_ratio`, 0.5 by default).

Every SST read of the store goes through a `TableReader`, one per file, kept in `LsmTree::readers_`.
An SST file is cut into data blocks of `block_entries` entries (this stands in for `block_size`),
an index block, and a filter block (the exact set of keys; it stands for a Bloom filter). Blocks are
built from the file's entries when they are "read from disk". `TableReader::RetrieveBlock` (BC9)
looks the block up and, on a miss, reads it and inserts it with a handle. Every block in use is held
by a `BlockHolder`, which releases its pin when it goes away (K9).

- `DB::Get` and the iterators read with `fill_cache = true`. `LsmTree::Get` still reads the files
  directly, because the invariant checks use it as the reference answer.
- Compaction (`LsmTree::CompactionMerge`) reads its inputs with `fill_cache = false` (K10).
- With `prepopulate_block_cache = kFlushOnly`, `FlushFinish` inserts the new file's data blocks.
- With `cache_index_and_filter_blocks`, the index and filter go through the cache too, with high
  priority if `cache_index_and_filter_blocks_with_high_priority`. With
  `pin_l0_filter_and_index_blocks_in_cache`, the reader of an L0 file pins them while it is open.
- When a MANIFEST edit removes a file (compaction, or a dropped CF), its reader closes and releases
  its pins. With `uncache_aggressiveness > 0`, its blocks are also erased.
- Cache keys are `session id / file number / block` (BC10). Each `DB` object takes a new session id
  when it opens, so a store that shares a cache with another store, or reopens after a crash, never
  meets its own old keys.

A strict cache that is full of pinned blocks makes `DB::Get` return `Incomplete`; a compaction still
succeeds, since it never inserts.

## Compaction in this code

`Options` has RocksDB's compaction settings: `compaction_style` (`kLevel` or `kUniversal`),
`level0_file_num_compaction_trigger`, `max_bytes_for_level_base` and `_multiplier`,
`level_compaction_dynamic_level_bytes`, `target_file_size_base` and `_multiplier`,
`compaction_pri` (`kMinOverlappingRatio` or `kRoundRobin`), `compaction_options_universal`
(`size_ratio`, `min_merge_width`, `max_merge_width`, `max_size_amplification_percent`), the L0
slowdown and stop triggers, and `soft_`/`hard_pending_compaction_bytes_limit`. The defaults are
RocksDB's, except that sizes are small, and `disable_auto_compactions` is on, so that the older tests
keep their exact file layout. `compaction_split_user_keys` lets an output end between two versions
of one key, which the TLA+ model allows; it is off by default, as in RocksDB.

A compaction is three steps. `DB::CompactStart` picks (LSM5 or LSM6), marks the inputs busy,
captures a pending output, and returns an id. `DB::CompactWrite` merges (LSM7) and writes the
outputs (LSM8). `DB::CompactInstall` installs them in one MANIFEST edit (LSM9) and deletes what no
version needs (LSM10). `DB::CompactFinish` runs the last two together. Other steps, and other
compactions, can run in between. `DB::MaybeCompact` runs every due compaction now (LSM11), and so
does `RunBackgroundWork` when auto compactions are on. `DB::CompactLevel(c, n)` is a manual
compaction of L_n into L_n+1. A pick of one file with nothing to merge is a trivial move: the file
just changes level.

The merge treats `lastPublished` as a snapshot, and each unpublished sequence number as its own
stripe, as the flush does (MT5). It drops a deletion only when no file below the output holds the
key, and never with `allow_ingest_behind`. Each dropped deletion is reported to the ghost, which
then accepts the key's absence from every SST.

`DB::PinVersion` returns a `VersionPin`, the files of the current version. While it is alive,
`PurgeObsoleteFiles` keeps those files, and `DB::ReadPinned` reads through them.

## The whole store in this code

`DB` is the `RocksDBStore` of `../rocksdb_store/`; `src/rocksdb_store.h` maps each field and each
algorithm. Four things were added for it.

- `DB::persistedThrough`: after every step it moves to `lastAllocated` when every log that holds
  records is synced, and to the largest logged sequence number in any SST file. Writes with
  `disableWAL` are not covered. The ghost keeps the last value, and at recovery checks ST1: every
  logged write up to it survived, and no surviving ingestion is newer than a lost logged write.
- `DB::accepting` (DB10): not stopped, and no unrecoverable background error. `GetStallState` now
  ignores L0 and compaction debt when auto compaction is off, and the constructor raises the
  slowdown and stop triggers to at least the compaction trigger (DB1), both as RocksDB does.
- Pending outputs (K14): every flush job and every compaction job captures `nextFileNumber` in
  `LsmTree::pendingOutputs` when it starts, and releases it when its record is applied.
  `DB::PurgeAllObsoleteFiles` is the full scan (DB8): it deletes every SST on disk that no live
  version, pin, or obsolete-but-pinned file uses, except numbers at or above the least pending output.
- `LsmTree::durableFiles` follows what the MANIFEST on disk lists. `DB::CheckStore` checks ST2
  (those files exist), ST3 (a written job's outputs exist), ST4 (no version is newer than
  `lastAllocated`), and ST5 (a stop has a background job that can end it) after every step.

Ingestion now syncs every live log before it places the file (DB9). RocksDB does not; switching
`FixSyncWalBeforeIngest` off gives its behavior, and ST1 then fails after a power failure.

## Test results

All 145 tests pass (see `results.txt`), also under AddressSanitizer and UndefinedBehaviorSanitizer.
Of these, 14 are in `test_store.cc` and 29 are in `test_cache.cc`; its random simulator found all 5 cache-side mechanisms when
each was switched off. 24 are in `test_compaction.cc`. Part 1 of the random test now also runs five
compaction configurations (leveled; leveled with round robin, static targets, and split keys;
leveled with ingest-behind; universal; universal with ingest-behind), 150 runs of 400 steps each,
with no violation.

In part 2 of the random test, the simulator found 37 of the 40 disabled mechanisms on its own,
each within 711 runs, including all 7 of the store's. Its compaction steps now run a job as
`CompactWrite` then `CompactInstall`, with full purges and MANIFEST syncs in between.
It did not find `FixResumeRule`, because the new header rule also resumes in
every case it covers. It did not find `FixInstallInOrder`, because it rarely finishes a newer flush
job before an older one and then installs. It did not find `FixIngestSkipCompacting`, because it
rarely ingests a file into the exact range a running compaction writes. The targeted tests in
`test_bugs.cc` and `test_compaction.cc` cover all three.

## Simplifications

- The block cache has one thread, so the per-shard mutex is left out. Its blocks are built from
  the file's entries, not decoded from the file's bytes, and charges are entry bytes plus small
  overheads. There is no `HyperClockCache`, secondary cache, or `SimCache`.

- The disk is simulated in memory. There is no POSIX backend.
- Memtable memory is counted in entry bytes (key, value, and 16 bytes), not in arena blocks.
- The write buffer manager's ranking of stores for FlushLargestAcrossDBs (reports, leases, a bounded
  executor) is reduced to "the store with the largest flushable memtable whose writers are not
  stalled".
- Everything runs in one thread. A blocked step returns `Busy` instead of waiting.
- Only puts and deletes are supported: no merge operator, range deletions, or in-place updates.
  The filter block is an exact key set, not a Bloom filter.
- Each operation in a batch gets its own sequence number, as in RocksDB. `MemSST.tla` gives a whole
  batch one number. Publication is still all at once, so batches stay atomic.
- Compaction has no periodic compaction, TTL, FIFO style, intra-L0 compaction, subcompactions,
  compaction filters, or sequence number zeroing. File sizes are entry bytes. Compactions run one
  after another inside a step; "concurrent" means started and not yet finished.
- The stall rule covers too many sealed memtables, the L0 triggers (sorted runs for universal), and
  pending compaction bytes. A slowdown is reported as `kDelay`; nothing sleeps.
- Recycled log files are truncated and reused. RocksDB instead overwrites them and tags records with
  the log number.
- When an idle CF's N[c] advances at a switch, the change is written to the MANIFEST at once.
- `max_write_buffer_size_to_maintain`, atomic flush, two-phase commit, and `two_write_queues` are not
  implemented.
- CF4 creates missing CFs right after WAL9 rather than before it. Nothing can observe the difference.
  A read-only open replays the logs into memory, as RocksDB does, but writes nothing.
