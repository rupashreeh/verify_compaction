// test_random.cc: a random simulator, in the spirit of TLC's simulation mode.
//
// Each run picks a random interleaving of algorithm steps: write groups advanced one
// step at a time (WAL1, WAL2, WAL4a per CF, WAL4b), manual flushes, flush jobs that
// start, finish and install in any order, log purges, compactions, CF create, drop
// and release, ingestion into one or two CFs, snapshots, WAL syncs, and process or
// machine crashes followed by recovery. Every invariant is checked after every step.
//
// Part 1 runs with every mechanism on and must find no violation.
// Part 2 switches one mechanism off at a time and reports how quickly the
// simulator finds a violation, like the MC_Bug_* rows of the TLC tables.
#include <cstdlib>

#include "harness.h"

using namespace lsm;

namespace {

struct Sim {
  Fixture f;
  Rng rng;
  std::vector<std::shared_ptr<WriteGroup>> inflight;
  std::vector<uint64_t> jobs;
  std::vector<const Snapshot*> snaps;
  std::vector<CfId> held;  // dropped CFs whose handle is still open
  std::vector<std::shared_ptr<MemtablePin>> pins;  // readers holding memtables (WBM2 waits for them)
  std::vector<uint64_t> comps;                       // compactions started and not finished
  std::vector<std::shared_ptr<VersionPin>> vpins;    // iterators holding versions (LSM10)
  int value = 0, ext = 0, cfName = 0;
  bool allowCrash;
  bool compaction = false;  // also take compaction steps (LSM5 to LSM10)

  Sim(uint64_t seed, bool crash, bool comp = false) : rng(seed), allowCrash(crash), compaction(comp) {}

  CfId RandomLive() {
    auto cs = f.db->tree.CFs();
    return cs[rng.Uniform(cs.size())];
  }
  std::string Key() { return "k" + std::to_string(rng.Uniform(5)); }

  void Reopen(bool machine) {
    std::vector<std::string> names;
    for (CfId c : f.db->tree.CFs()) names.push_back(f.db->tree.Cf(c)->name);
    f.names = names;
    inflight.clear();
    pins.clear();  // the readers die with the process
    comps.clear();
    vpins.clear();
    jobs.clear();
    snaps.clear();
    held.clear();
    Status s = f.Crash(machine, &rng);
    if (!s.ok()) {
      // a refused open is recorded as a RecoveryOpens violation; keep going on a fresh store
      f.env = SimEnv();
      f.names = {"default"};
      f.Open();
    }
  }

  void NewWrite() {
    if (inflight.size() >= 3) return;
    bool noWal = std::getenv("NOWAL0") ? false : rng.OneIn(8);
    std::vector<std::pair<WriteBatch, WriteOptions>> bs;
    int nb = 1 + rng.Uniform(2);
    for (int i = 0; i < nb; i++) {
      WriteBatch b;
      int nops = 1 + rng.Uniform(2);
      for (int j = 0; j < nops; j++) {
        CfId c = RandomLive();
        if (rng.OneIn(5)) b.Delete(c, Key());
        else b.Put(c, Key(), "v" + std::to_string(++value));
      }
      WriteOptions o;
      o.sync = rng.OneIn(3);
      o.disableWAL = noWal;
      bs.push_back({b, o});
    }
    Status s;
    auto G = f.db->NewGroup(bs, &s);
    if (s.ok()) inflight.push_back(G);
  }

  void AdvanceWrite() {
    if (inflight.empty()) return;
    size_t i = rng.Uniform(inflight.size());
    auto G = inflight[i];
    auto& b0 = G->batches;
    bool admitted = false;
    for (auto& b : b0) admitted |= b->stage != BatchStage::kQueued;
    Status s;
    if (!admitted) {
      s = f.db->Admit(*G);
      if (!s.ok() && !s.IsBusy()) { f.db->AbandonGroup(*G); inflight.erase(inflight.begin() + i); }
      if (s.IsBusy() && rng.OneIn(4)) { f.db->AbandonGroup(*G); inflight.erase(inflight.begin() + i); }
      return;
    }
    if (!G->appended) {
      s = f.db->AppendGroup(*G);
      if (!s.ok()) { f.db->AbandonGroup(*G); inflight.erase(inflight.begin() + i); }
      return;
    }
    if (!G->AllInserted() && !rng.OneIn(4)) {  // otherwise try to publish (WAL4b must refuse)
      std::vector<std::pair<size_t, CfId>> todo;
      for (size_t k = 0; k < b0.size(); k++)
        for (CfId c : b0[k]->Cfs())
          if (!b0[k]->insertedCfs.count(c) && b0[k]->stage != BatchStage::kFailed) todo.push_back({k, c});
      if (!todo.empty()) {
        auto [k, c] = todo[rng.Uniform(todo.size())];
        f.db->InsertBatchCf(*G, k, c);
        return;
      }
    }
    s = f.db->PublishGroup(*G);
    if (s.ok()) inflight.erase(inflight.begin() + i);
  }

  void Ingest() {
    std::vector<IngestArg> args;
    int n = 1 + rng.Uniform(2);
    std::set<CfId> used;
    bool behind = f.opts.allow_ingest_behind && rng.OneIn(3);
    for (int i = 0; i < n; i++) {
      CfId c = RandomLive();
      if (used.count(c)) continue;
      used.insert(c);
      std::string path = "ext/" + std::to_string(++ext) + ".sst";
      SstFileWriter w(&f.env);
      w.Open(path);
      int a = rng.Uniform(5), b = a + rng.Uniform(2);
      for (int k = a; k <= std::min(b, 4); k++) w.Put("k" + std::to_string(k), "i" + std::to_string(++value));
      w.Finish();
      IngestOptions o;
      o.ingest_behind = behind;
      args.push_back({c, {path}, o});
    }
    f.db->IngestExternalFiles(args);
  }

  std::vector<std::string> trace;
  void T(std::string x) { trace.push_back(std::move(x)); }
  // Write buffer manager steps: readers pin and release memtables, and the buffer
  // size changes (WBM8).
  void WbmStep() {
    int r = rng.Uniform(3);
    if (r == 0) { if (pins.size() < 2) pins.push_back(f.db->PinMemtables(RandomLive())); }
    else if (r == 1) { if (!pins.empty()) pins.erase(pins.begin() + rng.Uniform(pins.size())); }
    else f.db->Wbm().SetBufferSize(std::vector<size_t>{150, 250, 400}[rng.Uniform(3)]);
    f.db->Step("WBM reader / SetBufferSize");
  }

  // Compaction steps: a compaction starts and finishes later, so others run in
  // between (LSM5, LSM6, LSM9); iterators pin versions and read through them.
  void CompactionStep() {
    int r = rng.Uniform(8);
    if (r == 0) { uint64_t id = f.db->CompactStart(RandomLive()); if (id) comps.push_back(id); }
    else if (r == 1) {
      // DB6 then DB7: write the outputs, and install them in a later step
      if (!comps.empty()) {
        size_t i = rng.Uniform(comps.size());
        auto it = f.db->tree.compactions.find(comps[i]);
        if (it == f.db->tree.compactions.end()) comps.erase(comps.begin() + i);
        else if (!it->second.written) f.db->CompactWrite(comps[i]);
        else { f.db->CompactInstall(comps[i]); comps.erase(comps.begin() + i); }
      }
    } else if (r == 2) f.db->CompactLevel(RandomLive(), rng.Uniform(3));
    else if (r == 3) { if (vpins.size() < 2) vpins.push_back(f.db->PinVersion(RandomLive())); }
    else if (r == 4) {
      if (!vpins.empty()) {
        size_t i = rng.Uniform(vpins.size());
        std::string v;
        f.db->ReadPinned(*vpins[i], Key(), &v);
        f.db->ReleaseVersion(&vpins[i]);
        vpins.erase(vpins.begin() + i);
      }
    } else if (r == 5) f.db->PurgeAllObsoleteFiles();  // DB8, full scan
    else if (r == 6) f.db->SyncManifest();
    else f.db->MaybeCompact();
  }

  void Step() {
    if (f.opts.write_buffer_manager && rng.OneIn(10)) { T("wbm"); WbmStep(); return; }
    if (compaction && rng.OneIn(5)) { T("compaction"); CompactionStep(); return; }
    int r = rng.Uniform(100);
    T("r=" + std::to_string(r) + " published=" + std::to_string(f.db->tree.lastPublished) +
      " alloc=" + std::to_string(f.db->tree.lastAllocated) + " active=" + std::to_string(f.db->wal.ActiveNum()));
    if (r < 30) NewWrite();
    else if (r < 60) AdvanceWrite();
    else if (r < 64) f.db->Flush(RandomLive());
    else if (r < 68) { uint64_t j = f.db->FlushStart(RandomLive()); if (j) jobs.push_back(j); }
    else if (r < 72) {
      if (!jobs.empty()) {
        size_t i = rng.Uniform(jobs.size());
        f.db->FlushFinish(jobs[i]);
        jobs.erase(jobs.begin() + i);
      }
    } else if (r < 76) f.db->InstallFlush(RandomLive());
    else if (r < 78) f.db->PurgeObsoleteLogs();
    else if (r < 80) f.db->RunBackgroundWork();
    else if (r < 82) f.db->CompactLevel(RandomLive(), rng.Uniform(2));
    else if (r < 84) {
      if (f.db->tree.CFs().size() < 3) { CfId c; f.db->CreateColumnFamily("cf" + std::to_string(++cfName), &c); }
    } else if (r < 85) {
      CfId c = RandomLive();
      if (c != kDefaultCf && f.db->DropColumnFamily(c).ok()) held.push_back(c);
    } else if (r < 86) {
      if (!held.empty()) { f.db->ReleaseHandle(held.back()); held.pop_back(); }
    } else if (r < 89) Ingest();
    else if (r < 91) { if (snaps.size() < 2) snaps.push_back(f.db->GetSnapshot()); }
    else if (r < 93) { if (!snaps.empty()) { f.db->ReleaseSnapshot(snaps.back()); snaps.pop_back(); } }
    else if (r < 95) f.db->FlushWAL(true);
    else if (r < 96) f.db->Resume();
    else if (allowCrash && r < 98) Reopen(rng.OneIn(2));
  }
};

struct Config {
  const char* name;
  std::function<void(Options&)> set;
  bool crash = true;
  bool compaction = false;
};

// Small sizes so that a run of a few hundred steps builds several levels.
void Leveled(Options& o) {
  o.disable_auto_compactions = false;
  o.level0_file_num_compaction_trigger = 2;
  o.max_bytes_for_level_base = 200;
  o.target_file_size_base = 60;
  o.num_levels = 4;
}
void Universal(Options& o) {
  Leveled(o);
  o.compaction_style = CompactionStyle::kUniversal;
  o.level0_file_num_compaction_trigger = 3;
}

// Runs `seeds` random runs of `steps` steps. Returns the number of runs that found a
// violation; the first violation is stored in *first.
int Run(const Config& cfg, int seeds, int steps, std::string* first, int* firstStep, bool stopAtFirst) {
  int bad = 0;
  for (int seed = 1; seed <= seeds; seed++) {
    if (std::getenv("ONLYSEED") && seed != std::atoi(std::getenv("ONLYSEED"))) continue;
    Sim sim(seed * 7919 + 13, cfg.crash, cfg.compaction);
    sim.f.opts.inline_background = false;
    sim.f.opts.write_buffer_size = 120;
    sim.f.opts.max_total_wal_size = 500;
    sim.f.opts.max_write_buffer_number = 3;
    sim.f.opts.create_missing_column_families = true;
    sim.f.names = {"default", "a"};
    cfg.set(sim.f.opts);
    if (!sim.f.Open().ok()) { std::printf("    open failed\n"); return seeds; }
    for (int i = 0; i < steps && sim.f.Clean(); i++) {
      sim.Step();
      if (!sim.f.Clean() && first->empty()) {
        *first = sim.f.ghost.violations.front();
        *firstStep = i + 1;
      }
    }
    if (!sim.f.Clean()) {
      if (std::getenv("TRACE")) {
        std::printf("    seed %d\n", seed);
        for (auto& t : sim.trace) std::printf("      %s\n", t.c_str());
        sim.f.PrintViolations();
        if (std::getenv("DUMPCF")) DumpCf(*sim.f.db, std::atoi(std::getenv("DUMPCF")), std::getenv("DUMPKEY"));
      }
      bad++;
      if (stopAtFirst) return bad;
    }
  }
  return bad;
}

}  // namespace

TEST(RandomInterleavingsAllMechanismsOn) {
  std::vector<Config> cfgs = {
      {"default", [](Options&) {}},
      {"avoid_flush_during_recovery", [](Options& o) { o.avoid_flush_during_recovery = true; }},
      {"allow_ingest_behind", [](Options& o) { o.allow_ingest_behind = true; }},
      {"merge two memtables per flush", [](Options& o) { o.max_write_buffer_number_to_merge = 2; o.max_write_buffer_number = 4; }},
      {"recycle and archive logs", [](Options& o) { o.recycle_log_file_num = 2; }},
      {"write buffer manager, stalls", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(250, nullptr, true);
       }},
      {"write buffer manager, cache, across DBs", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(
             250, std::make_shared<BlockCache>(100000, false), true,
             WriteBufferFlushPolicy::kFlushLargestAcrossDBs, 64);
       }},
      {"leveled compaction", [](Options& o) { Leveled(o); }, true, true},
      {"leveled, round robin, split keys", [](Options& o) {
         Leveled(o);
         o.compaction_pri = CompactionPri::kRoundRobin;
         o.level_compaction_dynamic_level_bytes = false;
         o.compaction_split_user_keys = true;
       }, true, true},
      {"leveled, ingest behind", [](Options& o) { Leveled(o); o.allow_ingest_behind = true; }, true, true},
      {"universal compaction", [](Options& o) { Universal(o); }, true, true},
      {"universal, ingest behind", [](Options& o) { Universal(o); o.allow_ingest_behind = true; }, true, true},
  };
  int seeds = std::getenv("SEEDS") ? std::atoi(std::getenv("SEEDS")) : 150;
  for (auto& cfg : cfgs) {
    std::string first;
    int at = 0;
    int bad = Run(cfg, seeds, 400, &first, &at, false);
    std::printf("    %-32s %d runs x 400 steps: %s\n", cfg.name, seeds, bad == 0 ? "no violation" : first.c_str());
    CHECK(bad == 0);
  }
}

TEST(RandomInterleavingsFindEachBug) {
  std::vector<Config> bugs = {
      {"FixSyncAllLogs off", [](Options& o) { o.fix.FixSyncAllLogs = false; }},
      {"FixDirSync off", [](Options& o) { o.fix.FixDirSync = false; }},
      {"FixWaitPendingInserts off", [](Options& o) { o.fix.FixWaitPendingInserts = false; }},
      {"FixSyncClosedOnFlush off", [](Options& o) { o.fix.FixSyncClosedOnFlush = false; }},
      {"FixResumeRule off", [](Options& o) { o.fix.FixResumeRule = false; o.avoid_flush_during_recovery = true; }},
      {"track_and_verify_wals off", [](Options& o) { o.track_and_verify_wals = false; }},
      {"approx_pit_check on", [](Options& o) { o.approx_pit_check = true; }},
      {"FixInstallInOrder off", [](Options& o) { o.fix.FixInstallInOrder = false; }},
      {"FixFlushOverlap off", [](Options& o) { o.fix.FixFlushOverlap = false; }},
      {"FixGlobalSeqno off", [](Options& o) { o.fix.FixGlobalSeqno = false; }},
      {"FixLevelAbove off", [](Options& o) { o.fix.FixLevelAbove = false; }},
      {"FixBlockWrites off", [](Options& o) { o.fix.FixBlockWrites = false; }},
      {"FixAtomicMultiIngest off", [](Options& o) { o.fix.FixAtomicMultiIngest = false; }},
      {"FixPublishAfterInserts off", [](Options& o) { o.fix.FixPublishAfterInserts = false; }},
      {"FixDeferDropDelete off", [](Options& o) { o.fix.FixDeferDropDelete = false; }},
      {"FixKeepUnpublishedShadowed off", [](Options& o) { o.fix.FixKeepUnpublishedShadowed = false; }},
      {"FixSyncRecoveredLogs off", [](Options& o) { o.fix.FixSyncRecoveredLogs = false; o.avoid_flush_during_recovery = true; }},
      {"FixResumeAtRecoveryLog off", [](Options& o) { o.fix.FixResumeAtRecoveryLog = false; o.avoid_flush_during_recovery = true; }},
      {"FixScheduleFreeOnSeal off", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(250, nullptr, true);
         o.fix.FixScheduleFreeOnSeal = false; }},
      {"FixFreeAfterUnpin off", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(250, nullptr, true);
         o.fix.FixFreeAfterUnpin = false; }},
      {"FixEndStallOnFree off", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(250, nullptr, true);
         o.write_buffer_manager->fix.FixEndStallOnFree = false; }},
      {"FixEndStallOnResize off", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(250, nullptr, true);
         o.write_buffer_manager->fix.FixEndStallOnResize = false; }},
      {"FixCacheRoundUp off", [](Options& o) {
         o.write_buffer_manager = std::make_shared<WriteBufferManager>(
             250, std::make_shared<BlockCache>(100000, false), true, WriteBufferFlushPolicy::kFlushOldest, 64);
         o.write_buffer_manager->fix.FixCacheRoundUp = false; }},
      // compaction (LSMTree.tla)
      {"FixL0Overlap off", [](Options& o) { Leveled(o); o.fix.FixL0Overlap = false; }, true, true},
      {"FixCleanCut off", [](Options& o) {
         Leveled(o); o.compaction_split_user_keys = true; o.fix.FixCleanCut = false; }, true, true},
      {"FixOutputOverlap off", [](Options& o) { Leveled(o); o.fix.FixOutputOverlap = false; }, true, true},
      {"FixSkipBusy off", [](Options& o) { Leveled(o); o.fix.FixSkipBusy = false; }, true, true},
      {"FixSnapshotKeep off", [](Options& o) { Leveled(o); o.fix.FixSnapshotKeep = false; }, true, true},
      {"FixTombstoneBottom off", [](Options& o) { Leveled(o); o.fix.FixTombstoneBottom = false; }, true, true},
      {"FixAdjacentRuns off", [](Options& o) { Universal(o); o.fix.FixAdjacentRuns = false; }, true, true},
      {"FixIngestBehindKeep off", [](Options& o) {
         Leveled(o); o.allow_ingest_behind = true; o.fix.FixIngestBehindKeep = false; }, true, true},
      {"FixVersionRef off", [](Options& o) { Leveled(o); o.fix.FixVersionRef = false; }, true, true},
      {"FixIngestSkipCompacting off", [](Options& o) { Leveled(o); o.fix.FixIngestSkipCompacting = false; }, true, true},
      // ---- RocksDBStore.tla ----
      {"FixAtomicEdit off", [](Options& o) { Leveled(o); o.fix.FixAtomicEdit = false; }, true, true},
      {"FixSyncBeforeApply off", [](Options& o) { Leveled(o); o.fix.FixSyncBeforeApply = false; }, true, true},
      {"FixProtectOutputs off", [](Options& o) { Leveled(o); o.fix.FixProtectOutputs = false; }, true, true},
      {"FixRecordLastSeq off", [](Options& o) { Leveled(o); o.fix.FixRecordLastSeq = false; }, true, true},
      {"FixSyncWalBeforeIngest off", [](Options& o) { Leveled(o); o.fix.FixSyncWalBeforeIngest = false; }, true, true},
      {"FixStallNeedsCompaction off", [](Options& o) {
         Leveled(o); o.disable_auto_compactions = true; o.level0_file_num_compaction_trigger = 1;
         o.level0_slowdown_writes_trigger = 1; o.level0_stop_writes_trigger = 3;
         o.fix.FixStallNeedsCompaction = false; }, true, true},
      {"FixSanitizeTriggers off", [](Options& o) {
         Leveled(o); o.level0_file_num_compaction_trigger = 4; o.level0_slowdown_writes_trigger = 2;
         o.level0_stop_writes_trigger = 3; o.fix.FixSanitizeTriggers = false; }, true, true},
  };
  int seeds = std::getenv("SEEDS") ? std::atoi(std::getenv("SEEDS")) : 1000;
  int found = 0;
  for (auto& cfg : bugs) {
    std::string first;
    int at = 0, runs = 0;
    for (int s = 1; s <= seeds && first.empty(); s++) {
      runs = s;
      Config one = cfg;
      int bad = 0;
      (void)bad;
      Sim sim(s * 104729 + 7, true, cfg.compaction);
      sim.f.opts.inline_background = false;
      sim.f.opts.write_buffer_size = 120;
      sim.f.opts.max_total_wal_size = 500;
      sim.f.opts.create_missing_column_families = true;
      sim.f.names = {"default", "a"};
      one.set(sim.f.opts);
      sim.f.Open();
      for (int i = 0; i < 400 && sim.f.Clean(); i++) {
        sim.Step();
        if (!sim.f.Clean()) { first = sim.f.ghost.violations.front(); at = i + 1; }
      }
    }
    if (!first.empty()) found++;
    std::printf("    %-28s %s\n", cfg.name,
                first.empty() ? "not found" : ("run " + std::to_string(runs) + ", step " + std::to_string(at) + ": " + first).c_str());
  }
  std::printf("    found %d of %zu\n", found, bugs.size());
}

int main() { return testing::RunAll(); }
