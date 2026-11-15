// test_bugs.cc: one test per correctness mechanism. Each test runs the scenario of
// the TLC counterexample twice: with the mechanism on (no invariant may fail), and
// with it off (the invariant TLC reported must fail). This shows that the runtime
// checks catch the same bugs as the model checker, and that each mechanism is needed.
#include "harness.h"

using namespace lsm;

using Scenario = std::function<void(Fixture&)>;

// Runs the scenario with the mechanism on and off. `off` switches it off.
static bool Both(const char* expect, std::function<void(Options&)> off, Scenario run) {
  Fixture on;
  run(on);
  if (!on.Clean()) {
    std::printf("    with the mechanism ON, unexpected violations:\n");
    on.PrintViolations();
    return false;
  }
  Fixture bug;
  off(bug.opts);
  run(bug);
  if (!bug.Violated(expect)) {
    std::printf("    with the mechanism OFF, expected %s but got:\n", expect);
    bug.PrintViolations();
    return false;
  }
  for (auto& v : bug.ghost.violations)
    if (v.find(expect) != std::string::npos) {
      std::printf("    off -> %s\n", v.c_str());
      break;
    }
  return true;
}

static void TwoCfs(Fixture& f) {
  f.names = {"default", "one"};
  f.opts.create_missing_column_families = true;
  if (!f.Open().ok()) std::abort();
}

static void WriteExternal(SimEnv& env, const std::string& path,
                          std::vector<std::pair<std::string, std::string>> kvs) {
  SstFileWriter w(&env);
  w.Open(path);
  for (auto& [k, v] : kvs) w.Put(k, v);
  w.Finish();
}

// ===================================================================== WAL_EndToEnd.tla

TEST(Bug_SyncActiveOnly) {  // C1, MC_Bug_SyncActiveOnly -> RecoveryDurable
  CHECK(Both("RecoveryDurable", [](Options& o) { o.fix.FixSyncAllLogs = false; }, [](Fixture& f) {
    TwoCfs(f);
    f.db->Put({}, f.cf[0], "a", "1");       // log 1, not synced
    f.db->Flush(f.cf[1]);                   // switch: log 2 is active
    f.db->Put(Sync(), f.cf[1], "b", "2");   // acknowledged; only log 2 synced if C1 is off
    f.Crash(true);                          // log 1 loses its tail: a WAL hole, replay stops
  }));
}

TEST(Bug_NoDirSync) {  // C2, MC_Bug_NoDirSync -> RecoveryDurable
  CHECK(Both("RecoveryDurable", [](Options& o) { o.fix.FixDirSync = false; }, [](Fixture& f) {
    TwoCfs(f);
    f.db->Put({}, f.cf[0], "a", "1");
    f.db->Flush(f.cf[1]);                  // log 2 is created; its name is not yet durable
    f.db->Put(Sync(), f.cf[1], "b", "2");  // data synced, but without C2 the name is not
    f.Crash(true);                         // log 2 vanishes
  }));
}

TEST(Bug_NoWaitInserts) {  // F4, MC_Bug_NoWaitInserts -> Coverage
  CHECK(Both("Coverage", [](Options& o) { o.fix.FixWaitPendingInserts = false; }, [](Fixture& f) {
    f.opts.inline_background = false;
    f.Open();
    f.db->Put({}, f.cf[0], "a", "1");
    WriteBatch b;
    b.Put(f.cf[0], "b", "2");
    Status s;
    auto G = f.db->NewGroup({{b, WriteOptions{}}}, &s);
    f.db->Admit(*G);
    f.db->AppendGroup(*G);              // logged in log 1, not inserted
    Status fs = f.db->Flush(f.cf[0]);   // WAL6a must wait (Busy) ...
    if (fs.IsBusy()) {                  // ... so the writer finishes first
      f.db->InsertGroup(*G);
      f.db->PublishGroup(*G);
      f.db->Flush(f.cf[0]);
    } else {
      f.db->InsertGroup(*G);            // lands in the new memtable, but it is in log 1
      f.db->PublishGroup(*G);
    }
    f.db->RunBackgroundWork();          // N_c moves to 2 while "b" is only in log 1
  }));
}

TEST(Bug_NoSyncClosed) {  // F7, MC_Bug_NoSyncClosed -> RecoveryPrefix
  CHECK(Both("RecoveryPrefix", [](Options& o) { o.fix.FixSyncClosedOnFlush = false; }, [](Fixture& f) {
    TwoCfs(f);
    f.db->Put({}, f.cf[1], "x", "1");  // log 1, unsynced
    f.db->Put({}, f.cf[0], "y", "2");  // log 1, unsynced
    f.db->Flush(f.cf[0]);              // installs "y" in an SST; log 1 must be synced first
    f.Crash(true);                     // "x" is lost but the later "y" survives
  }));
}

TEST(Bug_NoResume) {  // F8, MC_Bug_NoResume -> RecoveryDurable
  // Both resume rules would rescue this scenario, so both are switched off.
  CHECK(Both("RecoveryDurable", [](Options& o) { o.fix.FixResumeRule = false; o.fix.FixResumeAtRecoveryLog = false; },
             [](Fixture& f) {
    f.opts.avoid_flush_during_recovery = true;
    f.Open();
    f.db->Put(Sync(), f.cf[0], "a", "1");
    f.db->Put({}, f.cf[0], "b", "2");
    std::string log = f.db->wal.a->path;
    size_t synced = f.env.Get(log)->synced, size = f.env.Get(log)->data.size();
    f.db.reset();
    f.env.CrashTruncate(log, synced + (size - synced) / 2);  // "b" is torn
    f.ghost.OnCrash();
    f.Open();                              // replay stops at "b"; the logs stay (no flush)
    f.db->Put(Sync(), f.cf[0], "c", "3");  // a new log continues the sequence
    f.Crash(true);                         // replay must resume at "c"
  }));
}

TEST(Bug_NoResumeAtRecoveryLog) {  // found by test_random -> RecoveryDurable
  // As above, but the first write after the first recovery skips the WAL. The new
  // log then starts at seq + 1, so the record-level resume rule (F8) never fires.
  CHECK(Both("RecoveryDurable", [](Options& o) { o.fix.FixResumeAtRecoveryLog = false; }, [](Fixture& f) {
    f.opts.avoid_flush_during_recovery = true;
    f.Open();
    f.db->Put(Sync(), f.cf[0], "a", "1");
    f.db->Put({}, f.cf[0], "b", "2");
    std::string log = f.db->wal.a->path;
    size_t synced = f.env.Get(log)->synced, size = f.env.Get(log)->data.size();
    f.db.reset();
    f.env.CrashTruncate(log, synced + (size - synced) / 2);  // "b" is torn
    f.ghost.OnCrash();
    f.Open();
    f.db->Put(NoWal(), f.cf[0], "x", "unlogged");  // takes the next sequence number
    f.db->Put(Sync(), f.cf[0], "c", "3");          // the new log starts one later
    f.Crash(true);
  }));
}

TEST(Bug_NoSyncRecoveredLogs) {  // found by test_random -> RecoveryPrefix
  // After a process crash the surviving log bytes may sit in the OS cache. WAL9
  // treats them as durable, so it must sync them.
  CHECK(Both("RecoveryPrefix", [](Options& o) { o.fix.FixSyncRecoveredLogs = false; }, [](Fixture& f) {
    f.opts.avoid_flush_during_recovery = true;
    f.Open();
    f.db->Put({}, f.cf[0], "a", "1");      // log 1, in the OS cache only
    f.Crash(false);                        // process crash; "a" is replayed into the memtable
    f.db->Put(Sync(), f.cf[0], "b", "2");  // log 2, synced and acknowledged
    f.Crash(true);                         // power failure: "a" is lost, "b" is not
  }));
}

TEST(Bug_NoPredecessorCheck) {  // MC_RocksDBDefault -> RecoveryPrefix
  CHECK(Both("RecoveryPrefix", [](Options& o) { o.track_and_verify_wals = false; }, [](Fixture& f) {
    TwoCfs(f);
    f.db->Put({}, f.cf[0], "a", "1");  // log 1
    std::string log1 = f.db->wal.a->path;
    f.db->Flush(f.cf[1]);              // log 2
    f.db->Put({}, f.cf[1], "b", "2");  // log 2
    f.db.reset();
    f.env.CrashTruncate(log1, kLogHeaderSize);      // log 1 loses its tail cleanly; log 2 survives
    f.ghost.OnCrash();
    f.Open();
  }));
}

TEST(Bug_ApproxPITCheck) {  // MC_RocksDBApproxCheck -> RecoveryOpens
  CHECK(Both("RecoveryOpens", [](Options& o) { o.approx_pit_check = true; }, [](Fixture& f) {
    TwoCfs(f);
    f.db->Put({}, f.cf[1], "x", "1");
    f.db->Flush(f.cf[1]);                  // cf one has an SST; log 2 active
    f.db->Put(Sync(), f.cf[0], "a", "2");  // log 2
    f.db->Put({}, f.cf[0], "b", "3");      // log 2, unsynced
    f.db->Flush(f.cf[1]);                  // cf one is idle: N moves to log 3
    std::string log2 = f.db->wal.Find(2)->path;
    size_t synced = f.env.Get(log2)->synced, size = f.env.Get(log2)->data.size();
    f.db.reset();
    f.env.CrashTruncate(log2, synced + (size - synced) / 2);  // "b" is torn
    f.ghost.OnCrash();
    f.Open();  // the exact check opens; RocksDB's approximate check refuses
  }));
}

// ===================================================================== MemSST.tla

TEST(Bug_SealNoWait) {  // MC_Bug_SealNoWait -> MS3 NewerShadowsOlder
  CHECK(Both("MS3", [](Options& o) { o.fix.FixWaitPendingInserts = false; }, [](Fixture& f) {
    f.opts.inline_background = false;
    f.Open();
    Status s;
    WriteBatch b1, b2;
    b1.Put(f.cf[0], "k", "older");
    b2.Put(f.cf[0], "k", "newer");
    auto G1 = f.db->NewGroup({{b1, WriteOptions{}}}, &s);
    auto G2 = f.db->NewGroup({{b2, WriteOptions{}}}, &s);
    f.db->Admit(*G1); f.db->AppendGroup(*G1);  // pipelined: both logged
    f.db->Admit(*G2); f.db->AppendGroup(*G2);
    f.db->InsertGroup(*G2);                    // the newer one is inserted first
    if (f.db->Flush(f.cf[0]).IsBusy()) {       // the seal must wait for G1
      f.db->InsertGroup(*G1);
      f.db->Flush(f.cf[0]);
    } else {
      f.db->InsertGroup(*G1);                  // the older version lands above the newer one
    }
    f.db->PublishGroup(*G1);
    f.db->PublishGroup(*G2);
  }));
}

TEST(Bug_DropUnpublishedShadowed) {  // found by test_random -> MS1 ReadLatest
  // WAL6a waits for pending inserts, not for publication. A sealed memtable can hold
  // an inserted but unpublished version; MT5 must not drop the version below it.
  CHECK(Both("MS1", [](Options& o) { o.fix.FixKeepUnpublishedShadowed = false; }, [](Fixture& f) {
    f.opts.inline_background = false;
    f.Open();
    f.db->Put({}, f.cf[0], "k", "visible");
    WriteBatch b;
    b.Put(f.cf[0], "k", "unpublished");
    Status s;
    auto G = f.db->NewGroup({{b, WriteOptions{}}}, &s);
    f.db->Admit(*G);
    f.db->AppendGroup(*G);
    f.db->InsertGroup(*G);   // inserted, not yet published
    f.db->Flush(f.cf[0]);    // the seal does not wait for publication
    f.db->RunBackgroundWork();
    f.db->PublishGroup(*G);
  }));
}

TEST(Bug_InstallOutOfOrder) {  // MC_Bug_InstallOutOfOrder -> MS7 LogNumberSafe
  CHECK(Both("MS7", [](Options& o) { o.fix.FixInstallInOrder = false; }, [](Fixture& f) {
    f.opts.inline_background = false;
    f.opts.max_write_buffer_number = 4;
    f.Open();
    f.db->Put({}, f.cf[0], "k", "1");
    f.db->Flush(f.cf[0]);  // sealed memtable 1 (logs < 2)
    f.db->Put({}, f.cf[0], "k", "2");
    f.db->Flush(f.cf[0]);  // sealed memtable 2 (logs < 3)
    uint64_t j1 = f.db->FlushStart(f.cf[0]);
    uint64_t j2 = f.db->FlushStart(f.cf[0]);
    f.db->FlushFinish(j2);           // the newer one finishes first
    f.db->InstallFlush(f.cf[0]);     // must wait; the bug installs it and sets N_c = 3
    f.db->FlushFinish(j1);
    f.db->InstallFlush(f.cf[0]);
  }));
}

TEST(Bug_PublishEarly) {  // MC_Bug_PublishEarly -> MS1 ReadLatest
  CHECK(Both("MS1", [](Options& o) { o.fix.FixPublishAfterInserts = false; }, [](Fixture& f) {
    f.Open();
    WriteBatch b;
    b.Put(f.cf[0], "k", "v");
    Status s;
    auto G = f.db->NewGroup({{b, WriteOptions{}}}, &s);
    f.db->Admit(*G);
    f.db->AppendGroup(*G);
    if (f.db->PublishGroup(*G).IsBusy()) {  // must wait for the insert
      f.db->InsertGroup(*G);
      f.db->PublishGroup(*G);
    } else {
      f.db->InsertGroup(*G);
    }
  }));
}

TEST(Bug_DropDeletesNow) {  // MC_Bug_DropDeletesNow -> MS1 ReadLatest
  CHECK(Both("MS1", [](Options& o) { o.fix.FixDeferDropDelete = false; }, [](Fixture& f) {
    TwoCfs(f);
    f.db->Put({}, f.cf[1], "k", "v");
    f.db->DropColumnFamily(f.cf[1]);  // the handle is still open
  }));
}

TEST(Bug_NoFlushOverlap) {  // MC_Bug_NoFlushOverlap -> MS1 ReadLatest
  CHECK(Both("MS1", [](Options& o) { o.fix.FixFlushOverlap = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "k", "old");
    WriteExternal(f.env, "ext/1.sst", {{"k", "new"}});
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});  // the memtable's old value hides it
  }));
}

TEST(Bug_NoGlobalSeqno) {  // MC_Bug_NoGlobalSeqno -> MS1 ReadLatest (after compaction)
  CHECK(Both("MS1", [](Options& o) { o.fix.FixGlobalSeqno = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "k", "old");
    f.db->Flush(f.cf[0]);
    WriteExternal(f.env, "ext/1.sst", {{"k", "new"}});
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});  // L0, above "old", but seqno 0
    f.db->CompactLevel(f.cf[0], 0);                    // the merge picks the larger seqno: "old"
  }));
}

TEST(Bug_LevelBelowOverlap) {  // MC_Bug_LevelBelowOverlap -> MS1 ReadLatest
  CHECK(Both("MS1", [](Options& o) { o.fix.FixLevelAbove = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "k", "old");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // "old" is in L1
    WriteExternal(f.env, "ext/1.sst", {{"k", "new"}});
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});  // the bug places it in L2, below "old"
  }));
}

TEST(Bug_NoBlockWrites) {  // MC_Bug_NoBlockWrites -> MS4 BatchAtomic
  CHECK(Both("MS4", [](Options& o) { o.fix.FixBlockWrites = false; }, [](Fixture& f) {
    f.opts.inline_background = false;
    TwoCfs(f);
    WriteBatch b;
    b.Put(f.cf[0], "a", "1");
    b.Put(f.cf[1], "a", "1");
    Status s;
    auto G = f.db->NewGroup({{b, WriteOptions{}}}, &s);
    f.db->Admit(*G);
    f.db->AppendGroup(*G);
    f.db->InsertBatchCf(*G, 0, f.cf[0]);  // half inserted
    WriteExternal(f.env, "ext/1.sst", {{"z", "1"}});
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});  // must wait; the bug publishes past G
    f.db->InsertBatchCf(*G, 0, f.cf[1]);
    f.db->PublishGroup(*G);
  }));
}

TEST(Bug_NonAtomicMultiIngest) {  // MC_Bug_NonAtomicMultiIngest -> MS5 IngestAllOrNothing
  CHECK(Both("MS5", [](Options& o) { o.fix.FixAtomicMultiIngest = false; }, [](Fixture& f) {
    TwoCfs(f);
    WriteExternal(f.env, "ext/a.sst", {{"a", "1"}});
    WriteExternal(f.env, "ext/b.sst", {{"b", "1"}});
    f.db->IngestExternalFiles({IngestArg{f.cf[0], {"ext/a.sst"}, {}}, IngestArg{f.cf[1], {"ext/b.sst"}, {}}});
  }));
}

TEST(NoSnapshotConsistency) {  // MC_NoSnapshotConsistency -> MS2 SnapshotRead (documented behavior)
  for (bool consistency : {true, false}) {
    Fixture f;
    f.Open();
    f.db->Put({}, f.cf[0], "z", "1");
    f.db->GetSnapshot();
    WriteExternal(f.env, "ext/1.sst", {{"a", "1"}});
    IngestOptions io;
    io.snapshot_consistency = consistency;  // off: the file keeps seqno 0
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"}, io);
    if (consistency) {
      CHECK(f.Clean());
    } else {
      CHECK(f.Violated("MS2"));
      std::printf("    off -> %s\n", f.ghost.violations.front().c_str());
    }
  }
}

int main() { return testing::RunAll(); }
