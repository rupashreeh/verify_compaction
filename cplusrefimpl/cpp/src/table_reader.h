// table_reader.h: reading SST files through the block cache (BC9 to BC11).
//
// An SST file here is split into data blocks of BlockBasedTableOptions::block_entries
// entries, an index block (the last internal key of each data block), and a filter
// block (the set of user keys; it stands for a Bloom filter with no false
// positives). Blocks are built from the file's entries when they are "read from
// disk".
//
// Every block that comes from the cache is held through a BlockHolder. The holder
// pins the cache entry and releases it when it goes away (contract K9).
#pragma once
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "block_cache.h"
#include "memtable.h"
#include "options.h"

namespace lsm {

struct Block {
  enum Kind { kData, kIndex, kFilter } kind = kData;
  std::vector<Entry> entries;            // kData
  std::vector<IKey> lastKeys;            // kIndex: last internal key of each data block
  std::unordered_set<std::string> keys;  // kFilter
  size_t Charge() const;
  bool operator==(const Block& o) const;
};

// RocksDB: ReadOptions::fill_cache. Compaction reads with fill_cache = false (K10).
struct BlockReadOptions {
  bool fill_cache = true;
  CacheOrigin origin = CacheOrigin::kUser;
};

// A block in use: either pinned in the cache, or owned by the reader.
class BlockHolder {
 public:
  BlockHolder() = default;
  BlockHolder(const BlockHolder&) = delete;
  BlockHolder& operator=(const BlockHolder&) = delete;
  ~BlockHolder() { Reset(); }
  void Reset();
  const Block* get() const {
    return h ? static_cast<const Block*>(BlockCache::Value(h)) : owned.get();
  }
  BlockCache* cache = nullptr;
  BlockCache::Handle* h = nullptr;
  std::shared_ptr<const Block> owned;
  bool releaseOnClose = true;  // Fixes::FixReleaseOnClose
};

struct TableStats {
  uint64_t dataHit = 0, dataMiss = 0, indexHit = 0, indexMiss = 0, filterHit = 0, filterMiss = 0;
  uint64_t bytesRead = 0, bytesWrite = 0, addFailures = 0, filterUseful = 0;
};

class TableReader {
 public:
  // keyPrefix is the cache key prefix of the file (BC10). inL0 says whether the
  // file is in L0 when it is opened (pin_l0_filter_and_index_blocks_in_cache).
  TableReader(std::shared_ptr<SstFile> f, std::string keyPrefix, const Options& opts,
              BlockCache* cache, TableStats* stats, bool inL0);
  ~TableReader();  // releases the pinned index and filter blocks (K9)

  size_t NumDataBlocks() const;
  std::string CacheKey(Block::Kind k, size_t i) const;                              // BC10
  Block BuildBlock(Block::Kind k, size_t i) const;                                  // read from the file
  Status RetrieveBlock(Block::Kind k, size_t i, const BlockReadOptions& ro, BlockHolder* out);  // BC9
  Status Get(const std::string& k, SeqNo s, const BlockReadOptions& ro, LookupResult* r);      // BC9
  Status ReadAll(const BlockReadOptions& ro, std::vector<Entry>* out);  // a compaction input scan
  void Prepopulate();  // BC11: put the data blocks of a new flush output in the cache
  void Uncache();      // BC11: erase the blocks of an obsolete file

  const std::shared_ptr<SstFile>& file() const { return f_; }
  int pinnedBlocks() const { return (pinIndex_.h ? 1 : 0) + (pinFilter_.h ? 1 : 0); }

 private:
  Status MetaBlock(Block::Kind k, const BlockReadOptions& ro, BlockHolder* tmp, const Block** out);
  std::shared_ptr<SstFile> f_;
  std::string prefix_;
  const Options& opts_;
  BlockCache* cache_;
  TableStats* stats_;
  // index and filter: kept in the reader (not cached), or pinned in the cache
  std::shared_ptr<const Block> index_, filter_;
  BlockHolder pinIndex_, pinFilter_;
};

}  // namespace lsm
