// test_store.cc: the whole store, as the RocksDBStore of rocksdb_store/ (DB1 to
// DB11). Part 1 checks the behavior the algorithms describe: persistedThrough,
// accepting, a compaction job in steps with crashes between them, the full purge,
// recovery's counters, and the stall rules. Part 2 has one test per mechanism of
// RocksDBStore.tla: with it on, no invariant fails; with it off, the invariant TLC
// reported fails.
#include "harness.h"

using namespace lsm;

namespace {

using Scenario = std::function<void(Fixture&)>;

bool Both(const char* expect, std::function<void(Options&)> off, Scenario run,
          std::function<void(Options&)> common = nullptr) {
  Fixture on;
  if (common) common(on.opts);
  run(on);
  if (!on.Clean()) {
    std::printf("    with the mechanism ON, unexpected violations:\n");
    on.PrintViolations();
    return false;
  }
  Fixture bug;
  if (common) common(bug.opts);
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

void WriteExternal(SimEnv& env, const std::string& path, const std::vector<std::pair<std::string, std::string>>& kvs) {
  SstFileWriter w(&env);
  if (!w.Open(path).ok()) std::abort();
  for (auto& [k, v] : kvs)
    if (!w.Put(k, v).ok()) std::abort();
  if (!w.Finish().ok()) std::abort();
}

// Two L0 files that both hold key "a", and a compaction trigger of 2: the next
// CompactStart picks both.
void TwoL0Files(Fixture& f) {
  f.db->Put({}, f.cf[0], "a", "1");
  f.db->Flush(f.cf[0]);
  f.db->Put({}, f.cf[0], "a", "2");
  f.db->Put({}, f.cf[0], "b", "2");
  f.db->Flush(f.cf[0]);
}
void Trigger2(Options& o) { o.level0_file_num_compaction_trigger = 2; }

}  // namespace

// ============================================================ part 1: behavior

TEST(PersistedThroughFollowsSyncAndFlush) {  // DB2, DB4
  Fixture f;
  CHECK_OK(f.Open());
  CHECK(f.db->persistedThrough == 0 && f.db->accepting);
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));  // not synced: a power failure may lose it
  CHECK(f.db->persistedThrough == 0);
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "b", "2"));  // syncing the log covers every earlier record
  CHECK(f.db->persistedThrough == 2);
  CHECK_OK(f.db->Put({}, f.cf[0], "c", "3"));
  CHECK(f.db->persistedThrough == 2);
  CHECK_OK(f.db->Flush(f.cf[0]));  // in an SST file now
  CHECK(f.db->persistedThrough == 3);
  CHECK_OK(f.Crash(true));
  CHECK(f.Get(f.cf[0], "c") == "3");
  CHECK(f.db->persistedThrough == 3);
  CHECK(f.Clean());
}

TEST(CompactionJobSurvivesACrashAtEveryStep) {  // DB5 to DB8, DB1
  for (int at = 0; at < 3; at++) {
    Fixture f;
    Trigger2(f.opts);
    CHECK_OK(f.Open());
    TwoL0Files(f);
    uint64_t id = f.db->CompactStart(f.cf[0]);
    CHECK(id != 0);
    if (at >= 1) CHECK_OK(f.db->CompactWrite(id));
    if (at >= 2) CHECK_OK(f.db->CompactInstall(id));
    uint64_t next = f.db->tree.nextFileNumber;
    CHECK_OK(f.Crash(true));
    CHECK(f.Get(f.cf[0], "a") == "2" && f.Get(f.cf[0], "b") == "2");
    CHECK(f.db->tree.nextFileNumber >= next);   // never reuse an orphan's number
    CHECK(f.db->tree.compactions.empty());
    CHECK(f.env.List("sst").size() == (at == 2 ? 1u : 2u));  // orphans are gone
    CHECK(f.Clean());
  }
}

TEST(FullPurgeKeepsTheOutputsOfARunningJob) {  // DB8
  Fixture f;
  Trigger2(f.opts);
  CHECK_OK(f.Open());
  TwoL0Files(f);
  uint64_t id = f.db->CompactStart(f.cf[0]);
  CHECK_OK(f.db->CompactWrite(id));
  CHECK(f.env.List("sst").size() == 3u);
  CHECK_OK(f.db->PurgeAllObsoleteFiles());
  CHECK(f.env.List("sst").size() == 3u);  // the output is pending, not obsolete
  CHECK_OK(f.db->CompactInstall(id));
  CHECK(f.env.List("sst").size() == 1u);
  CHECK(f.Get(f.cf[0], "a") == "2");
  CHECK(f.Clean());
}

TEST(NoAutoCompactionMeansNoL0Stop) {  // DB10, as RocksDB's GetWriteStallConditionAndCause
  Fixture f;
  f.opts.disable_auto_compactions = true;
  f.opts.level0_file_num_compaction_trigger = 1;
  f.opts.level0_slowdown_writes_trigger = 1;
  f.opts.level0_stop_writes_trigger = 2;
  CHECK_OK(f.Open());
  for (int i = 0; i < 4; i++) {
    CHECK_OK(f.db->Put({}, f.cf[0], "k" + std::to_string(i), "v"));
    CHECK_OK(f.db->Flush(f.cf[0]));
  }
  CHECK(f.db->tree.Cf(f.cf[0])->l0.size() == 4u);
  CHECK(f.db->accepting);
  CHECK(f.Clean());
}

TEST(TriggersAreSanitized) {  // DB1, as RocksDB's SanitizeOptions
  Fixture f;
  f.opts.level0_file_num_compaction_trigger = 4;
  f.opts.level0_slowdown_writes_trigger = 2;
  f.opts.level0_stop_writes_trigger = 3;
  CHECK_OK(f.Open());
  CHECK(f.db->opts.level0_slowdown_writes_trigger == 4);
  CHECK(f.db->opts.level0_stop_writes_trigger == 4);
  CHECK(f.Clean());
}

TEST(IngestionSyncsTheWalFirst) {  // DB9
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put(Sync(), f.cf[0], "a", "1"));
  CHECK_OK(f.db->Flush(f.cf[0]));
  CHECK_OK(f.db->Put({}, f.cf[0], "b", "1"));  // not synced
  WriteExternal(f.env, "ext/1.sst", {{"a", "x"}});
  CHECK_OK(f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"}));  // overlaps L0: a new sequence number
  CHECK(f.db->persistedThrough == 3);
  CHECK_OK(f.Crash(true));
  CHECK(f.Get(f.cf[0], "a") == "x" && f.Get(f.cf[0], "b") == "1");
  CHECK(f.Clean());
}

TEST(RecoveryWithoutBumpReusesAnOrphanNumberSafely) {  // DB1, MC_NoBump
  Fixture f;
  Trigger2(f.opts);
  f.opts.fix.FixBumpFileNumber = false;
  CHECK_OK(f.Open());
  TwoL0Files(f);
  uint64_t id = f.db->CompactStart(f.cf[0]);
  CHECK_OK(f.db->CompactWrite(id));
  uint64_t orphan = f.db->tree.compactions.at(id).outs.at(0)->number;
  CHECK_OK(f.Crash(false));
  CHECK(f.db->tree.nextFileNumber == orphan);  // the MANIFEST's number: the orphan's
  CHECK_OK(f.db->Put({}, f.cf[0], "c", "3"));
  CHECK_OK(f.db->Flush(f.cf[0]));              // reuses it (the orphan was deleted at open)
  CHECK(f.Get(f.cf[0], "a") == "2" && f.Get(f.cf[0], "c") == "3");
  CHECK(f.Clean());
}

// ============================================================ part 2: one test per mechanism

TEST(Bug_SplitInstall) {  // MC_Bug_SplitInstall -> LS2 NewerAbove
  CHECK(Both("MS3", [](Options& o) { o.fix.FixAtomicEdit = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               TwoL0Files(f);
               uint64_t id = f.db->CompactStart(f.cf[0]);
               if (id) f.db->CompactFinish(id);
             },
             Trigger2));
}

TEST(Bug_ApplyBeforeSync) {  // MC_Bug_ApplyBeforeSync -> ST2 ManifestLive
  CHECK(Both("ST2 ManifestLive", [](Options& o) { o.fix.FixSyncBeforeApply = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               TwoL0Files(f);
               uint64_t id = f.db->CompactStart(f.cf[0]);
               if (id) f.db->CompactFinish(id);  // the inputs are purged at once
               f.db->SyncManifest();
             },
             Trigger2));
}

TEST(Bug_PurgeOutputs) {  // MC_Bug_PurgeOutputs -> ST3 OutputsLive
  CHECK(Both("ST3 OutputsLive", [](Options& o) { o.fix.FixProtectOutputs = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               TwoL0Files(f);
               uint64_t id = f.db->CompactStart(f.cf[0]);
               if (!id) return;
               f.db->CompactWrite(id);
               f.db->PurgeAllObsoleteFiles();
               f.db->CompactInstall(id);
             },
             Trigger2));
}

TEST(Bug_NoLastSeq) {  // MC_Bug_NoLastSeq -> ST4 FreshSeq
  CHECK(Both("ST4 FreshSeq", [](Options& o) { o.fix.FixRecordLastSeq = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               f.db->Put({}, f.cf[0], "a", "1");
               f.db->Flush(f.cf[0]);
               WriteExternal(f.env, "ext/1.sst", {{"a", "x"}});
               f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});  // sequence number 2
               if (!f.Crash(false).ok()) return;                  // LastSequence: 1
               f.db->Put({}, f.cf[0], "a", "3");                  // would reuse 2
             }));
}

TEST(Bug_IngestNoWalSync) {  // MC_Bug_IngestNoWalSync -> ST1 DurablePrefix
  CHECK(Both("ST1 DurablePrefix", [](Options& o) { o.fix.FixSyncWalBeforeIngest = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               f.db->Put(Sync(), f.cf[0], "a", "1");
               f.db->Flush(f.cf[0]);
               f.db->Put({}, f.cf[0], "b", "1");
               WriteExternal(f.env, "ext/1.sst", {{"a", "x"}});
               f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});
               f.Crash(true);
             }));
}

TEST(Bug_StallNoCompaction) {  // MC_Bug_StallNoCompaction -> ST5 StallEnds
  CHECK(Both("ST5 StallEnds", [](Options& o) { o.fix.FixStallNeedsCompaction = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               for (int i = 0; i < 2; i++) {
                 f.db->Put({}, f.cf[0], "k" + std::to_string(i), "v");
                 f.db->Flush(f.cf[0]);
               }
             },
             [](Options& o) {
               o.disable_auto_compactions = true;
               o.level0_file_num_compaction_trigger = 1;
               o.level0_slowdown_writes_trigger = 1;
               o.level0_stop_writes_trigger = 2;
             }));
}

TEST(Bug_StopBelowTrigger) {  // MC_Bug_StopBelowTrigger -> ST5 StallEnds
  CHECK(Both("ST5 StallEnds", [](Options& o) { o.fix.FixSanitizeTriggers = false; },
             [](Fixture& f) {
               if (!f.Open().ok()) return;
               for (int i = 0; i < 2; i++) {
                 f.db->Put({}, f.cf[0], "k" + std::to_string(i), "v");
                 f.db->Flush(f.cf[0]);
               }
             },
             [](Options& o) {
               o.disable_auto_compactions = false;
               o.level0_file_num_compaction_trigger = 4;
               o.level0_slowdown_writes_trigger = 2;
               o.level0_stop_writes_trigger = 2;
             }));
}

int main() { return testing::RunAll(); }
