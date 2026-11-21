// test_wbm.cc: the write buffer manager (WBM1 to WBM9, WBM.tla).
//
// Part 1 checks normal behavior: the accounting follows the memtable life cycle, the
// flush trigger and the victim policies, stalls, the cache reservation, and the two
// liveness gaps TLC found (a stalled writer that waits for a flush nobody runs).
// Part 2 has one test per Fix switch, as in test_bugs.cc: with the mechanism on no
// invariant fails; with it off the invariant TLC reported fails.
#include "harness.h"

using namespace lsm;

namespace {

// One entry costs |key| + |value| + 16 bytes (MT1).
std::string Val(size_t entryBytes, const std::string& key) { return std::string(entryBytes - 16 - key.size(), 'v'); }

// Puts one entry of exactly `bytes` bytes.
Status PutBytes(Fixture& f, CfId c, const std::string& k, size_t bytes) {
  return f.db->Put({}, c, k, Val(bytes, k));
}

// A write driven step by step, so that a test can see it wait.
struct Writer {
  Fixture& f;
  std::shared_ptr<WriteGroup> G;
  Writer(Fixture& f, CfId c, const std::string& k, size_t bytes, WriteOptions o = {}) : f(f) {
    WriteBatch b;
    b.Put(c, k, Val(bytes, k));
    Status s;
    G = f.db->NewGroup({{b, o}}, &s);
  }
  Status Admit() { return f.db->Admit(*G); }
  Status Finish() {
    Status s = f.db->AppendGroup(*G);
    if (s.ok()) s = f.db->InsertGroup(*G);
    if (s.ok()) s = f.db->PublishGroup(*G);
    return s;
  }
};

std::shared_ptr<WriteBufferManager> Wbm(size_t size, bool stall = false,
                                        WriteBufferFlushPolicy p = WriteBufferFlushPolicy::kFlushOldest,
                                        std::shared_ptr<BlockCache> cache = nullptr, size_t dummy = 100) {
  return std::make_shared<WriteBufferManager>(size, cache, stall, p, dummy);
}

void Big(Fixture& f) { f.opts.write_buffer_size = 100000; }  // no memtable-full flushes

}  // namespace

// ===================================================================== part 1

TEST(AccountingFollowsMemtableLifeCycle) {  // WBM1, WBM2
  Fixture f;
  Big(f);
  f.opts.write_buffer_manager = Wbm(10000);
  CHECK_OK(f.Open());
  auto& B = f.db->Wbm();
  CHECK_OK(PutBytes(f, f.cf[0], "a", 100));
  CHECK_OK(PutBytes(f, f.cf[0], "b", 50));
  CHECK(B.MemoryUsage() == 150 && B.MutableMemtableMemoryUsage() == 150);  // WBM1
  f.opts.inline_background = false;
  f.db->opts.inline_background = false;
  CHECK_OK(f.db->Flush(f.cf[0]));                                     // MT4 seals: ScheduleFreeMem
  CHECK(B.MemoryUsage() == 150 && B.MutableMemtableMemoryUsage() == 0);
  CHECK_OK(f.db->RunBackgroundWork());                                // MT5, MT6, destroyed: FreeMem
  CHECK(B.MemoryUsage() == 0);
  CHECK(f.Clean());
}

TEST(ReaderPinDelaysFreeMem) {  // WBM2: FreeMem waits for the last reader
  Fixture f;
  Big(f);
  f.opts.write_buffer_manager = Wbm(10000);
  CHECK_OK(f.Open());
  auto& B = f.db->Wbm();
  CHECK_OK(PutBytes(f, f.cf[0], "a", 200));
  auto pin = f.db->PinMemtables(f.cf[0]);  // an iterator holds the super version
  CHECK_OK(f.db->Flush(f.cf[0]));
  CHECK(!f.db->tree.Cf(f.cf[0])->l0.empty());
  CHECK(B.MemoryUsage() == 200 && B.MutableMemtableMemoryUsage() == 0);  // flushed, still held
  pin.reset();
  CHECK(B.MemoryUsage() == 0);
  CHECK(f.Clean());
}

TEST(FlushTriggerMutableOverSevenEighths) {  // WBM4, first rule
  Fixture f;
  Big(f);
  f.opts.write_buffer_manager = Wbm(800);  // mutable limit 700
  CHECK_OK(f.Open());
  for (int i = 0; i < 7; i++) CHECK_OK(PutBytes(f, f.cf[0], "k" + std::to_string(i), 100));
  CHECK(f.db->Wbm().wbmFlushes == 0 && f.db->tree.Cf(f.cf[0])->l0.empty());  // 700 is not over 700
  CHECK_OK(PutBytes(f, f.cf[0], "k7", 100));  // its WAL1 still sees 700
  CHECK(f.db->Wbm().wbmFlushes == 0);
  CHECK_OK(PutBytes(f, f.cf[0], "k8", 100));  // its WAL1 sees 800 > 700: flush
  CHECK(f.db->Wbm().wbmFlushes == 1 && f.db->tree.Cf(f.cf[0])->l0.size() == 1);
  CHECK(f.Clean());
}

TEST(FlushTriggerOverLimitOnlyIfHalfIsMutable) {  // WBM4, second rule
  Fixture f;
  Big(f);
  f.opts.write_buffer_manager = Wbm(800);  // half is 400
  CHECK_OK(f.Open());
  CHECK_OK(PutBytes(f, f.cf[0], "old", 500));
  auto pin = f.db->PinMemtables(f.cf[0]);
  CHECK_OK(f.db->Flush(f.cf[0]));  // 500 bytes flushed but held: used counts them
  for (int i = 0; i < 3; i++) CHECK_OK(PutBytes(f, f.cf[0], "k" + std::to_string(i), 100));
  auto& B = f.db->Wbm();
  CHECK(B.MemoryUsage() == 800 && B.MutableMemtableMemoryUsage() == 300);
  CHECK(!B.ShouldFlush());  // over the limit, but most memory is not mutable: a flush would not help
  CHECK_OK(PutBytes(f, f.cf[0], "k3", 100));
  CHECK(B.MemoryUsage() == 900 && B.MutableMemtableMemoryUsage() == 400);
  CHECK(B.ShouldFlush());   // over the limit and half is mutable
  CHECK(f.Clean());
}

TEST(VictimOldestLargestAndSkipPending) {  // WBM4 PickVictims
  for (auto p : {WriteBufferFlushPolicy::kFlushOldest, WriteBufferFlushPolicy::kFlushLargest}) {
    Fixture f;
    Big(f);
    f.opts.inline_background = false;
    f.opts.write_buffer_manager = Wbm(10000, false, p);
    f.names = {"default", "a", "b"};
    f.opts.create_missing_column_families = true;
    CHECK_OK(f.Open());
    // all three mutable memtables exist from open, in CF order; seal "default" so it is newest
    CHECK_OK(PutBytes(f, f.cf[0], "x", 30));
    CHECK_OK(f.db->Flush(f.cf[0]));
    CHECK_OK(PutBytes(f, f.cf[0], "x", 900));  // newest memtable, largest, but a flush is pending
    CHECK_OK(PutBytes(f, f.cf[1], "y", 50));   // created first
    CHECK_OK(PutBytes(f, f.cf[2], "z", 300));  // larger
    auto v = f.db->tree.PickWbmVictims();
    CfId want = p == WriteBufferFlushPolicy::kFlushOldest ? f.cf[1] : f.cf[2];
    CHECK(v == std::set<CfId>{want});
    CHECK_OK(f.db->RunBackgroundWork());       // the pending flush of default is done
    v = f.db->tree.PickWbmVictims();
    want = p == WriteBufferFlushPolicy::kFlushOldest ? f.cf[1] : f.cf[0];
    CHECK(v == std::set<CfId>{want});
    CHECK(f.Clean());
  }
}

TEST(StallBlocksUntilFreeMemEndsIt) {  // WBM5, WBM6, WBM7
  Fixture f;
  Big(f);
  f.opts.inline_background = false;
  f.opts.write_buffer_manager = Wbm(300, true);
  CHECK_OK(f.Open());
  auto& B = f.db->Wbm();
  for (int i = 0; i < 3; i++) CHECK_OK(PutBytes(f, f.cf[0], "k" + std::to_string(i), 117));
  CHECK(B.MemoryUsage() == 351);
  WriteOptions ns;
  ns.noSlowdown = true;
  Writer w0(f, f.cf[0], "n", 50, ns);
  CHECK(w0.Admit().IsIncomplete());  // no_slowdown writes fail instead of waiting
  f.db->AbandonGroup(*w0.G);
  Writer w(f, f.cf[0], "k3", 50);
  CHECK(w.Admit().IsBusy());         // WAL1 sealed the memtable (WBM4), then stalled (WBM5)
  CHECK(B.StallActive() && f.db->Stall()->state == WbmStall::kBlocked);
  CHECK(w.Admit().IsBusy());         // still waiting: only a signal releases it
  CHECK_OK(f.db->RunBackgroundWork());  // flush, install, destroy: FreeMem ends the stall (WBM7)
  CHECK(!B.StallActive() && f.db->Stall()->state == WbmStall::kRunning);
  CHECK_OK(w.Admit());
  CHECK_OK(w.Finish());
  CHECK(f.Get(f.cf[0], "k3").size() == 50 - 16 - 2);
  CHECK(f.Clean());
}

TEST(SetBufferSizeEndsStall) {  // WBM8
  Fixture f;
  Big(f);
  f.opts.inline_background = false;
  f.opts.write_buffer_manager = Wbm(300, true);
  CHECK_OK(f.Open());
  for (int i = 0; i < 3; i++) CHECK_OK(PutBytes(f, f.cf[0], "k" + std::to_string(i), 117));
  Writer w(f, f.cf[0], "k3", 50);
  CHECK(w.Admit().IsBusy());
  f.db->Wbm().SetBufferSize(1000);
  CHECK_OK(w.Admit());
  CHECK_OK(w.Finish());
  CHECK(f.Clean());
}

TEST(CacheReservationGrowsAtOnceShrinksLate) {  // WBM3
  Fixture f;
  Big(f);
  auto cache = std::make_shared<BlockCache>(100000, false);
  f.opts.write_buffer_manager = Wbm(0, false, WriteBufferFlushPolicy::kFlushOldest, cache, 100);  // charge only
  CHECK_OK(f.Open());
  auto& B = f.db->Wbm();
  CHECK_OK(PutBytes(f, f.cf[0], "a", 250));
  CHECK(B.MemoryUsage() == 250 && B.DummyEntriesInCacheUsage() == 300 && cache->GetUsage() == 300);
  auto pin = f.db->PinMemtables(f.cf[0]);
  CHECK_OK(f.db->Flush(f.cf[0]));
  CHECK_OK(PutBytes(f, f.cf[0], "b", 30));
  CHECK(B.MemoryUsage() == 280 && B.DummyEntriesInCacheUsage() == 300);
  pin.reset();  // 250 freed: 30 < 3/4 of 300, so shrink to 100
  CHECK(B.MemoryUsage() == 30 && B.DummyEntriesInCacheUsage() == 100);
  CHECK(f.Clean());
}

TEST(StrictCacheCanLeaveMemoryUncharged) {  // documented: RocksDB ignores the failed insert
  Fixture f;
  Big(f);
  auto cache = std::make_shared<BlockCache>(200, true);
  f.opts.write_buffer_manager = Wbm(0, false, WriteBufferFlushPolicy::kFlushOldest, cache, 100);
  CHECK_OK(f.Open());
  CHECK_OK(PutBytes(f, f.cf[0], "a", 250));  // the write succeeds; the third dummy entry does not fit
  CHECK(f.db->Wbm().DummyEntriesInCacheUsage() == 200);
  CHECK(f.Violated("BM4 CacheCovers"));
}

// The liveness gap of MC_TwoDBs_Oldest: DB2 holds the memory and has no writer,
// so nobody flushes it, and DB1's writer waits forever. With
// kFlushLargestAcrossDBs, B hands the flush to DB2 (WBM9).
static void TwoDbsIdlePeer(WriteBufferFlushPolicy p, bool* finished, uint64_t* handoffs) {
  auto B = Wbm(400, true, p);
  Fixture d1, d2;
  for (Fixture* x : {&d1, &d2}) {
    Big(*x);
    x->opts.write_buffer_manager = B;
    if (!x->Open().ok()) std::abort();
  }
  for (int i = 0; i < 4; i++) PutBytes(d2, d2.cf[0], "k" + std::to_string(i), 100);  // 400: then idle
  Status s = PutBytes(d1, d1.cf[0], "x", 50);  // DB1 has nothing to flush, and stalls
  if (!s.ok() && !s.IsBusy()) std::abort();    // the only way not to finish is to keep waiting
  *finished = s.ok();
  *handoffs = B->handoffs;
  if (!d1.Clean() || !d2.Clean()) { d1.PrintViolations(); d2.PrintViolations(); *finished = false; }
}

TEST(TwoDbsIdlePeerOldestStallsForever) {
  bool done;
  uint64_t h;
  TwoDbsIdlePeer(WriteBufferFlushPolicy::kFlushOldest, &done, &h);
  CHECK(!done);  // still Busy after every retry: the liveness failure TLC found
}

TEST(TwoDbsIdlePeerAcrossDBsHandsOff) {
  bool done;
  uint64_t h;
  TwoDbsIdlePeer(WriteBufferFlushPolicy::kFlushLargestAcrossDBs, &done, &h);
  CHECK(done && h == 1);
}

// The liveness gap of MC_BigMemtable_Oldest, in one DB: WAL1 flushes only the
// oldest memtable, and the larger one alone keeps memory over the limit.
static bool BigMemtable(WriteBufferFlushPolicy p) {
  Fixture f;
  Big(f);
  f.opts.write_buffer_manager = Wbm(400, true, p);
  f.names = {"default", "a"};
  f.opts.create_missing_column_families = true;
  if (!f.Open().ok()) std::abort();
  // limit 400, mutable limit 350
  PutBytes(f, f.cf[0], "small", 40);   // the oldest memtable
  PutBytes(f, f.cf[1], "big", 300);    // active 340
  PutBytes(f, f.cf[1], "big2", 100);   // its WAL1 saw 340, no flush; now "a" alone holds 400
  // WAL1 sees 440 > 350 and flushes the oldest memtable (40 bytes), then stalls
  // (used 440). Freeing the 40 bytes leaves used = 400, still at the limit, and
  // nothing else will flush "a".
  Status s = PutBytes(f, f.cf[1], "big3", 20);
  if (!s.ok() && !s.IsBusy()) std::abort();
  bool ok = s.ok() && f.Clean();
  if (!f.Clean()) f.PrintViolations();
  return ok;
}

TEST(BigMemtableOldestStallsForever) { CHECK(!BigMemtable(WriteBufferFlushPolicy::kFlushOldest)); }
TEST(BigMemtableAcrossDBsRetriesLocally) { CHECK(BigMemtable(WriteBufferFlushPolicy::kFlushLargestAcrossDBs)); }

// ===================================================================== part 2

// Runs `run` with a fresh WBM whose switch is on, then off. `off` turns it off.
static bool Both(const char* expect, std::function<void(Options&, WriteBufferManager&)> off,
                 std::function<void(Fixture&, Fixture&)> run, bool twoDbs = false) {
  for (int bug = 0; bug < 2; bug++) {
    auto B = Wbm(400, true, WriteBufferFlushPolicy::kFlushOldest,
                 std::make_shared<BlockCache>(100000, false), 100);
    Fixture a, b;
    for (Fixture* x : {&a, &b}) {
      Big(*x);
      x->opts.inline_background = false;
      x->opts.write_buffer_manager = B;
      if (bug) off(x->opts, *B);
    }
    if (!a.Open().ok() || (twoDbs && !b.Open().ok())) return false;
    run(a, b);
    bool violated = a.Violated(expect) || b.Violated(expect);
    if (!bug && (!a.Clean() || !b.Clean())) {
      std::printf("    with the mechanism ON, unexpected violations:\n");
      a.PrintViolations();
      b.PrintViolations();
      return false;
    }
    if (bug) {
      if (!violated) {
        std::printf("    with the mechanism OFF, expected %s but got:\n", expect);
        a.PrintViolations();
        b.PrintViolations();
        return false;
      }
      for (auto* x : {&a, &b})
        for (auto& v : x->ghost.violations)
          if (v.find(expect) != std::string::npos) { std::printf("    off -> %s\n", v.c_str()); return true; }
    }
  }
  return true;
}

TEST(Bug_NoScheduleFree) {  // MC_Bug_NoScheduleFree -> BM1
  CHECK(Both("BM1", [](Options& o, WriteBufferManager&) { o.fix.FixScheduleFreeOnSeal = false; },
             [](Fixture& f, Fixture&) {
               PutBytes(f, f.cf[0], "a", 100);
               f.db->Flush(f.cf[0]);  // sealed, but its bytes stay in active
             }));
}

TEST(Bug_FreeWhilePinned) {  // MC_Bug_FreeWhilePinned -> BM1
  CHECK(Both("BM1", [](Options& o, WriteBufferManager&) { o.fix.FixFreeAfterUnpin = false; },
             [](Fixture& f, Fixture&) {
               PutBytes(f, f.cf[0], "a", 100);
               auto pin = f.db->PinMemtables(f.cf[0]);
               f.db->Flush(f.cf[0]);
               f.db->RunBackgroundWork();  // installed while a reader holds it: freed too early
               f.db->Step("after install");
             }));
}

TEST(Bug_NoEndOnFree) {  // MC_Bug_NoEndOnFree -> BM2
  CHECK(Both("BM2", [](Options&, WriteBufferManager& B) { B.fix.FixEndStallOnFree = false; },
             [](Fixture& f, Fixture&) {
               for (int i = 0; i < 4; i++) PutBytes(f, f.cf[0], "k" + std::to_string(i), 117);
               Writer w(f, f.cf[0], "x", 50);
               w.Admit();                 // stalls
               f.db->RunBackgroundWork();  // memory goes down, nobody ends the stall
               f.db->Step("after free");
             }));
}

TEST(Bug_NoEndOnResize) {  // MC_Bug_NoEndOnResize -> BM2
  CHECK(Both("BM2", [](Options&, WriteBufferManager& B) { B.fix.FixEndStallOnResize = false; },
             [](Fixture& f, Fixture&) {
               for (int i = 0; i < 4; i++) PutBytes(f, f.cf[0], "k" + std::to_string(i), 117);
               Writer w(f, f.cf[0], "x", 50);
               w.Admit();
               f.db->Wbm().SetBufferSize(2000);  // the limit is now far above used
               f.db->Step("after SetBufferSize");
             }));
}

TEST(Bug_NoRecheck) {  // MC_Bug_NoRecheck -> BM2: memory is freed between WBM5 and WBM6
  CHECK(Both("BM2", [](Options&, WriteBufferManager& B) { B.fix.FixRecheckUnderLock = false; },
             [](Fixture& f, Fixture&) {
               for (int i = 0; i < 4; i++) PutBytes(f, f.cf[0], "k" + std::to_string(i), 117);
               auto pin = f.db->PinMemtables(f.cf[0]);
               f.db->Flush(f.cf[0]);
               f.db->RunBackgroundWork();       // flushed but held: used stays over the limit
               auto& B = f.db->Wbm();
               if (!B.ShouldStall()) std::abort();  // the writer's WBM5 test
               pin.reset();                     // another thread frees it; the stall was not on yet
               f.db->Stall()->state = WbmStall::kBlocked;
               B.BeginWriteStall(f.db->Stall());  // the writer's WBM6, a moment late
               f.db->Step("after BeginWriteStall");
               B.RemoveDBFromQueue(f.db->Stall());
             }));
}

TEST(Bug_WakeOne) {  // MC_Bug_WakeOne -> BM2: two DBs wait, only one is signaled
  CHECK(Both("BM2", [](Options&, WriteBufferManager& B) { B.fix.FixWakeAll = false; },
             [](Fixture& a, Fixture& b) {
               for (int i = 0; i < 4; i++) PutBytes(a, a.cf[0], "k" + std::to_string(i), 117);
               Writer wa(a, a.cf[0], "x", 50), wb(b, b.cf[0], "y", 50);
               wa.Admit();
               wb.Admit();                 // both DBs are queued
               a.db->RunBackgroundWork();  // a's flush frees memory: WBM7
               a.db->Step("after free");
               b.db->Step("after free");
             }, true));
}

TEST(Bug_CacheRoundDown) {  // MC_Bug_CacheRoundDown -> BM4
  CHECK(Both("BM4", [](Options&, WriteBufferManager& B) { B.fix.FixCacheRoundUp = false; },
             [](Fixture& f, Fixture&) {
               PutBytes(f, f.cf[0], "a", 250);
               auto pin = f.db->PinMemtables(f.cf[0]);
               f.db->Flush(f.cf[0]);
               f.db->RunBackgroundWork();
               PutBytes(f, f.cf[0], "b", 30);
               pin.reset();  // shrink: 30 bytes need one whole dummy entry, not none
               f.db->Step("after free");
             }));
}

int main() { return testing::RunAll(); }
