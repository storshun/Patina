#pragma once
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace patina {

// Writes a PNG row by row (top row first), so images far larger than memory (16K, 32K)
// can be written as they are rendered. 8 or 16 bits per channel, 1-4 channels.
class PngStream {
 public:
  PngStream();
  ~PngStream();
  bool open(const std::string& path, int width, int height, int channels, int bits, std::string* err = nullptr);
  // `rows` rows of width*channels samples (uint8_t for 8 bit, uint16_t host order for 16 bit)
  bool writeRows(const void* data, int rows);
  bool close();  // writes the end; false if anything failed
  int rowsWritten() const { return rows_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> d_;
  int rows_ = 0;
};

}  // namespace patina
