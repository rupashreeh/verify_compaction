// common.h: basic types, Status, encoding helpers, CRC32, and a small RNG.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>

namespace lsm {

using SeqNo = uint64_t;   // sequence number (WAL2 assigns them)
using LogNum = uint64_t;  // log file number (num(l) in the spec)
using CfId = uint32_t;    // column family id

constexpr CfId kDefaultCf = 0;

enum class ValueType : uint8_t { kDelete = 0, kPut = 1 };

// One version of a key: (k, s, type, v). Used in memtables, SSTs, and log records.
struct Entry {
  std::string key;
  SeqNo seq = 0;
  ValueType type = ValueType::kPut;
  std::string value;
};

class Status {
 public:
  enum Code { kOk, kNotFound, kCorruption, kInvalidArgument, kIOError, kBusy, kIncomplete };
  Status() = default;
  static Status OK() { return Status(); }
  static Status NotFound(std::string m = "") { return Status(kNotFound, std::move(m)); }
  static Status Corruption(std::string m) { return Status(kCorruption, std::move(m)); }
  static Status InvalidArgument(std::string m) { return Status(kInvalidArgument, std::move(m)); }
  static Status IOError(std::string m) { return Status(kIOError, std::move(m)); }
  // Busy means "a precondition is not met yet; a real store would wait here".
  static Status Busy(std::string m) { return Status(kBusy, std::move(m)); }
  static Status Incomplete(std::string m) { return Status(kIncomplete, std::move(m)); }
  bool ok() const { return code_ == kOk; }
  bool IsBusy() const { return code_ == kBusy; }
  bool IsNotFound() const { return code_ == kNotFound; }
  bool IsCorruption() const { return code_ == kCorruption; }
  bool IsInvalidArgument() const { return code_ == kInvalidArgument; }
  bool IsIOError() const { return code_ == kIOError; }
  bool IsIncomplete() const { return code_ == kIncomplete; }
  Code code() const { return code_; }
  std::string ToString() const {
    static const char* names[] = {"OK", "NotFound", "Corruption", "InvalidArgument",
                                  "IOError", "Busy", "Incomplete"};
    return msg_.empty() ? names[code_] : std::string(names[code_]) + ": " + msg_;
  }

 private:
  Status(Code c, std::string m) : code_(c), msg_(std::move(m)) {}
  Code code_ = kOk;
  std::string msg_;
};

// ---- encoding (little endian, length-prefixed strings) ----
inline void PutFixed32(std::string* d, uint32_t v) { d->append(reinterpret_cast<const char*>(&v), 4); }
inline void PutFixed64(std::string* d, uint64_t v) { d->append(reinterpret_cast<const char*>(&v), 8); }
inline void PutStr(std::string* d, const std::string& s) {
  PutFixed32(d, static_cast<uint32_t>(s.size()));
  d->append(s);
}

// A cursor over a byte string. Every Get* returns false if the input is too short.
class Reader {
 public:
  Reader(const std::string& d, size_t pos = 0, size_t end = std::string::npos)
      : d_(d), pos_(pos), end_(end == std::string::npos ? d.size() : end) {}
  bool Fixed32(uint32_t* v) { return Raw(v, 4); }
  bool Fixed64(uint64_t* v) { return Raw(v, 8); }
  bool Byte(uint8_t* v) { return Raw(v, 1); }
  bool Str(std::string* s) {
    uint32_t n;
    if (!Fixed32(&n) || end_ - pos_ < n) return false;
    s->assign(d_, pos_, n);
    pos_ += n;
    return true;
  }
  size_t pos() const { return pos_; }
  size_t remaining() const { return end_ - pos_; }

 private:
  bool Raw(void* out, size_t n) {
    if (end_ - pos_ < n) return false;
    std::memcpy(out, d_.data() + pos_, n);
    pos_ += n;
    return true;
  }
  const std::string& d_;
  size_t pos_, end_;
};

inline uint32_t Crc32(const char* p, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = table[(c ^ static_cast<uint8_t>(p[i])) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}
inline uint32_t Crc32(const std::string& s) { return Crc32(s.data(), s.size()); }

// Deterministic xorshift RNG so that every simulated crash can be replayed from a seed.
class Rng {
 public:
  explicit Rng(uint64_t seed) : s_(seed * 0x9E3779B97F4A7C15ull + 1) {}
  uint64_t Next() {
    s_ ^= s_ << 13;
    s_ ^= s_ >> 7;
    s_ ^= s_ << 17;
    return s_;
  }
  uint64_t Uniform(uint64_t n) { return n == 0 ? 0 : Next() % n; }  // [0, n)
  bool OneIn(uint64_t n) { return Uniform(n) == 0; }

 private:
  uint64_t s_;
};

inline std::string NumberedName(const char* dir, uint64_t num, const char* ext) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s/%06llu.%s", dir, static_cast<unsigned long long>(num), ext);
  return buf;
}

}  // namespace lsm
