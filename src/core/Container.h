#pragma once
// Patina binary container: 8-byte magic, uint32 LE JSON header length, JSON header,
// then raw blocks in the order listed by header["blocks"] = [{name,type,count}].
// Used by .ptmesh / .ptuv (Blender bridge) and .patina project files.
#include <cstdint>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace patina::container {

struct Block {
  std::string name;
  std::string type;  // f32 u32 i32 u8
  size_t count = 0;
  std::vector<uint8_t> bytes;

  template <class T>
  static Block of(const std::string& name, const std::string& type, const T* data, size_t elems) {
    Block b;
    b.name = name;
    b.type = type;
    b.count = elems * sizeof(T) / typeSize(type);
    b.bytes.resize(elems * sizeof(T));
    if (elems) std::memcpy(b.bytes.data(), data, b.bytes.size());
    return b;
  }
  static Block raw(const std::string& name, std::vector<uint8_t> data) {
    Block b;
    b.name = name;
    b.type = "u8";
    b.count = data.size();
    b.bytes = std::move(data);
    return b;
  }
  static size_t typeSize(const std::string& t) { return t == "u8" ? 1 : (t == "f32" || t == "u32" || t == "i32") ? 4 : 0; }
};

struct File {
  nlohmann::json header;
  std::unordered_map<std::string, Block> blocks;
  const Block* get(const std::string& n) const {
    auto it = blocks.find(n);
    return it == blocks.end() ? nullptr : &it->second;
  }
  template <class T>
  bool read(const std::string& name, std::vector<T>& out, size_t expectElems) const {
    const Block* b = get(name);
    if (!b || b->bytes.size() != expectElems * sizeof(T)) return false;
    out.resize(expectElems);
    if (expectElems) std::memcpy(out.data(), b->bytes.data(), b->bytes.size());
    return true;
  }
};

std::string encode(const char magic[8], nlohmann::json header, const std::vector<Block>& blocks);
bool decode(const std::string& data, const char magic[8], File& out, std::string* err);
bool writeFile(const std::string& path, const char magic[8], const nlohmann::json& header,
               const std::vector<Block>& blocks, std::string* err);
bool readFile(const std::string& path, const char magic[8], File& out, std::string* err);
// The header alone, without reading the blocks (which file a project is, cheaply).
bool readHeader(const std::string& path, const char magic[8], nlohmann::json& out, std::string* err = nullptr);

}  // namespace patina::container
