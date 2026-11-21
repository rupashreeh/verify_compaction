// compaction.cc: compaction of the LSM tree, LSM2 to LSM12 of
// lsm_tree/lsm_tree_algorithm.pdf. Leveled and universal styles.
//
// A compaction has two steps, as in LSMTree.tla: CompactStart picks the inputs
// and marks them busy (LSM5 or LSM6); CompactFinish merges them (LSM7), cuts the
// output into files (LSM8), and installs the result in one MANIFEST record
// (LSM9). Between the two, other steps may run, so tests and the random
// simulator can run several compactions at once.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

#include "ghost.h"
#include "lsm_tree.h"

namespace lsm {

namespace {
using FileVec = std::vector<std::shared_ptr<SstFile>>;

// The read order (LSM2): L0 newest first (by epoch), then L1, L2, ...
constexpr uint64_t kPosBase = 1ull << 40;
uint64_t PosAt(int level, uint64_t epoch) { return level == 0 ? kPosBase - epoch : kPosBase + level; }

void RangeOf(const FileVec& I, std::string* lo, std::string* hi) {
  for (size_t i = 0; i < I.size(); i++) {
    if (i == 0 || I[i]->lo < *lo) *lo = I[i]->lo;
    if (i == 0 || I[i]->hi > *hi) *hi = I[i]->hi;
  }
}
bool Has(const FileVec& v, const SstFile* f) {
  for (auto& g : v)
    if (g.get() == f) return true;
  return false;
}
uint64_t SizeOf(const FileVec& v) {
  uint64_t n = 0;
  for (auto& f : v) n += f->Size();
  return n;
}
}  // namespace

// ============================================================ LSM2 SortedRuns
std::vector<std::vector<std::pair<int, std::shared_ptr<SstFile>>>> LsmTree::SortedRuns(CfId c) const {
  std::vector<std::vector<std::pair<int, std::shared_ptr<SstFile>>>> runs;
  const ColumnFamily* cf = Cf(c);
  if (cf == nullptr) return runs;
  for (auto& f : cf->l0) runs.push_back({{0, f}});  // each L0 file, newest first
  for (int l = 1; l <= Lmax(); l++) {
    if (cf->levels[l].empty()) continue;
    std::vector<std::pair<int, std::shared_ptr<SstFile>>> r;
    for (auto& f : cf->levels[l]) r.push_back({l, f});
    runs.push_back(r);  // a level is one run
  }
  return runs;
}

// ============================================================ LSM3
std::vector<std::shared_ptr<SstFile>> LsmTree::Overlapping(CfId c, int level, const std::string& lo,
                                                           const std::string& hi) const {
  FileVec I;
  const ColumnFamily* cf = Cf(c);
  if (cf == nullptr) return I;
  const FileVec& lv = level == 0 ? cf->l0 : cf->levels[level];
  for (auto& f : lv)
    if (f->Overlaps(lo, hi)) I.push_back(f);
  if (level == 0 && !I.empty()) {  // L0 files overlap each other: grow until stable
    bool grew = true;
    while (grew) {
      grew = false;
      std::string a, b;
      RangeOf(I, &a, &b);
      for (auto& f : lv)
        if (!Has(I, f.get()) && f->Overlaps(a, b)) { I.push_back(f); grew = true; }
    }
  }
  return I;
}

void LsmTree::ExpandToCleanCut(CfId c, int level, FileVec* I) const {
  const ColumnFamily* cf = Cf(c);
  if (cf == nullptr || level == 0 || I->empty()) return;
  bool grew = true;
  while (grew) {  // a neighbor that shares a boundary user key joins the pick
    grew = false;
    std::string a, b;
    RangeOf(*I, &a, &b);
    for (auto& f : cf->levels[level])
      if (!Has(*I, f.get()) && f->Overlaps(a, b)) { I->push_back(f); grew = true; }
  }
}

bool LsmTree::RangeBeingCompacted(CfId c, int level, const std::string& lo, const std::string& hi) const {
  if (level == 0) return false;  // L0 outputs may overlap
  for (auto& [id, C] : compactions)
    if (C.c == c && C.outLevel == level && !(C.hi < lo || hi < C.lo)) return true;
  return false;
}

// ============================================================ LSM4 LevelTargets
LevelInfo LsmTree::LevelTargets(CfId c) const {
  LevelInfo li;
  li.target.assign(Lmax() + 1, 0);
  li.score.assign(Lmax() + 1, 0);
  const ColumnFamily* cf = Cf(c);
  if (cf == nullptr) return li;
  const int last = LastOutputLevel();
  const double B = static_cast<double>(opts.max_bytes_for_level_base);
  const double m = std::max(2, opts.max_bytes_for_level_multiplier);
  if (last < 1) return li;
  if (!opts.level_compaction_dynamic_level_bytes) {
    li.baseLevel = 1;
    li.target[1] = B;
    for (int l = 2; l <= last; l++) li.target[l] = li.target[l - 1] * m;
  } else {
    // RocksDB CalculateBaseBytes: the largest level sets the targets; the base level
    // is the first non-empty level, moved up while its target is over B.
    double maxSize = 0;
    int first = last;
    for (int l = last; l >= 1; l--) {
      double sz = static_cast<double>(SizeOf(cf->levels[l]));
      maxSize = std::max(maxSize, sz);
      if (sz > 0) first = l;
    }
    double cur = maxSize;
    for (int l = last; l > first; l--) cur /= m;
    int base = first;
    while (base > 1 && cur > B) {
      base--;
      cur /= m;
    }
    li.baseLevel = base;
    li.target[base] = std::max(cur, B / m);
    for (int l = base + 1; l <= last; l++) li.target[l] = li.target[l - 1] * m;
  }
  // scores; files being compacted do not count
  int n0 = 0;
  uint64_t s0 = 0;
  for (auto& f : cf->l0)
    if (!f->beingCompacted) { n0++; s0 += f->Size(); }
  if (n0 >= opts.level0_file_num_compaction_trigger && opts.level0_file_num_compaction_trigger > 0)
    li.score[0] = std::max(static_cast<double>(n0) / opts.level0_file_num_compaction_trigger, s0 / B);
  for (int l = li.baseLevel; l < last; l++) {
    uint64_t sz = 0;
    for (auto& f : cf->levels[l])
      if (!f->beingCompacted) sz += f->Size();
    li.score[l] = li.target[l] > 0 ? sz / li.target[l] : 0;
  }
  return li;
}

// ============================================================ LSM5 PickLeveled
bool LsmTree::PickLeveled(CfId c, Compaction* out, int manualLevel) {
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return false;
  const int last = LastOutputLevel();
  LevelInfo li = LevelTargets(c);
  std::vector<int> order;
  if (manualLevel >= 0) {
    order.push_back(manualLevel);
  } else {
    for (int l = 0; l < last; l++)
      if (li.score[l] >= 1) order.push_back(l);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return li.score[a] > li.score[b]; });
  }
  for (int b : order) {
    int o = (manualLevel >= 0 || b > 0) ? b + 1 : li.baseLevel;
    if (o < 1 || o > last || b >= o) continue;
    FileVec cand = b == 0 ? cf->l0 : cf->levels[b];
    if (b == 0) {
      // L0 in its own order, newest first. The expansion (LSM3) makes the order
      // irrelevant: every older L0 file that overlaps comes along.
    } else if (opts.compaction_pri == CompactionPri::kRoundRobin) {
      // start after the cursor (LSM1's circulating cursor), and wrap around
      std::string cur = compactCursor[{c, b}];
      std::stable_partition(cand.begin(), cand.end(), [&](auto& f) { return cur.empty() || f->lo > cur; });
    } else {  // kMinOverlappingRatio: least overlap in L_o per byte first
      auto ratio = [&](const std::shared_ptr<SstFile>& f) {
        return static_cast<double>(SizeOf(Overlapping(c, o, f->lo, f->hi))) / std::max<uint64_t>(1, f->Size());
      };
      std::stable_sort(cand.begin(), cand.end(), [&](auto& x, auto& y) { return ratio(x) < ratio(y); });
    }
    for (auto& F : cand) {
      if (opts.fix.FixSkipBusy && F->beingCompacted) continue;
      FileVec I0;
      if (b == 0) {
        if (manualLevel == 0) I0 = cf->l0;  // CompactRange takes all of L0
        else if (opts.fix.FixL0Overlap) I0 = Overlapping(c, 0, F->lo, F->hi);
        else I0 = {F};  // bug: an older overlapping L0 file stays above
      } else {
        I0 = {F};
        if (opts.fix.FixCleanCut) ExpandToCleanCut(c, b, &I0);
      }
      std::string lo, hi;
      RangeOf(I0, &lo, &hi);
      FileVec I1;
      if (opts.fix.FixOutputOverlap) {
        I1 = Overlapping(c, o, lo, hi);
        if (opts.fix.FixCleanCut) ExpandToCleanCut(c, o, &I1);
      }
      FileVec all = I0;
      all.insert(all.end(), I1.begin(), I1.end());
      RangeOf(all, &lo, &hi);
      if (opts.fix.FixSkipBusy) {
        bool busy = false;
        for (auto& f : all) busy |= f->beingCompacted;
        if (busy || RangeBeingCompacted(c, o, lo, hi)) {
          if (manualLevel == 0) return false;
          continue;
        }
      }
      out->c = c;
      out->startLevel = b;
      out->outLevel = o;
      out->inputs.clear();
      for (auto& f : I0) out->inputs.push_back({b, f});
      for (auto& f : I1) out->inputs.push_back({o, f});
      out->lo = lo;
      out->hi = hi;
      out->outEpoch = 0;
      out->trivialMove = I1.empty() && I0.size() == 1;
      out->reason = manualLevel >= 0 ? "manual" : (b == 0 ? "L0 file count" : "level size");
      if (opts.compaction_pri == CompactionPri::kRoundRobin) compactCursor[{c, b}] = hi;
      return true;
    }
  }
  return false;
}

// ============================================================ LSM6 PickUniversal
bool LsmTree::PickUniversal(CfId c, Compaction* out) {
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return false;
  auto runs = SortedRuns(c);
  // With allow_ingest_behind the bottommost level is reserved for ingested files:
  // it is never an input, so newer data never lands beside them.
  while (!runs.empty() && runs.back().front().first > LastOutputLevel()) runs.pop_back();
  const int r = static_cast<int>(runs.size());
  const int t0 = std::max(1, opts.level0_file_num_compaction_trigger);
  if (r < t0 || r < 2) return false;
  std::vector<uint64_t> size(r);
  std::vector<bool> busy(r);
  for (int i = 0; i < r; i++)
    for (auto& [l, f] : runs[i]) {
      size[i] += f->Size();
      if (f->beingCompacted) busy[i] = true;
    }
  if (!opts.fix.FixSkipBusy) busy.assign(r, false);
  const auto& u = opts.compaction_options_universal;
  const int maxW = std::max(2, u.max_merge_width);

  std::vector<int> pick;  // run indexes, newest first
  std::string reason;
  if (!opts.fix.FixAdjacentRuns) {
    // bug: take the smallest runs, wherever they are in the age order
    std::vector<int> idx;
    for (int i = 0; i < r; i++)
      if (!busy[i]) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return size[a] < size[b]; });
    int n = std::max(2, r - t0 + 1);
    if (static_cast<int>(idx.size()) < n) return false;
    pick.assign(idx.begin(), idx.begin() + n);
    std::sort(pick.begin(), pick.end());
    reason = "smallest runs (bug)";
  } else {
    bool anyBusy = std::find(busy.begin(), busy.end(), true) != busy.end();
    uint64_t newer = 0;
    for (int i = 0; i + 1 < r; i++) newer += size[i];
    if (!anyBusy && newer * 100 > static_cast<uint64_t>(u.max_size_amplification_percent) * size[r - 1]) {
      for (int i = 0; i < r; i++) pick.push_back(i);  // space amplification: all runs
      reason = "size amplification";
    }
    for (int i = 0; pick.empty() && i < r; i++) {  // size ratio
      if (busy[i]) continue;
      int j = i;
      uint64_t sum = size[i];
      while (j + 1 < r && !busy[j + 1] && size[j + 1] * 100 <= static_cast<uint64_t>(100 + u.size_ratio) * sum) {
        j++;
        sum += size[j];
      }
      if (j - i + 1 >= std::max(2, u.min_merge_width)) {
        for (int x = i; x <= std::min(j, i + maxW - 1); x++) pick.push_back(x);
        reason = "size ratio";
      }
    }
    if (pick.empty()) {  // reduce the number of runs below the trigger
      int n = std::min(r - t0 + 1, maxW);
      int i = 0;
      while (i < r && busy[i]) i++;
      int j = i;
      while (j + 1 < r && !busy[j + 1] && j - i + 1 < n) j++;
      if (i < r && j - i + 1 >= 2)
        for (int x = i; x <= j; x++) pick.push_back(x);
      reason = "run count";
    }
  }
  if (pick.size() < 2) return false;
  // the output level: the highest level that keeps the age order
  int oldest = pick.back();
  int oldestLevel = runs[oldest].front().first;
  int o;
  if (oldestLevel > 0) o = oldestLevel;
  else if (oldest == r - 1) o = std::max(0, LastOutputLevel());
  else {
    int nl = runs[oldest + 1].front().first;
    o = nl == 0 ? 0 : nl - 1;
  }
  FileVec all;
  out->inputs.clear();
  uint64_t minEp = UINT64_MAX;
  for (int i : pick)
    for (auto& [l, f] : runs[i]) {
      out->inputs.push_back({l, f});
      all.push_back(f);
      if (l == 0) minEp = std::min(minEp, f->epoch);
    }
  RangeOf(all, &out->lo, &out->hi);
  if (opts.fix.FixSkipBusy && RangeBeingCompacted(c, o, out->lo, out->hi)) return false;
  out->c = c;
  out->startLevel = runs[pick.front()].front().first;
  out->outLevel = o;
  out->outEpoch = o == 0 ? minEp : 0;
  out->trivialMove = false;
  out->reason = reason;
  return true;
}

// ============================================================ start and finish
uint64_t LsmTree::CompactStart(CfId c, int manualLevel) {
  Compaction C;
  bool ok = (opts.compaction_style == CompactionStyle::kUniversal && manualLevel < 0)
                ? PickUniversal(c, &C)
                : PickLeveled(c, &C, manualLevel);
  if (!ok) return 0;
  C.id = nextCompactionId++;
  C.minOut = CapturePendingOutput();  // DB8: a full purge keeps every number from here on
  for (auto& [l, f] : C.inputs) f->beingCompacted = true;
  compactions[C.id] = C;
  return C.id;
}

bool LsmTree::KeyNotExistsBeyond(const Compaction& C, const std::string& k) const {
  if (opts.allow_ingest_behind && opts.fix.FixIngestBehindKeep) return false;  // Lmax may still get data
  const ColumnFamily* cf = Cf(C.c);
  if (cf == nullptr) return false;
  std::set<uint64_t> in;
  for (auto& [l, f] : C.inputs) in.insert(f->number);
  const uint64_t outPos = PosAt(C.outLevel, C.outEpoch);
  auto below = [&](int l, const std::shared_ptr<SstFile>& f) {
    return !in.count(f->number) && PosAt(l, f->epoch) > outPos && f->lo <= k && k <= f->hi;
  };
  for (auto& f : cf->l0)
    if (below(0, f)) return false;
  for (int l = 1; l <= Lmax(); l++)
    for (auto& f : cf->levels[l])
      if (below(l, f)) return false;
  return true;
}

// ============================================================ LSM7 CompactionIterator
Status LsmTree::CompactionMerge(const Compaction& C, std::vector<Entry>* out,
                                std::vector<std::pair<std::string, SeqNo>>* dropped) const {
  std::map<IKey, std::pair<ValueType, std::string>, IKeyCmp> merged;
  // Read every input through its table reader (BC9) with fill_cache = false (K10).
  BlockReadOptions cro;
  cro.fill_cache = !opts.fix.FixCompactionNoFill;
  cro.origin = CacheOrigin::kCompaction;
  for (auto& [l, f] : C.inputs) {
    std::vector<Entry> es;
    if (blockCache) {
      Status rs = Reader(f, l == 0)->ReadAll(cro, &es);
      if (!rs.ok()) return rs;
    } else {
      es = f->entries;
    }
    for (auto& e : es) merged[IKey{e.key, e.seq}] = {e.type, e.value};
  }
  // Snapshot stripes (K12). The published read point is a snapshot too, and every
  // unpublished sequence number is its own stripe: a reader may still be given it.
  std::set<SeqNo> S(snapshots.begin(), snapshots.end());
  S.insert(lastPublished);
  auto stripe = [&](SeqNo v) -> SeqNo { return v > lastPublished ? v : *S.lower_bound(v); };
  const SeqNo earliest = stripe(0);
  std::string cur;
  bool have = false, dropRest = false;
  SeqNo prevStripe = 0;
  for (auto& [ik, tv] : merged) {
    const SeqNo st = stripe(ik.seq);
    if (!have || ik.key != cur) {  // the newest version of a new key
      cur = ik.key;
      have = true;
      dropRest = false;
    } else {
      if (dropRest) continue;  // below a dropped deletion
      bool shadowed = opts.fix.FixSnapshotKeep ? st == prevStripe : true;
      if (shadowed) continue;  // a newer version in its stripe: no reader can see it
    }
    prevStripe = st;
    if (tv.first == ValueType::kDelete && st == earliest &&
        (!opts.fix.FixTombstoneBottom || KeyNotExistsBeyond(C, ik.key))) {
      dropRest = true;  // nothing older can be read: drop the deletion and what it hides
      if (dropped) dropped->push_back({ik.key, ik.seq});
      continue;
    }
    out->push_back(Entry{ik.key, ik.seq, tv.first, tv.second});
  }
  return Status::OK();
}

// ============================================================ LSM8, LSM9 (DB6, DB7)
Status LsmTree::CompactFinish(uint64_t id) {
  Status s = CompactWrite(id);
  if (!s.ok()) return s;
  if (stepHook) stepHook("DB6 CompactWrite (outputs on disk, not yet installed)");
  return CompactInstall(id);
}

// DB6: merge the inputs (LSM7) and write the outputs (LSM8). The outputs are on
// disk but in no version; pendingOutputs keeps a full purge (DB8) off them.
Status LsmTree::CompactWrite(uint64_t id) {
  auto it = compactions.find(id);
  if (it == compactions.end()) return Status::InvalidArgument("no such compaction");
  Compaction& C = it->second;
  if (C.written) return Status::OK();
  auto abort = [&](Status s) {
    for (auto& o : C.outs) env->Delete(o->path);
    for (auto& [l, f] : C.inputs) f->beingCompacted = false;
    ReleasePendingOutput(C.minOut);
    compactions.erase(id);
    return s;
  };
  ColumnFamily* cf = Cf(C.c);
  if (cf == nullptr || cf->state != CfState::kLive) return abort(Status::OK());  // dropped meanwhile
  C.written = true;
  if (C.trivialMove) return Status::OK();
  std::vector<Entry> merged;
  Status s = CompactionMerge(C, &merged, &C.dropped);  // LSM7
  if (!s.ok()) return abort(s);
  // LSM8: cut at the target size of L_o, never between two versions of one key
  uint64_t target = UINT64_MAX;
  if (C.outLevel > 0) {
    double t = static_cast<double>(opts.target_file_size_base);
    for (int l = 1; l < C.outLevel; l++) t *= std::max(1, opts.target_file_size_multiplier);
    target = static_cast<uint64_t>(t);
  }
  SeqNo logged = 0;
  for (auto& [l, f] : C.inputs) logged = std::max(logged, f->largestLoggedSeq);
  std::vector<std::vector<Entry>> groups(1);
  uint64_t sz = 0;
  for (auto& x : merged) {
    auto& g = groups.back();
    if (!g.empty() && sz >= target && (opts.compaction_split_user_keys || x.key != g.back().key)) {
      groups.emplace_back();
      sz = 0;
    }
    groups.back().push_back(x);
    sz += x.key.size() + x.value.size() + 16;
  }
  for (auto& g : groups) {
    if (g.empty()) continue;  // every version was dropped
    auto f = std::make_shared<SstFile>();
    f->number = nextFileNumber++;
    f->path = NumberedName("sst", f->number, "sst");
    f->entries = std::move(g);
    f->ComputeBounds();
    f->largestLoggedSeq = logged;
    f->epoch = C.outLevel == 0 ? C.outEpoch : nextEpoch++;
    s = env->WriteFileDurably(f->path, EncodeSst(f->entries, 0));
    if (!s.ok()) {
      env->Delete(f->path);
      return abort(s);
    }
    C.outs.push_back(f);
  }
  return Status::OK();
}

// DB7: install. With FixAtomicEdit, one record deletes the inputs and adds the
// outputs (LSM9). With FixSyncBeforeApply, LogAndApply syncs the record before the
// version changes; without it, the version changes first and the record waits in
// pendingEdits until SyncManifest.
Status LsmTree::CompactInstall(uint64_t id) {
  auto it = compactions.find(id);
  if (it == compactions.end()) return Status::InvalidArgument("no such compaction");
  if (!it->second.written) return Status::Busy("the outputs are not written yet");
  Compaction C = it->second;
  auto done = [&] {
    for (auto& [l, f] : C.inputs) f->beingCompacted = false;
    ReleasePendingOutput(C.minOut);
    compactions.erase(id);
  };
  ColumnFamily* cf = Cf(C.c);
  if (cf == nullptr || cf->state != CfState::kLive) {  // dropped meanwhile
    for (auto& o : C.outs) env->Delete(o->path);
    done();
    return Status::OK();
  }
  VersionEdit del, add;
  if (C.trivialMove) {
    auto& [l, f] = C.inputs[0];
    del.delFile.push_back({C.c, l, f->number});
    add.addFile.push_back(FileAdd{C.c, C.outLevel, f->number, f->globalSeqno, f->epoch, f->largestLoggedSeq, f});
  } else {
    for (auto& [l, f] : C.inputs) del.delFile.push_back({C.c, l, f->number});
    for (auto& f : C.outs) add.addFile.push_back(FileAdd{C.c, C.outLevel, f->number, 0, f->epoch, f->largestLoggedSeq, f});
  }
  Status s;
  if (opts.fix.FixAtomicEdit || C.trivialMove) {
    VersionEdit e = del;
    e.addFile = add.addFile;
    e.nextFileNumber = nextFileNumber;
    if (opts.fix.FixSyncBeforeApply) {
      s = LogAndApply(e);  // LSM9: one atomic record (K4), synced, then applied
    } else {
      Apply(e);  // bug: the new version is used before its record is on disk
      pendingEdits.push_back(e);
    }
  } else {
    // bug: two records. The outputs are added first, the inputs deleted after.
    add.nextFileNumber = nextFileNumber;
    s = LogAndApply(add);
    if (s.ok() && stepHook) stepHook("DB7 InstallCompaction (first of two records)");
    if (s.ok()) s = LogAndApply(del);
  }
  done();
  if (!s.ok()) {
    for (auto& o : C.outs) env->Delete(o->path);
    return s;
  }
  if (ghost) ghost->OnCompactionDropped(C.c, C.dropped);
  compactionsDone++;
  if (C.trivialMove) {
    trivialMoves++;
  } else {
    for (auto& [l, f] : C.inputs) obsoleteFiles.push_back(f);
  }
  return PurgeObsoleteFiles();  // LSM10
}

Status LsmTree::Compact(CfId c, int from) {
  ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return Status::InvalidArgument("CF is not live");
  if (from < 0 || from + 1 > Lmax()) return Status::InvalidArgument("bad level");
  if (from + 1 > LastOutputLevel()) return Status::InvalidArgument("bottommost level is reserved");
  if ((from == 0 ? cf->l0 : cf->levels[from]).empty()) return Status::OK();
  uint64_t id = CompactStart(c, from);
  if (id == 0) return Status::Busy("a running compaction uses these files");
  return CompactFinish(id);
}

bool LsmTree::NeedsCompaction(CfId c) const {
  const ColumnFamily* cf = Cf(c);
  if (cf == nullptr || cf->state != CfState::kLive) return false;
  if (opts.compaction_style == CompactionStyle::kUniversal)
    return static_cast<int>(SortedRuns(c).size()) >= std::max(2, opts.level0_file_num_compaction_trigger);
  LevelInfo li = LevelTargets(c);
  for (double s : li.score)
    if (s >= 1) return true;
  return false;
}

// ============================================================ LSM11
Status LsmTree::MaybeScheduleCompaction() {
  if (opts.disable_auto_compactions) return Status::OK();
  for (int round = 0; round < 1000; round++) {
    bool progress = false;
    for (CfId c : CFs()) {
      if (!NeedsCompaction(c)) continue;
      uint64_t id = CompactStart(c);
      if (id == 0) continue;
      Status s = CompactFinish(id);
      if (!s.ok()) return s;
      progress = true;
    }
    if (!progress) break;
  }
  return Status::OK();
}

// ============================================================ LSM12
uint64_t LsmTree::PendingCompactionBytes(CfId c) const {
  const ColumnFamily* cf = Cf(c);
  if (cf == nullptr || opts.compaction_style != CompactionStyle::kLevel) return 0;
  LevelInfo li = LevelTargets(c);
  uint64_t debt = 0;
  if (static_cast<int>(cf->l0.size()) >= opts.level0_file_num_compaction_trigger) debt += SizeOf(cf->l0);
  for (int l = li.baseLevel; l < LastOutputLevel(); l++) {
    double sz = static_cast<double>(SizeOf(cf->levels[l]));
    if (sz > li.target[l]) debt += static_cast<uint64_t>((sz - li.target[l]) * opts.max_bytes_for_level_multiplier);
  }
  return debt;
}

// ============================================================ LSM10
std::shared_ptr<VersionPin> LsmTree::PinVersion(CfId c) {
  auto p = std::make_shared<VersionPin>();
  p->c = c;
  p->seq = lastPublished;
  if (const ColumnFamily* cf = Cf(c)) {
    for (auto& f : cf->l0) p->files.push_back({0, f});
    for (int l = 1; l <= Lmax(); l++)
      for (auto& f : cf->levels[l]) p->files.push_back({l, f});
  }
  pins.push_back(p);
  return p;
}

LookupResult LsmTree::ReadPinned(const VersionPin& p, const std::string& k, Status* st) const {
  *st = Status::OK();
  for (auto& [l, f] : p.files) {
    if (k < f->lo || k > f->hi) continue;
    if (!env->Exists(f->path)) {
      *st = Status::IOError("file " + std::to_string(f->number) + " of a pinned version was deleted");
      return LookupResult{};
    }
    LookupResult x = f->Get(k, p.seq);
    if (x.kind != LookupResult::kNotFound) return x;
  }
  return LookupResult{};
}

// DB8, full scan: delete every SST on disk that no live version uses. A running
// flush or compaction writes files that no version lists yet; RocksDB keeps them by
// number: every file at or above the oldest pending output stays.
Status LsmTree::PurgeAllObsoleteFiles() {
  Status s = PurgeObsoleteFiles();  // the obsolete list first (it also drops dead pins)
  if (!s.ok()) return s;
  std::set<uint64_t> live;
  for (auto& [id, cf] : cfs) {
    for (auto& f : cf.l0) live.insert(f->number);
    for (auto& lv : cf.levels)
      for (auto& f : lv) live.insert(f->number);
  }
  for (auto& w : pins)
    if (auto p = w.lock())
      for (auto& [l, f] : p->files) live.insert(f->number);
  for (auto& f : obsoleteFiles) live.insert(f->number);  // still pinned
  const uint64_t minPending = pendingOutputs.empty() ? UINT64_MAX : *pendingOutputs.begin();
  for (auto& p : env->List("sst")) {
    unsigned long long n = 0;
    if (std::sscanf(p.c_str(), "sst/%llu.sst", &n) != 1) continue;
    if (live.count(n)) continue;
    if (opts.fix.FixProtectOutputs && n >= minPending) continue;
    s = env->Delete(p);
    if (!s.ok()) return s;
  }
  return Status::OK();
}

Status LsmTree::PurgeObsoleteFiles() {
  std::set<uint64_t> live;
  std::vector<std::weak_ptr<VersionPin>> keepPins;
  for (auto& w : pins)
    if (auto p = w.lock()) {
      keepPins.push_back(w);
      for (auto& [l, f] : p->files) live.insert(f->number);
    }
  pins.swap(keepPins);
  std::vector<std::shared_ptr<SstFile>> keep;
  for (auto& f : obsoleteFiles) {
    if (opts.fix.FixVersionRef && live.count(f->number)) {
      keep.push_back(f);
      continue;
    }
    if (env->Exists(f->path)) {
      Status s = env->Delete(f->path);
      if (!s.ok()) return s;
    }
  }
  obsoleteFiles.swap(keep);
  return Status::OK();
}

}  // namespace lsm
