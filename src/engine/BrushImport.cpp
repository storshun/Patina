// (1.2) Brushes people bring: the images in their brushes folder, and Photoshop's .abr brush files (their sampled tips,
// written out as grayscale PNG files Patina's image tips read; white paints).
#include "engine/BrushImport.h"

#include <algorithm>
#include <cstring>
#include <filesystem>

#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"

namespace fs = std::filesystem;

namespace patina::brushimport {

namespace {

struct Reader {
  const std::string& d;
  size_t pos = 0;
  bool ok = true;
  explicit Reader(const std::string& data) : d(data) {}
  bool need(size_t n) {
    if (pos + n > d.size()) ok = false;
    return ok;
  }
  uint8_t u8() { return need(1) ? (uint8_t)d[pos++] : 0; }
  uint16_t u16() {
    if (!need(2)) return 0;
    uint16_t v = uint16_t((uint8_t)d[pos] << 8 | (uint8_t)d[pos + 1]);
    pos += 2;
    return v;
  }
  uint32_t u32() {
    if (!need(4)) return 0;
    uint32_t v = uint32_t((uint8_t)d[pos]) << 24 | uint32_t((uint8_t)d[pos + 1]) << 16 | uint32_t((uint8_t)d[pos + 2]) << 8 |
                 uint32_t((uint8_t)d[pos + 3]);
    pos += 4;
    return v;
  }
  int32_t i32() { return (int32_t)u32(); }
  int16_t i16() { return (int16_t)u16(); }
  void skip(size_t n) {
    if (need(n)) pos += n;
  }
};

// a sampled tip: `w` x `h` at `depth` bits, raw or PackBits by rows (a table of each row's packed size first)
bool readSample(Reader& r, int w, int h, int depth, int compression, std::vector<uint8_t>& out) {
  if (w <= 0 || h <= 0 || w > 10000 || h > 10000 || (depth != 8 && depth != 16)) return false;
  const int bpp = depth / 8;
  std::vector<uint8_t> raw(size_t(w) * h * bpp);
  if (compression == 0) {
    if (!r.need(raw.size())) return false;
    std::memcpy(raw.data(), r.d.data() + r.pos, raw.size());
    r.pos += raw.size();
  } else {
    std::vector<uint16_t> rowLen(h);
    for (int y = 0; y < h; ++y) rowLen[y] = r.u16();
    for (int y = 0; y < h && r.ok; ++y) {
      size_t end = r.pos + rowLen[y], x = 0, rowBytes = size_t(w) * bpp;
      uint8_t* row = raw.data() + size_t(y) * rowBytes;
      while (r.pos < end && x < rowBytes && r.ok) {
        int8_t n = (int8_t)r.u8();
        if (n >= 0) {
          for (int k = 0; k <= n && x < rowBytes; ++k) row[x++] = r.u8();
        } else if (n != -128) {
          uint8_t v = r.u8();
          for (int k = 0; k <= -n && x < rowBytes; ++k) row[x++] = v;
        }
      }
      r.pos = end;
    }
  }
  out.resize(size_t(w) * h);
  for (size_t i = 0; i < out.size(); ++i) out[i] = bpp == 1 ? raw[i] : raw[i * 2];  // (16 bits: the high byte)
  return r.ok;
}

}  // namespace

std::vector<AbrTip> readAbr(const std::string& path, std::string* err) {
  std::vector<AbrTip> tips;
  bool okRead = false;
  std::string data = paths::readFile(path, &okRead);
  if (!okRead || data.size() < 4) {
    if (err) *err = "Could not read " + path;
    return tips;
  }
  Reader r(data);
  uint16_t version = r.u16();
  if (version == 1 || version == 2) {
    // Photoshop 6 and before: a count, then each brush (type 2: sampled; type 1: computed, left out)
    uint16_t count = r.u16();
    for (uint16_t i = 0; i < count && r.ok; ++i) {
      uint16_t type = r.u16();
      uint32_t size = r.u32();
      size_t next = r.pos + size;
      if (type == 2) {
        r.skip(4);  // misc
        r.skip(2);  // spacing
        std::string name;
        if (version == 2) {
          uint32_t chars = r.u32();
          for (uint32_t k = 0; k < chars && r.ok; ++k) {
            uint16_t c = r.u16();
            if (c && c < 128) name += (char)c;
          }
        }
        r.skip(1);  // antialiasing
        r.skip(8);  // the bounds of the whole brush (short)
        int top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
        int depth = r.u16(), comp = r.u8();
        AbrTip t;
        t.name = name;
        t.width = right - left, t.height = bottom - top;
        if (readSample(r, t.width, t.height, depth, comp, t.pixels)) tips.push_back(std::move(t));
      }
      r.pos = next;
    }
  } else if (version >= 6 && version <= 10) {
    // Photoshop 7 and after: sections ("8BIM" + a key + a length); the sampled tips are in "samp"
    uint16_t sub = r.u16();
    while (r.ok && r.pos + 12 <= data.size()) {
      std::string sig = data.substr(r.pos, 4), key = data.substr(r.pos + 4, 4);
      r.pos += 8;
      uint32_t len = r.u32();
      size_t end = r.pos + len;
      if (sig != "8BIM") break;
      if (key == "samp") {
        while (r.ok && r.pos + 4 < end) {
          uint32_t size = r.u32();
          size_t next = r.pos + ((size + 3) & ~3u);
          r.skip(sub == 1 ? 47 : 301);  // (the tip's id and its own header)
          int top = r.i32(), left = r.i32(), bottom = r.i32(), right = r.i32();
          int depth = r.i16(), comp = r.u8();
          AbrTip t;
          t.width = right - left, t.height = bottom - top;
          if (r.ok && readSample(r, t.width, t.height, depth, comp, t.pixels)) tips.push_back(std::move(t));
          r.ok = true;
          r.pos = next;
        }
      }
      r.pos = end;
      if (r.pos > data.size()) break;
    }
  } else if (err) {
    *err = "Not a Photoshop brush file Patina can read (version " + std::to_string(version) + ")";
  }
  if (tips.empty() && err && err->empty()) *err = "No sampled brush tips in " + fs::path(path).filename().string();
  return tips;
}

std::vector<std::string> abrTipFiles(const std::string& abrPath, std::string* err) {
  std::error_code ec;
  auto size = fs::file_size(abrPath, ec);
  auto time = fs::last_write_time(abrPath, ec).time_since_epoch().count();
  std::string stem = paths::sanitizeFileName(fs::path(abrPath).stem().string());
  fs::path dir = paths::cacheDir() / "abr" / (stem + "-" + std::to_string(size) + "-" + std::to_string((long long)time % 1000000007));
  std::vector<std::string> files;
  if (fs::exists(dir / "done", ec)) {
    for (auto& e : fs::directory_iterator(dir, ec))
      if (e.path().extension() == ".png") files.push_back(e.path().string());
    std::sort(files.begin(), files.end());
    return files;
  }
  auto tips = readAbr(abrPath, err);
  if (tips.empty()) return files;
  paths::ensureDir(dir);
  for (size_t i = 0; i < tips.size(); ++i) {
    const AbrTip& t = tips[i];
    Image img;
    img.resize(t.width, t.height, 1);
    for (size_t k = 0; k < t.pixels.size(); ++k) img.pixels[k] = t.pixels[k] / 255.f;
    char name[32];
    snprintf(name, sizeof name, "%03zu.png", i + 1);
    fs::path p = dir / name;
    auto png = encodePng(img);
    if (paths::writeFileAtomic(p, std::string(png.begin(), png.end()))) files.push_back(p.string());
  }
  paths::writeFileAtomic(dir / "done", std::to_string(files.size()));
  PLOG_I("%s: %zu brush tips", fs::path(abrPath).filename().string().c_str(), files.size());
  return files;
}

}  // namespace patina::brushimport
