// ghost.cc: ghost-state hooks and CheckInvariants.
#include "ghost.h"

#include <algorithm>
#include <sstream>

#include "db.h"

namespace lsm {

namespace {
std::string Str(std::initializer_list<std::string> parts) {
  std::string s;
  for (auto& p : parts) s += p;
  return s;
}
std::string N(uint64_t x) { return std::to_string(x); }

using KeyMax = std::map<std::pair<CfId, std::string>, SeqNo>;

void Note(KeyMax& m, CfId c, const std::string& k, SeqNo s) {
  auto key = std::make_pair(c, k);
  auto it = m.find(key);
  if (it == m.end() || it->second < s) m[key] = s;
}

// The newest sequence number of each key, in the SSTs and in the whole store.
// A write is "covered" if the store holds it or a newer version of its key
// (a flush may drop a shadowed version, MT5).
void Index(const LsmTree& T, KeyMax* sstMax, KeyMax* storeMax) {
  for (auto& [c, cf] : T.cfs) {
    if (cf.state == CfState::kGone || cf.state == CfState::kNone) continue;
    for (auto& [ik, v] : cf.mem->table) Note(*storeMax, c, ik.key, ik.seq);
    for (auto& M : cf.imm)
      for (auto& [ik, v] : M->table) Note(*storeMax, c, ik.key, ik.seq);
    auto files = [&](const std::vector<std::shared_ptr<SstFile>>& fs) {
      for (auto& f : fs)
        for (auto& e : f->entries) {
          Note(*sstMax, c, e.key, e.seq);
          Note(*storeMax, c, e.key, e.seq);
        }
    };
    files(cf.l0);
    for (auto& lv : cf.levels) files(lv);
  }
}

bool Covered(const KeyMax& m, const Ghost::Write& w) {
  auto it = m.find({w.cf, w.key});
  return it != m.end() && it->second >= w.seq;
}
}  // namespace

// ============================================================ hooks
void Ghost::OnAppend(const BatchState& b) {
  Batch& gb = batches[b.id];
  gb.cfs = b.Cfs();
  gb.first = b.firstSeq;
  gb.last = b.firstSeq + b.batch.ops.size() - 1;
  for (size_t i = 0; i < b.batch.ops.size(); i++) {
    auto& op = b.batch.ops[i];
    Write w{b.firstSeq + i, op.cf, op.key, op.type, op.value, b.id};
    w.logged = b.stage == BatchStage::kLogged;
    w.log = b.log;
    w.syncReq = b.o.sync;
    writes[w.seq] = w;
  }
}

void Ghost::OnInsert(const BatchState& b, CfId c) { batches[b.id].inserted.insert(c); }

void Ghost::OnPublish(const WriteGroup& G) {
  for (auto& b : G.batches) {
    if (b->stage == BatchStage::kFailed) continue;
    for (size_t i = 0; i < b->batch.ops.size(); i++) {
      auto& op = b->batch.ops[i];
      SeqNo s = b->firstSeq + i;
      hist.push_back(Hist{op.cf, op.key, op.type == ValueType::kDelete, op.value, ++clock, s, false});
      auto it = writes.find(s);
      if (it != writes.end()) it->second.published = true;
    }
  }
}

void Ghost::OnAck(const BatchState& b) {
  for (size_t i = 0; i < b.batch.ops.size(); i++) {
    auto it = writes.find(b.firstSeq + i);
    if (it != writes.end()) it->second.acked = true;
  }
}

void Ghost::OnFlushInstalled(const std::set<CfId>&, const std::vector<std::shared_ptr<MemTable>>& mems) {
  for (auto& M : mems) flushedRecords += M->table.size();  // D
}

void Ghost::OnIngestPrepare(uint64_t job, const std::set<CfId>& cfs) { jobs[job] = Job{cfs, {}}; }

void Ghost::OnIngestCommit(uint64_t job, const std::vector<LsmTree::Placement>& placed,
                           const std::set<CfId>& done) {
  bool behind = !placed.empty() && placed.front().behind;
  uint64_t t = behind ? 0 : ++clock;  // ingest_behind files are older than everything
  for (auto& p : placed)
    for (auto& e : p.f->entries)
      hist.push_back(Hist{p.c, e.key, e.type == ValueType::kDelete, e.value, t, e.seq, true});
  for (CfId c : done) jobs[job].done.insert(c);
}

void Ghost::OnIngestFail(uint64_t job) { jobs.erase(job); }

void Ghost::OnSnapshot(const Snapshot* s) { snaps[s] = {s->seq, clock}; }
void Ghost::OnReleaseSnapshot(const Snapshot* s) { snaps.erase(s); }

void Ghost::OnCrash() {
  snaps.clear();
  batches.clear();
}

void Ghost::OnRecoveryFailed(const Status& s) {
  rec = RecResult{true, true, true, true, s.ToString()};
  violations.push_back("RecoveryOpens: WAL9 refused to open: " + s.ToString());
}

void Ghost::OnRecovered(const DB& db) {
  const LsmTree& T = db.tree;
  KeyMax sstMax, storeMax;
  Index(T, &sstMax, &storeMax);
  rec = RecResult{true, false, true, true, ""};
  // RecoveryPrefix: the recovered logged writes are a prefix of the logged history
  SeqNo firstMissing = 0;
  for (auto& [s, w] : writes) {
    if (!T.IsLive(w.cf) || !w.logged) continue;
    bool present = Covered(storeMax, w);
    if (!present && firstMissing == 0) firstMissing = s;
    if (present && firstMissing != 0 && rec.prefixOK) {
      rec.prefixOK = false;
      rec.detail = "write " + N(s) + " survived but earlier write " + N(firstMissing) + " was lost";
      violations.push_back("RecoveryPrefix: " + rec.detail);
    }
  }
  // RecoveryDurable: every acknowledged sync write survives
  for (auto& [s, w] : writes) {
    if (!T.IsLive(w.cf) || !(w.acked && w.syncReq)) continue;
    if (!Covered(storeMax, w)) {
      rec.durableOK = false;
      violations.push_back("RecoveryDurable: acknowledged sync write " + N(s) + " (" + w.key + ") was lost");
    }
  }
  // the store now holds exactly what was recovered: issued' = present
  for (auto it = writes.begin(); it != writes.end();) {
    if (!T.IsLive(it->second.cf) || !Covered(storeMax, it->second)) it = writes.erase(it);
    else { it->second.published = true; ++it; }
  }
  // rebuild the commit history from what survived, in sequence order
  std::vector<Hist> h;
  for (auto& x : hist)
    if (x.ingest && T.IsLive(x.cf)) h.push_back(x);
  for (auto& [s, w] : writes) h.push_back(Hist{w.cf, w.key, w.type == ValueType::kDelete, w.value, 0, s, false});
  std::stable_sort(h.begin(), h.end(), [](const Hist& a, const Hist& b) {
    if (a.seq != b.seq) return a.seq < b.seq;
    return a.t < b.t;
  });
  clock = 0;
  for (auto& x : h) x.t = ++clock;
  hist = std::move(h);
  snaps.clear();
  batches.clear();
}

bool Ghost::Expected(CfId c, const std::string& k, uint64_t t, std::string* v) const {
  const Hist* best = nullptr;
  for (auto& h : hist)
    if (h.cf == c && h.key == k && h.t <= t && (best == nullptr || h.t >= best->t)) best = &h;
  if (best == nullptr || best->del) return false;
  *v = best->value;
  return true;
}

// ============================================================ CheckInvariants
std::vector<std::string> CheckInvariants(const DB& db, const Ghost& g) {
  std::vector<std::string> v;
  const LsmTree& T = db.tree;
  const Wal& W = db.wal;
  KeyMax sstMax, storeMax;
  Index(T, &sstMax, &storeMax);
  LogNum minKeep = W.MinLogToKeep();

  // ---------------- WAL_EndToEnd.tla ----------------
  for (auto& [s, w] : g.writes) {
    if (!T.IsLive(w.cf)) continue;
    // Coverage (I4): a record in a log below N_c is in c's SSTs
    if (w.logged && w.log < T.LogNumber(w.cf) && !Covered(sstMax, w))
      v.push_back(Str({"Coverage (I4): write ", N(s), " of CF ", N(w.cf), " is in log ", N(w.log),
                       " < N_c = ", N(T.LogNumber(w.cf)), " but not in an SST"}));
    // NeededImpliesLive (Lemma 1): a log with an unflushed record is live
    if (!db.readOnly && w.logged && !Covered(sstMax, w) && w.log < minKeep)
      v.push_back(Str({"NeededImpliesLive: write ", N(s), " is unflushed but its log ", N(w.log),
                       " is below MinLogToKeep = ", N(minKeep)}));
    // NoLostAckWhileUp: a running store holds every acknowledged sync write
    if (w.acked && w.syncReq && !Covered(storeMax, w))
      v.push_back(Str({"NoLostAckWhileUp: acknowledged sync write ", N(s), " is not in the store"}));
  }
  // Lifecycle (I5): only logs below MinLogToKeep are obsolete or disposed of
  for (auto& l : W.L)
    if (l->state != LogState::kActive && l->state != LogState::kClosed && l->num >= minKeep)
      v.push_back(Str({"Lifecycle (I5): log ", N(l->num), " is ", LogStateName(l->state),
                       " but MinLogToKeep = ", N(minKeep)}));
  // ActiveLive: the active log is live
  if (W.a && (W.a->num < minKeep || W.a->state != LogState::kActive))
    v.push_back(Str({"ActiveLive: active log ", N(W.a->num), " is not live"}));
  for (auto& [c, cf] : T.cfs) {
    if (cf.state != CfState::kLive) continue;
    // Alignment (I3): every logged record in a sealed memtable lies in a log before
    // the log the seal started
    for (auto& M : cf.imm)
      for (auto& [ik, val] : M->table) {
        auto it = g.writes.find(ik.seq);
        if (it != g.writes.end() && it->second.logged && it->second.log >= M->nextLog)
          v.push_back(Str({"Alignment (I3): write ", N(ik.seq), " is in log ", N(it->second.log),
                           " but its sealed memtable ends before log ", N(M->nextLog)}));
      }
    // WriteAhead (I1): a logged record in a memtable is in a live log
    auto wa = [&](const MemTable& M) {
      for (auto& [ik, val] : M.table) {
        auto it = g.writes.find(ik.seq);
        if (it == g.writes.end()) {
          v.push_back(Str({"WriteAhead (I1): memtable entry ", N(ik.seq), " was never appended"}));
          continue;
        }
        if (it->second.logged) {
          LogFile* l = W.Find(it->second.log);
          if (l == nullptr || (l->state != LogState::kActive && l->state != LogState::kClosed))
            v.push_back(Str({"WriteAhead (I1): write ", N(ik.seq), " is in a memtable but its log ",
                             N(it->second.log), " is gone"}));
        }
      }
    };
    wa(*cf.mem);
    for (auto& M : cf.imm) wa(*M);
  }
  // TypeOK
  if (T.lastPublished > T.lastAllocated) v.push_back("TypeOK: lastPublished > lastAllocated");

  // ---------------- MemSST.tla ----------------
  std::map<CfId, std::set<std::string>> keys;
  for (auto& h : g.hist) keys[h.cf].insert(h.key);
  for (auto& [s, w] : g.writes) keys[w.cf].insert(w.key);
  for (auto& [c, ks] : keys) {
    if (!T.IsReadable(c)) continue;
    for (auto& k : ks) {
      // MS1 ReadLatest: a read returns the latest committed value
      std::string want;
      bool has = g.Expected(c, k, g.clock, &want);
      LookupResult got = T.Get(c, k, T.lastPublished);
      bool gotHas = got.kind == LookupResult::kFound;
      if (has != gotHas || (has && got.value != want))
        v.push_back(Str({"MS1 ReadLatest: CF ", N(c), " key ", k, " reads ",
                         gotHas ? "'" + got.value + "'" : "nothing", ", expected ",
                         has ? "'" + want + "'" : "nothing"}));
      // MS2 SnapshotRead: a snapshot keeps showing what was committed when it was taken
      for (auto& [snap, st] : g.snaps) {
        std::string w2;
        bool h2 = g.Expected(c, k, st.second, &w2);
        LookupResult x = T.Get(c, k, st.first);
        bool xh = x.kind == LookupResult::kFound;
        if (h2 != xh || (h2 && x.value != w2))
          v.push_back(Str({"MS2 SnapshotRead: CF ", N(c), " key ", k, " at snapshot ", N(st.first), " reads ",
                           xh ? "'" + x.value + "'" : "nothing", ", expected ", h2 ? "'" + w2 + "'" : "nothing"}));
      }
    }
  }
  for (auto& [c, cf] : T.cfs) {
    if (cf.state != CfState::kLive && cf.state != CfState::kDropped) continue;
    // MS3 NewerShadowsOlder: along the read path a later component never holds a
    // newer version of a key than an earlier one
    std::vector<std::vector<std::pair<std::string, SeqNo>>> comps;
    auto fromMem = [](const MemTable& M) {
      std::vector<std::pair<std::string, SeqNo>> out;
      for (auto& [ik, val] : M.table) out.push_back({ik.key, ik.seq});
      return out;
    };
    comps.push_back(fromMem(*cf.mem));
    for (auto it = cf.imm.rbegin(); it != cf.imm.rend(); ++it) comps.push_back(fromMem(**it));
    for (auto& f : cf.l0) {
      std::vector<std::pair<std::string, SeqNo>> out;
      for (auto& e : f->entries) out.push_back({e.key, e.seq});
      comps.push_back(out);
    }
    for (int l = 1; l < static_cast<int>(cf.levels.size()); l++) {
      std::vector<std::pair<std::string, SeqNo>> out;
      for (auto& f : cf.levels[l])
        for (auto& e : f->entries) out.push_back({e.key, e.seq});
      comps.push_back(out);
    }
    std::map<std::string, SeqNo> minSoFar;
    for (size_t i = 0; i < comps.size(); i++) {
      std::map<std::string, std::pair<SeqNo, SeqNo>> mm;  // key -> (min, max) in this component
      for (auto& [k, s] : comps[i]) {
        auto it = mm.find(k);
        if (it == mm.end()) mm[k] = {s, s};
        else { it->second.first = std::min(it->second.first, s); it->second.second = std::max(it->second.second, s); }
      }
      for (auto& [k, mx] : mm) {
        auto it = minSoFar.find(k);
        if (it != minSoFar.end() && mx.second > it->second)
          v.push_back(Str({"MS3 NewerShadowsOlder: CF ", N(c), " key ", k, " has seq ", N(mx.second),
                           " in read-path component ", N(i), " below an older seq ", N(it->second)}));
      }
      for (auto& [k, mx] : mm) {
        auto it = minSoFar.find(k);
        if (it == minSoFar.end() || mx.first < it->second) minSoFar[k] = mx.first;
      }
    }
    // MS6 LevelsDisjoint: files in L1 and deeper have disjoint key ranges
    for (int l = 1; l < static_cast<int>(cf.levels.size()); l++) {
      auto& fs = cf.levels[l];
      for (size_t i = 0; i < fs.size(); i++)
        for (size_t j = i + 1; j < fs.size(); j++)
          if (fs[i]->Overlaps(fs[j]->lo, fs[j]->hi))
            v.push_back(Str({"MS6 LevelsDisjoint: CF ", N(c), " level ", N(l), " files ", N(fs[i]->number),
                             " and ", N(fs[j]->number), " overlap"}));
    }
    // MS7 LogNumberSafe: N_c never runs ahead of the log in which any of c's
    // unflushed memtables began
    if (cf.state == CfState::kLive) {
      if (cf.N > cf.mem->memLog)
        v.push_back(Str({"MS7 LogNumberSafe: CF ", N(c), " N_c = ", N(cf.N), " > memLog of the mutable memtable ",
                         N(cf.mem->memLog)}));
      for (auto& M : cf.imm)
        if (cf.N > M->memLog)
          v.push_back(Str({"MS7 LogNumberSafe: CF ", N(c), " N_c = ", N(cf.N),
                           " > memLog of an unflushed sealed memtable ", N(M->memLog)}));
    }
  }
  // MS4 BatchAtomic: a published batch is in every CF it writes
  for (auto& [id, b] : g.batches)
    if (b.first > 0 && b.last <= T.lastPublished && b.inserted != b.cfs)
      v.push_back(Str({"MS4 BatchAtomic: batch ", N(id), " (seq ", N(b.first), "..", N(b.last),
                       ") is visible but not inserted into every CF"}));
  // MS5 IngestAllOrNothing: an ingestion lands in all of its CFs or in none
  for (auto& [id, j] : g.jobs)
    if (!j.done.empty() && j.done != j.cfs)
      v.push_back(Str({"MS5 IngestAllOrNothing: ingestion ", N(id), " is in ", N(j.done.size()), " of ",
                       N(j.cfs.size()), " CFs"}));
  return v;
}

}  // namespace lsm
