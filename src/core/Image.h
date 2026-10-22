#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace patina {

// CPU-side image. Pixel data is always stored as float RGBA-ish with `channels`
// components per pixel, row 0 = top of the image (file order).
struct Image {
  int width = 0;
  int height = 0;
  int channels = 4;
  std::vector<float> pixels;  // width*height*channels, linear or sRGB as stored

  bool empty() const { return pixels.empty(); }
  float* at(int x, int y) { return &pixels[(size_t(y) * width + x) * channels]; }
  const float* at(int x, int y) const { return &pixels[(size_t(y) * width + x) * channels]; }
  void resize(int w, int h, int c) {
    width = w;
    height = h;
    channels = c;
    pixels.assign(size_t(w) * h * c, 0.f);
  }
  void flipVertical();
};

// Load PNG/JPG/TGA/BMP/HDR/EXR (8, 16 bit or float). Returns false on failure.
// `forceChannels` 0 = keep file channels.
bool loadImage(const std::string& path, Image& out, int forceChannels = 0);

// Save by extension: .png (bitDepth 8 or 16), .jpg, .tga, .hdr
bool saveImage(const std::string& path, const Image& img, int bitDepth = 8);

// Encode as PNG in memory (8 bit).
std::vector<uint8_t> encodePng(const Image& img);
std::vector<uint8_t> encodePng16(const Image& img);
// Encode as JPEG in memory (8 bit; alpha is dropped). `quality` 1-100.
std::vector<uint8_t> encodeJpg(const Image& img, int quality = 92);
// Decode PNG/JPG/... from memory (8 or 16 bit).
bool decodeImage(const uint8_t* data, size_t len, Image& out, int forceChannels = 0);
std::vector<uint8_t> encodePngRGBA8(const uint8_t* rgba, int w, int h, int channels = 4);

// PNG of 8- or 16-bit samples (16 bit in host byte order), 1-4 channels, rows top first or
// bottom first (`bottomUp`: as OpenGL reads textures back). Compresses quickly (miniz at a low
// level, a filter per row) and is safe to call from several threads at once.
std::vector<uint8_t> encodePngSamples(const void* samples, int w, int h, int channels, int bits, bool bottomUp);
// Decodes a PNG (or another stb format) into 8- or 16-bit samples (`bits`: the file's depth,
// 16-bit samples in host order) with `channels` channels, rows top first or bottom first.
// Safe to call from several threads at once.
bool decodeSamples(const uint8_t* data, size_t len, int channels, bool bottomUp, std::vector<uint8_t>& out, int& w,
                   int& h, int& bits);

// Size and channel count of an encoded image (PNG, JPG...) without decoding it.
bool imageInfo(const uint8_t* data, size_t len, int& w, int& h, int& channels);
bool imageInfo(const std::string& path, int& w, int& h, int& channels);

std::string base64Encode(const uint8_t* data, size_t len);
std::vector<uint8_t> base64Decode(const std::string& s);

}  // namespace patina
