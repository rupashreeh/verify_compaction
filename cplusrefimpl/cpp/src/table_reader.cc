// table_reader.cc: BC9 to BC11, the table reader side of the block cache.
#include "table_reader.h"

#include <algorithm>

namespace lsm {

// ============================================================ blocks
size_t Block::Charge() const {
  size_t c = 32;
  for (auto& e : entries) c += e.key.size() + e.value.size() + 16;
  for (auto& k : lastKeys) c += k.key.size() + 8;
  for (auto& k : keys) c += k.size() + 4;
  return c;
}

bool Block::operator==(const Block& o) const {
  if (kind != o.kind || entries.size() != o.entries.size() || lastKeys.size() != o.lastKeys.size() ||
      keys != o.keys)
    return false;
  for (size_t i = 0; i < entries.size(); i++) {
    const Entry &a = entries[i], &b = o.entries[i];
    if (a.key != b.key || a.seq != b.seq || a.type != b.type || a.value != b.value) return false;
  }
  for (size_t i = 0; i < lastKeys.size(); i++)
    if (lastKeys[i].key != o.lastKeys[i].key || lastKeys[i].seq != o.lastKeys[i].seq) return false;
  return true;
}

void BlockHolder::Reset() {
  if (h) {
    if (releaseOnClose) cache->Release(h);
    else cache->Forget(h);  // bug variant: the pin is never given back
  }
  h = nullptr;
  owned.reset();
}

// ============================================================ open and close
TableReader::TableReader(std::shared_ptr<SstFile> f, std::string keyPrefix, const Options& opts,
                         BlockCache* cache, TableStats* stats, bool inL0)
    : f_(std::move(f)), prefix_(std::move(keyPrefix)), opts_(opts), cache_(cache), stats_(stats) {
  const auto& t = opts_.table_options;
  if (!cache_ || !t.cache_index_and_filter_blocks) {
    // index and filter live in the reader, outside the block cache
    index_ = std::make_shared<Block>(BuildBlock(Block::kIndex, 0));
    filter_ = std::make_shared<Block>(BuildBlock(Block::kFilter, 0));
  } else if (t.pin_l0_filter_and_index_blocks_in_cache && inL0) {
    // pinned for the life of the reader; a failed insert just leaves them unpinned
    BlockReadOptions ro;
    RetrieveBlock(Block::kIndex, 0, ro, &pinIndex_);
    RetrieveBlock(Block::kFilter, 0, ro, &pinFilter_);
  }
}

TableReader::~TableReader() {
  pinIndex_.Reset();
  pinFilter_.Reset();
}

// ============================================================ BC10 cache keys
std::string TableReader::CacheKey(Block::Kind k, size_t i) const {
  // RocksDB: an OffsetableCacheKey from the DB id, the DB session id, and the file
  // number, plus the block's offset. Here: the prefix plus the block's kind and index.
  switch (k) {
    case Block::kIndex: return prefix_ + "/index";
    case Block::kFilter: return prefix_ + "/filter";
    default: return prefix_ + "/data" + std::to_string(i);
  }
}

size_t TableReader::NumDataBlocks() const {
  size_t n = std::max<size_t>(1, opts_.table_options.block_entries);
  return (f_->entries.size() + n - 1) / n;
}

Block TableReader::BuildBlock(Block::Kind k, size_t i) const {
  size_t n = std::max<size_t>(1, opts_.table_options.block_entries);
  Block b;
  b.kind = k;
  const auto& es = f_->entries;
  if (k == Block::kData) {
    for (size_t j = i * n; j < std::min(es.size(), (i + 1) * n); j++) b.entries.push_back(es[j]);
  } else if (k == Block::kIndex) {
    for (size_t d = 0; d < NumDataBlocks(); d++) {
      const Entry& last = es[std::min(es.size(), (d + 1) * n) - 1];
      b.lastKeys.push_back(IKey{last.key, last.seq});
    }
  } else {
    for (auto& e : es) b.keys.insert(e.key);
  }
  return b;
}

// ============================================================ BC9 RetrieveBlock
Status TableReader::RetrieveBlock(Block::Kind k, size_t i, const BlockReadOptions& ro, BlockHolder* out) {
  out->Reset();
  out->cache = cache_;
  out->releaseOnClose = opts_.fix.FixReleaseOnClose;
  auto tick = [&](bool hit) {
    uint64_t* c[3][2] = {{&stats_->dataMiss, &stats_->dataHit},
                         {&stats_->indexMiss, &stats_->indexHit},
                         {&stats_->filterMiss, &stats_->filterHit}};
    (*c[k][hit])++;
  };
  if (!cache_) {
    out->owned = std::make_shared<Block>(BuildBlock(k, i));
    return Status::OK();
  }
  std::string key = CacheKey(k, i);
  if (BlockCache::Handle* h = cache_->Lookup(key)) {  // BC2
    tick(true);
    out->h = h;
    stats_->bytesRead += h->charge;
    // CH4: the cached block is the block of this file
    if (opts_.check_invariants && !(*out->get() == BuildBlock(k, i)))
      cache_->Report("CH4 NoStaleBlock: key " + key + " of file " + std::to_string(f_->number) +
                     " returned a block of another file");
    return Status::OK();
  }
  tick(false);
  auto block = std::make_shared<Block>(BuildBlock(k, i));  // read and check the block
  if (!ro.fill_cache) {  // K10: compaction reads do not fill the cache
    out->owned = std::move(block);
    return Status::OK();
  }
  const auto& t = opts_.table_options;
  CachePriority pri = (k != Block::kData && t.cache_index_and_filter_blocks_with_high_priority)
                          ? CachePriority::kHigh : CachePriority::kLow;
  size_t charge = block->Charge();
  BlockCache::Handle* h = nullptr;
  Status s = cache_->Insert(key, block, charge, pri, &h, ro.origin);  // BC3
  if (!s.ok()) {  // strict_capacity_limit and the cache is full of pinned blocks
    stats_->addFailures++;
    return s;
  }
  stats_->bytesWrite += charge;
  out->h = h;
  return Status::OK();
}

Status TableReader::MetaBlock(Block::Kind k, const BlockReadOptions& ro, BlockHolder* tmp, const Block** out) {
  const std::shared_ptr<const Block>& own = k == Block::kIndex ? index_ : filter_;
  const BlockHolder& pin = k == Block::kIndex ? pinIndex_ : pinFilter_;
  if (own) { *out = own.get(); return Status::OK(); }
  if (pin.h) { *out = pin.get(); return Status::OK(); }
  Status s = RetrieveBlock(k, 0, ro, tmp);
  if (s.ok()) *out = tmp->get();
  return s;
}

// ============================================================ BC9 Get
Status TableReader::Get(const std::string& k, SeqNo s, const BlockReadOptions& ro, LookupResult* r) {
  *r = LookupResult{};
  if (f_->entries.empty() || k < f_->lo || k > f_->hi) return Status::OK();
  BlockHolder fh, ih, dh;
  const Block* filter = nullptr;
  Status st = MetaBlock(Block::kFilter, ro, &fh, &filter);
  if (!st.ok()) return st;
  if (!filter->keys.count(k)) {
    stats_->filterUseful++;
    return Status::OK();
  }
  const Block* index = nullptr;
  st = MetaBlock(Block::kIndex, ro, &ih, &index);
  if (!st.ok()) return st;
  IKey target{k, s};
  auto it = std::lower_bound(index->lastKeys.begin(), index->lastKeys.end(), target, IKeyCmp());
  if (it == index->lastKeys.end()) return Status::OK();
  st = RetrieveBlock(Block::kData, static_cast<size_t>(it - index->lastKeys.begin()), ro, &dh);
  if (!st.ok()) return st;
  const auto& es = dh.get()->entries;
  auto e = std::lower_bound(es.begin(), es.end(), target,
                            [](const Entry& x, const IKey& y) { return IKeyCmp()(IKey{x.key, x.seq}, y); });
  if (e == es.end() || e->key != k) return Status::OK();
  r->seq = e->seq;
  if (e->type == ValueType::kPut) {
    r->kind = LookupResult::kFound;
    r->value = e->value;
  } else {
    r->kind = LookupResult::kDeleted;
  }
  return Status::OK();
}

Status TableReader::ReadAll(const BlockReadOptions& ro, std::vector<Entry>* out) {
  for (size_t i = 0; i < NumDataBlocks(); i++) {
    BlockHolder h;  // the iterator pins one block at a time
    Status s = RetrieveBlock(Block::kData, i, ro, &h);
    if (!s.ok()) return s;
    for (auto& e : h.get()->entries) out->push_back(e);
  }
  return Status::OK();
}

// ============================================================ BC11 file life
void TableReader::Prepopulate() {
  if (!cache_) return;
  for (size_t i = 0; i < NumDataBlocks(); i++) {
    auto b = std::make_shared<Block>(BuildBlock(Block::kData, i));
    size_t c = b->Charge();
    cache_->Insert(CacheKey(Block::kData, i), b, c, CachePriority::kLow, nullptr, CacheOrigin::kFlush);
    stats_->bytesWrite += c;
  }
}

void TableReader::Uncache() {
  if (!cache_) return;
  pinIndex_.Reset();
  pinFilter_.Reset();
  for (size_t i = 0; i < NumDataBlocks(); i++) cache_->Erase(CacheKey(Block::kData, i));
  cache_->Erase(CacheKey(Block::kIndex, 0));
  cache_->Erase(CacheKey(Block::kFilter, 0));
}

}  // namespace lsm
