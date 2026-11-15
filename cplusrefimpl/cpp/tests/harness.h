// harness.h: a tiny test framework and helpers shared by the tests.
#pragma once
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "db.h"
#include "ghost.h"

namespace testing {

struct Case {
  const char* name;
  std::function<void()> fn;
};
inline std::vector<Case>& Cases() {
  static std::vector<Case> c;
  return c;
}
inline int& Failures() {
  static int f = 0;
  return f;
}
struct Register {
  Register(const char* n, std::function<void()> f) { Cases().push_back({n, std::move(f)}); }
};

#define TEST(name)                                                   \
  static void name();                                                \
  static ::testing::Register reg_##name(#name, name);                \
  static void name()

#define CHECK(cond)                                                                    \
  do {                                                                                 \
    if (!(cond)) {                                                                     \
      std::printf("    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
      ::testing::Failures()++;                                                         \
      return;                                                                          \
    }                                                                                  \
  } while (0)

#define CHECK_OK(expr)                                                                 \
  do {                                                                                 \
    ::lsm::Status _s = (expr);                                                         \
    if (!_s.ok()) {                                                                    \
      std::printf("    FAILED %s:%d: %s -> %s\n", __FILE__, __LINE__, #expr,           \
                  _s.ToString().c_str());                                              \
      ::testing::Failures()++;                                                         \
      return;                                                                          \
    }                                                                                  \
  } while (0)

inline int RunAll() {
  for (auto& c : Cases()) {
    int before = Failures();
    c.fn();
    std::printf("%s %s\n", Failures() == before ? "  ok  " : "  FAIL", c.name);
  }
  std::printf("%zu tests, %d failed\n", Cases().size(), Failures());
  return Failures() == 0 ? 0 : 1;
}

}  // namespace testing

// ---- helpers ---------------------------------------------------------------
namespace lsm {

// A store under test: the simulated disk, the ghost, and the open DB.
struct Fixture {
  SimEnv env;
  Ghost ghost;
  Options opts;
  std::unique_ptr<DB> db;
  std::vector<CfId> cf;  // handles, in the order of names
  std::vector<std::string> names{"default"};

  Fixture() {
    opts.ghost = &ghost;
    opts.check_invariants = true;
  }
  Status Open() {
    db.reset();
    return DB::Open(&env, opts, names, &db, &cf);
  }
  // A process crash, then (if machine) a power failure, then recovery.
  Status Crash(bool machine, Rng* rng = nullptr) {
    db.reset();
    if (machine) {
      if (rng) env.MachineCrash(*rng);
      else env.MachineCrashDropUnsynced();
    }
    ghost.OnCrash();
    return Open();
  }
  std::string Get(CfId c, const std::string& k, const Snapshot* s = nullptr) {
    std::string v;
    Status st = db->Get(c, k, &v, s);
    return st.ok() ? v : (st.IsNotFound() ? "<none>" : "<" + st.ToString() + ">");
  }
  bool Clean() const { return ghost.violations.empty(); }
  bool Violated(const std::string& prefix) const {
    for (auto& v : ghost.violations)
      if (v.find(prefix) != std::string::npos) return true;
    return false;
  }
  void PrintViolations() const {
    for (auto& v : ghost.violations) std::printf("      violation: %s\n", v.c_str());
  }
};

inline WriteOptions Sync() { WriteOptions o; o.sync = true; return o; }
inline WriteOptions NoWal() { WriteOptions o; o.disableWAL = true; return o; }

}  // namespace lsm

namespace lsm {
inline void DumpCf(const DB& db, CfId c, const std::string& key) {
  const ColumnFamily* cf = db.tree.Cf(c);
  if (!cf) return;
  auto pm = [&](const char* n, const MemTable& m) {
    std::printf("      %s memLog=%llu next=%llu st=%d:", n, (unsigned long long)m.memLog,
                (unsigned long long)m.nextLog, (int)m.state);
    for (auto& [ik, v] : m.table)
      if (key.empty() || ik.key == key) std::printf(" %s@%llu=%s", ik.key.c_str(), (unsigned long long)ik.seq, v.second.c_str());
    std::printf("\n");
  };
  std::printf("      CF %u N=%llu state=%d\n", c, (unsigned long long)cf->N, (int)cf->state);
  pm("mem", *cf->mem);
  for (auto& m : cf->imm) pm("imm", *m);
  for (auto& f : cf->l0) {
    std::printf("      L0 #%llu ep=%llu g=%llu:", (unsigned long long)f->number, (unsigned long long)f->epoch, (unsigned long long)f->globalSeqno);
    for (auto& e : f->entries) if (key.empty() || e.key == key) std::printf(" %s@%llu=%s", e.key.c_str(), (unsigned long long)e.seq, e.value.c_str());
    std::printf("\n");
  }
  for (size_t l = 1; l < cf->levels.size(); l++)
    for (auto& f : cf->levels[l]) {
      std::printf("      L%zu #%llu:", l, (unsigned long long)f->number);
      for (auto& e : f->entries) if (key.empty() || e.key == key) std::printf(" %s@%llu=%s", e.key.c_str(), (unsigned long long)e.seq, e.value.c_str());
      std::printf("\n");
    }
  if (db.opts.ghost)
    for (auto& h : db.opts.ghost->hist)
      if (h.cf == c && (key.empty() || h.key == key))
        std::printf("      hist t=%llu seq=%llu %s=%s%s\n", (unsigned long long)h.t, (unsigned long long)h.seq, h.key.c_str(), h.del ? "<del>" : h.value.c_str(), h.ingest ? " (ingest)" : "");
}
}  // namespace lsm
