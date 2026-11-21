// write_buffer_manager.cc: WBM1 to WBM9.
#include "write_buffer_manager.h"

#include <algorithm>

#include "memtable.h"

namespace lsm {

// ============================================================ construction
WriteBufferManager::WriteBufferManager(size_t buffer_size, std::shared_ptr<BlockCache> cache,
                                       bool allow_stall, WriteBufferFlushPolicy policy,
                                       size_t dummy_entry_size)
    : bufferSize_(buffer_size),
      mutableLimit_(buffer_size * 7 / 8),
      cache_(std::move(cache)),
      dummySize_(dummy_entry_size),
      allowStall_(allow_stall),
      policy_(policy) {}

WriteBufferManager::~WriteBufferManager() {
  if (cache_)
    for (BlockCache::Handle* h : dummyHandles_) cache_->Release(h, true);
}

// ============================================================ WBM1 ReserveMem
void WriteBufferManager::ReserveMem(MemTable* M, size_t m) {
  if (!Enabled() && !CostToCache()) return;
  M->allocated += m;  // M's AllocTracker
  if (CostToCache()) {
    used_ += m;
    UpdateCacheReservation(used_);  // WBM3; an error is ignored, as in RocksDB
  } else if (Enabled()) {
    used_ += m;
  }
  if (Enabled()) active_ += m;  // the memtable is still mutable
}

// ============================================================ WBM2 ScheduleFreeMem, FreeMem
void WriteBufferManager::ScheduleFreeMem(MemTable* M) {
  // once, when MT4 seals M (M.MarkImmutable)
  if (M->doneAllocating) return;
  if (Enabled()) active_ -= M->allocated;  // M takes no more writes
  M->doneAllocating = true;
}

void WriteBufferManager::FreeMem(MemTable* M) {
  // once, when M is destroyed: MT6 installed it and the last reader let go
  if (!M->doneAllocating) ScheduleFreeMem(M);  // a memtable dropped without a seal
  if (M->freed) return;
  if (CostToCache()) {
    used_ -= M->allocated;
    UpdateCacheReservation(used_);  // WBM3
  } else if (Enabled()) {
    used_ -= M->allocated;
  }
  M->freed = true;
  if (fix.FixEndStallOnFree) MaybeEndWriteStall();  // WBM7: memory went down
}

// ============================================================ WBM3 UpdateCacheReservation
void WriteBufferManager::UpdateCacheReservation(size_t x) {
  if (x > resv_) {
    while (x > resv_) {
      // An empty entry under a key of its own, pinned for as long as B holds it
      // (RocksDB: CacheReservationManager). BC3.
      BlockCache::Handle* h = nullptr;
      std::string key = "wbm-dummy/" + std::to_string(reinterpret_cast<uintptr_t>(this)) + "/" +
                        std::to_string(nextDummy_++);
      Status s = cache_->Insert(key, nullptr, dummySize_, CachePriority::kLow, &h, CacheOrigin::kWbm);
      if (!s.ok()) return;  // only with a strict capacity limit; the caller ignores it
      dummyHandles_.push_back(h);
      resv_ += dummySize_;
    }
  } else if (x < resv_) {
    if (x >= resv_ / 4 * 3) return;  // delayed decrease: memory is likely to come back
    auto release = [&] {
      cache_->Release(dummyHandles_.back(), true);  // BC4 with erase_if_last_ref
      dummyHandles_.pop_back();
      resv_ -= dummySize_;
    };
    if (fix.FixCacheRoundUp) {
      while (x + dummySize_ <= resv_) release();  // the smallest multiple of D that is >= x
    } else {
      while (resv_ > x && !dummyHandles_.empty()) release();  // bug: rounds down
    }
  }
}

// ============================================================ WBM4 ShouldFlush
bool WriteBufferManager::ShouldFlush() const {
  if (!Enabled()) return false;
  if (active_ > mutableLimit_) return true;  // mutable memtables use more than 7/8
  if (used_ >= bufferSize_ && active_ >= bufferSize_ / 2) return true;  // and flushing still helps
  return false;  // over half of used is already sealed and being flushed
}

// ============================================================ WBM5 ShouldStall
bool WriteBufferManager::ShouldStall() const {
  return allowStall_ && Enabled() && (stallActive_ || used_ >= bufferSize_);
}

// ============================================================ WBM6 BeginWriteStall
void WriteBufferManager::BeginWriteStall(WbmStall* x) {
  // lock mu
  if (!fix.FixRecheckUnderLock || ShouldStall()) {  // test again: memory may have gone down
    stallActive_ = true;
    queue_.push_back(x);
    return;  // x waits for WBM7
  }
  x->Signal();  // the stall ended in between
}

// ============================================================ WBM7 MaybeEndWriteStall
void WriteBufferManager::MaybeEndWriteStall() {
  if (allowStall_ && used_ >= bufferSize_) return;  // tested without mu in RocksDB
  // lock mu
  if (!stallActive_) return;
  stallActive_ = false;  // new writers pass WBM5 again
  if (fix.FixWakeAll) {
    for (WbmStall* x : queue_) x->Signal();  // every queued DB
    queue_.clear();
  } else if (!queue_.empty()) {
    queue_.front()->Signal();  // bug: only the first
    queue_.pop_front();
  }
}

// ============================================================ WBM8
void WriteBufferManager::SetBufferSize(size_t n) {
  bufferSize_ = n;
  mutableLimit_ = n * 7 / 8;
  if (fix.FixEndStallOnResize) MaybeEndWriteStall();  // a larger limit may end the stall
}

void WriteBufferManager::SetAllowStall(bool v) {
  allowStall_ = v;
  MaybeEndWriteStall();  // turning stalls off releases every writer
}

void WriteBufferManager::RemoveDBFromQueue(WbmStall* x) {
  if (Enabled() && allowStall_) queue_.remove(x);
  x->Signal();
}

void WriteBufferManager::RegisterFlushInitiator(FlushInitiator* d) {
  registry_.push_back(d);
  d->Stall()->name = "DB" + std::to_string(++registered_);  // for messages only
}

void WriteBufferManager::DeregisterFlushInitiator(FlushInitiator* d) {
  registry_.erase(std::remove(registry_.begin(), registry_.end(), d), registry_.end());
  RemoveDBFromQueue(d->Stall());
}

// ============================================================ WBM9 FlushAcrossDBs
bool WriteBufferManager::FlushAcrossDBs(FlushInitiator* self) {
  if (policy_ != WriteBufferFlushPolicy::kFlushLargestAcrossDBs) return false;
  if (!ShouldFlush()) return false;
  // Hand-off: the DB with the most flushable memory whose writers are not stalled.
  FlushInitiator* best = nullptr;
  size_t bestMem = 0;
  for (FlushInitiator* d : registry_) {
    if (d->WritersStalled()) continue;  // never queue work behind a stalled DB
    size_t m = d->FlushableMemory();
    if (m > bestMem) { best = d; bestMem = m; }
  }
  if (best && best->FlushLargestCf()) {
    wbmFlushes++;
    handoffs++;
    return false;
  }
  // Local retry: the stalled writer leaves the queue and runs WBM4 in its own DB.
  if (self->FlushableMemory() == 0) return false;
  RemoveDBFromQueue(self->Stall());
  return true;
}

// ============================================================ checking
std::vector<std::string> WriteBufferManager::CheckInvariants() const {
  std::vector<std::string> v;
  // BM1 exact accounting: used and active equal the memory they stand for
  size_t realUsed = 0, realActive = 0;
  for (MemTable* M : live_) {
    realUsed += M->allocated;
    if (M->state == MemState::kMutable) realActive += M->allocated;
  }
  if (Enabled() || CostToCache())
    if (used_ != realUsed)
      v.push_back("BM1 ExactAccounting: used=" + std::to_string(used_) + " but memtables hold " +
                  std::to_string(realUsed));
  if (Enabled() && active_ != realActive)
    v.push_back("BM1 ExactAccounting: active=" + std::to_string(active_) + " but mutable memtables hold " +
                std::to_string(realActive));
  // BM2 no lost wake-up and BM5 queue consistency
  for (FlushInitiator* d : registry_) {
    WbmStall* x = d->Stall();
    bool queued = std::find(queue_.begin(), queue_.end(), x) != queue_.end();
    if (x->state == WbmStall::kBlocked && !(allowStall_ && used_ >= bufferSize_))
      v.push_back("BM2 NoLostWakeup: " + x->name + " is blocked but used=" + std::to_string(used_) +
                  " < buffer_size=" + std::to_string(bufferSize_));
    if (queued != (x->state == WbmStall::kBlocked))
      v.push_back("BM5 QueueConsistent: " + x->name + (queued ? " is queued but not blocked" : " is blocked but not queued"));
  }
  if (!queue_.empty() && !stallActive_) v.push_back("BM5 QueueConsistent: the queue is not empty but the stall is off");
  // BM4 the cache reservation covers memory
  if (CostToCache()) {
    size_t ceil = (used_ + dummySize_ - 1) / dummySize_ * dummySize_;
    if (resv_ % dummySize_ != 0 || used_ > resv_ || !(resv_ == ceil || used_ >= resv_ / 4 * 3))
      v.push_back("BM4 CacheCovers: reserved=" + std::to_string(resv_) + " for used=" + std::to_string(used_));
  }
  return v;
}

}  // namespace lsm
