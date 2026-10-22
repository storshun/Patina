#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace patina {

// A layered Photoshop file (PSD, version 1: up to 30000 x 30000) in RGB, 8 or 16 bits.
// Layers are added bottom first; each keeps only its RLE-compressed channels in memory.
class PsdWriter {
 public:
  enum Section { Normal = 0, GroupOpen = 1, GroupEnd = 3 };
  PsdWriter(int width, int height, int bits);
  // planes: R, G, B, A interleaved as RGBA, top row first; 8 bit: uint8_t, 16 bit: uint16_t.
  // mask (optional): one plane, same size (white = shown). blendKey: Photoshop's 4-letter key.
  void addLayer(const std::string& name, const void* rgba, const void* mask, uint8_t opacity, const char* blendKey,
                bool visible);
  // groups: begin at the bottom (end marker), then the children, then the group itself
  void beginGroup();
  void endGroup(const std::string& name, const void* mask, uint8_t opacity, const char* blendKey, bool visible);
  // the flattened image shown by programs without layer support (RGB, top row first)
  void setComposite(const void* rgb);
  bool save(const std::string& path, std::string* err = nullptr) const;
  static const char* blendKeyFor(const std::string& patinaBlend);

 private:
  struct Channel {
    int16_t id;
    std::vector<uint8_t> data;  // compression word + row byte counts + packed rows
  };
  struct Layer {
    std::string name;
    std::vector<Channel> channels;
    bool hasMask = false;
    uint8_t opacity = 255;
    char blend[4] = {'n', 'o', 'r', 'm'};
    bool visible = true;
    int section = Normal;
  };
  std::vector<uint8_t> packPlane(const void* src, int stride, int offset) const;  // one channel, RLE
  int w_, h_, bits_;
  std::vector<Layer> layers_;
  std::vector<uint8_t> composite_;  // planar, raw
};

}  // namespace patina
