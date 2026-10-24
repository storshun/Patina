#include "engine/Undo.h"
#include <algorithm>
#include <chrono>

#include "core/Log.h"
#include "engine/Document.h"

namespace patina {

void TextureSwapEntry::swap(Document& d) {
  Layer* l = d.findLayer(layerUid);
  if (!l) return;
  for (auto& s : slots) s.tex.prefetch();
  for (auto& s : slots) s.tex.swapWith(s.channel < 0 ? l->mask : l->channels[s.channel]);
  d.markComposite();
}

size_t TextureSwapEntry::bytes() const {
  size_t b = 0;
  for (auto& s : slots) b += s.tex.gpuBytes();
  return b;
}

size_t TextureSwapEntry::ramBytes() const {
  size_t b = 0;
  for (auto& s : slots) b += s.tex.ramBytes();
  return b;
}

size_t TextureSwapEntry::packOne() {
  for (auto& s : slots) {
    size_t g = s.tex.gpuBytes();
    if (g && s.tex.pack()) return g;
  }
  return 0;
}

ivec2 TileSwapEntry::atlasSize(size_t n, int ts) {
  int cols = (int)std::min<size_t>(n, 32);
  int rows = (int)((n + 31) / 32);
  return {std::max(1, cols) * ts, std::max(1, rows) * ts};
}

static void copyTiles(GLuint src, GLuint dst, const std::vector<ivec2>& tiles, int ts, bool toAtlas) {
  for (size_t i = 0; i < tiles.size(); ++i) {
    int ax = int(i % 32) * ts, ay = int(i / 32) * ts;
    int tx = tiles[i].x * ts, ty = tiles[i].y * ts;
    if (toAtlas)
      glCopyImageSubData(src, GL_TEXTURE_2D, 0, tx, ty, 0, dst, GL_TEXTURE_2D, 0, ax, ay, 0, ts, ts, 1);
    else
      glCopyImageSubData(src, GL_TEXTURE_2D, 0, ax, ay, 0, dst, GL_TEXTURE_2D, 0, tx, ty, 0, ts, ts, 1);
  }
}

void TileSwapEntry::swap(Document& d) {
  Layer* l = d.findLayer(layerUid);
  if (!l) return;
  for (auto& s : slots) s.atlas.prefetch();
  for (auto& s : slots) {
    Texture2D* live = s.channel < 0 ? l->mask.get() : l->channels[s.channel].get();
    if (!live || !s.atlas) continue;
    std::unique_ptr<Texture2D> atlas = s.atlas.take();
    // live tiles -> fresh atlas, stored atlas -> live, keep the fresh atlas
    auto fresh = std::make_unique<Texture2D>(atlas->width(), atlas->height(), atlas->format(), 1);
    copyTiles(live->id(), fresh->id(), tiles, tileSize, true);
    copyTiles(atlas->id(), live->id(), tiles, tileSize, false);
    s.atlas = std::move(fresh);
  }
  d.markComposite();
}

size_t TileSwapEntry::bytes() const {
  size_t b = 0;
  for (auto& s : slots) b += s.atlas.gpuBytes();
  return b;
}

size_t TileSwapEntry::ramBytes() const {
  size_t b = 0;
  for (auto& s : slots) b += s.atlas.ramBytes();
  return b;
}

size_t TileSwapEntry::packOne() {
  for (auto& s : slots) {
    size_t g = s.atlas.gpuBytes();
    if (g && s.atlas.pack()) return g;
  }
  return 0;
}

void StackStateEntry::undo(Document& d) { d.applyState(before); }
void StackStateEntry::redo(Document& d) { d.applyState(after); }

// ------------------------------------------------------------------ every object's history together

namespace {
std::vector<UndoStack*>& stacks() {
  static std::vector<UndoStack*> all;
  return all;
}
uint64_t g_age = 0;
}  // namespace

size_t UndoLimits::defaultRam() {
  size_t mem = physicalMemoryBytes();
  size_t gb = size_t(1) << 30;
  return mem ? std::clamp(mem / 4, gb, 8 * gb) : 4 * gb;
}

UndoLimits& UndoStack::limits() {
  static UndoLimits l = [] {
    UndoLimits d;
    d.ramBytes = UndoLimits::defaultRam();
    return d;
  }();
  return l;
}

UndoStack::UndoStack() { stacks().push_back(this); }
UndoStack::~UndoStack() {
  auto& s = stacks();
  s.erase(std::remove(s.begin(), s.end(), this), s.end());
}
UndoStack::UndoStack(UndoStack&& o) noexcept : UndoStack() { *this = std::move(o); }
UndoStack& UndoStack::operator=(UndoStack&& o) noexcept {
  if (this == &o) return *this;
  entries_ = std::move(o.entries_);
  pos_ = o.pos_, version_ = o.version_ + 1, nextSerial_ = o.nextSerial_, appliedFloor_ = o.appliedFloor_;
  droppedRedo_ = std::move(o.droppedRedo_);
  groupDepth_ = o.groupDepth_, groupPushed_ = o.groupPushed_, groupStart_ = o.groupStart_;
  groupLabel_ = std::move(o.groupLabel_);
  o.entries_.clear();
  o.pos_ = 0, o.groupDepth_ = 0;
  return *this;
}

size_t UndoStack::totalBytes() {
  size_t b = 0;
  for (UndoStack* s : stacks()) b += s->bytes();
  return b;
}

size_t UndoStack::totalRamBytes() {
  size_t b = 0;
  for (UndoStack* s : stacks()) b += s->ramBytes();
  return b;
}

bool UndoStack::dropOldest() {
  if (entries_.empty() || groupDepth_ > 0) return false;
  appliedFloor_ = std::max(appliedFloor_, entries_.front()->serial);
  if (pos_ == 0) droppedRedo_.push_back(entries_.front()->serial);  // (undone: it was not in effect)
  else --pos_;
  entries_.pop_front();
  ++version_;
  return true;
}

void UndoStack::enforceAll() {
  for (UndoStack* s : stacks()) s->enforceLimits();
}

namespace {
using Clock = std::chrono::steady_clock;
Clock::time_point g_touched;
uint64_t g_usedAge = 0;  // the entry undone or redone last: it stays on the card, like the newest
}  // namespace

void UndoStack::touched() { g_touched = Clock::now(); }

// Past the RAM budget the oldest steps of any object go; never the newest step of all, nor the one used last.
void UndoStack::dropPastRam() {
  if (totalRamBytes() <= limits().ramBytes) return;
  UndoTexture::finishPacking();  // the compressed sizes, not the raw ones
  while (totalRamBytes() > limits().ramBytes) {
    UndoStack* oldest = nullptr;
    uint64_t newest = 0;
    for (UndoStack* s : stacks())
      if (!s->entries_.empty()) newest = std::max(newest, s->entries_.back()->age);
    for (UndoStack* s : stacks()) {
      if (s->entries_.empty() || s->groupDepth_ > 0) continue;
      uint64_t a = s->entries_.front()->age;
      if (a != newest && a != g_usedAge && (!oldest || a < oldest->entries_.front()->age)) oldest = s;
    }
    if (!oldest || !oldest->dropOldest()) break;
  }
}

// The texture copies on the card past their budget go to RAM, compressed, oldest first, one texture per call: a
// big model's step holds gigabytes, and reading it all back at once stopped the window for seconds.
bool UndoStack::idleWork() {
  const UndoLimits& L = limits();
  if (UndoTexture::packingBytes() > (size_t(256) << 20)) return true;  // (the raw copies waiting stay few)
  size_t gpu = totalBytes();
  if (gpu <= L.gpuBytes) {
    dropPastRam();
    return false;
  }
  uint64_t newest = 0;
  for (UndoStack* s : stacks())
    if (!s->entries_.empty()) newest = std::max(newest, s->entries_.back()->age);
  UndoEntry* pick = nullptr;
  for (UndoStack* s : stacks())
    if (s->groupDepth_ == 0)
      for (auto& e : s->entries_)
        if (e->age != newest && e->age != g_usedAge && e->bytes() && (!pick || e->age < pick->age)) pick = e.get();
  if (!pick || !pick->packOne()) {
    dropPastRam();
    return false;
  }
  return true;
}

void UndoStack::settle() {
  while (idleWork())
    if (UndoTexture::packingBytes() > (size_t(256) << 20)) UndoTexture::finishPacking();
  UndoTexture::finishPacking();
  dropPastRam();
}

bool UndoStack::idleDue() {
  return Clock::now() - g_touched > std::chrono::milliseconds(700);
}

void UndoStack::push(std::unique_ptr<UndoEntry> e) {
  ++version_;
  while (entries_.size() > pos_) {  // drop redo history
    droppedRedo_.push_back(entries_.back()->serial);
    entries_.pop_back();
  }
  e->serial = nextSerial_++;
  e->age = ++g_age;
  entries_.push_back(std::move(e));
  touched();
  pos_ = entries_.size();
  groupPushed_ |= groupDepth_ > 0;
  if (!groupDepth_) enforceLimits();  // an open group's entries stay put until it folds
}

void UndoStack::enforceLimits() {
  // the number of steps: oldest first (the copies on the card are packed by idleWork)
  while (entries_.size() > 1 && entries_.size() > std::max<size_t>(limits().steps, 1)) dropOldest();
  if (droppedRedo_.size() > 4096) droppedRedo_.erase(droppedRedo_.begin(), droppedRedo_.begin() + 2048);
  dropPastRam();
}

void UndoStack::beginGroup(const std::string& label) {
  if (groupDepth_++ > 0) return;
  // what was undone before the group starts is gone once the group pushes anything
  groupStart_ = pos_;
  groupLabel_ = label;
  groupPushed_ = false;
}

bool UndoStack::endGroup() {
  if (groupDepth_ == 0 || --groupDepth_ > 0) return false;
  size_t start = std::min(groupStart_, pos_);
  if (pos_ <= start) {
    enforceLimits();
    return false;
  }
  ++version_;
  while (entries_.size() > pos_) {  // undone inside the group: gone
    droppedRedo_.push_back(entries_.back()->serial);
    entries_.pop_back();
  }
  if (pos_ - start == 1) {  // a single entry stays as it is, under the group's name
    if (!groupLabel_.empty()) entries_.back()->label = groupLabel_;
    enforceLimits();
    return true;
  }
  auto c = std::make_unique<CompoundEntry>();
  c->label = groupLabel_;
  for (size_t i = start; i < pos_; ++i) c->parts.push_back(std::move(entries_[i]));
  entries_.erase(entries_.begin() + start, entries_.end());
  c->serial = nextSerial_++;
  c->age = ++g_age;
  entries_.push_back(std::move(c));
  pos_ = entries_.size();
  enforceLimits();
  return true;
}

void UndoStack::cancelGroup(Document& d) {
  if (groupDepth_ == 0) return;
  groupDepth_ = 0;
  if (!groupPushed_) return;
  size_t start = std::min(groupStart_, pos_);
  ++version_;
  while (pos_ > start) entries_[--pos_]->undo(d);
  while (entries_.size() > start) {
    entries_.back()->visit([&](const UndoEntry& e) { droppedRedo_.push_back(e.serial); });
    entries_.pop_back();
  }
  enforceLimits();
}

bool UndoStack::isApplied(uint64_t serial) const {
  for (size_t i = 0; i < entries_.size(); ++i)
    if (entries_[i]->contains(serial)) return i < pos_;
  if (std::find(droppedRedo_.begin(), droppedRedo_.end(), serial) != droppedRedo_.end()) return false;
  return serial <= appliedFloor_;
}

bool UndoStack::undo(Document& d) {
  if (!canUndo()) return false;
  ++version_;
  --pos_;
  entries_[pos_]->undo(d);
  PLOG_I("Undo: %s", entries_[pos_]->label.c_str());
  g_usedAge = entries_[pos_]->age;  // (its copies came back to the card: it stays there)
  touched();
  return true;
}

bool UndoStack::redo(Document& d) {
  if (!canRedo()) return false;
  ++version_;
  entries_[pos_]->redo(d);
  PLOG_I("Redo: %s", entries_[pos_]->label.c_str());
  g_usedAge = entries_[pos_]->age;
  touched();
  ++pos_;
  return true;
}

void UndoStack::clear() {
  ++version_;
  for (size_t i = 0; i < entries_.size(); ++i) {  // what is applied stays in effect
    if (i < pos_) appliedFloor_ = std::max(appliedFloor_, entries_[i]->serial);
    else droppedRedo_.push_back(entries_[i]->serial);
  }
  entries_.clear();
  pos_ = 0;
}

size_t UndoStack::bytes() const {
  size_t b = 0;
  for (auto& e : entries_) b += e->bytes();
  return b;
}

size_t UndoStack::ramBytes() const {
  size_t b = 0;
  for (auto& e : entries_) b += e->ramBytes();
  return b;
}

}  // namespace patina
