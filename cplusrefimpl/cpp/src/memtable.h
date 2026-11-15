// memtable.h: the MemTable module (MT1, MT2, MT3) and the SST file (SST1 format).
#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "common.h"
#include "sim_env.h"

namespace lsm {

// Result of a point lookup in one component (MT2, SST Get).
struct LookupResult {
  enum Kind { kNotFound, kFound, kDeleted } kind = kNotFound;
  std::string value;
  SeqNo seq = 0;
};

// Internal key order: key ascending, then sequence number descending, so the
// newest version of a key comes first.
struct IKey {
  std::string key;
  SeqNo seq;
};
struct IKeyCmp {
  bool operator()(const IKey& a, const IKey& b) const {
    if (a.key != b.key) return a.key < b.key;
    return a.seq > b.seq;
  }
};

// ----------------------------------------------------------------------------
// SST file. Keys are sorted (key ascending, seq descending). An external file
// written by SstFileWriter (SST1) has every seq = 0; if globalSeqno > 0, every key
// reads with that sequence number instead.
struct SstFile {
  uint64_t number = 0;
  std::string path;
  std::string lo, hi;          // smallest and largest user key
  SeqNo smallestSeq = 0, largestSeq = 0;
  SeqNo globalSeqno = 0;
  uint64_t epoch = 0;          // install order; L0 is kept newest first by epoch
  SeqNo largestLoggedSeq = 0;  // largest seq that came through the WAL (0 for ingested files)
  std::vector<Entry> entries;  // with effective sequence numbers

  LookupResult Get(const std::string& k, SeqNo s) const;
  bool Overlaps(const std::string& a, const std::string& b) const { return !(hi < a || b < lo); }
  void ApplyGlobalSeqno(SeqNo g);  // sets every entry's seq to g (g > 0)
  void ComputeBounds();
};

// File format: magic(8) | globalSeqno(8) | count(4) | entries | crc32 of the entries (4).
// The crc does not cover the global seqno field, so SST4 can rewrite it in place.
std::string EncodeSst(const std::vector<Entry>& entries, SeqNo globalSeqno);
Status DecodeSst(const std::string& data, std::vector<Entry>* entries, SeqNo* globalSeqno);
Status LoadSst(SimEnv* env, const std::string& path, uint64_t number, std::shared_ptr<SstFile>* out);

// SST1: writes a sorted file outside the store. Keys must be strictly increasing.
// Every key has sequence number 0. (Range deletions are not supported.)
class SstFileWriter {
 public:
  explicit SstFileWriter(SimEnv* env) : env_(env) {}
  Status Open(const std::string& path);
  Status Put(const std::string& k, const std::string& v) { return Add(k, ValueType::kPut, v); }
  Status Delete(const std::string& k) { return Add(k, ValueType::kDelete, ""); }
  Status Finish();  // writes and syncs the file

 private:
  Status Add(const std::string& k, ValueType t, const std::string& v);
  SimEnv* env_;
  std::string path_;
  bool open_ = false, hasLast_ = false;
  std::string last_;
  std::vector<Entry> entries_;
};

// ----------------------------------------------------------------------------
// MemTable. Its life cycle is Mutable -> Sealed -> Building -> Built -> installed.
enum class MemState { kMutable, kSealed, kBuilding, kBuilt };

class MemTable {
 public:
  MemTable(uint64_t id, LogNum memLog, size_t writeBufferSize)
      : id(id), memLog(memLog), writeBufferSize_(writeBufferSize) {}

  void Insert(const Entry& r, bool logged = true);              // MT1
  LookupResult Get(const std::string& k, SeqNo s) const;        // MT2
  bool ShouldFlush() const { return bytes >= writeBufferSize_; } // MT3
  bool IsEmpty() const { return table.empty(); }                // MT3
  std::vector<Entry> Entries() const;                           // sorted

  const uint64_t id;
  LogNum memLog;       // the log that was active when this memtable was created
  LogNum nextLog = 0;  // set by MT4: every logged write in it lies in logs < nextLog
  MemState state = MemState::kMutable;
  size_t bytes = 0;
  SeqNo firstSeq = UINT64_MAX, largestSeq = 0;
  SeqNo largestLoggedSeq = 0;       // ignores disableWAL writes
  uint64_t jobId = 0;               // the flush job building it (MT5)
  std::shared_ptr<SstFile> output;  // set when Built
  std::map<IKey, std::pair<ValueType, std::string>, IKeyCmp> table;

 private:
  size_t writeBufferSize_;
};

}  // namespace lsm
