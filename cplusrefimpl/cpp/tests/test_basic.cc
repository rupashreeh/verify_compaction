// test_basic.cc: the normal paths, with every invariant checked after every step.
#include "harness.h"

using namespace lsm;

TEST(PutGetDelete) {
  Fixture f;
  CHECK_OK(f.Open());
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));
  CHECK_OK(f.db->Put({}, f.cf[0], "b", "2"));
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "3"));
  CHECK(f.Get(f.cf[0], "a") == "3");
  CHECK_OK(f.db->Delete({}, f.cf[0], "a"));
  CHECK(f.Get(f.cf[0], "a") == "<none>");
  CHECK(f.Get(f.cf[0], "b") == "2");
  CHECK(f.Clean());
}

TEST(FlushMovesDataToL0AndAdvancesLogNumber) {
  Fixture f;
  CHECK_OK(f.Open());
  auto& T = f.db->tree;
  CHECK_OK(f.db->Put({}, f.cf[0], "a", "1"));
  LogNum before = T.LogNumber(f.cf[0]);
  CHECK_OK(f.db->Flush(f.cf[0]));  // WAL6b -> WAL6a (rotates the log) -> MT5 -> MT6
  CHECK(T.Cf(f.cf[0])->l0.size() == 1);
  CHECK(T.Cf(f.cf[0])->imm.empty());
  CHECK(T.LogNumber(f.cf[0]) == before + 1);  // K2: N_c moved past the flushed memtable
  CHECK(f.db->wal.ActiveNum() == before + 1);
  CHECK(f.Get(f.cf[0], "a") == "1");
  CHECK(f.db->wal.L.size() == 1);  // the old log became obsolete and was deleted (WAL8a, WAL8b)
  CHECK(f.Clean());
}

TEST(MemtableFullTriggersFlushAtNextAdmit) {
  Fixture f;
  f.opts.write_buffer_size = 100;
  CHECK_OK(f.Open());
  for (int i = 0; i < 20; i++) CHECK_OK(f.db->Put({}, f.cf[0], "k" + std::to_string(i), std::string(10, 'x')));
  CHECK(!f.db->tree.Cf(f.cf[0])->l0.empty());
  for (int i = 0; i < 20; i++) CHECK(f.Get(f.cf[0], "k" + std::to_string(i)) == std::string(10, 'x'));
  CHECK(f.Clean());
}

TEST(ReadPathNewestFirst) {
  // MT7: mem, then imm newest first, then L0 newest first, then L1...
  Fixture f;
  f.opts.inline_background = false;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "k", "v1"));
  CHECK_OK(f.db->Flush(c));
  CHECK_OK(f.db->RunBackgroundWork());  // v1 in L0
  CHECK_OK(f.db->Put({}, c, "k", "v2"));
  CHECK_OK(f.db->Flush(c));             // v2 sealed, not flushed
  CHECK_OK(f.db->Put({}, c, "k", "v3"));  // v3 in the mutable memtable
  CHECK(f.Get(c, "k") == "v3");
  CHECK_OK(f.db->Delete({}, c, "k"));
  CHECK(f.Get(c, "k") == "<none>");
  CHECK_OK(f.db->RunBackgroundWork());
  CHECK_OK(f.db->CompactLevel(c, 0));
  CHECK(f.Get(c, "k") == "<none>");
  CHECK(f.Clean());
}

TEST(SnapshotsSeeTheirPointInTime) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "k", "old"));
  const Snapshot* s = f.db->GetSnapshot();
  CHECK_OK(f.db->Put({}, c, "k", "new"));
  CHECK_OK(f.db->Flush(c));  // MT5 keeps "old" because the snapshot can see it
  CHECK(f.Get(c, "k", s) == "old");
  CHECK(f.Get(c, "k") == "new");
  f.db->ReleaseSnapshot(s);
  CHECK(f.Clean());
}

TEST(ColumnFamiliesShareTheLog) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c1;
  CHECK_OK(f.db->CreateColumnFamily("one", &c1));  // CF1: N_c = active log
  CHECK(f.db->tree.LogNumber(c1) == f.db->wal.ActiveNum());
  WriteBatch b;
  b.Put(f.cf[0], "k", "default");
  b.Put(c1, "k", "one");
  CHECK_OK(f.db->Write({}, b));  // one batch over two CFs, one log record
  CHECK(f.Get(f.cf[0], "k") == "default");
  CHECK(f.Get(c1, "k") == "one");
  LogNum a = f.db->wal.ActiveNum();
  CHECK_OK(f.db->Flush(c1));  // any freeze starts a new log for everyone
  CHECK(f.db->wal.ActiveNum() == a + 1);
  // the default CF still needs the old log, so it stays live
  CHECK(f.db->wal.MinLogToKeep() == a);
  CHECK(f.Clean());
}

TEST(WalPressureFlushesTheCfHoldingTheOldestLog) {
  Fixture f;
  f.opts.max_total_wal_size = 300;
  CHECK_OK(f.Open());
  CfId hot, cold;
  CHECK_OK(f.db->CreateColumnFamily("hot", &hot));
  CHECK_OK(f.db->CreateColumnFamily("cold", &cold));
  CHECK_OK(f.db->Put({}, cold, "c", "rarely written"));
  for (int i = 0; i < 30; i++) {
    CHECK_OK(f.db->Put({}, hot, "h" + std::to_string(i), std::string(20, 'h')));
    if (i % 5 == 4) CHECK_OK(f.db->Flush(hot));
  }
  // WAL5 must have forced the cold CF's tiny memtable out to L0
  CHECK(!f.db->tree.Cf(cold)->l0.empty());
  CHECK(f.db->wal.AccountedWALBytes() <= 2 * f.db->wal.EffectiveWALLimit());
  CHECK(f.Get(cold, "c") == "rarely written");
  CHECK(f.Clean());
}

TEST(WriteBufferManagerFlushesOldestMemtable) {
  Fixture f;
  f.opts.db_write_buffer_size = 200;
  CHECK_OK(f.Open());
  CfId a, b;
  CHECK_OK(f.db->CreateColumnFamily("a", &a));
  CHECK_OK(f.db->CreateColumnFamily("b", &b));
  CHECK_OK(f.db->Put({}, a, "first", std::string(50, 'a')));
  for (int i = 0; i < 10; i++) CHECK_OK(f.db->Put({}, b, "b" + std::to_string(i), std::string(20, 'b')));
  CHECK(!f.db->tree.Cf(a)->l0.empty());  // the oldest mutable memtable was a's
  CHECK(f.Clean());
}

TEST(WriteStallStopsAndResumes) {
  Fixture f;
  f.opts.inline_background = false;
  f.opts.max_write_buffer_number = 2;  // one sealed memtable stops writes
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "a", "1"));
  CHECK_OK(f.db->Flush(c));
  WriteOptions ns;
  ns.noSlowdown = true;
  CHECK(f.db->Put(ns, c, "b", "2").IsIncomplete());  // WAL1 fails noSlowdown writes
  CHECK(f.db->Put({}, c, "b", "2").IsBusy());       // others would wait
  CHECK_OK(f.db->RunBackgroundWork());
  CHECK_OK(f.db->Put({}, c, "b", "2"));
  CHECK(f.Clean());
}

TEST(OutOfOrderFlushJobsInstallInOrder) {
  // MT5 jobs may finish in any order; MT6 installs them in seal order.
  Fixture f;
  f.opts.inline_background = false;
  f.opts.max_write_buffer_number = 4;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "k", "1"));
  CHECK_OK(f.db->Flush(c));
  CHECK_OK(f.db->Put({}, c, "k", "2"));
  CHECK_OK(f.db->Flush(c));
  uint64_t j1 = f.db->FlushStart(c), j2 = f.db->FlushStart(c);
  CHECK(j1 && j2);
  CHECK_OK(f.db->FlushFinish(j2));        // the newer one finishes first
  CHECK(f.db->InstallFlush(c).IsBusy());  // it must wait for the older one
  CHECK_OK(f.db->FlushFinish(j1));
  CHECK_OK(f.db->InstallFlush(c));        // both, in one MANIFEST edit
  CHECK(f.db->tree.Cf(c)->imm.empty());
  CHECK(f.Get(c, "k") == "2");
  CHECK(f.Clean());
}

TEST(DropKeepsDataUntilLastHandle) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c;
  CHECK_OK(f.db->CreateColumnFamily("tmp", &c));
  CHECK_OK(f.db->Put({}, c, "k", "v"));
  CHECK(f.db->DropColumnFamily(f.cf[0]).IsInvalidArgument());  // default cannot be dropped
  CHECK_OK(f.db->DropColumnFamily(c));                          // CF2
  CHECK(f.db->Put({}, c, "k2", "v").IsInvalidArgument());       // no more writes
  CHECK(f.Get(c, "k") == "v");                                  // still readable through the handle
  CHECK_OK(f.db->ReleaseHandle(c));                             // CF3
  CHECK(f.db->tree.Cf(c)->state == CfState::kGone);
  CHECK(f.Clean());
}

TEST(OpenMustNameEveryColumnFamily) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c;
  CHECK_OK(f.db->CreateColumnFamily("extra", &c));
  CHECK_OK(f.db->Put(Sync(), c, "k", "v"));
  f.db.reset();
  f.ghost.OnCrash();
  CHECK(f.Open().IsInvalidArgument());  // CF4: "extra" is missing
  f.names = {"default", "extra"};
  CHECK_OK(f.Open());
  CHECK(f.Get(f.cf[1], "k") == "v");
  std::unique_ptr<DB> ro;
  f.db.reset();
  CHECK_OK(DB::Open(&f.env, f.opts, {"extra"}, &ro, nullptr, true));  // read-only may name a subset
  std::string v;
  CHECK_OK(ro->Get(f.cf[1], "k", &v));
  CHECK(v == "v");
  CHECK(ro->Put({}, f.cf[1], "k", "x").IsInvalidArgument());
}

TEST(IteratorsAcrossColumnFamiliesShareOnePointInTime) {
  // CF5
  Fixture f;
  f.opts.inline_background = false;
  CHECK_OK(f.Open());
  CfId c1;
  CHECK_OK(f.db->CreateColumnFamily("one", &c1));
  WriteBatch b1;
  b1.Put(f.cf[0], "x", "1");
  b1.Put(c1, "x", "1");
  CHECK_OK(f.db->Write({}, b1));
  const Snapshot* s = f.db->GetSnapshot();
  WriteBatch b2;
  b2.Put(f.cf[0], "x", "2");
  b2.Put(c1, "y", "2");
  Status st;
  auto G = f.db->NewGroup({{b2, WriteOptions{}}}, &st);
  CHECK_OK(f.db->Admit(*G));
  CHECK_OK(f.db->AppendGroup(*G));
  CHECK_OK(f.db->InsertBatchCf(*G, 0, f.cf[0]));  // inserted into one CF only
  auto its = f.db->NewIterators({f.cf[0], c1});
  CHECK(its[0].size() == 1 && its[0][0].second == "1");  // not published: neither CF shows it
  CHECK(its[1].size() == 1);
  CHECK_OK(f.db->InsertBatchCf(*G, 0, c1));
  CHECK_OK(f.db->PublishGroup(*G));
  its = f.db->NewIterators({f.cf[0], c1});
  CHECK(its[0][0].second == "2" && its[1].size() == 2);  // both CFs show it
  its = f.db->NewIterators({f.cf[0], c1}, s);
  CHECK(its[0][0].second == "1" && its[1].size() == 1);
  CHECK(f.Clean());
}

// ---------------------------------------------------------------- ingestion
static void WriteExternal(SimEnv& env, const std::string& path,
                          std::vector<std::pair<std::string, std::string>> kvs) {
  SstFileWriter w(&env);
  w.Open(path);
  for (auto& [k, v] : kvs) w.Put(k, v);
  w.Finish();
}

TEST(SstFileWriterRequiresSortedKeys) {
  SimEnv env;
  SstFileWriter w(&env);
  CHECK_OK(w.Open("ext/a.sst"));
  CHECK_OK(w.Put("b", "1"));
  CHECK(w.Put("a", "2").IsInvalidArgument());
  CHECK(w.Put("b", "2").IsInvalidArgument());
  CHECK_OK(w.Put("c", "3"));
  CHECK_OK(w.Finish());
  SstFileWriter empty(&env);
  empty.Open("ext/empty.sst");
  CHECK(empty.Finish().IsInvalidArgument());
}

TEST(IngestNonOverlappingGoesDeepWithSeqnoZero) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  WriteExternal(f.env, "ext/1.sst", {{"a", "1"}, {"b", "1"}});
  CHECK_OK(f.db->IngestExternalFile(c, {"ext/1.sst"}));
  auto* cf = f.db->tree.Cf(c);
  CHECK(cf->levels[f.db->tree.Lmax()].size() == 1);  // as deep as it fits
  CHECK(cf->levels[f.db->tree.Lmax()][0]->globalSeqno == 0);
  CHECK(f.Get(c, "a") == "1");
  CHECK(f.Clean());
}

TEST(IngestOverlappingMemtableFlushesFirstAndGetsSeqno) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "b", "old"));
  WriteExternal(f.env, "ext/1.sst", {{"a", "new"}, {"b", "new"}});
  SeqNo before = f.db->tree.lastPublished;
  CHECK_OK(f.db->IngestExternalFile(c, {"ext/1.sst"}));
  auto* cf = f.db->tree.Cf(c);
  CHECK(cf->mem->IsEmpty());  // SST2 step 3 flushed the overlapping memtable
  CHECK(cf->l0.size() == 2);  // the flushed file, and the ingested one above it
  CHECK(cf->l0[0]->globalSeqno == before + 1);  // SST3: newer than what it overlaps
  CHECK(f.db->tree.lastPublished == before + 1);
  CHECK(f.Get(c, "b") == "new");
  CHECK_OK(f.db->CompactLevel(c, 0));
  CHECK(f.Get(c, "b") == "new");
  CHECK(f.Clean());
}

TEST(IngestWithOpenSnapshotGetsSeqno) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "z", "1"));
  const Snapshot* s = f.db->GetSnapshot();
  WriteExternal(f.env, "ext/1.sst", {{"a", "1"}});
  CHECK_OK(f.db->IngestExternalFile(c, {"ext/1.sst"}));
  CHECK(f.Get(c, "a", s) == "<none>");  // snapshot_consistency
  CHECK(f.Get(c, "a") == "1");
  CHECK(f.Clean());
}

TEST(IngestBehindGoesToBottomAndLoses) {
  Fixture f;
  f.opts.allow_ingest_behind = true;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "a", "live"));
  CHECK_OK(f.db->Flush(c));
  WriteExternal(f.env, "ext/1.sst", {{"a", "behind"}, {"b", "behind"}});
  IngestOptions o;
  o.ingest_behind = true;
  CHECK_OK(f.db->IngestExternalFile(c, {"ext/1.sst"}, o));
  CHECK(f.db->tree.Cf(c)->levels[f.db->tree.Lmax()].size() == 1);
  CHECK(f.Get(c, "a") == "live");    // an existing version wins
  CHECK(f.Get(c, "b") == "behind");
  CHECK(f.Clean());
}

TEST(IngestAtomicAcrossColumnFamilies) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c1;
  CHECK_OK(f.db->CreateColumnFamily("one", &c1));
  WriteExternal(f.env, "ext/a.sst", {{"a", "1"}});
  WriteExternal(f.env, "ext/b.sst", {{"b", "1"}});
  CHECK_OK(f.db->IngestExternalFiles({IngestArg{f.cf[0], {"ext/a.sst"}, {}}, IngestArg{c1, {"ext/b.sst"}, {}}}));
  CHECK(f.Get(f.cf[0], "a") == "1" && f.Get(c1, "b") == "1");
  // a bad file anywhere means nothing goes in
  WriteExternal(f.env, "ext/c.sst", {{"c", "1"}});
  f.env.InjectFault(SimEnv::Op::kAppend, "sst/", 1);
  WriteExternal(f.env, "ext/d.sst", {{"d", "1"}});
  CHECK(!f.db->IngestExternalFiles({IngestArg{f.cf[0], {"ext/c.sst"}, {}}, IngestArg{c1, {"ext/d.sst"}, {}}}).ok());
  CHECK(f.Get(f.cf[0], "c") == "<none>" && f.Get(c1, "d") == "<none>");
  CHECK(f.Clean());
}

TEST(IngestRejectsOverlappingFilesInOneCf) {
  Fixture f;
  CHECK_OK(f.Open());
  WriteExternal(f.env, "ext/a.sst", {{"a", "1"}, {"c", "1"}});
  WriteExternal(f.env, "ext/b.sst", {{"b", "1"}});
  CHECK(f.db->IngestExternalFile(f.cf[0], {"ext/a.sst", "ext/b.sst"}).IsInvalidArgument());
  CHECK(f.env.List("sst").empty());  // the copies were deleted
  CHECK(f.Clean());
}

TEST(LevelPlacementStaysAboveOverlap) {
  Fixture f;
  CHECK_OK(f.Open());
  CfId c = f.cf[0];
  CHECK_OK(f.db->Put({}, c, "m", "1"));
  CHECK_OK(f.db->Flush(c));
  CHECK_OK(f.db->CompactLevel(c, 0));  // "m" now in L1
  WriteExternal(f.env, "ext/1.sst", {{"a", "x"}, {"z", "x"}});  // range covers "m"
  CHECK_OK(f.db->IngestExternalFile(c, {"ext/1.sst"}));
  CHECK(f.db->tree.Cf(c)->l0.size() == 1);  // L1 has a key in range, so it stays in L0
  CHECK(f.db->tree.Cf(c)->l0[0]->globalSeqno > 0);
  CHECK(f.Get(c, "m") == "1");
  CHECK(f.Clean());
}

int main() { return testing::RunAll(); }
