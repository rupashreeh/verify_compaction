// manifest.h: VersionEdit and the MANIFEST file (contract K4).
//
// The MANIFEST is a log of edits. LogAndApply appends one edit as one checksummed
// record and syncs it, so each edit is atomic and durable. On open, a torn last
// record is ignored: it was never acknowledged.
#pragma once
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "common.h"
#include "memtable.h"
#include "sim_env.h"

namespace lsm {

struct FileAdd {
  CfId cf;
  int level;
  uint64_t number;
  SeqNo globalSeqno;  // kept in the MANIFEST even when write_global_seqno is off
  uint64_t epoch;
  SeqNo largestLoggedSeq;
  std::shared_ptr<SstFile> file;  // in memory only; loaded from disk on recovery
};

struct VersionEdit {
  std::vector<std::pair<CfId, std::string>> addCf;
  std::vector<CfId> dropCf;
  std::vector<std::pair<CfId, LogNum>> logNumber;  // N_c
  std::vector<FileAdd> addFile;
  std::vector<std::tuple<CfId, int, uint64_t>> delFile;
  std::optional<SeqNo> lastSequence;
  std::optional<uint64_t> nextFileNumber;
  std::optional<CfId> nextCfId;

  bool empty() const {
    return addCf.empty() && dropCf.empty() && logNumber.empty() && addFile.empty() &&
           delFile.empty() && !lastSequence && !nextFileNumber && !nextCfId;
  }
  std::string Encode() const;
  bool Decode(const std::string& s);
};

constexpr const char* kManifestPath = "MANIFEST";

// Appends one edit and syncs it. A new MANIFEST also gets its directory entry synced.
Status ManifestAppend(SimEnv* env, const VersionEdit& e);
// Reads every intact edit, in order.
Status ManifestRead(SimEnv* env, std::vector<VersionEdit>* edits);

}  // namespace lsm
