#pragma once

#include "music/composition.hpp"
#include "project/document_change.hpp"
#include "project/undo_history.hpp"
#include <cstdint>
#include <string>
#include <utility>

namespace midi_composer::project {

class ProjectDocument final {
public:
    explicit ProjectDocument(music::Composition composition);

    [[nodiscard]] music::Composition& composition() noexcept { return composition_; }
    [[nodiscard]] const music::Composition& composition() const noexcept { return composition_; }

    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] bool dirty() const noexcept { return dirty_; }

    void mark_dirty() noexcept { dirty_ = true; }
    void clear_dirty() noexcept { dirty_ = false; }
    void bump_revision() noexcept { revision_++; }

    [[nodiscard]] UndoHistory& history() noexcept { return history_; }
    [[nodiscard]] const UndoHistory& history() const noexcept { return history_; }

    // UTF-8 path of the backing project file; empty when never saved.
    [[nodiscard]] const std::string& file_path() const noexcept { return file_path_; }
    void set_file_path(std::string path) { file_path_ = std::move(path); }

    // ── Incremental change recording ─────────────────────────────────────────
    // Mutation helpers append here as they run; the facade drains the list after
    // the operation and publishes it to the UI as one patch. A discarded list
    // (failed command, rolled-back batch) is simply never published.

    void record_change(DocumentChange change) { pending_changes_.push_back(change); }

    [[nodiscard]] DocumentChangeList take_pending_changes() {
        return std::exchange(pending_changes_, DocumentChangeList{});
    }

private:
    music::Composition composition_;
    UndoHistory history_;
    std::string file_path_;
    DocumentChangeList pending_changes_;
    std::uint64_t revision_{0};
    bool dirty_{false};
};

} // namespace midi_composer::project
