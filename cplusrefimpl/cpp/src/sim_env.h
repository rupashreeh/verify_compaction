// sim_env.h: a simulated file system with the crash model of WAL_EndToEnd.tla.
//
// Every file has three layers:
//   data        bytes the process has handed to the OS (survive a process crash)
//   synced      length of the prefix forced onto disk (survives a machine crash)
//   dirSynced   whether the file's directory entry is on disk
//
// A machine crash keeps each file's synced prefix plus any part of its unsynced
// suffix, possibly ending in garbage (a torn record). A file whose directory entry
// was never synced may vanish entirely. This matches the Crash and SettleLog
// actions of WAL_EndToEnd.tla, at the byte level.
#pragma once
#include <map>
#include <string>
#include <vector>

#include "common.h"

namespace lsm {

class SimEnv {
 public:
  struct File {
    std::string data;
    size_t synced = 0;
    bool dirSynced = false;
  };

  bool Exists(const std::string& path) const { return files_.count(path) > 0; }
  // Creates the file if needed. A new file's directory entry is not yet durable.
  Status Append(const std::string& path, const std::string& bytes);
  Status Sync(const std::string& path);      // fsync: data is durable up to its current size
  Status SyncDir();                          // fsync of the directory: every name is durable
  Status Read(const std::string& path, std::string* out) const;
  Status Delete(const std::string& path);
  Status Rename(const std::string& from, const std::string& to);
  Status Truncate(const std::string& path, size_t len);
  // Writes a whole file and syncs it and the directory (used for SSTs and copies).
  Status WriteFileDurably(const std::string& path, const std::string& bytes);
  std::vector<std::string> List(const std::string& dirPrefix) const;  // names under "dir/"
  const File* Get(const std::string& path) const;

  // ---- crashes ----
  // Process crash: data handed to the OS survives. Nothing changes in the env.
  void ProcessCrash() {}
  // Machine crash with random choices, as in SettleLog. keepUnsyncedProb is the chance
  // that some of a file's unsynced bytes survive; tornProb the chance that what
  // survives ends in garbage; vanishProb the chance that a file with an unsynced
  // directory entry disappears.
  void MachineCrash(Rng& rng, int keepUnsyncedPct = 50, int tornPct = 30, int vanishPct = 50);
  // Worst case: every file keeps exactly its synced prefix, and every file whose
  // directory entry was not synced vanishes.
  void MachineCrashDropUnsynced();
  // A crash that leaves exactly `len` bytes of one file on disk (for hand-made torn
  // writes and WAL holes). What survives is durable, as after any crash.
  void CrashTruncate(const std::string& path, size_t len);

  // ---- fault injection ----
  // The next `count` operations of the given kind on a path that starts with `prefix` fail.
  enum class Op { kAppend, kSync };
  void InjectFault(Op op, const std::string& prefix, int count = 1) { faults_.push_back({op, prefix, count}); }

  size_t syncCount = 0, dirSyncCount = 0;  // for tests that check I/O was done

 private:
  bool Faulty(Op op, const std::string& path);
  struct Fault {
    Op op;
    std::string prefix;
    int count;
  };
  std::map<std::string, File> files_;
  std::vector<Fault> faults_;
};

}  // namespace lsm
