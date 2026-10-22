#pragma once
// (1.2) Brushes people bring: Photoshop's .abr brush files (engine/BrushImport.cpp). Image files in the brushes folder
// need nothing: BrushLibrary makes a brush of each.
#include <cstdint>
#include <string>
#include <vector>

namespace patina::brushimport {

struct AbrTip {
  std::string name;              // (Photoshop 6's files name them)
  int width = 0, height = 0;
  std::vector<uint8_t> pixels;   // 8-bit gray, row 0 at the top; 255 paints
};
// The sampled tips of a .abr file (versions 1 and 2, and 6 to 10: Photoshop 7 and after); computed tips are left out
std::vector<AbrTip> readAbr(const std::string& path, std::string* err);
// Its tips as grayscale PNG files in the cache (written once for each version of the file), in order
std::vector<std::string> abrTipFiles(const std::string& abrPath, std::string* err);

}  // namespace patina::brushimport
