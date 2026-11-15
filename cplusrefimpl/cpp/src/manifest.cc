#include "manifest.h"

namespace lsm {

std::string VersionEdit::Encode() const {
  std::string d;
  for (auto& [c, name] : addCf) { d.push_back('A'); PutFixed32(&d, c); PutStr(&d, name); }
  for (auto c : dropCf) { d.push_back('D'); PutFixed32(&d, c); }
  for (auto& [c, n] : logNumber) { d.push_back('N'); PutFixed32(&d, c); PutFixed64(&d, n); }
  for (auto& f : addFile) {
    d.push_back('F');
    PutFixed32(&d, f.cf);
    PutFixed32(&d, static_cast<uint32_t>(f.level));
    PutFixed64(&d, f.number);
    PutFixed64(&d, f.globalSeqno);
    PutFixed64(&d, f.epoch);
    PutFixed64(&d, f.largestLoggedSeq);
  }
  for (auto& [c, l, n] : delFile) {
    d.push_back('X');
    PutFixed32(&d, c);
    PutFixed32(&d, static_cast<uint32_t>(l));
    PutFixed64(&d, n);
  }
  if (lastSequence) { d.push_back('S'); PutFixed64(&d, *lastSequence); }
  if (nextFileNumber) { d.push_back('U'); PutFixed64(&d, *nextFileNumber); }
  if (nextCfId) { d.push_back('C'); PutFixed32(&d, *nextCfId); }
  return d;
}

bool VersionEdit::Decode(const std::string& s) {
  Reader r(s);
  while (r.remaining() > 0) {
    uint8_t tag;
    r.Byte(&tag);
    uint32_t c, l;
    uint64_t n, g, ep, ll;
    std::string name;
    switch (tag) {
      case 'A': if (!r.Fixed32(&c) || !r.Str(&name)) return false; addCf.push_back({c, name}); break;
      case 'D': if (!r.Fixed32(&c)) return false; dropCf.push_back(c); break;
      case 'N': if (!r.Fixed32(&c) || !r.Fixed64(&n)) return false; logNumber.push_back({c, n}); break;
      case 'F':
        if (!r.Fixed32(&c) || !r.Fixed32(&l) || !r.Fixed64(&n) || !r.Fixed64(&g) || !r.Fixed64(&ep) ||
            !r.Fixed64(&ll))
          return false;
        addFile.push_back(FileAdd{c, static_cast<int>(l), n, g, ep, ll, nullptr});
        break;
      case 'X': if (!r.Fixed32(&c) || !r.Fixed32(&l) || !r.Fixed64(&n)) return false;
        delFile.push_back({c, static_cast<int>(l), n}); break;
      case 'S': if (!r.Fixed64(&n)) return false; lastSequence = n; break;
      case 'U': if (!r.Fixed64(&n)) return false; nextFileNumber = n; break;
      case 'C': if (!r.Fixed32(&c)) return false; nextCfId = c; break;
      default: return false;
    }
  }
  return true;
}

Status ManifestAppend(SimEnv* env, const VersionEdit& e) {
  bool fresh = !env->Exists(kManifestPath);
  std::string payload = e.Encode();
  std::string rec;
  PutFixed32(&rec, Crc32(payload));
  PutFixed32(&rec, static_cast<uint32_t>(payload.size()));
  rec += payload;
  Status s = env->Append(kManifestPath, rec);
  if (s.ok()) s = env->Sync(kManifestPath);
  if (s.ok() && fresh) s = env->SyncDir();
  return s;
}

Status ManifestRead(SimEnv* env, std::vector<VersionEdit>* edits) {
  std::string data;
  Status s = env->Read(kManifestPath, &data);
  if (!s.ok()) return s;
  size_t pos = 0;
  while (data.size() - pos >= 8) {
    Reader r(data, pos);
    uint32_t crc = 0, len = 0;
    r.Fixed32(&crc);
    r.Fixed32(&len);
    if (r.remaining() < len) break;  // torn tail: never acknowledged
    std::string payload = data.substr(pos + 8, len);
    if (Crc32(payload) != crc) break;
    VersionEdit e;
    if (!e.Decode(payload)) break;
    edits->push_back(std::move(e));
    pos += 8 + len;
  }
  return Status::OK();
}

}  // namespace lsm
