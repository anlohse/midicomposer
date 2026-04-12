#pragma once

#include "project/project_document.hpp"
#include "base/strong_id.hpp"
#include <memory>
#include <string>
#include <unordered_map>
#include <optional>

namespace midi_composer::app {

class DocumentManager {
public:
    DocumentManager();
    ~DocumentManager();

    base::CompositionId create_new();
    // Wraps a loaded/imported composition in a new document, assigning a
    // fresh composition id and re-basing the track/note id counters above
    // any id present in the composition.
    base::CompositionId adopt(music::Composition composition, std::string file_path = {});
    bool close_document(base::CompositionId id);

    [[nodiscard]] project::ProjectDocument* get_document(base::CompositionId id);
    [[nodiscard]] const std::unordered_map<base::CompositionId, std::unique_ptr<project::ProjectDocument>>& documents() const { return m_documents; }

    void set_active_document(base::CompositionId id);
    [[nodiscard]] std::optional<base::CompositionId> active_document_id() const { return m_active_document; }

    [[nodiscard]] base::NoteId get_next_note_id() { return base::NoteId{m_next_note_id++}; }
    [[nodiscard]] base::TrackId get_next_track_id() { return base::TrackId{m_next_track_id++}; }

private:
    std::unordered_map<base::CompositionId, std::unique_ptr<project::ProjectDocument>> m_documents;
    std::optional<base::CompositionId> m_active_document;
    uint64_t m_next_id{1};
    uint64_t m_next_track_id{1};
    uint64_t m_next_note_id{1};
};

} // namespace midi_composer::app
