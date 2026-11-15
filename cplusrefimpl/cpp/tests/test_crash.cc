// test_crash.cc: crashes and recovery (WAL9), with every invariant checked,
// including RecoveryOpens, RecoveryPrefix and RecoveryDurable.
#include "harness.h"

using namespace lsm;

TEST(SyncWriteSurvivesPowerFailure) {
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "unsynced"));
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "b", "synced"));
  CHECK_OK(f.Crash(true));  // keep only synced bytes; unsynced directory entries vanish
  CHECK(f.Get(f.cf[0], "b") == "synced");
  CHECK(f.Get(f.cf[0], "a") == "unsynced");  // the sync covered the earlier write too
  CHECK(f.Clean());
}

TEST(UnsyncedWriteLostOnPowerFailureButNotOnProcessCrash) {
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "a", "1"));
  CHECK_OK(f.db->Put({}, f.cf[0], "b", "2"));
  CHECK_OK(f.Crash(false));  // process crash: bytes handed to the OS survive
  CHECK(f.Get(f.cf[0], "b") == "2");
  CHECK_OK(f.db->Put({}, f.cf[0], "c", "3"));
  CHECK_OK(f.Crash(true));  // power failure: the unsynced write is gone
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.Get(f.cf[0], "c") == "<none>");
  CHECK(f.Clean());
}

TEST(ManualWalFlushLosesBufferedBytesOnProcessCrash) {
  Fixture f;
  f.opts.manual_wal_flush = true;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));
  CHECK_OK(f.db->FlushWAL(false));  // WAL3a by hand
  CHECK_OK(f.db->Put({}, f.cf[0], "b", "2"));
  CHECK_OK(f.Crash(false));
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.Get(f.cf[0], "b") == "<none>");
  CHECK(f.Clean());
}

TEST(TornTailStopsPointInTimeReplay) {
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "a", "1"));
  CHECK_OK(f.db->Put({}, f.cf[0], "b", "2"));
  CHECK_OK(f.db->Put({}, f.cf[0], "c", "3"));
  std::string log = f.db->wal.a->path;
  const SimEnv::File* file = f.env.Get(log);
  size_t synced = file->synced, size = file->data.size();
  f.db.reset();
  // keep part of the second record and scramble a byte, as a torn write would
  f.env.CrashTruncate(log, synced + (size - synced) / 3);
  f.ghost.OnCrash();
  CHECK_OK(f.Open());
  CHECK(f.db->wal.lastRecovery.stopped);
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.Get(f.cf[0], "b") == "<none>");
  CHECK(f.Get(f.cf[0], "c") == "<none>");
  CHECK(f.Clean());
}

TEST(RecoveryModes) {
  for (auto mode : {WALRecoveryMode::kAbsoluteConsistency, WALRecoveryMode::kTolerateCorruptedTailRecords,
                    WALRecoveryMode::kSkipAnyCorruptedRecords}) {
    Fixture f;
    f.opts.wal_recovery_mode = mode;
    f.opts.check_invariants = false;
    CHECK_OK(f.Open());
    CHECK_OK(f.db->Put(Sync(), f.cf[0], "a", "1"));
    CHECK_OK(f.db->Put({}, f.cf[0], "b", "2"));
    std::string log = f.db->wal.a->path;
    size_t size = f.env.Get(log)->data.size();
    f.db.reset();
    f.env.CrashTruncate(log, size - 3);  // an incomplete record at the tail of the last log
    Status s = f.Open();
    if (mode == WALRecoveryMode::kAbsoluteConsistency) {
      CHECK(s.IsCorruption());  // any damage fails Open
    } else {
      CHECK_OK(s);
      CHECK(f.Get(f.cf[0], "a") == "1");
    }
  }
}

TEST(WalHoleCaughtByPredecessorCheck) {
  // MC_RocksDBDefault: log 1 loses its tail cleanly, log 2 survives. Without the
  // predecessor check, replay applies log 2 and loses the earlier write: the
  // recovered state is not a prefix.
  for (bool verify : {true, false}) {
    Fixture f;
    f.opts.track_and_verify_wals = verify;
    f.opts.check_invariants = false;
    f.names = {"default", "two"};
    f.opts.create_missing_column_families = true;
    CHECK_OK(f.Open());
    CHECK_OK(f.db->Put({}, f.cf[0], "a", "lost"));   // log 1, unsynced
    std::string log1 = f.db->wal.a->path;
    CHECK_OK(f.db->Flush(f.cf[1]));                  // a switch: log 2 becomes active
    CHECK_OK(f.db->Put({}, f.cf[0], "x", "dummy"));  // log 2
    CHECK_OK(f.db->Flush(f.cf[1]));                  // another switch: log 3
    CHECK_OK(f.db->Put({}, f.cf[1], "b", "later"));  // the newest log
    f.db.reset();
    // log 1 loses its tail cleanly: keep only the header
    f.env.CrashTruncate(log1, kLogHeaderSize);
    f.ghost.OnCrash();
    CHECK_OK(f.Open());
    if (verify) {
      CHECK(f.Clean());
      CHECK(f.Get(f.cf[1], "b") == "<none>");  // dropped: replay stopped at the hole
    } else {
      CHECK(f.Violated("RecoveryPrefix"));
    }
  }
}

TEST(AvoidFlushDuringRecoveryAcrossTwoCrashes) {
  Fixture f;
  f.opts.avoid_flush_during_recovery = true;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "a", "1"));
  CHECK_OK(f.Crash(true));
  CHECK(f.db->tree.Cf(f.cf[0])->l0.empty());  // kept in the memtable
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "b", "2"));
  CHECK_OK(f.Crash(true));
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.Get(f.cf[0], "b") == "2");
  CHECK(f.Clean());
}

TEST(MidRecoveryFlush) {
  Fixture f;
  f.opts.write_buffer_size = 120;
  f.opts.avoid_flush_during_recovery = true;
  f.opts.inline_background = false;
  f.opts.max_write_buffer_number = 100;
  CHECK_OK(f.Open());
  for (int i = 0; i < 20; i++) {
    Status s = f.db->Put({}, f.cf[0], "k" + std::to_string(i), std::string(20, 'v'));
    CHECK_OK(s);
    f.db->wal.F.clear();  // never flush while running
  }
  CHECK_OK(f.Crash(false));
  CHECK(f.db->wal.lastRecovery.flushedMid);  // replay filled a memtable and flushed it
  CHECK(!f.db->tree.Cf(f.cf[0])->l0.empty());
  for (int i = 0; i < 20; i++) CHECK(f.Get(f.cf[0], "k" + std::to_string(i)) == std::string(20, 'v'));
  CHECK(f.Clean());
}

TEST(RecoverySkipsFlushedRecordsPerColumnFamily) {
  Fixture f;
  f.names = {"default", "one"};
  f.opts.create_missing_column_families = true;
  CHECK_OK(f.Open());
  WriteBatch b;
  b.Put(f.cf[0], "k", "d");
  b.Put(f.cf[1], "k", "o");
  CHECK_OK(f.db->Write(Sync(), b));
  CHECK_OK(f.db->Flush(f.cf[1]));  // cf one has it in an SST; the log is still needed by default
  CHECK_OK(f.Crash(true));
  CHECK(f.db->wal.lastRecovery.recordsReplayed == 1);  // only default's record was replayed
  CHECK(f.Get(f.cf[0], "k") == "d" && f.Get(f.cf[1], "k") == "o");
  CHECK(f.Clean());
}

TEST(DroppedColumnFamilyIsNotRecovered) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c;
  CHECK_OK(f.db->CreateColumnFamily("tmp", &c));
  CHECK_OK(f.db->Put(Sync(), c, "k", "v"));
  CHECK_OK(f.db->DropColumnFamily(c));
  CHECK_OK(f.Crash(true));
  CHECK(f.db->tree.Cf(c) == nullptr);
  CHECK(f.Clean());
}

TEST(IngestionIsAtomicAcrossACrash) {
  Fixture f;
  f.names = {"default", "one"};
  f.opts.create_missing_column_families = true;
  CHECK_OK(f.Open());
  SstFileWriter w(&f.env);
  w.Open("ext/a.sst"); w.Put("a", "1"); w.Finish();
  SstFileWriter w2(&f.env);
  w2.Open("ext/b.sst"); w2.Put("b", "1"); w2.Finish();
  CHECK_OK(f.db->IngestExternalFiles({IngestArg{f.cf[0], {"ext/a.sst"}, {}}, IngestArg{f.cf[1], {"ext/b.sst"}, {}}}));
  CHECK_OK(f.Crash(true));
  CHECK(f.Get(f.cf[0], "a") == "1" && f.Get(f.cf[1], "b") == "1");
  CHECK(f.Clean());
}

TEST(WalSyncFailureIsPermanentUntilReopen) {
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "a", "1"));
  f.env.InjectFault(SimEnv::Op::kSync, "wal/", 1);
  CHECK(!f.db->Put(Sync(), f.cf[0], "b", "2").ok());  // not acknowledged
  CHECK(!f.db->Put({}, f.cf[0], "c", "3").ok());      // WAL1 returns the background error
  CHECK(!f.db->Resume().ok());
  CHECK_OK(f.Crash(true));
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK_OK(f.db->Put({}, f.cf[0], "c", "3"));
  CHECK(f.Clean());
}

TEST(FlushFailureIsRetriedAfterResume) {
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));
  f.env.InjectFault(SimEnv::Op::kAppend, "sst/", 1);
  CHECK(!f.db->Flush(f.cf[0]).ok());
  CHECK(f.db->tree.Cf(f.cf[0])->imm.front()->state == MemState::kSealed);  // back to Sealed
  CHECK(!f.db->Put({}, f.cf[0], "b", "2").ok());
  CHECK_OK(f.db->Resume());
  CHECK_OK(f.db->Flush(f.cf[0]));
  CHECK(f.db->tree.Cf(f.cf[0])->imm.empty());
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.Clean());
}

TEST(ObsoleteLogsAreArchivedOrRecycled) {
  {
    Fixture f;
    f.opts.archive_wal = true;
    CHECK_OK(f.Open());
    CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));
    CHECK_OK(f.db->Flush(f.cf[0]));
    CHECK(f.env.List("archive").size() == 1);
    CHECK(f.Clean());
  }
  {
    Fixture f;
    f.opts.recycle_log_file_num = 1;
    CHECK_OK(f.Open());
    CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));
    CHECK_OK(f.db->Flush(f.cf[0]));
    CHECK(f.db->wal.recyclePool.size() == 1);
    CHECK_OK(f.db->Put({}, f.cf[0], "b", "1"));
    CHECK_OK(f.db->Flush(f.cf[0]));  // the new log reuses the recycled file
    CHECK(f.Get(f.cf[0], "a") == "1" && f.Get(f.cf[0], "b") == "1");
    CHECK_OK(f.Crash(true));
    CHECK(f.Get(f.cf[0], "a") == "1" && f.Get(f.cf[0], "b") == "1");
    CHECK(f.Clean());
  }
}

TEST(RandomPowerFailuresKeepPrefixAndDurability) {
  // Many seeds of: random writes (some sync, some not), random flushes, then a
  // power failure that keeps a random part of every unsynced suffix, possibly torn.
  int recoveries = 0;
  for (uint64_t seed = 1; seed <= 300; seed++) {
    Rng rng(seed);
    Fixture f;
    f.names = {"default", "one"};
    f.opts.create_missing_column_families = true;
    f.opts.write_buffer_size = 200;
    f.opts.max_total_wal_size = 600;
    f.opts.avoid_flush_during_recovery = rng.OneIn(2);
    CHECK_OK(f.Open());
    for (int round = 0; round < 3; round++) {
      int n = 5 + rng.Uniform(20);
      for (int i = 0; i < n; i++) {
        WriteOptions o;
        o.sync = rng.OneIn(4);
        CfId c = f.cf[rng.Uniform(2)];
        std::string k = "k" + std::to_string(rng.Uniform(8));
        if (rng.OneIn(6)) CHECK_OK(f.db->Delete(o, c, k));
        else CHECK_OK(f.db->Put(o, c, k, "v" + std::to_string(seed) + "." + std::to_string(round) + "." + std::to_string(i)));
        if (rng.OneIn(10)) CHECK_OK(f.db->Flush(c));
      }
      Status s = f.Crash(true, &rng);
      recoveries++;
      if (!s.ok() || !f.Clean()) {
        std::printf("    seed %llu round %d: %s\n", (unsigned long long)seed, round, s.ToString().c_str());
        f.PrintViolations();
        CHECK(false);
      }
    }
  }
  std::printf("    %d recoveries checked\n", recoveries);
}

int main() { return testing::RunAll(); }
