// test_compaction.cc: compaction of the LSM tree (LSM2 to LSM12), leveled and
// universal. Part 1 checks the behavior the algorithms describe. Part 2 has one
// test per mechanism of LSMTree.tla: with it on, no invariant fails; with it off,
// the invariant TLC reported fails.
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

int Files(const DB& db, CfId c, int level) {
  const ColumnFamily* cf = db.tree.Cf(c);
  return static_cast<int>(level == 0 ? cf->l0.size() : cf->levels[level].size());
}
int Entries(const DB& db, CfId c) {
  const ColumnFamily* cf = db.tree.Cf(c);
  int n = 0;
  for (auto& f : cf->l0) n += static_cast<int>(f->entries.size());
  for (size_t l = 1; l < cf->levels.size(); l++)
    for (auto& f : cf->levels[l]) n += static_cast<int>(f->entries.size());
  return n;
}
// One flush of n distinct keys with the given prefix (the same size every time).
void FlushKeys(Fixture& f, const std::string& prefix, int n) {
  for (int i = 0; i < n; i++) f.db->Put({}, f.cf[0], prefix + std::to_string(i), "vvvvvvvv");
  f.db->Flush(f.cf[0]);
}
std::vector<uint64_t> RunSizes(const DB& db, CfId c) {
  std::vector<uint64_t> out;
  for (auto& r : db.tree.SortedRuns(c)) {
    uint64_t n = 0;
    for (auto& [l, f] : r) n += f->entries.size();
    out.push_back(n);
  }
  return out;
}

}  // namespace

// ============================================================ part 1: behavior

TEST(LeveledL0TriggerCompactsIntoBaseLevel) {  // LSM4, LSM5, LSM11
  Fixture f;
  f.opts.disable_auto_compactions = false;
  f.opts.level0_file_num_compaction_trigger = 2;
  CHECK_OK(f.Open());
  FlushKeys(f, "a", 3);
  CHECK(Files(*f.db, f.cf[0], 0) == 1);
  FlushKeys(f, "a", 3);  // the second L0 file overlaps the first and reaches the trigger
  CHECK(Files(*f.db, f.cf[0], 0) == 0);
  // dynamic level bytes: a small tree has its base level at the bottom (L2)
  CHECK(f.db->tree.LevelTargets(f.cf[0]).baseLevel == 2);
  CHECK(Files(*f.db, f.cf[0], 2) >= 1);
  CHECK(f.Get(f.cf[0], "a1") == "vvvvvvvv");
  CHECK(f.db->tree.compactionsDone >= 1);
  CHECK(f.Clean());
}

TEST(StaticTargetsGrowByTheMultiplier) {  // LSM4
  Fixture f;
  f.opts.level_compaction_dynamic_level_bytes = false;
  f.opts.max_bytes_for_level_base = 1000;
  f.opts.max_bytes_for_level_multiplier = 10;
  f.opts.num_levels = 4;
  CHECK_OK(f.Open());
  LevelInfo li = f.db->tree.LevelTargets(f.cf[0]);
  CHECK(li.baseLevel == 1);
  CHECK(li.target[1] == 1000 && li.target[2] == 10000 && li.target[3] == 100000);
  // L1 over its target gets a score >= 1
  for (int i = 0; i < 60; i++) f.db->Put({}, f.cf[0], "k" + std::to_string(100 + i), "vvvvvvvv");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  li = f.db->tree.LevelTargets(f.cf[0]);
  CHECK(li.score[1] >= 1);
  CHECK(f.db->tree.NeedsCompaction(f.cf[0]));
  CHECK_OK(f.db->MaybeCompact());
  CHECK(Files(*f.db, f.cf[0], 2) >= 1);
  CHECK(f.Clean());
}

TEST(TrivialMoveKeepsTheFile) {  // LSM5, LSM9
  Fixture f;
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "a", "1");
  f.db->Flush(f.cf[0]);
  uint64_t num = f.db->tree.Cf(f.cf[0])->l0.front()->number;
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(f.db->tree.trivialMoves == 1);
  CHECK(f.db->tree.Cf(f.cf[0])->levels[1].front()->number == num);
  CHECK(f.env.Exists(NumberedName("sst", num, "sst")));
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.Clean());
}

TEST(MergeKeepsTheNewestVersionInEachSnapshotStripe) {  // LSM7
  Fixture f;
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "k", "v1");
  const Snapshot* s = f.db->GetSnapshot();
  f.db->Put({}, f.cf[0], "k", "v2");
  f.db->Put({}, f.cf[0], "k", "v3");
  f.db->Flush(f.cf[0]);  // the flush already drops v2 (MT5)
  f.db->Put({}, f.cf[0], "x", "1");
  f.db->Flush(f.cf[0]);
  f.db->Put({}, f.cf[0], "k", "v4");
  f.db->Flush(f.cf[0]);
  CHECK(Entries(*f.db, f.cf[0]) == 4);  // v1, v3, x, v4
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(Entries(*f.db, f.cf[0]) == 3);  // v1 (seen by s), x, v4; v3 is shadowed in its stripe
  CHECK(f.Get(f.cf[0], "k", s) == "v1");
  f.db->ReleaseSnapshot(s);
  f.db->Put({}, f.cf[0], "k", "v5");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));  // overlaps the L1 file, so a real merge
  CHECK(Entries(*f.db, f.cf[0]) == 2);       // k=v5, x: one stripe is left
  CHECK(f.Get(f.cf[0], "k") == "v5");
  CHECK(f.Clean());
}

TEST(DeletionIsDroppedOnlyAtTheBottom) {  // LSM7 KeyNotExistsBeyond
  Fixture f;
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "k", "old");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK_OK(f.db->CompactLevel(f.cf[0], 1));  // "old" is in L2
  f.db->Delete({}, f.cf[0], "k");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));  // L0 -> L1: "old" is below, keep the deletion
  CHECK(Files(*f.db, f.cf[0], 1) == 1 && Entries(*f.db, f.cf[0]) == 2);
  CHECK(f.Get(f.cf[0], "k") == "<none>");
  CHECK_OK(f.db->CompactLevel(f.cf[0], 1));  // L1 -> L2: the bottom, both go
  CHECK(Entries(*f.db, f.cf[0]) == 0);
  CHECK(f.Get(f.cf[0], "k") == "<none>");
  CHECK(f.Clean());
}

TEST(IngestBehindKeepsEveryDeletion) {  // LSM7 with allow_ingest_behind
  Fixture f;
  f.opts.allow_ingest_behind = true;
  f.opts.num_levels = 3;  // L2 is reserved
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "k", "a");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  f.db->Delete({}, f.cf[0], "k");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));  // a merge into L1, the last output level; nothing is below yet
  CHECK(Entries(*f.db, f.cf[0]) == 1);       // but an ingested file may still come under it
  WriteExternal(f.env, "ext/1.sst", {{"k", "old"}});
  IngestOptions io;
  io.ingest_behind = true;
  CHECK_OK(f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"}, io));
  CHECK(f.Get(f.cf[0], "k") == "<none>");
  CHECK(f.Clean());
}

TEST(CleanCutTakesFilesThatShareAKey) {  // LSM3
  Fixture f;
  f.opts.compaction_split_user_keys = true;
  f.opts.target_file_size_base = 1;  // cut after every version
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "a", "0");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));  // L1: [a@1]
  f.db->Put({}, f.cf[0], "a", "2");
  const Snapshot* s = f.db->GetSnapshot();
  f.db->Put({}, f.cf[0], "a", "3");
  f.db->Put({}, f.cf[0], "b", "4");
  f.db->Flush(f.cf[0]);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(Files(*f.db, f.cf[0], 1) == 3);  // [a@3] [a@2] [b@4]: two files share "a"
  Compaction C;
  CHECK(f.db->tree.PickLeveled(f.cf[0], &C, 1));
  CHECK(C.inputs.size() == 2);  // a@3 alone would put the newer version under the older
  f.db->ReleaseSnapshot(s);
  CHECK(f.Clean());
}

TEST(ConcurrentCompactionsUseDisjointFilesAndRanges) {  // LSM5, LS4
  Fixture f;
  f.opts.level0_file_num_compaction_trigger = 1;
  f.opts.level_compaction_dynamic_level_bytes = false;
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "a", "1");
  f.db->Flush(f.cf[0]);
  f.db->Put({}, f.cf[0], "z", "1");
  f.db->Flush(f.cf[0]);
  uint64_t one = f.db->CompactStart(f.cf[0]);  // newest L0 file first: "z"
  uint64_t two = f.db->CompactStart(f.cf[0]);  // "a" does not overlap it: may run at once
  CHECK(one != 0 && two != 0);
  CHECK(f.db->CompactStart(f.cf[0]) == 0);     // nothing left that is not busy
  CHECK_OK(f.db->CompactFinish(two));
  CHECK_OK(f.db->CompactFinish(one));
  CHECK(Files(*f.db, f.cf[0], 1) == 2 && Files(*f.db, f.cf[0], 0) == 0);
  CHECK(f.Clean());
}

TEST(PinnedVersionKeepsItsFiles) {  // LSM10, LS5
  Fixture f;
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "a", "1");
  f.db->Flush(f.cf[0]);
  f.db->Put({}, f.cf[0], "a", "2");
  f.db->Flush(f.cf[0]);
  auto pin = f.db->PinVersion(f.cf[0]);
  std::string path = f.db->tree.Cf(f.cf[0])->l0.front()->path;
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));  // both L0 files are obsolete now
  CHECK(f.env.Exists(path));                 // but the pinned version still lists them
  std::string v;
  CHECK_OK(f.db->ReadPinned(*pin, "a", &v));
  CHECK(v == "2");
  f.db->ReleaseVersion(&pin);
  CHECK(!f.env.Exists(path));
  CHECK(f.Clean());
}

TEST(IngestionStaysAboveARunningCompaction) {  // SST3 with LSM5
  Fixture f;
  f.opts.level0_file_num_compaction_trigger = 1;
  CHECK_OK(f.Open());
  f.db->Put({}, f.cf[0], "a", "1");
  f.db->Put({}, f.cf[0], "c", "1");
  f.db->Flush(f.cf[0]);
  uint64_t id = f.db->CompactStart(f.cf[0]);  // [a, c] into L2 (the base level)
  CHECK(id != 0);
  WriteExternal(f.env, "ext/1.sst", {{"b", "x"}});
  CHECK_OK(f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"}));
  CHECK(Files(*f.db, f.cf[0], 2) == 0 && Files(*f.db, f.cf[0], 1) == 1);  // L1, not L2
  CHECK_OK(f.db->CompactFinish(id));
  CHECK(f.Get(f.cf[0], "b") == "x");
  CHECK(f.Clean());
}

TEST(WritesSlowDownThenStop) {  // LSM12
  // Auto compaction is on (DB10 ignores L0 otherwise), so the flushes run by hand
  // and nothing compacts until the test asks.
  Fixture f;
  f.opts.disable_auto_compactions = false;
  f.opts.inline_background = false;
  f.opts.level0_file_num_compaction_trigger = 1;
  f.opts.level0_slowdown_writes_trigger = 2;
  f.opts.level0_stop_writes_trigger = 3;
  CHECK_OK(f.Open());
  auto flush = [&](const char* k) {
    CHECK_OK(f.db->Put({}, f.cf[0], k, "v"));
    CHECK_OK(f.db->Flush(f.cf[0]));
    uint64_t j = f.db->FlushStart(f.cf[0]);
    CHECK(j != 0);
    CHECK_OK(f.db->FlushFinish(j));
    CHECK_OK(f.db->InstallFlush(f.cf[0]));
  };
  flush("a");
  CHECK(f.db->tree.GetStallState() == StallState::kGo);
  flush("b");
  CHECK(f.db->tree.GetStallState() == StallState::kDelay);
  flush("c");
  CHECK(f.db->tree.GetStallState() == StallState::kStop);
  CHECK(!f.db->accepting);
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(f.db->tree.GetStallState() == StallState::kGo);
  CHECK(f.db->accepting);
  CHECK(f.Clean());
}

TEST(UniversalSizeRatioFollowsTheWikiExample) {  // LSM6
  Fixture f;
  f.opts.compaction_style = CompactionStyle::kUniversal;
  f.opts.disable_auto_compactions = false;
  f.opts.level0_file_num_compaction_trigger = 5;
  f.opts.compaction_options_universal.size_ratio = 0;
  f.opts.compaction_options_universal.max_size_amplification_percent = 1 << 20;  // off
  f.opts.num_levels = 1;  // every run is an L0 file, as in the wiki's example
  f.opts.write_buffer_size = 1 << 20;
  CHECK_OK(f.Open());
  int k = 0;
  auto flush = [&] { FlushKeys(f, "k" + std::to_string(k++) + "_", 4); };
  for (int i = 0; i < 5; i++) flush();  // 1 1 1 1 1 => 5
  CHECK((RunSizes(*f.db, f.cf[0]) == std::vector<uint64_t>{20}));
  for (int i = 0; i < 4; i++) flush();  // 1 1 1 1 5 => 4 5
  CHECK((RunSizes(*f.db, f.cf[0]) == std::vector<uint64_t>{16, 20}));
  for (int i = 0; i < 3; i++) flush();  // 1 1 1 4 5 => 3 4 5
  CHECK((RunSizes(*f.db, f.cf[0]) == std::vector<uint64_t>{12, 16, 20}));
  CHECK(f.Clean());
}

TEST(UniversalSizeAmplificationCompactsEverything) {  // LSM6
  Fixture f;
  f.opts.compaction_style = CompactionStyle::kUniversal;
  f.opts.disable_auto_compactions = false;
  f.opts.level0_file_num_compaction_trigger = 2;
  f.opts.compaction_options_universal.max_size_amplification_percent = 25;
  f.opts.num_levels = 1;
  f.opts.write_buffer_size = 1 << 20;
  CHECK_OK(f.Open());
  int k = 0;
  auto flush = [&] { FlushKeys(f, "k" + std::to_string(k++) + "_", 4); };
  flush();
  flush();  // 1 1 => 2
  CHECK((RunSizes(*f.db, f.cf[0]) == std::vector<uint64_t>{8}));
  flush();  // 1 2 => 3 (1/2 > 25%)
  flush();  // 1 3 => 4
  flush();  // 1 4: 1/4 <= 25%, but the size ratio rule merges 1 and 4? no: 4 > 1.01 x 1
  CHECK((RunSizes(*f.db, f.cf[0]) == std::vector<uint64_t>{4, 16}));
  CHECK(f.Clean());
}

TEST(UniversalOutputGoesJustAboveTheNextOlderRun) {  // LSM6 placement
  Fixture f;
  f.opts.compaction_style = CompactionStyle::kUniversal;
  f.opts.level0_file_num_compaction_trigger = 2;
  f.opts.compaction_options_universal.max_size_amplification_percent = 1 << 20;
  f.opts.num_levels = 5;
  f.opts.write_buffer_size = 1 << 20;
  CHECK_OK(f.Open());
  FlushKeys(f, "big", 40);
  FlushKeys(f, "big2_", 40);
  uint64_t id = f.db->CompactStart(f.cf[0]);  // 1 1 => one run, at the bottom (L4)
  CHECK(id != 0);
  CHECK_OK(f.db->CompactFinish(id));
  CHECK(Files(*f.db, f.cf[0], 4) >= 1);
  FlushKeys(f, "s", 2);
  FlushKeys(f, "t", 2);
  id = f.db->CompactStart(f.cf[0]);  // the two small runs; L4 is much bigger
  CHECK(id != 0);
  CHECK(f.db->tree.compactions[id].outLevel == 3);
  CHECK_OK(f.db->CompactFinish(id));
  CHECK(Files(*f.db, f.cf[0], 3) >= 1);
  CHECK(f.Clean());
}

// ============================================================ part 2: one test per mechanism

TEST(Bug_L0PartialPick) {  // MC_Bug_L0PartialPick -> LS2 (MS3)
  CHECK(Both("MS3", [](Options& o) { o.fix.FixL0Overlap = false; }, [](Fixture& f) {
    f.opts.level0_file_num_compaction_trigger = 2;
    f.Open();
    f.db->Put({}, f.cf[0], "k", "old");
    f.db->Flush(f.cf[0]);
    f.db->Put({}, f.cf[0], "k", "new");
    f.db->Flush(f.cf[0]);
    uint64_t id = f.db->CompactStart(f.cf[0]);  // picks the newest L0 file
    if (id) f.db->CompactFinish(id);            // the bug moves "new" below "old"
  }));
}

TEST(Bug_NoCleanCut) {  // MC_Bug_NoCleanCut -> LS2 (MS3)
  CHECK(Both("MS3", [](Options& o) { o.fix.FixCleanCut = false; }, [](Fixture& f) {
    f.opts.compaction_split_user_keys = true;
    f.opts.target_file_size_base = 1;
    f.Open();
    f.db->Put({}, f.cf[0], "a", "0");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);
    f.db->Put({}, f.cf[0], "a", "2");
    const Snapshot* s = f.db->GetSnapshot();
    f.db->Put({}, f.cf[0], "a", "3");
    f.db->Put({}, f.cf[0], "b", "4");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // L1: [a@3] [a@2] [b@4]
    f.db->CompactLevel(f.cf[0], 1);  // the bug moves [a@3] alone to L2
    f.db->ReleaseSnapshot(s);
  }));
}

TEST(Bug_SkipOutputOverlap) {  // MC_Bug_SkipOutputOverlap -> LS1
  CHECK(Both("LS1", [](Options& o) { o.fix.FixOutputOverlap = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "a", "1");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);
    f.db->Put({}, f.cf[0], "a", "2");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // the bug leaves the old L1 file out of the merge
  }));
}

TEST(Bug_PickBusy) {  // MC_Bug_PickBusy -> LS4
  CHECK(Both("LS4", [](Options& o) { o.fix.FixSkipBusy = false; }, [](Fixture& f) {
    f.opts.level0_file_num_compaction_trigger = 1;
    f.Open();
    f.db->Put({}, f.cf[0], "a", "1");
    f.db->Flush(f.cf[0]);
    f.db->Put({}, f.cf[0], "b", "1");
    f.db->Flush(f.cf[0]);
    uint64_t one = f.db->CompactStart(f.cf[0]);  // the newest L0 file, alone
    uint64_t two = f.db->CompactStart(f.cf[0]);  // the bug picks it again
    if (two) f.db->CompactFinish(two);
    if (one) f.db->CompactFinish(one);
  }));
}

TEST(Bug_IgnoreSnapshots) {  // MC_Bug_IgnoreSnapshots -> LS3 (MS2)
  CHECK(Both("MS2", [](Options& o) { o.fix.FixSnapshotKeep = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "k", "v0");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // L1 holds "k", so the next one merges
    f.db->Put({}, f.cf[0], "k", "v1");
    f.db->GetSnapshot();
    f.db->Put({}, f.cf[0], "k", "v2");
    f.db->Flush(f.cf[0]);            // the flush keeps v1 for the snapshot
    f.db->CompactLevel(f.cf[0], 0);  // the bug drops it
  }));
}

TEST(Bug_DropTombstoneEarly) {  // MC_Bug_DropTombstoneEarly -> LS3 (MS1)
  CHECK(Both("MS1", [](Options& o) { o.fix.FixTombstoneBottom = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "k", "old");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);
    f.db->CompactLevel(f.cf[0], 1);  // "old" is in L2
    f.db->Put({}, f.cf[0], "j", "1");
    f.db->Put({}, f.cf[0], "l", "1");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // L1: [j, l], so the next one merges
    f.db->Delete({}, f.cf[0], "k");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // the bug drops the deletion: "old" comes back
  }));
}

TEST(Bug_NonAdjacentRuns) {  // MC_Bug_NonAdjacentRuns -> LS2 (MS3)
  CHECK(Both("MS3", [](Options& o) { o.fix.FixAdjacentRuns = false; }, [](Fixture& f) {
    f.opts.compaction_style = CompactionStyle::kUniversal;
    f.opts.level0_file_num_compaction_trigger = 3;
    f.opts.compaction_options_universal.max_size_amplification_percent = 1 << 20;
    f.opts.write_buffer_size = 1 << 20;
    f.Open();
    f.db->Put({}, f.cf[0], "k", "old");
    f.db->Flush(f.cf[0]);  // small
    f.db->Put({}, f.cf[0], "k", "mid");
    for (int i = 0; i < 20; i++) f.db->Put({}, f.cf[0], "x" + std::to_string(i), "vvvvvvvv");
    f.db->Flush(f.cf[0]);  // big
    f.db->Put({}, f.cf[0], "k", "new");
    f.db->Flush(f.cf[0]);  // small
    uint64_t id = f.db->CompactStart(f.cf[0]);  // the bug merges the two small runs around "mid"
    if (id) f.db->CompactFinish(id);
  }));
}

TEST(Bug_IngestBehindDrop) {  // MC_Bug_IngestBehindDrop -> LS3 (MS1)
  CHECK(Both("MS1", [](Options& o) { o.fix.FixIngestBehindKeep = false; }, [](Fixture& f) {
    f.opts.allow_ingest_behind = true;
    f.Open();
    f.db->Put({}, f.cf[0], "k", "a");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);
    f.db->Delete({}, f.cf[0], "k");
    f.db->Flush(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // the bug drops the deletion: nothing is below yet
    WriteExternal(f.env, "ext/1.sst", {{"k", "old"}});
    IngestOptions io;
    io.ingest_behind = true;
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"}, io);  // "old" comes back
  }));
}

TEST(Bug_DeleteReferenced) {  // MC_Bug_DeleteReferenced -> LS5
  CHECK(Both("LS5", [](Options& o) { o.fix.FixVersionRef = false; }, [](Fixture& f) {
    f.Open();
    f.db->Put({}, f.cf[0], "a", "1");
    f.db->Flush(f.cf[0]);
    f.db->Put({}, f.cf[0], "a", "2");
    f.db->Flush(f.cf[0]);
    auto pin = f.db->PinVersion(f.cf[0]);
    f.db->CompactLevel(f.cf[0], 0);  // the bug deletes the inputs the pin still reads
    std::string v;
    f.db->ReadPinned(*pin, "a", &v);
    f.db->ReleaseVersion(&pin);
  }));
}

TEST(Bug_IngestIntoCompaction) {  // MC_Bug_IngestIntoCompaction -> LS1
  CHECK(Both("LS1", [](Options& o) { o.fix.FixIngestSkipCompacting = false; }, [](Fixture& f) {
    f.opts.level0_file_num_compaction_trigger = 1;
    f.Open();
    f.db->Put({}, f.cf[0], "a", "1");
    f.db->Put({}, f.cf[0], "c", "1");
    f.db->Flush(f.cf[0]);
    uint64_t id = f.db->CompactStart(f.cf[0]);  // [a, c] into L2
    WriteExternal(f.env, "ext/1.sst", {{"b", "x"}});
    f.db->IngestExternalFile(f.cf[0], {"ext/1.sst"});  // the bug puts [b] in L2 too
    if (id) f.db->CompactFinish(id);
  }));
}

int main() { return testing::RunAll(); }
