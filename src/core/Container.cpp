#include "core/Container.h"

#include <fstream>

#include "core/Paths.h"

namespace patina::container {

std::string encode(const char magic[8], nlohmann::json header, const std::vector<Block>& blocks) {
  nlohmann::json arr = nlohmann::json::array();
  for (auto& b : blocks) arr.push_back({{"name", b.name}, {"type", b.type}, {"count", b.count}});
  header["blocks"] = arr;
  std::string hs = header.dump();
  std::string out(magic, 8);
  uint32_t hl = (uint32_t)hs.size();
  out.append((const char*)&hl, 4);
  out += hs;
  for (auto& b : blocks) out.append((const char*)b.bytes.data(), b.bytes.size());
  return out;
}

bool decode(const std::string& data, const char magic[8], File& out, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  if (data.size() < 12 || std::memcmp(data.data(), magic, 8) != 0) return fail("bad file magic");
  uint32_t hl;
  std::memcpy(&hl, data.data() + 8, 4);
  if (12 + size_t(hl) > data.size()) return fail("truncated header");
  try {
    out.header = nlohmann::json::parse(data.substr(12, hl));
  } catch (std::exception& e) {
    return fail(std::string("bad header: ") + e.what());
  }
  size_t off = 12 + hl;
  for (auto& bj : out.header.value("blocks", nlohmann::json::array())) {
    Block b;
    b.name = bj.at("name");
    b.type = bj.at("type");
    b.count = bj.at("count");
    size_t ts = Block::typeSize(b.type);
    if (!ts) return fail("unknown block type " + b.type);
    size_t n = b.count * ts;
    if (off + n > data.size()) return fail("truncated block " + b.name);
    b.bytes.assign((const uint8_t*)data.data() + off, (const uint8_t*)data.data() + off + n);
    off += n;
    out.blocks[b.name] = std::move(b);
  }
  return true;
}

bool writeFile(const std::string& path, const char magic[8], const nlohmann::json& header,
               const std::vector<Block>& blocks, std::string* err) {
  if (!paths::writeFileAtomic(path, encode(magic, header, blocks))) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  return true;
}

bool readFile(const std::string& path, const char magic[8], File& out, std::string* err) {
  bool ok = false;
  std::string data = paths::readFile(path, &ok);
  if (!ok) {
    if (err) *err = "cannot open " + path;
    return false;
  }
  return decode(data, magic, out, err);
}

bool readHeader(const std::string& path, const char magic[8], nlohmann::json& out, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  std::ifstream f(std::filesystem::path(path), std::ios::binary);
  if (!f) return fail("cannot open " + path);
  char head[12];
  if (!f.read(head, 12) || std::memcmp(head, magic, 8) != 0) return fail("bad file magic");
  uint32_t hl;
  std::memcpy(&hl, head + 8, 4);
  if (hl > (64u << 20)) return fail("bad header");
  std::string hs(hl, '\0');
  if (!f.read(hs.data(), hl)) return fail("truncated header");
  try {
    out = nlohmann::json::parse(hs);
  } catch (std::exception& e) {
    return fail(std::string("bad header: ") + e.what());
  }
  return true;
}

}  // namespace patina::container
