#include "sim_env.h"

namespace lsm {

bool SimEnv::Faulty(Op op, const std::string& path) {
  for (auto& f : faults_) {
    if (f.op == op && f.count > 0 && path.compare(0, f.prefix.size(), f.prefix) == 0) {
      f.count--;
      return true;
    }
  }
  return false;
}

Status SimEnv::Append(const std::string& path, const std::string& bytes) {
  if (Faulty(Op::kAppend, path)) return Status::IOError("injected append failure: " + path);
  files_[path].data.append(bytes);
  return Status::OK();
}

Status SimEnv::Sync(const std::string& path) {
  auto it = files_.find(path);
  if (it == files_.end()) return Status::IOError("sync of missing file " + path);
  if (Faulty(Op::kSync, path)) return Status::IOError("injected sync failure: " + path);
  it->second.synced = it->second.data.size();
  syncCount++;
  return Status::OK();
}

Status SimEnv::SyncDir() {
  for (auto& [name, f] : files_) f.dirSynced = true;
  dirSyncCount++;
  return Status::OK();
}

Status SimEnv::Read(const std::string& path, std::string* out) const {
  auto it = files_.find(path);
  if (it == files_.end()) return Status::NotFound(path);
  *out = it->second.data;
  return Status::OK();
}

Status SimEnv::Delete(const std::string& path) {
  files_.erase(path);  // deletions are treated as durable at once
  return Status::OK();
}

Status SimEnv::Rename(const std::string& from, const std::string& to) {
  auto it = files_.find(from);
  if (it == files_.end()) return Status::NotFound(from);
  File f = it->second;
  files_.erase(it);
  f.dirSynced = false;  // the new name is durable only after the next SyncDir
  files_[to] = f;
  return Status::OK();
}

Status SimEnv::Truncate(const std::string& path, size_t len) {
  auto it = files_.find(path);
  if (it == files_.end()) return Status::NotFound(path);
  if (len < it->second.data.size()) it->second.data.resize(len);
  if (it->second.synced > len) it->second.synced = len;
  return Status::OK();
}

Status SimEnv::WriteFileDurably(const std::string& path, const std::string& bytes) {
  files_.erase(path);
  Status s = Append(path, bytes);
  if (s.ok()) s = Sync(path);
  if (s.ok()) s = SyncDir();
  return s;
}

std::vector<std::string> SimEnv::List(const std::string& dirPrefix) const {
  std::vector<std::string> out;
  std::string p = dirPrefix + "/";
  for (auto& [name, f] : files_)
    if (name.compare(0, p.size(), p) == 0) out.push_back(name);
  return out;
}

const SimEnv::File* SimEnv::Get(const std::string& path) const {
  auto it = files_.find(path);
  return it == files_.end() ? nullptr : &it->second;
}

void SimEnv::MachineCrash(Rng& rng, int keepUnsyncedPct, int tornPct, int vanishPct) {
  for (auto it = files_.begin(); it != files_.end();) {
    File& f = it->second;
    if (!f.dirSynced && static_cast<int>(rng.Uniform(100)) < vanishPct) {
      it = files_.erase(it);  // the directory entry never reached the disk
      continue;
    }
    size_t unsynced = f.data.size() - f.synced;
    size_t keep = f.synced;
    if (unsynced > 0 && static_cast<int>(rng.Uniform(100)) < keepUnsyncedPct)
      keep += rng.Uniform(unsynced + 1);
    f.data.resize(keep);
    if (keep > f.synced && static_cast<int>(rng.Uniform(100)) < tornPct) {
      size_t at = f.synced + rng.Uniform(keep - f.synced);
      f.data[at] = static_cast<char>(f.data[at] ^ 0x5A);  // garbage in the unsynced part
    }
    f.synced = f.data.size();  // what survived is on disk now
    f.dirSynced = true;
    ++it;
  }
}

void SimEnv::MachineCrashDropUnsynced() {
  for (auto it = files_.begin(); it != files_.end();) {
    if (!it->second.dirSynced) {
      it = files_.erase(it);
      continue;
    }
    it->second.data.resize(it->second.synced);
    ++it;
  }
}

void SimEnv::CrashTruncate(const std::string& path, size_t len) {
  auto it = files_.find(path);
  if (it == files_.end()) return;
  if (len < it->second.data.size()) it->second.data.resize(len);
  it->second.synced = it->second.data.size();
  it->second.dirSynced = true;
}

}  // namespace lsm
