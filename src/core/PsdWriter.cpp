#include "core/PsdWriter.h"

#include <cstdio>
#include <cstring>

namespace patina {

namespace {
void be16(std::vector<uint8_t>& o, uint32_t v) { o.push_back(uint8_t(v >> 8)), o.push_back(uint8_t(v)); }
void be32(std::vector<uint8_t>& o, uint32_t v) {
  o.push_back(uint8_t(v >> 24)), o.push_back(uint8_t(v >> 16)), o.push_back(uint8_t(v >> 8)), o.push_back(uint8_t(v));
}
void put32At(std::vector<uint8_t>& o, size_t at, uint32_t v) {
  o[at] = uint8_t(v >> 24), o[at + 1] = uint8_t(v >> 16), o[at + 2] = uint8_t(v >> 8), o[at + 3] = uint8_t(v);
}
// PackBits, as Photoshop uses it
void packBits(const uint8_t* s, size_t n, std::vector<uint8_t>& out) {
  size_t i = 0;
  while (i < n) {
    size_t run = 1;
    while (i + run < n && run < 128 && s[i + run] == s[i]) ++run;
    if (run >= 2) {
      out.push_back(uint8_t(257 - run));
      out.push_back(s[i]);
      i += run;
      continue;
    }
    size_t lit = 1;
    while (i + lit < n && lit < 128 && !(i + lit + 1 < n && s[i + lit] == s[i + lit + 1])) ++lit;
    out.push_back(uint8_t(lit - 1));
    out.insert(out.end(), s + i, s + i + lit);
    i += lit;
  }
}
}  // namespace

PsdWriter::PsdWriter(int width, int height, int bits) : w_(width), h_(height), bits_(bits == 16 ? 16 : 8) {}

const char* PsdWriter::blendKeyFor(const std::string& b) {
  if (b == "multiply") return "mul ";
  if (b == "screen") return "scrn";
  if (b == "overlay") return "over";
  if (b == "add") return "lddg";
  if (b == "subtract") return "fsub";
  if (b == "darken") return "dark";
  if (b == "lighten") return "lite";
  if (b == "add_signed") return "lLit";
  if (b == "soft_light") return "sLit";
  return "norm";
}

std::vector<uint8_t> PsdWriter::packPlane(const void* src, int stride, int offset) const {
  int bpc = bits_ / 8;
  size_t rowBytes = size_t(w_) * bpc;
  std::vector<uint8_t> row(rowBytes), packed, counts;
  packed.reserve(rowBytes);
  std::vector<uint8_t> out;
  be16(out, 1);  // RLE
  size_t countsAt = out.size();
  out.resize(out.size() + size_t(h_) * 2);
  for (int y = 0; y < h_; ++y) {
    for (int x = 0; x < w_; ++x) {
      size_t i = (size_t(y) * w_ + x) * stride + offset;
      if (bpc == 1) {
        row[x] = ((const uint8_t*)src)[i];
      } else {
        uint16_t v = ((const uint16_t*)src)[i];
        row[x * 2] = uint8_t(v >> 8), row[x * 2 + 1] = uint8_t(v);
      }
    }
    packed.clear();
    packBits(row.data(), rowBytes, packed);
    out[countsAt + y * 2] = uint8_t(packed.size() >> 8);
    out[countsAt + y * 2 + 1] = uint8_t(packed.size());
    out.insert(out.end(), packed.begin(), packed.end());
  }
  return out;
}

void PsdWriter::addLayer(const std::string& name, const void* rgba, const void* mask, uint8_t opacity,
                         const char* blendKey, bool visible) {
  Layer l;
  l.name = name;
  l.opacity = opacity;
  std::memcpy(l.blend, blendKey, 4);
  l.visible = visible;
  l.channels.push_back({-1, packPlane(rgba, 4, 3)});
  for (int c = 0; c < 3; ++c) l.channels.push_back({int16_t(c), packPlane(rgba, 4, c)});
  if (mask) {
    l.hasMask = true;
    l.channels.push_back({-2, packPlane(mask, 1, 0)});
  }
  layers_.push_back(std::move(l));
}

void PsdWriter::beginGroup() {
  Layer l;
  l.name = "</Layer group>";
  l.section = GroupEnd;
  for (int16_t c : {-1, 0, 1, 2}) {
    std::vector<uint8_t> d;
    be16(d, 0);  // raw, empty (the layer has no pixels)
    l.channels.push_back({c, d});
  }
  layers_.push_back(std::move(l));
}

void PsdWriter::endGroup(const std::string& name, const void* mask, uint8_t opacity, const char* blendKey, bool visible) {
  Layer l;
  l.name = name;
  l.section = GroupOpen;
  l.opacity = opacity;
  std::memcpy(l.blend, blendKey, 4);
  l.visible = visible;
  for (int16_t c : {-1, 0, 1, 2}) {
    std::vector<uint8_t> d;
    be16(d, 0);
    l.channels.push_back({c, d});
  }
  if (mask) {
    l.hasMask = true;
    l.channels.push_back({-2, packPlane(mask, 1, 0)});
  }
  layers_.push_back(std::move(l));
}

void PsdWriter::setComposite(const void* rgb) {
  int bpc = bits_ / 8;
  composite_.resize(size_t(w_) * h_ * 3 * bpc);
  for (int c = 0; c < 3; ++c)
    for (size_t i = 0; i < size_t(w_) * h_; ++i) {
      size_t o = (size_t(c) * w_ * h_ + i) * bpc;
      if (bpc == 1) {
        composite_[o] = ((const uint8_t*)rgb)[i * 3 + c];
      } else {
        uint16_t v = ((const uint16_t*)rgb)[i * 3 + c];
        composite_[o] = uint8_t(v >> 8), composite_[o + 1] = uint8_t(v);
      }
    }
}

bool PsdWriter::save(const std::string& path, std::string* err) const {
  std::vector<uint8_t> o;
  o.insert(o.end(), {'8', 'B', 'P', 'S'});
  be16(o, 1);
  o.insert(o.end(), 6, 0);
  be16(o, 3);  // channels of the composite
  be32(o, uint32_t(h_));
  be32(o, uint32_t(w_));
  be16(o, uint16_t(bits_));
  be16(o, 3);  // RGB
  be32(o, 0);  // color mode data
  be32(o, 0);  // image resources
  // layer and mask information
  size_t lmiAt = o.size();
  be32(o, 0);
  size_t liAt = o.size();
  be32(o, 0);
  be16(o, uint16_t(layers_.size()));
  for (const Layer& l : layers_) {
    bool empty = l.section != Normal;
    be32(o, 0);
    be32(o, 0);
    be32(o, empty ? 0 : uint32_t(h_));
    be32(o, empty ? 0 : uint32_t(w_));
    be16(o, uint16_t(l.channels.size()));
    for (const Channel& c : l.channels) {
      be16(o, uint16_t(c.id));
      be32(o, uint32_t(c.data.size()));
    }
    o.insert(o.end(), {'8', 'B', 'I', 'M'});
    o.insert(o.end(), l.blend, l.blend + 4);
    o.push_back(l.opacity);
    o.push_back(0);  // clipping: base
    o.push_back(uint8_t((l.visible ? 0 : 2) | 8));  // bit 1: hidden; bit 3: bit 4 is meaningful
    o.push_back(0);
    size_t extraAt = o.size();
    be32(o, 0);
    // layer mask data
    if (l.hasMask) {
      be32(o, 20);
      be32(o, 0), be32(o, 0), be32(o, uint32_t(h_)), be32(o, uint32_t(w_));
      o.push_back(0);  // default color outside the mask: black
      o.push_back(0);  // flags
      o.push_back(0), o.push_back(0);
    } else {
      be32(o, 0);
    }
    be32(o, 0);  // blending ranges
    // name: Pascal string padded to 4 bytes
    std::string nm = l.name.substr(0, 255);
    size_t nameAt = o.size();
    o.push_back(uint8_t(nm.size()));
    o.insert(o.end(), nm.begin(), nm.end());
    while ((o.size() - nameAt) % 4) o.push_back(0);
    // unicode name ('luni') so names with accents survive
    {
      o.insert(o.end(), {'8', 'B', 'I', 'M', 'l', 'u', 'n', 'i'});
      std::vector<uint16_t> u;
      for (size_t i = 0; i < nm.size();) {  // UTF-8 -> UTF-16 (BMP)
        unsigned char ch = nm[i];
        uint32_t cp = ch;
        int n = ch < 0x80 ? 1 : (ch >> 5) == 6 ? 2 : (ch >> 4) == 14 ? 3 : 4;
        if (n > 1) cp = ch & (0x7F >> n);
        for (int k = 1; k < n && i + k < nm.size(); ++k) cp = (cp << 6) | (nm[i + k] & 0x3F);
        u.push_back(cp > 0xFFFF ? 0xFFFD : uint16_t(cp));
        i += n;
      }
      uint32_t len = 4 + uint32_t(u.size()) * 2;
      len = (len + 3) & ~3u;
      be32(o, len);
      be32(o, uint32_t(u.size()));
      for (uint16_t c : u) be16(o, c);
      for (uint32_t pad = 4 + uint32_t(u.size()) * 2; pad < len; ++pad) o.push_back(0);
    }
    if (l.section != Normal) {
      o.insert(o.end(), {'8', 'B', 'I', 'M', 'l', 's', 'c', 't'});
      be32(o, 4);
      be32(o, uint32_t(l.section));
    }
    put32At(o, extraAt, uint32_t(o.size() - extraAt - 4));
  }
  // the channel data is written straight from the layers (it can be large): sizes first
  uint64_t chanBytes = 0;
  for (const Layer& l : layers_)
    for (const Channel& c : l.channels) chanBytes += c.data.size();
  uint64_t liLen = (o.size() - liAt - 4) + chanBytes;
  bool pad = liLen % 2;
  liLen += pad;
  if (liLen + 8 > 0xFFFFFFFFull) {
    if (err) *err = "too much layer data for a PSD file: export at a lower size";
    return false;
  }
  put32At(o, liAt, uint32_t(liLen));
  put32At(o, lmiAt, uint32_t(liLen + 4 + 4));  // layer info + its length word + global mask info word
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  bool ok = std::fwrite(o.data(), 1, o.size(), f) == o.size();
  for (const Layer& l : layers_)
    for (const Channel& c : l.channels) ok &= std::fwrite(c.data.data(), 1, c.data.size(), f) == c.data.size();
  std::vector<uint8_t> tail;
  if (pad) tail.push_back(0);
  be32(tail, 0);  // global layer mask info
  be16(tail, 0);  // composite: raw
  ok &= std::fwrite(tail.data(), 1, tail.size(), f) == tail.size();
  if (composite_.empty()) {
    std::vector<uint8_t> zero(size_t(w_) * 3 * (bits_ / 8), 0);
    for (int y = 0; y < h_; ++y) ok &= std::fwrite(zero.data(), 1, zero.size(), f) == zero.size();
  } else {
    ok &= std::fwrite(composite_.data(), 1, composite_.size(), f) == composite_.size();
  }
  ok &= std::fclose(f) == 0;
  if (!ok && err) *err = "could not write all of " + path;
  return ok;
}

}  // namespace patina
