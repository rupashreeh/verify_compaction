// block_cache.cc: BC1 to BC8, the sharded LRU cache.
#include "block_cache.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace lsm {

// ============================================================ shard set-up
LRUCacheShard::~LRUCacheShard() {
  for (auto& [k, e] : table_) delete e;
  for (LRUHandle* e : detached_) delete e;
  for (LRUHandle* e : zombies_) delete e;
}

void LRUCacheShard::Configure(size_t capacity, bool strict, double highRatio, const CacheFixes* fix,
                              std::vector<std::string>* report) {
  capacity_ = capacity;
  strict_ = strict;
  highRatio_ = highRatio;
  highCap_ = static_cast<size_t>(capacity * highRatio);
  fix_ = fix;
  report_ = report;
}

void LRUCacheShard::Delete(LRUHandle* e) {
  detached_.erase(e);
  zombies_.erase(e);
  delete e;
}

// A bug variant frees an entry that someone still pins. The entry is kept (so the
// program stays memory safe) but counts as freed, and CH2 is reported.
void LRUCacheShard::FreePinned(LRUHandle* e, const char* why) {
  report_->push_back(std::string("CH2 PinSafety: ") + why + " freed block " + e->key + " while it was pinned");
  e->freed = true;
  e->inCache = false;
  detached_.erase(e);
  zombies_.insert(e);
}

// ============================================================ BC6 the two-part LRU list
void LRUCacheShard::LruRemove(LRUHandle* e) {
  if (lruLowPri_ == e) lruLowPri_ = e->prev;
  e->next->prev = e->prev;
  e->prev->next = e->next;
  e->prev = e->next = nullptr;
  e->onLru = false;
  lruUsage_ -= e->charge;
  if (e->inHighPool) {
    highUsage_ -= e->charge;
    e->inHighPool = false;
  }
}

void LRUCacheShard::LruInsert(LRUHandle* e) {
  if (highRatio_ > 0 && (e->isHighPri || e->hit)) {
    // the head of the list (the newest end), in the high-priority pool
    e->next = &lru_;
    e->prev = lru_.prev;
    e->prev->next = e;
    e->next->prev = e;
    e->inHighPool = true;
    highUsage_ += e->charge;
    MaintainPoolSize();
  } else {
    // the head of the low-priority part
    e->next = lruLowPri_->next;
    e->prev = lruLowPri_;
    e->prev->next = e;
    e->next->prev = e;
    e->inHighPool = false;
    lruLowPri_ = e;
  }
  e->onLru = true;
  lruUsage_ += e->charge;
}

void LRUCacheShard::MaintainPoolSize() {
  if (!fix_->FixMaintainPool) return;
  while (highUsage_ > highCap_) {
    // the oldest entry of the pool becomes the newest of the low-priority part
    lruLowPri_ = lruLowPri_->next;
    lruLowPri_->inHighPool = false;
    highUsage_ -= lruLowPri_->charge;
  }
}

// ============================================================ BC7 EvictFromLRU
void LRUCacheShard::EvictFromLRU(size_t charge, std::vector<LRUHandle*>* out) {
  while (usage_ + charge > capacity_ && lru_.next != &lru_) {
    LRUHandle* old = lru_.next;  // the oldest entry
    LruRemove(old);
    table_.erase(old->key);
    old->inCache = false;
    usage_ -= old->charge;
    stats.evictions++;
    if (old->refs > 0) FreePinned(old, "eviction");  // only if a hit left it on the list
    else out->push_back(old);
  }
}

// ============================================================ BC3 Insert
Status LRUCacheShard::Insert(const std::string& key, uint32_t hash, std::shared_ptr<const void> value,
                             size_t charge, CachePriority pri, CacheOrigin origin, LRUHandle** handle) {
  std::vector<LRUHandle*> deleted;
  EvictFromLRU(charge, &deleted);
  bool strictRefuse = strict_ && fix_->FixStrictRefuse;
  Status s;
  if (usage_ + charge > capacity_ && (handle == nullptr || strictRefuse)) {
    if (handle == nullptr) {
      // RocksDB: the insert "succeeds", and the entry is freed at once.
    } else {
      *handle = nullptr;
      s = Status::Incomplete("Insert failed due to LRU cache being full");
    }
    stats.addFailures++;
  } else {
    auto* e = new LRUHandle;
    e->key = key;
    e->hash = hash;
    e->value = std::move(value);
    e->charge = charge;
    e->isHighPri = pri == CachePriority::kHigh;
    e->origin = origin;
    e->inCache = true;
    usage_ += charge;
    auto it = table_.find(key);
    if (it != table_.end()) {
      LRUHandle* old = it->second;
      old->inCache = false;
      if (old->onLru) LruRemove(old);
      if (old->refs == 0) {
        usage_ -= old->charge;
        deleted.push_back(old);
      } else if (fix_->FixDetachOnReplace) {
        detached_.insert(old);  // freed by its last Release
      } else {
        usage_ -= old->charge;
        FreePinned(old, "a replacing insert");
      }
      it->second = e;
    } else {
      table_[key] = e;
    }
    if (handle == nullptr) {
      LruInsert(e);
    } else {
      e->refs = 1;
      e->holders = 1;
      *handle = e;
    }
    stats.adds++;
  }
  for (LRUHandle* d : deleted) Delete(d);
  return s;
}

// ============================================================ BC2 Lookup
LRUHandle* LRUCacheShard::Lookup(const std::string& key) {
  stats.lookups++;
  auto it = table_.find(key);
  if (it == table_.end()) {
    stats.misses++;
    return nullptr;
  }
  LRUHandle* e = it->second;
  if (e->refs == 0 && e->onLru && fix_->FixLookupUnpins) LruRemove(e);
  e->refs++;
  e->holders++;
  e->hit = true;
  stats.hits++;
  return e;
}

// ============================================================ BC4 Release
bool LRUCacheShard::Release(LRUHandle* e, bool eraseIfLastRef) {
  e->holders--;
  e->refs--;
  if (e->refs > 0) return false;
  if (e->freed) {  // a bug variant freed it earlier; usage was already taken off
    Delete(e);
    return true;
  }
  if (e->inCache) {
    if (eraseIfLastRef || (fix_->FixEraseOverCap && usage_ > capacity_)) {
      if (e->onLru) LruRemove(e);
      table_.erase(e->key);
      e->inCache = false;
    } else if (!e->onLru) {
      LruInsert(e);
    }
  }
  if (!e->inCache) {
    if (e->onLru) LruRemove(e);
    usage_ -= e->charge;
    Delete(e);
    return true;
  }
  return false;
}

void LRUCacheShard::Forget(LRUHandle* e) { e->holders--; }

// ============================================================ BC5 Erase
void LRUCacheShard::Erase(const std::string& key) {
  auto it = table_.find(key);
  if (it == table_.end()) return;
  LRUHandle* e = it->second;
  table_.erase(it);
  e->inCache = false;
  if (e->refs == 0) {
    if (e->onLru) LruRemove(e);
    usage_ -= e->charge;
    Delete(e);
  } else {
    if (e->onLru) LruRemove(e);  // only with FixLookupUnpins off
    detached_.insert(e);
  }
}

void LRUCacheShard::EraseUnRefEntries() {
  while (lru_.next != &lru_) {
    LRUHandle* old = lru_.next;
    LruRemove(old);
    table_.erase(old->key);
    old->inCache = false;
    usage_ -= old->charge;
    if (old->refs > 0) FreePinned(old, "EraseUnRefEntries");
    else Delete(old);
  }
}

// ============================================================ BC8 SetCapacity
void LRUCacheShard::SetCapacity(size_t capacity) {
  if (capacity < capacity_) lowered_ = true;
  capacity_ = capacity;
  highCap_ = static_cast<size_t>(capacity * highRatio_);
  std::vector<LRUHandle*> deleted;
  EvictFromLRU(0, &deleted);  // as in RocksDB, the pool is not trimmed here
  for (LRUHandle* d : deleted) Delete(d);
}

// ============================================================ checking
void LRUCacheShard::Check(int shard, std::vector<std::string>* v) const {
  std::string where = "shard " + std::to_string(shard) + ": ";
  // CH1 exact accounting, and the LRU list holds exactly the unpinned entries
  size_t real = 0, lru = 0, high = 0, onList = 0;
  for (auto& [k, e] : table_) real += e->charge;
  for (LRUHandle* e : detached_) real += e->charge;
  for (LRUHandle* e = lru_.next; e != &lru_; e = e->next) {
    lru += e->charge;
    onList++;
    if (e->inHighPool) high += e->charge;
    if (!e->inCache || e->refs != 0)
      v->push_back("CH1 ExactAccounting: " + where + "pinned or erased block " + e->key + " is on the LRU list");
  }
  for (auto& [k, e] : table_)
    if (e->refs == 0 && !e->onLru)
      v->push_back("CH1 ExactAccounting: " + where + "unpinned block " + k + " is not on the LRU list");
  if (usage_ != real || lruUsage_ != lru || highUsage_ != high)
    v->push_back("CH1 ExactAccounting: " + where + "usage=" + std::to_string(usage_) + " lru=" +
                 std::to_string(lruUsage_) + " high=" + std::to_string(highUsage_) + " but entries hold " +
                 std::to_string(real) + "/" + std::to_string(lru) + "/" + std::to_string(high));
  // CH3 capacity: over capacity only because of pins; strict inserts never go over
  if (usage_ > capacity_ && lru_.next != &lru_)
    v->push_back("CH3 CapacityBound: " + where + "usage=" + std::to_string(usage_) + " > capacity=" +
                 std::to_string(capacity_) + " with evictable entries");
  if (strict_ && !lowered_ && usage_ > capacity_)
    v->push_back("CH3 CapacityBound: " + where + "strict cache holds " + std::to_string(usage_) + " > " +
                 std::to_string(capacity_));
  // CH5 no block came from a compaction read
  for (auto& [k, e] : table_)
    if (e->origin == CacheOrigin::kCompaction)
      v->push_back("CH5 CompactionNoFill: " + where + "block " + k + " was inserted by a compaction read");
  // CH6 every pin has a holder
  auto pins = [&](const LRUHandle* e) {
    if (e->refs != e->holders)
      v->push_back("CH6 NoLeakedPin: " + where + "block " + e->key + " has " + std::to_string(e->refs) +
                   " pins but " + std::to_string(e->holders) + " holders");
  };
  for (auto& [k, e] : table_) pins(e);
  for (LRUHandle* e : detached_) pins(e);
  // CH7 the high-priority pool stays within its share
  if (highRatio_ > 0 && highUsage_ > highCap_)
    v->push_back("CH7 PoolBound: " + where + "high-priority pool holds " + std::to_string(highUsage_) + " > " +
                 std::to_string(highCap_));
}

// ============================================================ BC1 the sharded cache
int BlockCache::DefaultShardBits(size_t capacity) {
  const size_t kMinShardSize = 512 * 1024;
  int bits = 0;
  size_t n = capacity / kMinShardSize;
  while (n >>= 1)
    if (++bits >= 6) return 6;
  return bits;
}

BlockCache::BlockCache(size_t capacity, bool strict, int num_shard_bits, double high_pri_pool_ratio)
    : capacity_(capacity), bits_(num_shard_bits < 0 ? DefaultShardBits(capacity) : num_shard_bits) {
  size_t n = size_t{1} << bits_;
  size_t per = (capacity + n - 1) / n;  // each shard gets an equal share, rounded up
  for (size_t i = 0; i < n; i++) {
    shards_.push_back(std::make_unique<LRUCacheShard>());
    shards_.back()->Configure(per, strict, high_pri_pool_ratio, &fix, &report_);
  }
}

uint32_t BlockCache::Hash(const std::string& key) { return Crc32(key); }

int BlockCache::ShardOf(const std::string& key) const {
  return bits_ == 0 ? 0 : static_cast<int>(Hash(key) >> (32 - bits_));  // the top bits
}

Status BlockCache::Insert(const std::string& key, std::shared_ptr<const void> value, size_t charge,
                          CachePriority pri, Handle** handle, CacheOrigin origin) {
  return shards_[ShardOf(key)]->Insert(key, Hash(key), std::move(value), charge, pri, origin, handle);
}
BlockCache::Handle* BlockCache::Lookup(const std::string& key) { return shards_[ShardOf(key)]->Lookup(key); }
bool BlockCache::Release(Handle* h, bool eraseIfLastRef) {
  return shards_[ShardOf(h->key)]->Release(h, eraseIfLastRef);
}
void BlockCache::Forget(Handle* h) { shards_[ShardOf(h->key)]->Forget(h); }
void BlockCache::Erase(const std::string& key) { shards_[ShardOf(key)]->Erase(key); }
void BlockCache::EraseUnRefEntries() {
  for (auto& s : shards_) s->EraseUnRefEntries();
}
void BlockCache::SetCapacity(size_t capacity) {
  capacity_ = capacity;
  size_t n = shards_.size();
  for (auto& s : shards_) s->SetCapacity((capacity + n - 1) / n);
}
void BlockCache::SetStrictCapacityLimit(bool v) {
  for (auto& s : shards_) s->SetStrictCapacityLimit(v);
}
size_t BlockCache::GetUsage() const {
  size_t u = 0;
  for (auto& s : shards_) u += s->Usage();
  return u;
}
size_t BlockCache::GetPinnedUsage() const {
  size_t u = 0;
  for (auto& s : shards_) u += s->PinnedUsage();
  return u;
}
size_t BlockCache::GetHighPriPoolUsage() const {
  size_t u = 0;
  for (auto& s : shards_) u += s->HighPriPoolUsage();
  return u;
}
size_t BlockCache::GetEntries() const {
  size_t u = 0;
  for (auto& s : shards_) u += s->Entries();
  return u;
}
CacheStats BlockCache::Stats() const {
  CacheStats t;
  for (auto& s : shards_) {
    t.lookups += s->stats.lookups;
    t.hits += s->stats.hits;
    t.misses += s->stats.misses;
    t.adds += s->stats.adds;
    t.addFailures += s->stats.addFailures;
    t.evictions += s->stats.evictions;
  }
  return t;
}

std::string BlockCache::NewSessionId() {
  static std::atomic<uint64_t> next{1};
  return "s" + std::to_string(next++);
}

std::vector<std::string> BlockCache::CheckInvariants() const {
  std::vector<std::string> v = report_;
  for (size_t i = 0; i < shards_.size(); i++) shards_[i]->Check(static_cast<int>(i), &v);
  return v;
}

}  // namespace lsm
