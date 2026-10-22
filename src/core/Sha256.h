#pragma once
// SHA-256 (FIPS 180-4), for checking downloaded files such as the Assistant's model.
#include <cstdint>
#include <string>

namespace patina {

class Sha256 {
 public:
  Sha256();
  void update(const void* data, size_t n);
  std::string hex();  // finishes; the object can't be updated afterward

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t len_ = 0;
  uint64_t bits_ = 0;
};

// The SHA-256 of a file in lower-case hex, "" when it can't be read.
std::string sha256File(const std::string& path);

}  // namespace patina
