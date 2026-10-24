#pragma once
// (1.2.7) Where undo keeps its texture copies. Recent steps keep them on the graphics card, so they undo at once;
// older ones are compressed into RAM (in the background) once the copies on the card pass their budget. Undo data
// is never written to disk: undo steps come and go all the time, and writing them would wear out the user's SSD.
#include <cstddef>
#include <memory>

#include "gfx/Texture.h"

namespace patina {

class UndoTexture {
 public:
  UndoTexture() = default;
  UndoTexture(std::nullptr_t) {}
  UndoTexture(std::unique_ptr<Texture2D> t) : gpu_(std::move(t)) {}  // NOLINT: a slot takes a texture copy as is
  UndoTexture(UndoTexture&&) noexcept = default;
  UndoTexture& operator=(UndoTexture&&) noexcept = default;
  ~UndoTexture();

  explicit operator bool() const { return gpu_ || packed_; }  // false: the slot had no texture
  bool packed() const { return packed_ != nullptr; }
  // the texture on the graphics card (nullptr when packed or empty)
  Texture2D* get() const { return gpu_.get(); }
  Texture2D* operator->() const { return gpu_.get(); }
  void reset() { gpu_.reset(), packed_.reset(); }
  // to RAM, compressed: reads the texture back now and compresses it on worker threads. False when it can't be
  // (an unusual format): it stays on the card.
  bool pack();
  // back on the graphics card (waits for the compression, then unpacks); the slot is empty afterward
  std::unique_ptr<Texture2D> take();
  // starts unpacking on the worker threads, so that the take() of many textures overlaps
  void prefetch();
  // exchanges the copy with a live texture slot (a layer's channel or mask)
  void swapWith(std::unique_ptr<Texture2D>& live);
  size_t gpuBytes() const;
  size_t ramBytes() const;  // compressed size (the raw size while it is still being compressed)

  // every texture still being compressed is done
  static void finishPacking();
  // raw bytes read back and still being compressed (in RAM for a moment)
  static size_t packingBytes();
  // bytes per pixel of a texture format (0 = one undo can't pack)
  static int pixelBytes(GLenum internalFormat);

  struct Packed;

 private:
  std::unique_ptr<Texture2D> gpu_;
  std::shared_ptr<Packed> packed_;
};

// The machine's physical memory in bytes (0 if unknown).
size_t physicalMemoryBytes();

}  // namespace patina
