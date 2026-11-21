#include "memtable.h"

#include <algorithm>

#include "write_buffer_manager.h"

namespace lsm {

MemTable::MemTable(uint64_t id, LogNum memLog, size_t writeBufferSize,
                   std::shared_ptr<WriteBufferManager> wbm)
    : id(id), memLog(memLog), writeBufferSize_(writeBufferSize), wbm_(std::move(wbm)) {
  if (wbm_) wbm_->Track(this);
}

// ~AllocTracker: the memtable is gone, so B.FreeMem (WBM2).
MemTable::~MemTable() {
  if (!wbm_) return;
  if (!freed) wbm_->FreeMem(this);
  wbm_->Untrack(this);
}

void MemTable::MarkImmutable() {
  if (wbm_) wbm_->ScheduleFreeMem(this);
}

void MemTable::FreeNow() {
  if (wbm_) wbm_->FreeMem(this);
}

// ---------------------------------------------------------------- MT1 Insert
void MemTable::Insert(const Entry& r, bool logged) {
  // in-place updates and concurrent skiplist inserts are not modelled: the single
  // writer inserts serially, which is the "else" branch of MT1.
  auto [it, fresh] = table.insert_or_assign(IKey{r.key, r.seq}, std::make_pair(r.type, r.value));
  (void)it;
  if (fresh) {
    size_t e = r.key.size() + r.value.size() + 16;  // |e|
    bytes += e;
    if (wbm_) wbm_->ReserveMem(this, e);  // B.Charge (WBM1)
  }
  firstSeq = std::min(firstSeq, r.seq);
  largestSeq = std::max(largestSeq, r.seq);
  if (logged) largestLoggedSeq = std::max(largestLoggedSeq, r.seq);
}

// ---------------------------------------------------------------- MT2 Get
LookupResult MemTable::Get(const std::string& k, SeqNo s) const {
  // seek to the first entry with user key k and sequence number <= s
  auto it = table.lower_bound(IKey{k, s});
  LookupResult r;
  if (it == table.end() || it->first.key != k) return r;  // NotFound
  r.seq = it->first.seq;
  if (it->second.first == ValueType::kPut) {
    r.kind = LookupResult::kFound;
    r.value = it->second.second;
  } else {
    r.kind = LookupResult::kDeleted;
  }
  return r;
}

std::vector<Entry> MemTable::Entries() const {
  std::vector<Entry> out;
  out.reserve(table.size());
  for (auto& [ik, tv] : table) out.push_back(Entry{ik.key, ik.seq, tv.first, tv.second});
  return out;
}

// ---------------------------------------------------------------- SST
LookupResult SstFile::Get(const std::string& k, SeqNo s) const {
  LookupResult r;
  if (k < lo || k > hi) return r;
  auto it = std::lower_bound(entries.begin(), entries.end(), IKey{k, s},
                             [](const Entry& e, const IKey& x) { return IKeyCmp()(IKey{e.key, e.seq}, x); });
  if (it == entries.end() || it->key != k) return r;
  r.seq = it->seq;
  if (it->type == ValueType::kPut) {
    r.kind = LookupResult::kFound;
    r.value = it->value;
  } else {
    r.kind = LookupResult::kDeleted;
  }
  return r;
}

void SstFile::ApplyGlobalSeqno(SeqNo g) {
  globalSeqno = g;
  for (auto& e : entries) e.seq = g;
  ComputeBounds();
}

void SstFile::ComputeBounds() {
  if (entries.empty()) return;
  lo = entries.front().key;
  hi = entries.back().key;
  smallestSeq = UINT64_MAX;
  largestSeq = 0;
  for (auto& e : entries) {
    smallestSeq = std::min(smallestSeq, e.seq);
    largestSeq = std::max(largestSeq, e.seq);
  }
}

static const char kSstMagic[9] = "LSMSST01";

std::string EncodeSst(const std::vector<Entry>& entries, SeqNo globalSeqno) {
  std::string out(kSstMagic, 8);
  PutFixed64(&out, globalSeqno);
  std::string body;
  PutFixed32(&body, static_cast<uint32_t>(entries.size()));
  for (auto& e : entries) {
    PutStr(&body, e.key);
    PutFixed64(&body, e.seq);
    body.push_back(static_cast<char>(e.type));
    PutStr(&body, e.value);
  }
  out += body;
  PutFixed32(&out, Crc32(body));
  return out;
}

Status DecodeSst(const std::string& data, std::vector<Entry>* entries, SeqNo* globalSeqno) {
  if (data.size() < 24 || data.compare(0, 8, kSstMagic) != 0) return Status::Corruption("bad sst magic");
  uint32_t crc;
  std::memcpy(&crc, data.data() + data.size() - 4, 4);
  if (Crc32(data.data() + 16, data.size() - 20) != crc) return Status::Corruption("bad sst checksum");
  Reader r(data, 8, data.size() - 4);
  uint32_t n;
  if (!r.Fixed64(globalSeqno) || !r.Fixed32(&n)) return Status::Corruption("short sst");
  entries->clear();
  for (uint32_t i = 0; i < n; i++) {
    Entry e;
    uint8_t t;
    if (!r.Str(&e.key) || !r.Fixed64(&e.seq) || !r.Byte(&t) || !r.Str(&e.value))
      return Status::Corruption("short sst entry");
    e.type = static_cast<ValueType>(t);
    entries->push_back(std::move(e));
  }
  return Status::OK();
}

Status LoadSst(SimEnv* env, const std::string& path, uint64_t number, std::shared_ptr<SstFile>* out) {
  std::string data;
  Status s = env->Read(path, &data);
  if (!s.ok()) return s;
  auto f = std::make_shared<SstFile>();
  f->number = number;
  f->path = path;
  SeqNo g;
  s = DecodeSst(data, &f->entries, &g);
  if (!s.ok()) return s;
  if (f->entries.empty()) return Status::Corruption("empty sst " + path);
  if (g > 0) f->ApplyGlobalSeqno(g);
  f->ComputeBounds();
  *out = f;
  return Status::OK();
}

// ---------------------------------------------------------------- SST1 SstFileWriter
Status SstFileWriter::Open(const std::string& path) {
  path_ = path;
  open_ = true;
  hasLast_ = false;
  entries_.clear();
  return Status::OK();
}

Status SstFileWriter::Add(const std::string& k, ValueType t, const std::string& v) {
  if (!open_) return Status::InvalidArgument("writer not open");
  if (hasLast_ && k <= last_) return Status::InvalidArgument("keys must be strictly increasing");
  entries_.push_back(Entry{k, 0, t, v});  // every key has sequence number 0
  last_ = k;
  hasLast_ = true;
  return Status::OK();
}

Status SstFileWriter::Finish() {
  if (entries_.empty()) return Status::InvalidArgument("the file has no entries");
  open_ = false;
  return env_->WriteFileDurably(path_, EncodeSst(entries_, 0));  // global seqno field = 0
}

}  // namespace lsm
