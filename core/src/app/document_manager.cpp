#include "document_manager.hpp"
#include "base/logger.hpp"

namespace midi_composer::app {

DocumentManager::DocumentManager() = default;
DocumentManager::~DocumentManager() = default;

base::CompositionId DocumentManager::create_new() {
    base::CompositionId id{m_next_id++};
    music::Composition comp(id);
    
    // Add default track
    comp.tracks().emplace_back(base::TrackId{m_next_track_id++}, "Piano");
    
    auto doc = std::make_unique<project::ProjectDocument>(std::move(comp));
    m_documents[id] = std::move(doc);
    
    m_active_document = id;
    
    MC_LOG_INFO("Created new document with ID: {}", id.value());
    return id;
}

base::CompositionId DocumentManager::adopt(music::Composition composition, std::string file_path) {
    base::CompositionId id{m_next_id++};

    // Preserve stored entity ids, but move the global counters past them so
    // future allocations never collide.
    for (const auto& track : composition.tracks()) {
        m_next_track_id = std::max(m_next_track_id, track.id().value() + 1);
        for (const auto& note : track.notes()) {
            m_next_note_id = std::max(m_next_note_id, note.id.value() + 1);
        }
    }

    // The composition id is process-local, not persisted.
    music::Composition comp(id);
    comp.set_title(std::string{composition.title()});
    comp.set_ppqn(composition.ppqn());
    // Every event map has to be carried across explicitly. Anything added to
    // Composition and forgotten here is silently dropped by both project load
    // and MIDI import, since both arrive through adopt().
    comp.tracks() = std::move(composition.tracks());
    comp.tempo_map().events() = std::move(composition.tempo_map().events());
    comp.time_signature_map().events() = std::move(composition.time_signature_map().events());
    comp.key_signature_map().events() = std::move(composition.key_signature_map().events());

    auto doc = std::make_unique<project::ProjectDocument>(std::move(comp));
    doc->set_file_path(std::move(file_path));
    m_documents[id] = std::move(doc);
    m_active_document = id;

    MC_LOG_INFO("Adopted document with ID: {}", id.value());
    return id;
}

bool DocumentManager::close_document(base::CompositionId id) {
    auto it = m_documents.find(id);
    if (it == m_documents.end()) {
        return false;
    }
    
    m_documents.erase(it);
    
    if (m_active_document == id) {
        if (!m_documents.empty()) {
            m_active_document = m_documents.begin()->first;
        } else {
            m_active_document = std::nullopt;
        }
    }
    
    MC_LOG_INFO("Closed document with ID: {}", id.value());
    return true;
}

project::ProjectDocument* DocumentManager::get_document(base::CompositionId id) {
    auto it = m_documents.find(id);
    if (it == m_documents.end()) {
        return nullptr;
    }
    return it->second.get();
}

void DocumentManager::set_active_document(base::CompositionId id) {
    if (m_documents.contains(id)) {
        m_active_document = id;
    }
}

} // namespace midi_composer::app
