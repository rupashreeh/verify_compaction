// block_cache.h: the block cache (BC1 to BC8): a sharded LRU cache with pinning,
// a high-priority pool, and an optional strict capacity limit, as RocksDB's
// LRUCache.
//
// The cache holds blocks of SST files (and the write buffer manager's dummy
// entries). It knows nothing of files: a caller gives a key, a value, and a charge.
// The table reader (table_reader.h, BC9 to BC11) builds the keys and decides what
// to insert.
//
// An entry is pinned while some caller holds a handle to it (Lookup, or Insert with
// a handle). Pinned entries are never evicted. Entries with no pins sit on the
// shard's LRU list, which has a low-priority part (evicted first) and a
// high-priority pool. Each shard has its own capacity, list, and table.
//
// The store runs in one thread (see db.h), so the per-shard mutex of RocksDB is
// not needed. CheckInvariants checks CH1, CH3, CH5, CH6, and CH7 of
// block_cache/README.md; CH2 and CH4 are reported when they happen.
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common.h"
#include "options.h"

namespace lsm {

enum class CachePriority { kLow, kHigh };
// Who put an entry in the cache. Only used by the checks (CH5).
enum class CacheOrigin { kUser, kCompaction, kFlush, kWbm };

// One cache entry (RocksDB's LRUHandle).
struct LRUHandle {
  std::string key;
  std::shared_ptr<const void> value;
  size_t charge = 0;
  uint32_t hash = 0;
  uint32_t refs = 0;      // pins: handles given out and not yet released
  uint32_t holders = 0;   // handles their holders still believe they hold (CH6)
  bool inCache = false;   // in the hash table
  bool isHighPri = false; // inserted with high priority
  bool hit = false;       // looked up at least once
  bool inHighPool = false;
  bool onLru = false;
  bool freed = false;     // freed while pinned: only possible with a Fix* switch off
  CacheOrigin origin = CacheOrigin::kUser;
  LRUHandle* next = nullptr;
  LRUHandle* prev = nullptr;
};

// Statistics (RocksDB's BLOCK_CACHE_* tickers that the cache itself counts).
struct CacheStats {
  uint64_t lookups = 0, hits = 0, misses = 0;
  uint64_t adds = 0, addFailures = 0;
  uint64_t evictions = 0;
};

// Switches for the mechanisms inside the cache (the TLA+ Fix* constants).
struct CacheFixes {
  bool FixLookupUnpins = true;     // BC2: a hit takes the entry off the LRU list
  bool FixDetachOnReplace = true;  // BC3: a replaced entry that is pinned is detached, not freed
  bool FixEraseOverCap = true;     // BC4: a last release while over capacity erases the entry
  bool FixStrictRefuse = true;     // BC3: strict_capacity_limit refuses an insert that does not fit
  bool FixMaintainPool = true;     // BC6: the high-priority pool spills its oldest entries
};

class LRUCacheShard {
 public:
  LRUCacheShard() { lru_.next = lru_.prev = &lru_; lruLowPri_ = &lru_; }
  ~LRUCacheShard();
  void Configure(size_t capacity, bool strict, double highRatio, const CacheFixes* fix,
                 std::vector<std::string>* report);
  Status Insert(const std::string& key, uint32_t hash, std::shared_ptr<const void> value, size_t charge,
                CachePriority pri, CacheOrigin origin, LRUHandle** handle);   // BC3
  LRUHandle* Lookup(const std::string& key);                                  // BC2
  bool Release(LRUHandle* e, bool eraseIfLastRef);                            // BC4
  void Forget(LRUHandle* e);           // a holder drops a handle without Release (a bug variant)
  void Erase(const std::string& key);  // BC5
  void EraseUnRefEntries();            // BC5
  void SetCapacity(size_t capacity);   // BC8
  void SetStrictCapacityLimit(bool v) { strict_ = v; }
  size_t Capacity() const { return capacity_; }
  size_t Usage() const { return usage_; }
  size_t PinnedUsage() const { return usage_ - lruUsage_; }
  size_t HighPriPoolUsage() const { return highUsage_; }
  size_t HighPriPoolCapacity() const { return highCap_; }
  size_t Entries() const { return table_.size(); }
  void Check(int shard, std::vector<std::string>* v) const;
  CacheStats stats;

 private:
  void LruRemove(LRUHandle* e);                                    // BC6
  void LruInsert(LRUHandle* e);                                    // BC6
  void MaintainPoolSize();                                         // BC6
  void EvictFromLRU(size_t charge, std::vector<LRUHandle*>* out);  // BC7
  void FreePinned(LRUHandle* e, const char* why);
  void Delete(LRUHandle* e);

  size_t capacity_ = 0, highCap_ = 0;
  double highRatio_ = 0;
  bool strict_ = false, lowered_ = false;
  size_t usage_ = 0, lruUsage_ = 0, highUsage_ = 0;
  LRUHandle lru_;            // lru_.next is the oldest entry, lru_.prev the newest
  LRUHandle* lruLowPri_;     // newest entry of the low-priority part (or &lru_)
  std::unordered_map<std::string, LRUHandle*> table_;
  std::unordered_set<LRUHandle*> detached_;  // out of the table but pinned
  std::unordered_set<LRUHandle*> zombies_;   // freed while pinned (bug variants), kept to stay memory safe
  const CacheFixes* fix_ = nullptr;
  std::vector<std::string>* report_ = nullptr;
};

// BC1: the sharded cache. RocksDB: NewLRUCache(capacity, num_shard_bits,
// strict_capacity_limit, high_pri_pool_ratio).
class BlockCache {
 public:
  using Handle = LRUHandle;
  // num_shard_bits < 0 picks the default: shards of at least 512 KB, at most 64 shards.
  explicit BlockCache(size_t capacity, bool strict_capacity_limit = false, int num_shard_bits = -1,
                      double high_pri_pool_ratio = 0.5);
  static int DefaultShardBits(size_t capacity);

  Status Insert(const std::string& key, std::shared_ptr<const void> value, size_t charge,
                CachePriority pri = CachePriority::kLow, Handle** handle = nullptr,
                CacheOrigin origin = CacheOrigin::kUser);
  Handle* Lookup(const std::string& key);
  bool Release(Handle* h, bool eraseIfLastRef = false);
  void Forget(Handle* h);
  void Erase(const std::string& key);
  void EraseUnRefEntries();
  void SetCapacity(size_t capacity);
  void SetStrictCapacityLimit(bool v);
  static const void* Value(Handle* h) { return h->value.get(); }

  size_t GetCapacity() const { return capacity_; }
  size_t GetUsage() const;
  size_t GetPinnedUsage() const;
  size_t GetHighPriPoolUsage() const;
  size_t GetEntries() const;
  int NumShards() const { return static_cast<int>(shards_.size()); }
  int ShardOf(const std::string& key) const;
  CacheStats Stats() const;
  const LRUCacheShard& Shard(int i) const { return *shards_[i]; }

  // Each DB that opens takes a new session id. Cache keys start with it, so a file
  // number that is used again (another DB, or the same DB after a crash) gets new
  // keys (BC10).
  static std::string NewSessionId();

  // CH1, CH3, CH5, CH6, CH7 now, plus CH2 and CH4 violations reported earlier.
  std::vector<std::string> CheckInvariants() const;
  void Report(const std::string& v) { report_.push_back(v); }

  CacheFixes fix;

 private:
  static uint32_t Hash(const std::string& key);
  size_t capacity_;
  int bits_;
  std::vector<std::unique_ptr<LRUCacheShard>> shards_;  // never moved: the LRU lists point into them
  std::vector<std::string> report_;
};

}  // namespace lsm
