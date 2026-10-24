#pragma once
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/Math.h"
#include "engine/UndoStore.h"
#include "gfx/Texture.h"

namespace patina {

class Document;

struct UndoEntry {
  std::string label;
  uint64_t serial = 0;  // set by UndoStack::push, unique for the session
  virtual ~UndoEntry() = default;
  virtual void undo(Document& d) = 0;
  virtual void redo(Document& d) = 0;
  virtual size_t bytes() const { return 0; }     // on the graphics card
  virtual size_t ramBytes() const { return 0; }  // (1.2.7) compressed in RAM
  // (1.2.7) moves one of the entry's texture copies to RAM, compressed; returns the bytes freed on the card
  // (0: none left there)
  virtual size_t packOne() { return 0; }
  uint64_t age = 0;  // set by UndoStack::push: the order of every entry in every object's history
  // this entry, or one folded into it (a group), has the serial
  virtual bool contains(uint64_t s) const { return s == serial; }
  // calls fn for this entry and every entry folded into it
  virtual void visit(const std::function<void(const UndoEntry&)>& fn) const { fn(*this); }
};

// Entries undone and redone as one step: an undo group (a Patina Assistant request, a command
// made of several edits). Undo runs the parts in reverse order.
struct CompoundEntry : UndoEntry {
  std::vector<std::unique_ptr<UndoEntry>> parts;  // in the order they were made
  void undo(Document& d) override {
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) (*it)->undo(d);
  }
  void redo(Document& d) override {
    for (auto& p : parts) p->redo(d);
  }
  size_t bytes() const override {
    size_t b = 0;
    for (auto& p : parts) b += p->bytes();
    return b;
  }
  size_t ramBytes() const override {
    size_t b = 0;
    for (auto& p : parts) b += p->ramBytes();
    return b;
  }
  size_t packOne() override {
    for (auto& p : parts)
      if (size_t b = p->packOne()) return b;
    return 0;
  }
  bool contains(uint64_t s) const override {
    if (s == serial) return true;
    for (auto& p : parts)
      if (p->contains(s)) return true;
    return false;
  }
  void visit(const std::function<void(const UndoEntry&)>& fn) const override {
    fn(*this);
    for (auto& p : parts) p->visit(fn);
  }
};

// Swaps layer textures (channel index 0..5 or -1 = mask) with stored copies.
struct TextureSwapEntry : UndoEntry {
  uint32_t layerUid = 0;
  struct Slot {
    int channel;  // -1 = mask
    UndoTexture tex;  // empty = slot did not exist
  };
  std::vector<Slot> slots;
  void swap(Document& d);
  void undo(Document& d) override { swap(d); }
  void redo(Document& d) override { swap(d); }
  size_t bytes() const override;
  size_t ramBytes() const override;
  size_t packOne() override;
};

// Like TextureSwapEntry but stores only the tiles a stroke touched, packed in a
// small atlas per channel. Undo/redo swap those tiles with the layer textures.
struct TileSwapEntry : UndoEntry {
  uint32_t layerUid = 0;
  int tileSize = 64;
  std::vector<ivec2> tiles;  // tile coordinates (in tiles)
  struct Slot {
    int channel;  // -1 = mask
    UndoTexture atlas;
  };
  std::vector<Slot> slots;
  void swap(Document& d);
  void undo(Document& d) override { swap(d); }
  void redo(Document& d) override { swap(d); }
  size_t bytes() const override;
  size_t ramBytes() const override;
  size_t packOne() override;
  static ivec2 atlasSize(size_t tileCount, int tileSize);
};

// Generic entry from two closures.
struct LambdaEntry : UndoEntry {
  std::function<void(Document&)> onUndo, onRedo;
  size_t size = 0;
  void undo(Document& d) override { onUndo(d); }
  void redo(Document& d) override { onRedo(d); }
  size_t bytes() const override { return size; }
};

// Layer-stack metadata (order, folders, every property except textures). Layers that
// leave the stack are parked in Document::graveyard so a later undo can bring them back.
struct StackState {
  std::vector<uint32_t> order;
  std::vector<std::string> meta;  // per layer, JSON without transient keys
  bool operator==(const StackState&) const = default;
};

struct StackStateEntry : UndoEntry {
  StackState before, after;
  size_t size = 0;
  void undo(Document& d) override;
  void redo(Document& d) override;
  size_t bytes() const override { return size; }
};

// (1.2.7) Limits for every object's history together (the user's preferences).
struct UndoLimits {
  size_t steps = 100;                       // steps per object
  size_t gpuBytes = size_t(1024) << 20;     // texture copies kept on the graphics card; older ones are compressed
  size_t ramBytes = size_t(4096) << 20;     // compressed copies in RAM; the oldest steps go beyond it
  static size_t defaultRam();               // a quarter of the machine's memory, 1 to 8 GB
};

class UndoStack {
 public:
  UndoStack();
  ~UndoStack();
  UndoStack(UndoStack&& o) noexcept;
  UndoStack& operator=(UndoStack&& o) noexcept;
  UndoStack(const UndoStack&) = delete;
  UndoStack& operator=(const UndoStack&) = delete;
  static UndoLimits& limits();
  // applies the limits to every history now (after they changed)
  static void enforceAll();
  // (1.2.7) a little of the packing: the oldest copies past the graphics card's budget go to RAM, one texture per
  // call (Engine::tick, while no stroke is on and nothing was pushed or undone for a moment). True: more to do.
  static bool idleWork();
  // all of that work now (commands that report the memory)
  static void settle();
  static bool idleDue();  // nothing was pushed, undone or redone for a moment
  // every object's history together
  static size_t totalBytes();
  static size_t totalRamBytes();
  void push(std::unique_ptr<UndoEntry> e);
  bool undo(Document& d);
  bool redo(Document& d);
  bool canUndo() const { return pos_ > 0; }
  bool canRedo() const { return pos_ < entries_.size(); }
  std::string undoLabel() const { return canUndo() ? entries_[pos_ - 1]->label : ""; }
  std::string redoLabel() const { return canRedo() ? entries_[pos_]->label : ""; }
  void clear();
  size_t bytes() const;     // on the graphics card
  size_t ramBytes() const;  // (1.2.7) compressed in RAM
  // history panel
  size_t count() const { return entries_.size(); }
  size_t position() const { return pos_; }  // entries [0, pos) are applied
  const std::string& label(size_t i) const { return entries_[i]->label; }
  const UndoEntry& entry(size_t i) const { return *entries_[i]; }
  uint64_t version() const { return version_; }
  // Entries by serial (macro recorder): the newest applied entry, and whether an entry is in
  // effect (applied, or trimmed off the old end) or not (undone, or dropped with the redo history).
  uint64_t topSerial() const { return pos_ ? entries_[pos_ - 1]->serial : appliedFloor_; }
  bool isApplied(uint64_t serial) const;
  // Undo groups: the entries pushed between beginGroup and endGroup become one CompoundEntry
  // called `label` (one step in the history, one serial for the macro recorder). Groups nest;
  // only the outermost folds. cancelGroup undoes what the group pushed and drops it. Use the
  // Document wrappers, which record pending layer edits first.
  void beginGroup(const std::string& label);
  bool endGroup();  // false: the group pushed nothing
  void cancelGroup(Document& d);
  bool inGroup() const { return groupDepth_ > 0; }

 private:
  std::deque<std::unique_ptr<UndoEntry>> entries_;
  size_t pos_ = 0;
  uint64_t version_ = 0;
  uint64_t nextSerial_ = 1;
  uint64_t appliedFloor_ = 0;          // serials up to here that are no longer listed were applied
  std::vector<uint64_t> droppedRedo_;  // ... except these: undone, then dropped
  int groupDepth_ = 0;
  bool groupPushed_ = false;
  size_t groupStart_ = 0;              // entries [groupStart_, pos_) belong to the open group
  std::string groupLabel_;
  void enforceLimits();
  bool dropOldest();  // false: nothing left to drop
  static void dropPastRam();
  static void touched();  // a push, undo or redo: idle work waits a moment
};

}  // namespace patina
