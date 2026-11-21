// write_buffer_manager.h: the write buffer manager B (WBM1 to WBM9). With a block
// cache (block_cache.h), B charges memtable memory to it as pinned dummy entries (WBM3).
//
// One WriteBufferManager may be shared by several column families and several DBs.
// It keeps two counters, as RocksDB does: used (memory_used_, every memtable not yet
// freed) and active (memory_active_, the mutable memtables). The WAL reads them
// through ShouldFlush (WBM4) and ShouldStall (WBM5) in WAL1. Memtables move memory
// through three calls: ReserveMem when they grow (WBM1, from MT1), ScheduleFreeMem
// when they are sealed (WBM2, from MT4), and FreeMem when they are destroyed (WBM2).
//
// The store runs in one thread (see db.h), so B has no mutex here. The one race
// that the TLA+ model needs two steps for, MaybeEndWriteStall testing the threshold
// before it takes the mutex (WBM7), cannot happen in this code.
#pragma once
#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "block_cache.h"
#include "common.h"
#include "options.h"

namespace lsm {

class MemTable;

enum class WriteBufferFlushPolicy { kFlushOldest, kFlushLargest, kFlushLargestAcrossDBs };

// ---------------------------------------------------------------------------
// A DB's stall handle (RocksDB's StallInterface). Block() cannot block in a
// single-threaded step engine: the DB's WAL1 returns Busy while the handle is
// kBlocked, and goes on once WBM7 (or WBM8) has signaled it.
struct WbmStall {
  enum State { kRunning, kBlocked } state = kRunning;
  void Signal() { state = kRunning; }
  std::string name;
};

// A DB that B may ask to flush on behalf of another DB's stalled writer (WBM9).
class FlushInitiator {
 public:
  virtual ~FlushInitiator() = default;
  virtual size_t FlushableMemory() const = 0;  // largest flushable CF, in bytes; 0 if none
  virtual bool WritersStalled() const = 0;     // its own writers wait in WBM5
  virtual bool FlushLargestCf() = 0;           // WAL6b on its largest CF; false if it could not
  virtual WbmStall* Stall() = 0;
};

class WriteBufferManager {
 public:
  // buffer_size 0 disables the limit (Enabled() is false); a cache may still be
  // charged. dummy_entry_size is RocksDB's kSizeDummyEntry (256 KB).
  explicit WriteBufferManager(size_t buffer_size, std::shared_ptr<BlockCache> cache = nullptr,
                              bool allow_stall = false,
                              WriteBufferFlushPolicy policy = WriteBufferFlushPolicy::kFlushOldest,
                              size_t dummy_entry_size = 256 * 1024);
  ~WriteBufferManager();

  bool Enabled() const { return bufferSize_ > 0; }
  bool CostToCache() const { return cache_ != nullptr; }
  size_t MemoryUsage() const { return used_; }               // u
  size_t MutableMemtableMemoryUsage() const { return active_; }  // a
  size_t BufferSize() const { return bufferSize_; }           // beta
  size_t MutableLimit() const { return mutableLimit_; }       // lambda = 7 beta / 8
  size_t DummyEntriesInCacheUsage() const { return resv_; }   // r
  bool StallActive() const { return stallActive_; }
  bool AllowStall() const { return allowStall_; }
  WriteBufferFlushPolicy Policy() const { return policy_; }

  // ---- accounting (called by the memtable's tracker) ----
  void ReserveMem(MemTable* M, size_t m);  // WBM1
  void ScheduleFreeMem(MemTable* M);       // WBM2, at seal (MT4)
  void FreeMem(MemTable* M);               // WBM2, when the memtable is destroyed

  // ---- the flush trigger and the stall (read by WAL1) ----
  bool ShouldFlush() const;                // WBM4
  bool ShouldStall() const;                // WBM5
  void BeginWriteStall(WbmStall* x);       // WBM6
  void MaybeEndWriteStall();               // WBM7

  // ---- dynamic options and DB life cycle (WBM8) ----
  void SetBufferSize(size_t n);
  void SetAllowStall(bool v);
  void RemoveDBFromQueue(WbmStall* x);
  void RegisterFlushInitiator(FlushInitiator* d);
  void DeregisterFlushInitiator(FlushInitiator* d);

  // ---- WBM9: kFlushLargestAcrossDBs ----
  // Called by a stalled writer of `self` each time it retries (RocksDB: every 20 ms).
  // Hands the flush to the registered DB with the most flushable memory, or, if no
  // other DB can take it, lets `self` flush locally. Returns true if `self` should
  // run its own WBM4 flush now (it has been removed from the queue).
  bool FlushAcrossDBs(FlushInitiator* self);

  // ---- checking: BM1, BM2, BM4, BM5 (see write_buffer/README.md) ----
  std::vector<std::string> CheckInvariants() const;
  void Track(MemTable* M) { live_.insert(M); }
  void Untrack(MemTable* M) { live_.erase(M); }

  // Switches for the mechanisms inside B (the TLA+ Fix* constants). The ones on the
  // memtable side (FixScheduleFreeOnSeal, FixFreeAfterUnpin) are read from the DB's
  // Options::fix.
  Fixes fix;
  uint64_t wbmFlushes = 0;    // flushes B asked for, for tests
  uint64_t handoffs = 0;      // WBM9 hand-offs to another DB

 private:
  void UpdateCacheReservation(size_t x);  // WBM3
  size_t bufferSize_, mutableLimit_;
  size_t used_ = 0, active_ = 0;
  std::shared_ptr<BlockCache> cache_;
  size_t dummySize_;
  size_t resv_ = 0;
  std::vector<BlockCache::Handle*> dummyHandles_;
  uint64_t nextDummy_ = 0;
  bool allowStall_;
  bool stallActive_ = false;
  std::list<WbmStall*> queue_;
  WriteBufferFlushPolicy policy_;
  std::vector<FlushInitiator*> registry_;
  int registered_ = 0;
  std::set<MemTable*> live_;  // every memtable that charges B and still exists
};

}  // namespace lsm
