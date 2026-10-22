#include "core/Image.h"

#include <tinyexr.h>

#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES  // Image.cpp has its own crc32
#include <miniz.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>

#include "core/Log.h"

extern "C" unsigned char* stbi_zlib_compress(unsigned char* data, int data_len, int* out_len,
                                               int quality);

namespace patina {

void Image::flipVertical() {
  size_t row = size_t(width) * channels;
  std::vector<float> tmp(row);
  for (int y = 0; y < height / 2; ++y) {
    float* a = &pixels[size_t(y) * row];
    float* b = &pixels[size_t(height - 1 - y) * row];
    std::memcpy(tmp.data(), a, row * sizeof(float));
    std::memcpy(a, b, row * sizeof(float));
    std::memcpy(b, tmp.data(), row * sizeof(float));
  }
}

static std::string lowerExt(const std::string& path) {
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos) return "";
  std::string e = path.substr(dot + 1);
  std::transform(e.begin(), e.end(), e.begin(), ::tolower);
  return e;
}

bool loadImage(const std::string& path, Image& out, int forceChannels) {
  int w, h, n;
  std::string ext = lowerExt(path);
  if (ext == "exr") {
    float* rgba = nullptr;
    const char* exrErr = nullptr;
    if (LoadEXR(&rgba, &w, &h, path.c_str(), &exrErr) != TINYEXR_SUCCESS) {
      PLOG_E("Failed to load image '%s': %s", path.c_str(), exrErr ? exrErr : "EXR error");
      if (exrErr) FreeEXRErrorMessage(exrErr);
      return false;
    }
    int c = forceChannels ? forceChannels : 4;
    out.resize(w, h, c);
    for (size_t i = 0; i < size_t(w) * h; ++i)
      for (int k = 0; k < c; ++k) out.pixels[i * c + k] = rgba[i * 4 + std::min(k, 3)];
    free(rgba);
    return true;
  }
  if (ext == "hdr") {
    float* data = stbi_loadf(path.c_str(), &w, &h, &n, forceChannels);
    if (!data) goto fail;
    int c = forceChannels ? forceChannels : n;
    out.resize(w, h, c);
    std::memcpy(out.pixels.data(), data, out.pixels.size() * sizeof(float));
    stbi_image_free(data);
    return true;
  }
  if (stbi_is_16_bit(path.c_str())) {
    uint16_t* data = stbi_load_16(path.c_str(), &w, &h, &n, forceChannels);
    if (!data) goto fail;
    int c = forceChannels ? forceChannels : n;
    out.resize(w, h, c);
    for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = data[i] / 65535.f;
    stbi_image_free(data);
    return true;
  } else {
    uint8_t* data = stbi_load(path.c_str(), &w, &h, &n, forceChannels);
    if (!data) goto fail;
    int c = forceChannels ? forceChannels : n;
    out.resize(w, h, c);
    for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = data[i] / 255.f;
    stbi_image_free(data);
    return true;
  }
fail:
  PLOG_E("Failed to load image '%s': %s", path.c_str(), stbi_failure_reason());
  return false;
}

static std::vector<uint8_t> toU8(const Image& img) {
  std::vector<uint8_t> v(img.pixels.size());
  for (size_t i = 0; i < v.size(); ++i)
    v[i] = (uint8_t)std::lround(std::clamp(img.pixels[i], 0.f, 1.f) * 255.f);
  return v;
}

static uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

static void putU32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24);
  v.push_back(x >> 16);
  v.push_back(x >> 8);
  v.push_back(x);
}

static void pngChunk(std::vector<uint8_t>& out, const char* type, const uint8_t* data, size_t len) {
  putU32(out, (uint32_t)len);
  size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  if (len) out.insert(out.end(), data, data + len);
  putU32(out, crc32(out.data() + start, len + 4));
}

// 16-bit PNG writer (stb_image_write only does 8 bit).
std::vector<uint8_t> encodePng16(const Image& img) {
  int c = img.channels;
  uint8_t colorType = c == 1 ? 0 : c == 2 ? 4 : c == 3 ? 2 : 6;
  size_t stride = size_t(img.width) * c * 2 + 1;
  std::vector<uint8_t> raw(stride * img.height);
  for (int y = 0; y < img.height; ++y) {
    uint8_t* row = &raw[y * stride];
    row[0] = 0;  // filter: none
    const float* src = img.at(0, y);
    for (int i = 0; i < img.width * c; ++i) {
      uint16_t v = (uint16_t)std::lround(std::clamp(src[i], 0.f, 1.f) * 65535.f);
      row[1 + i * 2] = v >> 8;
      row[2 + i * 2] = v & 0xFF;
    }
  }
  int zlen = 0;
  uint8_t* z = stbi_zlib_compress(raw.data(), (int)raw.size(), &zlen, 6);
  std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  putU32(ihdr, img.width);
  putU32(ihdr, img.height);
  ihdr.push_back(16);
  ihdr.push_back(colorType);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  pngChunk(out, "IHDR", ihdr.data(), ihdr.size());
  pngChunk(out, "IDAT", z, zlen);
  pngChunk(out, "IEND", nullptr, 0);
  free(z);
  return out;
}

static void appendToVector(void* ctx, void* data, int size) {
  auto* v = (std::vector<uint8_t>*)ctx;
  v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
}

std::vector<uint8_t> encodePngRGBA8(const uint8_t* rgba, int w, int h, int channels) {
  std::vector<uint8_t> out;
  stbi_write_png_to_func(appendToVector, &out, w, h, channels, rgba, w * channels);
  return out;
}

std::vector<uint8_t> encodePng(const Image& img) {
  auto u8 = toU8(img);
  return encodePngRGBA8(u8.data(), img.width, img.height, img.channels);
}

std::vector<uint8_t> encodeJpg(const Image& img, int quality) {
  auto u8 = toU8(img);
  std::vector<uint8_t> out;
  auto put = [](void* ctx, void* data, int size) {
    auto* v = (std::vector<uint8_t>*)ctx;
    v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
  };
  stbi_write_jpg_to_func(put, &out, img.width, img.height, img.channels, u8.data(), std::clamp(quality, 1, 100));
  return out;
}

std::vector<uint8_t> encodePngSamples(const void* samples, int w, int h, int channels, int bits, bool bottomUp) {
  const int bpp = channels * (bits == 16 ? 2 : 1);
  const size_t rowBytes = size_t(w) * bpp;
  std::vector<uint8_t> raw((rowBytes + 1) * size_t(h));
  std::vector<uint8_t> cur(rowBytes), prev(rowBytes, 0), trial(rowBytes);
  const uint8_t* src = (const uint8_t*)samples;
  auto paeth = [](int a, int b, int c) {
    int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
  };
  for (int y = 0; y < h; ++y) {
    const uint8_t* s = src + size_t(bottomUp ? h - 1 - y : y) * rowBytes;
    if (bits == 16) {
      for (size_t i = 0; i < rowBytes; i += 2) {
        uint16_t v;
        std::memcpy(&v, s + i, 2);
        cur[i] = uint8_t(v >> 8), cur[i + 1] = uint8_t(v);
      }
    } else {
      std::memcpy(cur.data(), s, rowBytes);
    }
    // the filter with the smallest sum of absolute (signed) bytes, as libpng picks
    uint8_t* out = &raw[size_t(y) * (rowBytes + 1)];
    uint64_t best = ~0ull;
    for (int f = 0; f < 5; ++f) {
      uint64_t sum = 0;
      for (size_t i = 0; i < rowBytes; ++i) {
        int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0;
        int pred = f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) / 2 : paeth(a, b, c);
        uint8_t v = uint8_t(cur[i] - pred);
        trial[i] = v;
        sum += v < 128 ? v : 256 - v;
      }
      if (sum < best) {
        best = sum;
        out[0] = uint8_t(f);
        std::memcpy(out + 1, trial.data(), rowBytes);
      }
      if (sum == 0) break;
    }
    std::swap(prev, cur);
  }
  size_t zlen = 0;
  int flags = tdefl_create_comp_flags_from_zip_params(2, 15, MZ_DEFAULT_STRATEGY) | TDEFL_WRITE_ZLIB_HEADER;
  void* z = tdefl_compress_mem_to_heap(raw.data(), raw.size(), &zlen, flags);
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  auto chunk = [&](const char* type, const uint8_t* data, size_t len) {
    putU32(png, (uint32_t)len);
    size_t start = png.size();
    png.insert(png.end(), type, type + 4);
    if (len) png.insert(png.end(), data, data + len);
    putU32(png, (uint32_t)mz_crc32(MZ_CRC32_INIT, png.data() + start, len + 4));
  };
  std::vector<uint8_t> ihdr;
  putU32(ihdr, (uint32_t)w);
  putU32(ihdr, (uint32_t)h);
  ihdr.push_back(uint8_t(bits == 16 ? 16 : 8));
  ihdr.push_back(uint8_t(channels == 1 ? 0 : channels == 2 ? 4 : channels == 3 ? 2 : 6));
  ihdr.push_back(0), ihdr.push_back(0), ihdr.push_back(0);
  chunk("IHDR", ihdr.data(), ihdr.size());
  chunk("IDAT", (const uint8_t*)z, z ? zlen : 0);
  chunk("IEND", nullptr, 0);
  mz_free(z);
  return png;
}

bool decodeSamples(const uint8_t* data, size_t len, int channels, bool bottomUp, std::vector<uint8_t>& out, int& w,
                   int& h, int& bits) {
  int n = 0;
  void* p = nullptr;
  bits = stbi_is_16_bit_from_memory(data, (int)len) ? 16 : 8;
  if (bits == 16) p = stbi_load_16_from_memory(data, (int)len, &w, &h, &n, channels);
  else p = stbi_load_from_memory(data, (int)len, &w, &h, &n, channels);
  if (!p) return false;
  size_t row = size_t(w) * channels * (bits / 8);
  out.resize(row * size_t(h));
  for (int y = 0; y < h; ++y)
    std::memcpy(&out[size_t(y) * row], (const uint8_t*)p + size_t(bottomUp ? h - 1 - y : y) * row, row);
  stbi_image_free(p);
  return true;
}

bool imageInfo(const uint8_t* data, size_t len, int& w, int& h, int& channels) {
  return stbi_info_from_memory(data, (int)len, &w, &h, &channels) != 0;
}

bool imageInfo(const std::string& path, int& w, int& h, int& channels) {
  return stbi_info(path.c_str(), &w, &h, &channels) != 0;
}

bool saveImage(const std::string& path, const Image& img, int bitDepth) {
  std::string ext = lowerExt(path);
  bool ok = false;
  if (ext == "png") {
    std::vector<uint8_t> bytes = bitDepth == 16 ? encodePng16(img) : encodePng(img);
    std::ofstream f(path, std::ios::binary);
    f.write((const char*)bytes.data(), bytes.size());
    ok = f.good();
  } else if (ext == "jpg" || ext == "jpeg") {
    auto u8 = toU8(img);
    ok = stbi_write_jpg(path.c_str(), img.width, img.height, img.channels, u8.data(), 95);
  } else if (ext == "tga") {
    auto u8 = toU8(img);
    ok = stbi_write_tga(path.c_str(), img.width, img.height, img.channels, u8.data());
  } else if (ext == "hdr") {
    ok = stbi_write_hdr(path.c_str(), img.width, img.height, img.channels, img.pixels.data());
  } else {
    PLOG_E("Unsupported image extension: %s", path.c_str());
    return false;
  }
  if (!ok) PLOG_E("Failed to write image %s", path.c_str());
  return ok;
}

bool decodeImage(const uint8_t* data, size_t len, Image& out, int forceChannels) {
  int w, h, n;
  if (stbi_is_16_bit_from_memory(data, (int)len)) {
    uint16_t* p = stbi_load_16_from_memory(data, (int)len, &w, &h, &n, forceChannels);
    if (!p) return false;
    out.resize(w, h, forceChannels ? forceChannels : n);
    for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = p[i] / 65535.f;
    stbi_image_free(p);
    return true;
  }
  uint8_t* p = stbi_load_from_memory(data, (int)len, &w, &h, &n, forceChannels);
  if (!p) return false;
  out.resize(w, h, forceChannels ? forceChannels : n);
  for (size_t i = 0; i < out.pixels.size(); ++i) out.pixels[i] = p[i] / 255.f;
  stbi_image_free(p);
  return true;
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64Encode(const uint8_t* data, size_t len) {
  std::string out;
  out.reserve((len + 2) / 3 * 4);
  for (size_t i = 0; i < len; i += 3) {
    uint32_t n = data[i] << 16;
    if (i + 1 < len) n |= data[i + 1] << 8;
    if (i + 2 < len) n |= data[i + 2];
    out.push_back(kB64[(n >> 18) & 63]);
    out.push_back(kB64[(n >> 12) & 63]);
    out.push_back(i + 1 < len ? kB64[(n >> 6) & 63] : '=');
    out.push_back(i + 2 < len ? kB64[n & 63] : '=');
  }
  return out;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
  int map[256];
  std::fill(std::begin(map), std::end(map), -1);
  for (int i = 0; i < 64; ++i) map[(uint8_t)kB64[i]] = i;
  std::vector<uint8_t> out;
  uint32_t buf = 0;
  int bits = 0;
  for (char ch : s) {
    int v = map[(uint8_t)ch];
    if (v < 0) continue;
    buf = (buf << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back((buf >> bits) & 0xFF);
    }
  }
  return out;
}

}  // namespace patina
