#pragma once

#include <cstddef>
#include <functional>
#include <vector>

namespace midi_composer::project {

// Linear undo/redo stack of paired inverse operations. Entries are opaque
// closures so the history has no dependency on the edit layer; callers are
// responsible for revision bumps and change notifications around undo()/redo().
class UndoHistory final {
public:
    struct Entry {
        std::function<void()> undo;
        std::function<void()> redo;
    };

    void push(Entry entry) {
        entries_.resize(index_); // drop any redo tail
        entries_.push_back(std::move(entry));
        if (entries_.size() > kMaxDepth) {
            entries_.erase(entries_.begin());
        }
        index_ = entries_.size();
    }

    [[nodiscard]] bool can_undo() const noexcept { return index_ > 0; }
    [[nodiscard]] bool can_redo() const noexcept { return index_ < entries_.size(); }

    bool undo() {
        if (!can_undo()) return false;
        entries_[--index_].undo();
        return true;
    }

    bool redo() {
        if (!can_redo()) return false;
        entries_[index_++].redo();
        return true;
    }

    void clear() {
        entries_.clear();
        index_ = 0;
    }

private:
    static constexpr std::size_t kMaxDepth = 200;
    std::vector<Entry> entries_;
    std::size_t index_{0};
};

} // namespace midi_composer::project
