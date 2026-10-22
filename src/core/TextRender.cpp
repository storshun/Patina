#include "core/TextRender.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

#include "core/Paths.h"

#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

namespace patina {

namespace fs = std::filesystem;

namespace {

struct Bundled {
  const char* name;
  const char* file;
};
const Bundled kBundled[] = {{"sans", "DejaVuSans.ttf"},
                            {"sans-bold", "DejaVuSans-Bold.ttf"},
                            {"serif", "DejaVuSerif.ttf"},
                            {"mono", "DejaVuSansMono.ttf"}};

std::vector<fs::path> systemFontDirs() {
  std::vector<fs::path> dirs;
#ifdef _WIN32
  if (const char* w = std::getenv("WINDIR")) dirs.push_back(fs::path(w) / "Fonts");
  else dirs.push_back("C:/Windows/Fonts");
  if (const char* l = std::getenv("LOCALAPPDATA")) dirs.push_back(fs::path(l) / "Microsoft/Windows/Fonts");
#else
  dirs.push_back("/usr/share/fonts");
  dirs.push_back("/usr/local/share/fonts");
  dirs.push_back(paths::homeDir() / ".local/share/fonts");
  dirs.push_back(paths::homeDir() / ".fonts");
#endif
  return dirs;
}

std::string lower(std::string s) {
  for (char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// UTF-8 to code points (invalid bytes become U+FFFD)
std::vector<uint32_t> decodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp = 0xFFFD;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (n == 1) cp = c;
    else if (n && i + n <= s.size()) {
      cp = c & (0x7F >> n);
      for (int k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    } else n = 1;
    out.push_back(cp);
    i += n;
  }
  return out;
}

}  // namespace

std::vector<FontInfo> availableFonts() {
  static std::mutex mu;
  static std::vector<FontInfo> cache;
  static bool scanned = false;
  std::lock_guard<std::mutex> lock(mu);
  if (scanned) return cache;
  scanned = true;
  for (auto& b : kBundled) {
    fs::path p = paths::dataDir() / "fonts" / b.file;
    std::error_code ec;
    if (fs::exists(p, ec)) cache.push_back({b.name, p.string(), true});
  }
  std::map<std::string, std::string> found;
  for (auto& dir : systemFontDirs()) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    int guard = 0;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator() && guard < 20000; it.increment(ec), ++guard) {
      if (ec) break;
      std::string ext = lower(it->path().extension().string());
      if (ext != ".ttf" && ext != ".otf" && ext != ".ttc") continue;
      std::string stem = it->path().stem().string();
      if (!found.count(stem)) found[stem] = it->path().string();
      if (found.size() >= 600) break;
    }
  }
  for (auto& [name, path] : found) cache.push_back({name, path, false});
  return cache;
}

std::string resolveFont(const std::string& nameOrPath) {
  std::string n = nameOrPath.empty() ? "sans" : nameOrPath;
  std::error_code ec;
  if (fs::exists(fs::path(n), ec) && fs::is_regular_file(fs::path(n), ec)) return n;
  for (auto& b : kBundled)
    if (n == b.name) {
      fs::path p = paths::dataDir() / "fonts" / b.file;
      if (fs::exists(p, ec)) return p.string();
    }
  std::string ln = lower(n);
  for (auto& f : availableFonts())
    if (lower(f.name) == ln) return f.path;
  return "";
}

int fontCoverage(const std::string& path, const std::vector<uint32_t>& codepoints, int index, std::vector<uint8_t>* has) {
  bool ok = false;
  std::string data = paths::readFile(path, &ok);
  if (!ok || data.size() < 12) return -1;
  const unsigned char* bytes = (const unsigned char*)data.data();
  int offset = stbtt_GetFontOffsetForIndex(bytes, index);
  stbtt_fontinfo font;
  if (offset < 0 || !stbtt_InitFont(&font, bytes, offset)) return -1;
  int n = 0;
  if (has) has->assign(codepoints.size(), 0);
  for (size_t i = 0; i < codepoints.size(); ++i) {
    bool g = stbtt_FindGlyphIndex(&font, (int)codepoints[i]) != 0;
    n += g;
    if (has) (*has)[i] = g;
  }
  return n;
}

std::vector<uint32_t> utf8Codepoints(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = (unsigned char)s[i];
    uint32_t cp = c;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (n > 1) {
      cp = c & (0x7F >> n);
      for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | ((unsigned char)s[i + k] & 63);
    }
    out.push_back(cp);
    i += n;
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

bool renderText(const std::string& utf8, const TextStyle& st, Image& out, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  std::string path = resolveFont(st.font);
  if (path.empty()) return fail("font not found: " + st.font);
  bool ok = false;
  std::string data = paths::readFile(path, &ok);
  if (!ok || data.empty()) return fail("cannot read font " + path);
  const unsigned char* bytes = (const unsigned char*)data.data();
  stbtt_fontinfo font;
  int offset = stbtt_GetFontOffsetForIndex(bytes, 0);
  if (offset < 0 || !stbtt_InitFont(&font, bytes, offset)) return fail("not a TrueType / OpenType font: " + path);

  float px = std::clamp(st.size, 4.f, 2048.f);
  float scale = stbtt_ScaleForPixelHeight(&font, px);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
  float lineH = (ascent - descent + gap) * scale * std::max(st.lineSpacing, 0.3f);
  float base0 = ascent * scale;

  // split into lines of code points
  std::vector<std::vector<uint32_t>> lines(1);
  for (uint32_t cp : decodeUtf8(utf8)) {
    if (cp == '\r') continue;
    if (cp == '\n') lines.emplace_back();
    else lines.back().push_back(cp == '\t' ? ' ' : cp);
  }
  auto lineWidth = [&](const std::vector<uint32_t>& L) {
    float x = 0;
    for (size_t i = 0; i < L.size(); ++i) {
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&font, (int)L[i], &adv, &lsb);
      x += adv * scale + st.letterSpacing;
      if (i + 1 < L.size()) x += stbtt_GetCodepointKernAdvance(&font, (int)L[i], (int)L[i + 1]) * scale;
    }
    return std::max(0.f, x - (L.empty() ? 0.f : st.letterSpacing));
  };
  std::vector<float> widths;
  float maxW = 0;
  for (auto& L : lines) widths.push_back(lineWidth(L)), maxW = std::max(maxW, widths.back());
  int pad = int(std::ceil(std::max(0.f, st.outline))) + int(std::ceil(px * 0.08f)) + 2;
  int W = std::max(1, int(std::ceil(maxW)) + 2 * pad);
  int H = std::max(1, int(std::ceil(lineH * (lines.size() - 1) + (ascent - descent) * scale)) + 2 * pad);
  if (size_t(W) * H > size_t(16384) * 16384 / 4) return fail("text image too large: reduce the size or the text");
  std::vector<float> cov(size_t(W) * H, 0.f);

  for (size_t li = 0; li < lines.size(); ++li) {
    auto& L = lines[li];
    float x = pad + (st.align == 1 ? (maxW - widths[li]) * 0.5f : st.align == 2 ? maxW - widths[li] : 0.f);
    float baseline = pad + base0 + lineH * li;
    for (size_t i = 0; i < L.size(); ++i) {
      int cp = (int)L[i];
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&font, cp, &adv, &lsb);
      float fx = x - std::floor(x);
      int x0, y0, x1, y1;
      stbtt_GetCodepointBitmapBoxSubpixel(&font, cp, scale, scale, fx, 0.f, &x0, &y0, &x1, &y1);
      int gw = x1 - x0, gh = y1 - y0;
      if (gw > 0 && gh > 0) {
        std::vector<unsigned char> bm(size_t(gw) * gh);
        stbtt_MakeCodepointBitmapSubpixel(&font, bm.data(), gw, gh, gw, scale, scale, fx, 0.f, cp);
        int ox = int(std::floor(x)) + x0, oy = int(std::round(baseline)) + y0;
        for (int y = 0; y < gh; ++y)
          for (int xx = 0; xx < gw; ++xx) {
            int X = ox + xx, Y = oy + y;
            if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
            float v = bm[size_t(y) * gw + xx] / 255.f;
            float& c = cov[size_t(Y) * W + X];
            c = std::max(c, v);
          }
      }
      x += adv * scale + st.letterSpacing;
      if (i + 1 < L.size()) x += stbtt_GetCodepointKernAdvance(&font, cp, (int)L[i + 1]) * scale;
    }
  }

  // outline: the coverage grown by a disc (max filter), soft-edged
  std::vector<float> outl;
  float r = std::max(0.f, st.outline);
  if (r > 0.f) {
    int ri = int(std::ceil(r));
    std::vector<float> tmp(cov.size(), 0.f);
    outl.assign(cov.size(), 0.f);
    // rows with any coverage within ri (horizontally) of x, to skip empty rows quickly
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        float m = 0;
        for (int dx = -ri; dx <= ri; ++dx) {
          int X = x + dx;
          if (X >= 0 && X < W) m = std::max(m, cov[size_t(y) * W + X]);
        }
        tmp[size_t(y) * W + x] = m;
      }
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        float m = 0;
        for (int dy = -ri; dy <= ri && m < 1.f; ++dy) {
          int Y = y + dy;
          if (Y < 0 || Y >= H || tmp[size_t(Y) * W + x] <= 0.f) continue;
          for (int dx = -ri; dx <= ri; ++dx) {
            int X = x + dx;
            if (X < 0 || X >= W) continue;
            float d = std::sqrt(float(dx * dx + dy * dy));
            float w = std::clamp(r + 0.5f - d, 0.f, 1.f);
            if (w > 0) m = std::max(m, cov[size_t(Y) * W + X] * w);
          }
        }
        outl[size_t(y) * W + x] = m;
      }
  }

  out.resize(W, H, 4);
  for (size_t i = 0; i < cov.size(); ++i) {
    float a = cov[i];
    vec3 c = st.color;
    if (!outl.empty()) {
      float ao = outl[i];
      float A = a + ao * (1.f - a);
      c = A > 1e-5f ? (st.color * a + st.outlineColor * ao * (1.f - a)) / A : st.color;
      a = A;
    }
    float* p = &out.pixels[i * 4];
    p[0] = c.x, p[1] = c.y, p[2] = c.z, p[3] = a;
  }
  return true;
}

}  // namespace patina
