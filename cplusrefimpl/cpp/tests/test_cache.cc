// test_cache.cc: the block cache (BC1 to BC11, BlockCache.tla).
//
// Part 1 checks the cache alone: LRU order, pins, the strict limit, the
// high-priority pool, replacement and erase of pinned entries, shards, and
// SetCapacity. Part 2 checks the cache inside the store: reads fill it, compaction
// does not, obsolete files are uncached and unpinned, two stores share one cache,
// and the write buffer manager charges it. Part 3 has one test per Fix switch: with
// the mechanism on no invariant fails; with it off the invariant TLC reported
// fails. Part 4 is a random simulator of the cache, in the style of TLC.
#include <map>

#include "harness.h"

using namespace lsm;

namespace {

std::shared_ptr<const void> V(int i) { return std::make_shared<int>(i); }

bool Has(const std::vector<std::string>& v, const std::string& prefix) {
  for (auto& s : v)
    if (s.find(prefix) != std::string::npos) return true;
  return false;
}
void Print(const std::vector<std::string>& v) {
  for (auto& s : v) std::printf("      violation: %s\n", s.c_str());
}
bool Clean(const BlockCache& c) {
  auto v = c.CheckInvariants();
  Print(v);
  return v.empty();
}
bool Cached(BlockCache& c, const std::string& k) {
  auto* h = c.Lookup(k);
  if (h) c.Release(h);
  return h != nullptr;
}

// A one-shard cache of `cap` units, every entry of charge 1.
std::shared_ptr<BlockCache> Small(size_t cap, double ratio = 0.0, bool strict = false) {
  return std::make_shared<BlockCache>(cap, strict, 0, ratio);
}

// A store whose files are small: four entries per data block.
void Setup(Fixture& f, std::shared_ptr<BlockCache> cache) {
  f.opts.write_buffer_size = 100000;
  f.opts.table_options.block_cache = std::move(cache);
  f.opts.table_options.block_entries = 2;
}
Status PutKeys(Fixture& f, const std::string& prefix, int n, const std::string& v = "v") {
  for (int i = 0; i < n; i++) {
    Status s = f.db->Put({}, f.cf[0], prefix + std::to_string(i), v + std::to_string(i));
    if (!s.ok()) return s;
  }
  return f.db->Flush(f.cf[0]);
}

}  // namespace

// ===================================================================== part 1: the cache

TEST(DefaultShardsAreAtLeast512KB) {  // BC1
  CHECK(BlockCache::DefaultShardBits(100 << 10) == 0);
  CHECK(BlockCache::DefaultShardBits(1 << 20) == 1);
  CHECK(BlockCache::DefaultShardBits(8 << 20) == 4);
  CHECK(BlockCache::DefaultShardBits(32 << 20) == 6);
  CHECK(BlockCache::DefaultShardBits(1ull << 34) == 6);  // at most 64 shards
  BlockCache c(1 << 20);
  CHECK(c.NumShards() == 2 && c.Shard(0).Capacity() == 512 << 10);
}

TEST(EvictsOldestUnpinnedFirst) {  // BC2, BC3, BC7
  auto c = Small(3);
  for (auto k : {"a", "b", "c"}) CHECK_OK(c->Insert(k, V(0), 1));
  CHECK(Cached(*c, "a"));  // a is now the newest
  CHECK_OK(c->Insert("d", V(0), 1));
  CHECK(!Cached(*c, "b") && Cached(*c, "a") && Cached(*c, "c") && Cached(*c, "d"));
  CHECK(c->GetUsage() == 3 && c->Stats().evictions == 1);
  CHECK(Clean(*c));
}

TEST(PinnedBlocksAreNeverEvicted) {  // CH2, CH3
  auto c = Small(2);
  BlockCache::Handle *ha, *hb, *hc;
  CHECK_OK(c->Insert("a", V(0), 1, CachePriority::kLow, &ha));
  CHECK_OK(c->Insert("b", V(0), 1, CachePriority::kLow, &hb));
  CHECK_OK(c->Insert("c", V(0), 1, CachePriority::kLow, &hc));  // not strict: goes over capacity
  CHECK(c->GetUsage() == 3 && c->GetPinnedUsage() == 3);
  CHECK(Clean(*c));
  c->Release(hc);  // over capacity: the last release erases it
  CHECK(!Cached(*c, "c") && c->GetUsage() == 2);
  c->Release(ha);
  c->Release(hb);
  CHECK(Cached(*c, "a") && Cached(*c, "b") && c->GetPinnedUsage() == 0);
  CHECK(Clean(*c));
}

TEST(StrictLimitRefusesPinnedInsert) {  // BC3, CH3
  auto c = Small(2, 0.0, true);
  BlockCache::Handle *ha, *hb, *hc = nullptr;
  CHECK_OK(c->Insert("a", V(0), 1, CachePriority::kLow, &ha));
  CHECK_OK(c->Insert("b", V(0), 1, CachePriority::kLow, &hb));
  Status s = c->Insert("c", V(0), 1, CachePriority::kLow, &hc);
  CHECK(s.IsIncomplete() && hc == nullptr);
  CHECK_OK(c->Insert("d", V(0), 1));  // no handle: "succeeds", and is dropped at once
  CHECK(!Cached(*c, "d") && c->GetUsage() == 2 && c->Stats().addFailures == 2);
  c->Release(ha);
  c->Release(hb);
  CHECK(Clean(*c));
}

TEST(HighPriorityPoolSpillsAndIsEvictedLast) {  // BC6, CH7
  auto c = Small(4, 0.5);  // pool of 2
  CHECK_OK(c->Insert("i1", V(0), 1, CachePriority::kHigh));
  CHECK_OK(c->Insert("i2", V(0), 1, CachePriority::kHigh));
  CHECK_OK(c->Insert("d1", V(0), 1));
  CHECK(c->GetHighPriPoolUsage() == 2);
  CHECK_OK(c->Insert("i3", V(0), 1, CachePriority::kHigh));  // the pool spills i1 to the low part
  CHECK(c->GetHighPriPoolUsage() == 2);
  CHECK_OK(c->Insert("d2", V(0), 1));  // evicts d1, the oldest low-priority entry
  CHECK(c->GetEntries() == 4);
  BlockCache::Handle* h = c->Lookup("d1");
  CHECK(h == nullptr);
  CHECK_OK(c->Insert("d3", V(0), 1));  // evicts i1: it is low priority now, and older than d2
  CHECK(!Cached(*c, "i1"));
  CHECK(Clean(*c));
}

TEST(HitBlockMovesToHighPool) {  // BC6: a looked-up block is released into the pool
  auto c = Small(4, 0.5);
  CHECK_OK(c->Insert("d1", V(0), 1));
  CHECK(c->GetHighPriPoolUsage() == 0);
  CHECK(Cached(*c, "d1"));
  CHECK(c->GetHighPriPoolUsage() == 1);
  CHECK(Clean(*c));
}

TEST(ReplacedPinnedEntryIsDetached) {  // BC3: two readers miss on one block and both insert
  auto c = Small(4);
  BlockCache::Handle *h1, *h2;
  CHECK_OK(c->Insert("k", V(1), 1, CachePriority::kLow, &h1));
  CHECK_OK(c->Insert("k", V(2), 1, CachePriority::kLow, &h2));
  CHECK(c->GetUsage() == 2 && c->GetEntries() == 1);
  CHECK(*static_cast<const int*>(BlockCache::Value(h1)) == 1);  // still valid for its holder
  CHECK(Clean(*c));
  c->Release(h1);
  CHECK(c->GetUsage() == 1);
  c->Release(h2);
  CHECK(Clean(*c));
}

TEST(ErasePinnedEntryFreesItAtRelease) {  // BC5
  auto c = Small(4);
  BlockCache::Handle* h;
  CHECK_OK(c->Insert("k", V(1), 1, CachePriority::kLow, &h));
  c->Erase("k");
  CHECK(c->GetEntries() == 0 && c->GetUsage() == 1 && !Cached(*c, "k"));
  c->Release(h);
  CHECK(c->GetUsage() == 0);
  CHECK(Clean(*c));
}

TEST(ShardsDoNotShareCapacity) {  // BC1: one shard can refuse while another has room
  BlockCache c(4, true, 1, 0.0);  // two shards of 2
  std::map<int, std::vector<std::string>> byShard;
  for (int i = 0; byShard[0].size() < 3 || byShard[1].size() < 1; i++) {
    std::string k = "k" + std::to_string(i);
    byShard[c.ShardOf(k)].push_back(k);
  }
  std::vector<BlockCache::Handle*> hs(2);
  CHECK_OK(c.Insert(byShard[0][0], V(0), 1, CachePriority::kLow, &hs[0]));
  CHECK_OK(c.Insert(byShard[0][1], V(0), 1, CachePriority::kLow, &hs[1]));
  BlockCache::Handle* h = nullptr;
  CHECK(c.Insert(byShard[0][2], V(0), 1, CachePriority::kLow, &h).IsIncomplete());
  CHECK_OK(c.Insert(byShard[1][0], V(0), 1, CachePriority::kLow, &h));  // the other shard is empty
  c.Release(h);
  for (auto* x : hs) c.Release(x);
  CHECK(Clean(c));
}

TEST(SetCapacityEvictsButDoesNotTrimPool) {  // BC8; documented: MC_ResizePool
  auto c = Small(4, 0.5);
  CHECK_OK(c->Insert("i1", V(0), 1, CachePriority::kHigh));
  CHECK_OK(c->Insert("i2", V(0), 1, CachePriority::kHigh));
  CHECK_OK(c->Insert("d1", V(0), 1));
  c->SetCapacity(2);  // evicts d1; the pool keeps 2 > 2 * 0.5
  CHECK(c->GetUsage() == 2 && !Cached(*c, "d1"));
  CHECK(Has(c->CheckInvariants(), "CH7 PoolBound"));
  CHECK_OK(c->Insert("i3", V(0), 1, CachePriority::kHigh));  // the next pool insert trims it
  CHECK(Clean(*c));
}

TEST(CompactionReleaseOverCapacityEvicts) {  // documented: MC_CompactionEvicts
  auto c = Small(2);
  CHECK_OK(c->Insert("hot", V(0), 1));
  BlockCache::Handle* comp = c->Lookup("hot");  // a compaction scan hits the user's hot block
  BlockCache::Handle *u1, *u2;
  CHECK_OK(c->Insert("u1", V(0), 1, CachePriority::kLow, &u1));  // user iterators pin two blocks
  CHECK_OK(c->Insert("u2", V(0), 1, CachePriority::kLow, &u2));
  c->Release(comp);  // over capacity: the hot block is erased
  CHECK(!Cached(*c, "hot"));
  c->Release(u1);
  c->Release(u2);
  CHECK(Clean(*c));
}

// ===================================================================== part 2: in the store

TEST(GetFillsCacheThenHits) {  // BC9
  auto cache = std::make_shared<BlockCache>(1 << 20);
  Fixture f;
  Setup(f, cache);
  CHECK_OK(f.Open());
  CHECK_OK(PutKeys(f, "k", 6));  // one L0 file, three data blocks
  CHECK(f.Get(f.cf[0], "k3") == "v3");
  auto& st = f.db->tree.tableStats;
  CHECK(st.dataMiss == 1 && st.dataHit == 0);
  CHECK(f.Get(f.cf[0], "k2") == "v2");  // same block
  CHECK(st.dataHit == 1);
  CHECK(f.Get(f.cf[0], "k2x") == "<none>" && st.filterUseful == 1);  // in range, but the filter says no
  CHECK(cache->GetPinnedUsage() == 0);
  CHECK(f.Clean());
}

TEST(CompactionDoesNotFillCache) {  // K10, CH5
  auto cache = std::make_shared<BlockCache>(1 << 20);
  Fixture f;
  Setup(f, cache);
  CHECK_OK(f.Open());
  CHECK_OK(PutKeys(f, "a", 6));
  CHECK_OK(PutKeys(f, "b", 6));
  CHECK(f.Get(f.cf[0], "a0") == "v0");  // one data block of the first file is cached
  size_t before = cache->GetEntries();
  uint64_t hits = cache->Stats().hits;
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(cache->GetEntries() == before);       // no new blocks
  CHECK(cache->Stats().hits == hits + 1);    // the cached block was used
  CHECK(cache->GetPinnedUsage() == 0);
  CHECK(f.Get(f.cf[0], "b5") == "v5");
  CHECK(f.Clean());
}

TEST(StrictCacheFailsReadsButNotCompaction) {  // CH3, K10
  auto cache = Small(300, 0.0, true);
  Fixture f;
  Setup(f, cache);
  CHECK_OK(f.Open());
  CHECK_OK(PutKeys(f, "a", 4));
  CHECK_OK(PutKeys(f, "b", 4));
  BlockCache::Handle* wall;  // someone pins the whole cache
  CHECK_OK(cache->Insert("wall", V(0), 300, CachePriority::kLow, &wall));
  std::string v;
  CHECK(f.db->Get(f.cf[0], "a1", &v).IsIncomplete());
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));  // fill_cache = false: no insert, no failure
  cache->Release(wall);
  CHECK(f.Get(f.cf[0], "a1") == "v1");
  CHECK(f.Clean());
}

TEST(UncacheErasesBlocksOfObsoleteFiles) {  // BC11
  for (int agg : {0, 1}) {
    auto cache = std::make_shared<BlockCache>(1 << 20);
    Fixture f;
    Setup(f, cache);
    f.opts.uncache_aggressiveness = agg;
    CHECK_OK(f.Open());
    CHECK_OK(PutKeys(f, "a", 4));
    CHECK_OK(PutKeys(f, "b", 4));
    for (auto k : {"a0", "a3", "b0", "b3"}) f.Get(f.cf[0], k);
    CHECK(cache->GetEntries() == 4);
    CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
    CHECK(cache->GetEntries() == (agg ? 0u : 4u));  // without uncache they wait for eviction
    CHECK(f.Clean());
  }
}

TEST(PinnedL0IndexReleasedWhenFileIsObsolete) {  // K9, CH6
  auto cache = std::make_shared<BlockCache>(1 << 20);
  Fixture f;
  Setup(f, cache);
  f.opts.table_options.cache_index_and_filter_blocks = true;
  f.opts.table_options.pin_l0_filter_and_index_blocks_in_cache = true;
  CHECK_OK(f.Open());
  CHECK_OK(PutKeys(f, "a", 4));
  CHECK_OK(PutKeys(f, "b", 4));
  f.Get(f.cf[0], "a1");
  f.Get(f.cf[0], "b1");
  CHECK(cache->GetPinnedUsage() > 0);              // index and filter of both L0 files
  CHECK(cache->GetHighPriPoolUsage() == 0);        // pinned, so not on the LRU list
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(cache->GetPinnedUsage() == 0);             // released with the readers
  CHECK(f.Get(f.cf[0], "b1") == "v1");
  CHECK(f.Clean());
}

TEST(PrepopulateWarmsFlushOutputOnly) {  // BC11
  auto cache = std::make_shared<BlockCache>(1 << 20);
  Fixture f;
  Setup(f, cache);
  f.opts.table_options.prepopulate_block_cache = PrepopulateBlockCache::kFlushOnly;
  CHECK_OK(f.Open());
  CHECK_OK(PutKeys(f, "a", 4));
  CHECK(cache->GetEntries() == 2);
  CHECK_OK(PutKeys(f, "b", 4));
  CHECK_OK(f.db->CompactLevel(f.cf[0], 0));
  CHECK(cache->GetEntries() == 4);  // the compaction output is not added
  CHECK(f.Get(f.cf[0], "a1") == "v1");
  CHECK(f.Clean());
}

TEST(TwoStoresShareOneCache) {  // BC10, CH4
  auto cache = std::make_shared<BlockCache>(1 << 20);
  Fixture f1, f2;
  Setup(f1, cache);
  Setup(f2, cache);
  CHECK_OK(f1.Open());
  CHECK_OK(f2.Open());
  CHECK_OK(PutKeys(f1, "k", 4, "one"));  // both stores name their first SST the same
  CHECK_OK(PutKeys(f2, "k", 4, "two"));
  CHECK(f1.Get(f1.cf[0], "k1") == "one1");
  CHECK(f2.Get(f2.cf[0], "k1") == "two1");
  CHECK(cache->GetEntries() == 2);
  CHECK(f1.Clean() && f2.Clean());
}

TEST(WbmChargesTheSameCache) {  // WBM3 through BC3 and BC4
  auto cache = Small(2000, 0.0);
  Fixture f;
  Setup(f, cache);
  f.opts.write_buffer_manager = std::make_shared<WriteBufferManager>(0, cache, false,
                                                                     WriteBufferFlushPolicy::kFlushOldest, 500);
  CHECK_OK(f.Open());
  CHECK_OK(PutKeys(f, "a", 8));
  for (int i = 0; i < 8; i++) f.Get(f.cf[0], "a" + std::to_string(i));
  size_t blocks = cache->GetEntries();
  CHECK(blocks >= 4);
  // a big memtable: the dummy entries are pinned and push data blocks out
  for (int i = 0; i < 30; i++) CHECK_OK(f.db->Put({}, f.cf[0], "m" + std::to_string(i), std::string(40, 'x')));
  CHECK(f.db->Wbm().DummyEntriesInCacheUsage() >= 1500);
  CHECK(cache->GetEntries() < blocks + 3);
  CHECK(cache->GetUsage() <= 2000 + 500);
  CHECK(f.Clean());
}

// ===================================================================== part 3: Fix switches

namespace {
// Runs `scenario` with the switch on and with it off. Passes if on gives no
// violation and off gives one that starts with `inv`.
bool CacheBoth(const char* inv, std::function<void(CacheFixes&)> off,
               std::function<std::vector<std::string>(std::shared_ptr<BlockCache>)> scenario,
               std::function<std::shared_ptr<BlockCache>()> make) {
  auto on = make();
  auto v1 = scenario(on);
  auto bad = make();
  off(bad->fix);
  auto v2 = scenario(bad);
  if (!v1.empty()) { std::printf("    on:\n"); Print(v1); return false; }
  for (auto& s : v2)
    if (s.rfind(inv, 0) == 0) { std::printf("    off -> %s\n", s.c_str()); return true; }
  std::printf("    off: no %s\n", inv);
  Print(v2);
  return false;
}
std::vector<std::string> All(const BlockCache& c, std::vector<std::string> acc = {}) {
  for (auto& s : c.CheckInvariants()) acc.push_back(s);
  return acc;
}
// A scenario in the store. Runs with Options::fix changed by `off` (or not).
bool StoreBoth(const char* inv, std::function<void(Options&)> off,
               std::function<void(std::vector<Fixture*>&)> scenario, int stores = 1) {
  bool ok[2];
  std::vector<std::string> got[2];
  for (int bug = 0; bug < 2; bug++) {
    auto cache = std::make_shared<BlockCache>(1 << 20);
    std::vector<std::unique_ptr<Fixture>> fs;
    std::vector<Fixture*> ps;
    for (int i = 0; i < stores; i++) {
      fs.push_back(std::make_unique<Fixture>());
      Setup(*fs.back(), cache);
      if (bug) off(fs.back()->opts);
      if (!fs.back()->Open().ok()) return false;
      ps.push_back(fs.back().get());
    }
    scenario(ps);
    for (auto* p : ps) {
      p->db->Step("end");
      for (auto& v : p->ghost.violations) got[bug].push_back(v);
    }
    ok[bug] = got[bug].empty();
  }
  if (!ok[0]) { std::printf("    on:\n"); Print(got[0]); return false; }
  for (auto& s : got[1])
    if (s.find(inv) != std::string::npos) { std::printf("    off -> %s\n", s.c_str()); return true; }
  std::printf("    off: no %s\n", inv);
  Print(got[1]);
  return false;
}
}  // namespace

TEST(Bug_KeyReuse) {  // MC_Bug_KeyReuse -> CH4
  CHECK(StoreBoth("CH4 NoStaleBlock", [](Options& o) { o.fix.FixUniqueKey = false; },
                  [](std::vector<Fixture*>& f) {
                    PutKeys(*f[0], "k", 4, "one");
                    PutKeys(*f[1], "k", 4, "two");
                    f[0]->Get(f[0]->cf[0], "k1");
                    f[1]->Get(f[1]->cf[0], "k1");  // with the bug: "one1"
                  }, 2));
}

TEST(Bug_LookupKeepsOnLRU) {  // MC_Bug_LookupKeepsOnLRU -> CH2
  CHECK(CacheBoth("CH2 PinSafety", [](CacheFixes& x) { x.FixLookupUnpins = false; },
                  [](std::shared_ptr<BlockCache> c) {
                    c->Insert("a", V(0), 1);
                    auto* h = c->Lookup("a");  // a compaction iterator pins a
                    c->Insert("b", V(0), 1);
                    c->Insert("c", V(0), 1);   // eviction takes the oldest list entry
                    auto v = All(*c);
                    c->Release(h);
                    // CH1 also fails at once (a pinned entry on the list); CH2 is the harm
                    std::vector<std::string> out;
                    for (auto& s : v) if (s.rfind("CH1", 0) != 0) out.push_back(s);
                    return out;
                  }, [] { return Small(2); }));
}

TEST(Bug_FreeReplaced) {  // MC_Bug_FreeReplaced -> CH2
  CHECK(CacheBoth("CH2 PinSafety", [](CacheFixes& x) { x.FixDetachOnReplace = false; },
                  [](std::shared_ptr<BlockCache> c) {
                    BlockCache::Handle *h1, *h2;
                    c->Insert("k", V(1), 1, CachePriority::kLow, &h1);
                    c->Insert("k", V(2), 1, CachePriority::kLow, &h2);
                    auto v = All(*c);
                    c->Release(h1);
                    c->Release(h2);
                    return All(*c, v);
                  }, [] { return Small(4); }));
}

TEST(Bug_KeepOverCap) {  // MC_Bug_KeepOverCap -> CH3
  CHECK(CacheBoth("CH3 CapacityBound", [](CacheFixes& x) { x.FixEraseOverCap = false; },
                  [](std::shared_ptr<BlockCache> c) {
                    BlockCache::Handle *a, *b, *d;
                    c->Insert("a", V(0), 1, CachePriority::kLow, &a);
                    c->Insert("b", V(0), 1, CachePriority::kLow, &b);
                    c->Insert("d", V(0), 1, CachePriority::kLow, &d);
                    c->Release(d);
                    auto v = All(*c);
                    c->Release(a);
                    c->Release(b);
                    return v;
                  }, [] { return Small(2); }));
}

TEST(Bug_StrictAdmits) {  // MC_Bug_StrictAdmits -> CH3
  CHECK(CacheBoth("CH3 CapacityBound", [](CacheFixes& x) { x.FixStrictRefuse = false; },
                  [](std::shared_ptr<BlockCache> c) {
                    BlockCache::Handle *a, *b, *d = nullptr;
                    c->Insert("a", V(0), 1, CachePriority::kLow, &a);
                    c->Insert("b", V(0), 1, CachePriority::kLow, &b);
                    c->Insert("d", V(0), 1, CachePriority::kLow, &d);
                    auto v = All(*c);
                    c->Release(a);
                    c->Release(b);
                    if (d) c->Release(d);
                    return v;
                  }, [] { return Small(2, 0.0, true); }));
}

TEST(Bug_NoPoolSpill) {  // MC_Bug_NoPoolSpill -> CH7
  CHECK(CacheBoth("CH7 PoolBound", [](CacheFixes& x) { x.FixMaintainPool = false; },
                  [](std::shared_ptr<BlockCache> c) {
                    for (auto k : {"i1", "i2", "i3"}) c->Insert(k, V(0), 1, CachePriority::kHigh);
                    return All(*c);
                  }, [] { return Small(4, 0.5); }));
}

TEST(Bug_CompactionFills) {  // MC_Bug_CompactionFills -> CH5
  CHECK(StoreBoth("CH5 CompactionNoFill", [](Options& o) { o.fix.FixCompactionNoFill = false; },
                  [](std::vector<Fixture*>& f) {
                    PutKeys(*f[0], "a", 4);
                    PutKeys(*f[0], "b", 4);
                    f[0]->db->CompactLevel(f[0]->cf[0], 0);
                  }));
}

TEST(Bug_LeakPin) {  // MC_Bug_LeakPin -> CH6
  CHECK(StoreBoth("CH6 NoLeakedPin", [](Options& o) { o.fix.FixReleaseOnClose = false; },
                  [](std::vector<Fixture*>& f) {
                    PutKeys(*f[0], "a", 4);
                    f[0]->Get(f[0]->cf[0], "a1");
                  }));
}

// ===================================================================== part 4: random

namespace {
// Random operations on one small cache by a few holders, checking every invariant
// after every operation. Returns the first violation, or "".
std::string RandomRun(uint64_t seed, std::function<void(CacheFixes&)> tweak, bool strict, int steps) {
  Rng rng(seed);
  BlockCache c(6, strict, rng.Uniform(2), 0.5);
  tweak(c.fix);
  std::vector<std::pair<BlockCache::Handle*, bool>> held;  // handle, released by Forget
  for (int i = 0; i < steps; i++) {
    std::string k = "k" + std::to_string(rng.Uniform(5));
    switch (rng.Uniform(7)) {
      case 0: case 1: {  // a reader looks a block up, and on a miss reads and inserts it
        BlockCache::Handle* h = c.Lookup(k);
        if (!h) {
          CachePriority p = rng.Uniform(3) == 0 ? CachePriority::kHigh : CachePriority::kLow;
          if (!c.Insert(k, V(0), 1 + rng.Uniform(2), p, &h).ok()) h = nullptr;
        }
        if (h) held.push_back({h, false});
        break;
      }
      case 2: c.Insert(k, V(0), 1 + rng.Uniform(2), CachePriority::kLow); break;  // prepopulate
      case 3: case 4:
        if (!held.empty()) {
          size_t j = rng.Uniform(held.size());
          c.Release(held[j].first, rng.Uniform(4) == 0);
          held.erase(held.begin() + j);
        }
        break;
      case 5: c.Erase(k); break;  // uncache an obsolete file's block
      case 6:
        if (rng.Uniform(4) == 0) c.SetCapacity(4 + rng.Uniform(5));
        break;
    }
    for (auto& v : c.CheckInvariants())
      if (v.rfind("CH7", 0) != 0 || !c.fix.FixMaintainPool) {  // CH7 can fail after SetCapacity
        for (auto& [h, f] : held) c.Release(h);
        return v;
      }
  }
  for (auto& [h, f] : held) c.Release(h);
  return "";
}
}  // namespace

TEST(RandomOperationsKeepInvariants) {
  for (uint64_t seed = 1; seed <= 300; seed++)
    for (bool strict : {false, true}) {
      std::string v = RandomRun(seed, [](CacheFixes&) {}, strict, 400);
      if (!v.empty()) std::printf("    seed %llu: %s\n", (unsigned long long)seed, v.c_str());
      CHECK(v.empty());
    }
}

TEST(RandomOperationsFindEachBug) {
  struct B { const char* name; std::function<void(CacheFixes&)> off; bool strict; const char* inv; };
  std::vector<B> bugs = {
      {"LookupKeepsOnLRU", [](CacheFixes& x) { x.FixLookupUnpins = false; }, false, "CH"},
      {"FreeReplaced", [](CacheFixes& x) { x.FixDetachOnReplace = false; }, false, "CH2"},
      {"KeepOverCap", [](CacheFixes& x) { x.FixEraseOverCap = false; }, false, "CH3"},
      {"StrictAdmits", [](CacheFixes& x) { x.FixStrictRefuse = false; }, true, "CH3"},
      {"NoPoolSpill", [](CacheFixes& x) { x.FixMaintainPool = false; }, false, "CH7"},
  };
  int found = 0;
  for (auto& b : bugs) {
    std::string v;
    for (uint64_t seed = 1; seed <= 200 && v.empty(); seed++) v = RandomRun(seed, b.off, b.strict, 400);
    bool ok = v.rfind(b.inv, 0) == 0;
    std::printf("    %-18s %s\n", b.name, ok ? v.c_str() : "not found");
    found += ok;
  }
  std::printf("    found %d of %zu\n", found, bugs.size());
  CHECK(found == static_cast<int>(bugs.size()));
}

int main() { return testing::RunAll(); }
