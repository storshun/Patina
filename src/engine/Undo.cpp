#include "engine/Undo.h"
#include <algorithm>

#include "core/Log.h"
#include "engine/Document.h"

namespace patina {

void TextureSwapEntry::swap(Document& d) {
  Layer* l = d.findLayer(layerUid);
  if (!l) return;
  for (auto& s : slots) {
    std::unique_ptr<Texture2D>& dst = s.channel < 0 ? l->mask : l->channels[s.channel];
    std::swap(dst, s.tex);
  }
  d.markComposite();
}

size_t TextureSwapEntry::bytes() const {
  size_t b = 0;
  for (auto& s : slots)
    if (s.tex) b += size_t(s.tex->width()) * s.tex->height() * 4;
  return b;
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
  for (auto& s : slots) {
    Texture2D* live = s.channel < 0 ? l->mask.get() : l->channels[s.channel].get();
    if (!live || !s.atlas) continue;
    // live tiles -> fresh atlas, stored atlas -> live, keep the fresh atlas
    auto fresh = std::make_unique<Texture2D>(s.atlas->width(), s.atlas->height(), s.atlas->format(), 1);
    copyTiles(live->id(), fresh->id(), tiles, tileSize, true);
    copyTiles(s.atlas->id(), live->id(), tiles, tileSize, false);
    s.atlas = std::move(fresh);
  }
  d.markComposite();
}

size_t TileSwapEntry::bytes() const {
  size_t b = 0;
  for (auto& s : slots)
    if (s.atlas) b += size_t(s.atlas->width()) * s.atlas->height() * 4;
  return b;
}

void StackStateEntry::undo(Document& d) { d.applyState(before); }
void StackStateEntry::redo(Document& d) { d.applyState(after); }

void UndoStack::push(std::unique_ptr<UndoEntry> e) {
  ++version_;
  while (entries_.size() > pos_) {  // drop redo history
    droppedRedo_.push_back(entries_.back()->serial);
    entries_.pop_back();
  }
  e->serial = nextSerial_++;
  entries_.push_back(std::move(e));
  pos_ = entries_.size();
  groupPushed_ |= groupDepth_ > 0;
  if (!groupDepth_) enforceLimits();  // an open group's entries stay put until it folds
}

void UndoStack::enforceLimits() {
  // oldest first
  while (entries_.size() > 1 && (entries_.size() > maxEntries || bytes() > maxBytes)) {
    appliedFloor_ = std::max(appliedFloor_, entries_.front()->serial);
    entries_.pop_front();
    --pos_;
  }
  if (droppedRedo_.size() > 4096) droppedRedo_.erase(droppedRedo_.begin(), droppedRedo_.begin() + 2048);
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
  return true;
}

bool UndoStack::redo(Document& d) {
  if (!canRedo()) return false;
  ++version_;
  entries_[pos_]->redo(d);
  PLOG_I("Redo: %s", entries_[pos_]->label.c_str());
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

}  // namespace patina
