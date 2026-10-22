#include "core/PngStream.h"

#include <cstdlib>
#include <cstring>
#include <vector>

#include <miniz.h>

namespace patina {

struct PngStream::Impl {
  FILE* f = nullptr;
  int w = 0, h = 0, ch = 0, bits = 8;
  bool ok = true;
  tdefl_compressor* comp = nullptr;
  std::vector<uint8_t> pending;  // compressed bytes waiting for an IDAT chunk
  std::vector<uint8_t> row;

  void put32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
  }
  void chunk(const char* type, const uint8_t* data, size_t n) {
    uint8_t hdr[8];
    put32(hdr, uint32_t(n));
    std::memcpy(hdr + 4, type, 4);
    mz_ulong crc = mz_crc32(MZ_CRC32_INIT, (const unsigned char*)type, 4);
    if (n) crc = mz_crc32(crc, data, n);
    uint8_t c[4];
    put32(c, uint32_t(crc));
    ok &= std::fwrite(hdr, 1, 8, f) == 8;
    if (n) ok &= std::fwrite(data, 1, n, f) == n;
    ok &= std::fwrite(c, 1, 4, f) == 4;
  }
  void flushIdat(bool all) {
    const size_t kChunk = 1 << 20;
    while (pending.size() >= kChunk || (all && !pending.empty())) {
      size_t n = std::min(pending.size(), kChunk);
      chunk("IDAT", pending.data(), n);
      pending.erase(pending.begin(), pending.begin() + n);
    }
  }
  static mz_bool sink(const void* buf, int len, void* user) {
    auto* self = (Impl*)user;
    const uint8_t* b = (const uint8_t*)buf;
    self->pending.insert(self->pending.end(), b, b + len);
    return MZ_TRUE;
  }
};

PngStream::PngStream() : d_(new Impl) {}
PngStream::~PngStream() {
  if (d_->f) std::fclose(d_->f);
  if (d_->comp) std::free(d_->comp);
}

bool PngStream::open(const std::string& path, int width, int height, int channels, int bits, std::string* err) {
  auto& d = *d_;
  d.f = std::fopen(path.c_str(), "wb");
  if (!d.f) {
    if (err) *err = "cannot write " + path;
    return false;
  }
  d.w = width, d.h = height, d.ch = channels, d.bits = bits == 16 ? 16 : 8;
  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  std::fwrite(sig, 1, 8, d.f);
  uint8_t ihdr[13];
  d.put32(ihdr, uint32_t(width));
  d.put32(ihdr + 4, uint32_t(height));
  ihdr[8] = uint8_t(d.bits);
  ihdr[9] = channels == 1 ? 0 : channels == 2 ? 4 : channels == 3 ? 2 : 6;
  ihdr[10] = ihdr[11] = ihdr[12] = 0;
  d.chunk("IHDR", ihdr, 13);
  d.comp = (tdefl_compressor*)std::malloc(sizeof(tdefl_compressor));
  int flags = tdefl_create_comp_flags_from_zip_params(4, 15, MZ_DEFAULT_STRATEGY) | TDEFL_WRITE_ZLIB_HEADER;
  if (tdefl_init(d.comp, &Impl::sink, &d, flags) != TDEFL_STATUS_OKAY) {
    if (err) *err = "compressor init failed";
    return false;
  }
  d.row.resize(1 + size_t(width) * channels * (d.bits / 8));
  return d.ok;
}

bool PngStream::writeRows(const void* data, int rows) {
  auto& d = *d_;
  if (!d.f || !d.ok) return false;
  size_t rowBytes = size_t(d.w) * d.ch * (d.bits / 8);
  const uint8_t* src = (const uint8_t*)data;
  for (int r = 0; r < rows && rows_ < d.h; ++r, ++rows_) {
    d.row[0] = 0;  // filter: none
    if (d.bits == 16) {
      const uint16_t* s = (const uint16_t*)(src + r * rowBytes);
      for (size_t i = 0; i < size_t(d.w) * d.ch; ++i) d.row[1 + i * 2] = uint8_t(s[i] >> 8), d.row[2 + i * 2] = uint8_t(s[i]);
    } else {
      std::memcpy(d.row.data() + 1, src + r * rowBytes, rowBytes);
    }
    if (tdefl_compress_buffer(d.comp, d.row.data(), d.row.size(), TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) d.ok = false;
    d.flushIdat(false);
  }
  return d.ok;
}

bool PngStream::close() {
  auto& d = *d_;
  if (!d.f) return false;
  bool ok = d.ok && rows_ == d.h;
  if (tdefl_compress_buffer(d.comp, nullptr, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) ok = false;
  d.flushIdat(true);
  d.chunk("IEND", nullptr, 0);
  ok &= d.ok;
  ok &= std::fclose(d.f) == 0;
  d.f = nullptr;
  return ok;
}

}  // namespace patina
