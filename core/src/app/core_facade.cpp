#include "core_facade.hpp"
#include "base/logger.hpp"
#include "base/json_types.hpp"
#include "project/project_serializer.hpp"
#include "io/midi_file.hpp"
#include <algorithm>
#include <filesystem>

namespace {

// Filename stem of a UTF-8 path, for use as a document title.
std::string title_from_path(const std::string& utf8_path) {
    const std::filesystem::path p(reinterpret_cast<const char8_t*>(utf8_path.c_str()));
    const auto stem = p.stem().u8string();
    if (stem.empty()) return "Untitled";
    return std::string(reinterpret_cast<const char*>(stem.c_str()), stem.size());
}

// ── UI DTO serializers ───────────────────────────────────────────────────────
// Shared by the full snapshot and the incremental patches so both always agree
// on field names and shapes.

namespace mc = midi_composer;

nlohmann::json note_to_json(const mc::music::Note& note) {
    nlohmann::json n;
    n["id"] = note.id.value();
    n["startTick"] = note.start.value();
    n["durationTicks"] = note.duration.value();
    n["pitch"] = note.pitch;
    n["velocity"] = note.velocity;
    return n;
}

// Scalar track fields only — never the note or event collections.
nlohmann::json track_props_to_json(const mc::music::Track& track) {
    nlohmann::json t;
    t["id"] = track.id().value();
    t["name"] = track.name();
    t["midiChannel"] = track.midi_channel();
    t["clef"] = mc::music::clef_to_string(track.clef());
    t["volume"] = track.volume();
    t["pan"] = track.pan();
    t["muted"] = track.is_muted();
    t["solo"] = track.is_solo();
    t["armed"] = track.is_armed();
    return t;
}

nlohmann::json program_changes_to_json(const mc::music::Track& track) {
    auto arr = nlohmann::json::array();
    for (const auto& ev : track.program_changes()) {
        arr.push_back({{"id", ev.id.value()}, {"tick", ev.tick.value()}, {"program", ev.program}});
    }
    return arr;
}

nlohmann::json controller_events_to_json(const mc::music::Track& track) {
    auto arr = nlohmann::json::array();
    for (const auto& ev : track.controller_events()) {
        arr.push_back({{"id", ev.id.value()}, {"tick", ev.tick.value()},
                       {"controller", ev.controller}, {"value", ev.value}});
    }
    return arr;
}

nlohmann::json pitch_bends_to_json(const mc::music::Track& track) {
    auto arr = nlohmann::json::array();
    for (const auto& ev : track.pitch_bends()) {
        arr.push_back({{"id", ev.id.value()}, {"tick", ev.tick.value()}, {"value", ev.value}});
    }
    return arr;
}

nlohmann::json tempo_map_to_json(const mc::music::Composition& comp) {
    auto arr = nlohmann::json::array();
    for (const auto& ev : comp.tempo_map().events()) {
        arr.push_back({{"tick", ev.tick.value()}, {"bpm", ev.bpm()}});
    }
    return arr;
}

nlohmann::json key_signature_map_to_json(const mc::music::Composition& comp) {
    auto arr = nlohmann::json::array();
    for (const auto& ev : comp.key_signature_map().events()) {
        arr.push_back({{"tick", ev.tick.value()}, {"fifths", ev.fifths}, {"minor", ev.minor}});
    }
    return arr;
}

nlohmann::json time_signature_map_to_json(const mc::music::Composition& comp) {
    auto arr = nlohmann::json::array();
    for (const auto& ev : comp.time_signature_map().events()) {
        arr.push_back({{"tick", ev.tick.value()},
                       {"numerator", ev.numerator},
                       {"denominator", ev.denominator}});
    }
    return arr;
}

const mc::music::Track* find_track_const(const mc::music::Composition& comp, mc::base::TrackId id) {
    const auto& tracks = comp.tracks();
    auto it = std::find_if(tracks.begin(), tracks.end(), [id](const auto& t) { return t.id() == id; });
    return it == tracks.end() ? nullptr : &*it;
}

} // namespace

namespace midi_composer::app {

CoreFacade::CoreFacade() : m_playback_engine(m_midi_service) {
    MC_LOG_INFO("CoreFacade initialized");
}

CoreFacade::~CoreFacade() {
    MC_LOG_INFO("CoreFacade destroyed");
}

void CoreFacade::initialize() {
    MC_LOG_INFO("Initializing CoreFacade...");

    m_playback_engine.set_note_commit_callback(
        [this](uint8_t pitch, uint8_t velocity, int64_t start_tick, int64_t duration) {
            on_recorded_note(pitch, velocity, start_tick, duration);
        });

    // Auto-open first available MIDI output port
    auto devices = m_midi_service.get_output_devices();
    if (!devices.empty()) {
        MC_LOG_INFO("Auto-opening first available MIDI device: {}", devices[0].name);
        if (auto result = m_midi_service.open_output_port(devices[0].index); !result) {
            MC_LOG_WARN("Failed to open MIDI output: {}", result.error().message);
        }
    } else {
        MC_LOG_WARN("No MIDI output devices found during initialization");
    }
}

std::string CoreFacade::get_version() const {
    // Comes from project(VERSION) through the build, deliberately with no
    // fallback: a missing definition has to fail the compile rather than ship a
    // number that quietly disagrees with the installer and the release tag.
    return MIDI_COMPOSER_VERSION;
}

void CoreFacade::exit_application() {
    MC_LOG_INFO("Exit application requested via CoreFacade");
    // Stop the playback thread and silence sounding notes before terminating,
    // otherwise std::exit skips destructors and leaves notes hanging.
    m_playback_engine.shutdown();
    std::exit(0);
}

base::CompositionId CoreFacade::new_project() {
    std::lock_guard lock(m_doc_mutex);
    return m_document_manager.create_new();
}

bool CoreFacade::close_project(base::CompositionId id) {
    {
        std::lock_guard lock(m_doc_mutex);
        if (m_transport_doc_id == id) {
            m_transport_doc_id.reset();
        }
    }
    // Stop outside the lock; the playback engine has its own locking and the
    // recorded-note callback also takes m_doc_mutex.
    m_playback_engine.stop();
    std::lock_guard lock(m_doc_mutex);
    return m_document_manager.close_document(id);
}

void CoreFacade::set_active_document(base::CompositionId id) {
    std::lock_guard lock(m_doc_mutex);
    m_document_manager.set_active_document(id);
}

nlohmann::json CoreFacade::get_document_snapshot(base::CompositionId id) const {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = const_cast<DocumentManager&>(m_document_manager).get_document(id);
    if (!doc) {
        return nullptr;
    }

    nlohmann::json snapshot;
    snapshot["id"] = doc->composition().id().value();
    snapshot["title"] = doc->composition().title();
    snapshot["ppqn"] = doc->composition().ppqn();
    snapshot["masterVolume"] = doc->composition().master_volume();
    snapshot["revision"] = doc->revision();
    snapshot["dirty"] = doc->dirty();
    snapshot["filePath"] = doc->file_path();
    snapshot["canUndo"] = doc->history().can_undo();
    snapshot["canRedo"] = doc->history().can_redo();

    snapshot["tempoMap"] = tempo_map_to_json(doc->composition());
    snapshot["timeSignatureMap"] = time_signature_map_to_json(doc->composition());
    snapshot["keySignatureMap"] = key_signature_map_to_json(doc->composition());

    auto tracks = nlohmann::json::array();
    for (const auto& track : doc->composition().tracks()) {
        nlohmann::json t = track_props_to_json(track);

        auto notes = nlohmann::json::array();
        for (const auto& note : track.notes()) notes.push_back(note_to_json(note));
        t["notes"] = notes;

        // Through the same helpers the incremental patches use, so a snapshot and
        // a patch can never disagree about the shape of an event.
        t["controllerEvents"] = controller_events_to_json(track);
        t["pitchBends"] = pitch_bends_to_json(track);
        t["programChanges"] = program_changes_to_json(track);

        tracks.push_back(t);
    }
    snapshot["tracks"] = tracks;

    return snapshot;
}

std::vector<base::CompositionId> CoreFacade::get_open_documents() const {
    std::lock_guard lock(m_doc_mutex);
    std::vector<base::CompositionId> result;
    for (const auto& [id, _] : m_document_manager.documents()) {
        result.push_back(id);
    }
    // Deterministic tab order for the UI (unordered_map has none).
    std::sort(result.begin(), result.end());
    return result;
}

void CoreFacade::refresh_playback_if_active(base::CompositionId doc_id, const project::ProjectDocument& doc) {
    if (m_transport_doc_id != doc_id) return;
    auto state = m_playback_engine.state();
    if (state == playback::TransportState::Playing || state == playback::TransportState::Recording) {
        m_playback_engine.refresh_snapshot(doc);
    }
}

base::Result<base::NoteId> CoreFacade::create_note(base::CompositionId doc_id, base::TrackId track_id, int64_t tick, int64_t duration, uint8_t pitch, uint8_t velocity) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();

    auto result = m_edit_service.create_note(*doc, track_id, timeline::Tick{tick}, timeline::TickDuration{duration}, pitch, velocity, m_document_manager.get_next_note_id());
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::delete_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.delete_note(*doc, track_id, note_id);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::move_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id, int64_t new_tick) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.move_note(*doc, track_id, note_id, timeline::Tick{new_tick});
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::resize_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id, int64_t new_duration) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.resize_note(*doc, track_id, note_id, timeline::TickDuration{new_duration});
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::update_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id, std::optional<uint8_t> pitch, std::optional<uint8_t> velocity) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.update_note(*doc, track_id, note_id, pitch, velocity);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<std::vector<base::NoteId>> CoreFacade::batch_edit(base::CompositionId doc_id, std::vector<edit::BatchOperation> operations) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});

    // Created notes get their ids up front so the batch is self-contained
    // (and redo re-creates them with the same ids).
    for (auto& op : operations) {
        if (op.type == edit::BatchOperation::Type::CreateNote) {
            op.note_id = m_document_manager.get_next_note_id();
        }
    }

    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.batch_edit(*doc, operations);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::undo(base::CompositionId doc_id) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    if (!doc->history().undo()) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState, "Nothing to undo"});
    }
    doc->mark_dirty();
    doc->bump_revision();
    refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return {};
}

base::Result<void> CoreFacade::redo(base::CompositionId doc_id) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    if (!doc->history().redo()) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState, "Nothing to redo"});
    }
    doc->mark_dirty();
    doc->bump_revision();
    refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return {};
}

base::Result<base::TrackId> CoreFacade::create_track(base::CompositionId doc_id, const std::string& name) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.create_track(*doc, m_document_manager.get_next_track_id(), name);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::rename_track(base::CompositionId doc_id, base::TrackId track_id, const std::string& name) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.rename_track(*doc, track_id, name);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::delete_track(base::CompositionId doc_id, base::TrackId track_id) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.delete_track(*doc, track_id);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::set_tempo(base::CompositionId doc_id, double bpm) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.set_tempo(*doc, bpm);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::save_project(base::CompositionId id, const std::string& path) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});

    // Title follows the filename; set it before serializing so the file
    // stores the title it will reopen with.
    doc->composition().set_title(title_from_path(path));

    auto result = project::ProjectSerializer::save_file(doc->composition(), path);
    if (!result) return result;

    doc->set_file_path(path);
    doc->clear_dirty();
    doc->bump_revision();
    MC_LOG_INFO("Saved project to {}", path);
    return {};
}

base::Result<base::CompositionId> CoreFacade::open_project(const std::string& path) {
    std::lock_guard lock(m_doc_mutex);
    auto comp = project::ProjectSerializer::load_file(path);
    if (!comp) return std::unexpected(comp.error());
    comp->set_title(title_from_path(path));
    auto id = m_document_manager.adopt(std::move(*comp), path);
    MC_LOG_INFO("Opened project {} as document {}", path, id.value());
    return id;
}

base::Result<void> CoreFacade::export_midi(base::CompositionId id, const std::string& path) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    auto result = io::MidiFile::export_file(doc->composition(), path);
    if (result) MC_LOG_INFO("Exported MIDI to {}", path);
    return result;
}

base::Result<base::CompositionId> CoreFacade::import_midi(const std::string& path) {
    std::lock_guard lock(m_doc_mutex);
    auto comp = io::MidiFile::import_file(path);
    if (!comp) return std::unexpected(comp.error());
    comp->set_title(title_from_path(path));
    auto id = m_document_manager.adopt(std::move(*comp));
    MC_LOG_INFO("Imported MIDI {} as document {}", path, id.value());
    return id;
}

std::string CoreFacade::get_project_path(base::CompositionId id) const {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = const_cast<DocumentManager&>(m_document_manager).get_document(id);
    return doc ? doc->file_path() : std::string{};
}

template <typename Fn>
base::Result<void> CoreFacade::with_track(base::CompositionId doc_id, base::TrackId track_id, Fn&& fn,
                                          PlaybackSync sync) {
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    auto& tracks = doc->composition().tracks();
    auto it = std::find_if(tracks.begin(), tracks.end(), [track_id](const auto& t) { return t.id() == track_id; });
    if (it == tracks.end()) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Track not found"});
    const uint64_t base_revision = doc->revision();
    fn(*it);
    // These bypass the edit service (no undo entry), so the change record has
    // to be made here rather than inside an apply_* helper.
    doc->record_change({project::ChangeKind::TrackPropsUpdated, track_id, base::NoteId{}, {}});
    doc->mark_dirty();
    doc->bump_revision();
    if (sync == PlaybackSync::Rebuild) {
        refresh_playback_if_active(doc_id, *doc);
    } else {
        // Straight to the channel. Safe under m_doc_mutex: the engine takes its
        // own lock and never calls back into the facade while holding it.
        m_playback_engine.set_channel_mix(it->midi_channel(), it->volume(), it->pan());
    }
    publish_changes(doc_id, *doc, base_revision);
    return {};
}

base::Result<void> CoreFacade::set_track_volume(base::CompositionId doc_id, base::TrackId track_id, uint8_t volume) {
    std::lock_guard lock(m_doc_mutex);
    return with_track(doc_id, track_id, [volume](auto& t) { t.set_volume(volume); },
                      PlaybackSync::MixOnly);
}

base::Result<void> CoreFacade::set_master_volume(base::CompositionId doc_id, uint8_t volume) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();

    doc->composition().set_master_volume(volume);
    // Belongs to no track, so it carries no track id.
    doc->record_change({project::ChangeKind::MasterVolumeUpdated, base::TrackId{}, base::NoteId{}, {}});
    doc->mark_dirty();
    doc->bump_revision();
    // Straight to the engine for the same reason the track faders are: it
    // changes no note, and it arrives once per pixel of a drag.
    m_playback_engine.set_master_volume(volume);
    publish_changes(doc_id, *doc, base_revision);
    return {};
}

base::Result<void> CoreFacade::set_track_pan(base::CompositionId doc_id, base::TrackId track_id, uint8_t pan) {
    std::lock_guard lock(m_doc_mutex);
    return with_track(doc_id, track_id, [pan](auto& t) { t.set_pan(pan); },
                      PlaybackSync::MixOnly);
}

base::Result<void> CoreFacade::set_track_mute(base::CompositionId doc_id, base::TrackId track_id, bool mute) {
    std::lock_guard lock(m_doc_mutex);
    return with_track(doc_id, track_id, [mute](auto& t) { t.set_muted(mute); });
}

base::Result<void> CoreFacade::set_track_solo(base::CompositionId doc_id, base::TrackId track_id, bool solo) {
    std::lock_guard lock(m_doc_mutex);
    return with_track(doc_id, track_id, [solo](auto& t) { t.set_solo(solo); });
}

base::Result<void> CoreFacade::set_track_arm(base::CompositionId doc_id, base::TrackId track_id, bool arm) {
    std::lock_guard lock(m_doc_mutex);
    return with_track(doc_id, track_id, [arm](auto& t) { t.set_armed(arm); });
}

base::Result<void> CoreFacade::set_track_channel(base::CompositionId doc_id, base::TrackId track_id, uint8_t channel) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.set_track_channel(*doc, track_id, channel);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::set_time_signature(base::CompositionId doc_id, int64_t tick, uint8_t numerator, uint8_t denominator) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.set_time_signature(*doc, timeline::Tick{tick}, numerator, denominator);
    // The metronome reads the meter off the playback snapshot.
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::set_key_signature(base::CompositionId doc_id, int64_t tick, int8_t fifths, bool minor) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.set_key_signature(*doc, timeline::Tick{tick}, fifths, minor);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::set_track_clef(base::CompositionId doc_id, base::TrackId track_id, music::Clef clef) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.set_track_clef(*doc, track_id, clef);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

// Controller events and pitch bends. All six follow the same shape as the other
// undoable edits: lock, look the document up, run the edit, publish the patch.
// refresh_playback_if_active matters here — both are scheduled during playback,
// so an edit made while the transport runs has to reach the snapshot.

base::Result<base::EventId> CoreFacade::create_controller_event(
    base::CompositionId doc_id, base::TrackId track_id,
    int64_t tick, uint8_t controller, uint8_t value)
{
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.create_controller_event(*doc, track_id, timeline::Tick{tick},
                                                        controller, value);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::update_controller_event(
    base::CompositionId doc_id, base::TrackId track_id, base::EventId event_id,
    std::optional<int64_t> tick, std::optional<uint8_t> controller, std::optional<uint8_t> value)
{
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    std::optional<timeline::Tick> at;
    if (tick) at = timeline::Tick{*tick};
    auto result = m_edit_service.update_controller_event(*doc, track_id, event_id, at, controller, value);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::delete_controller_event(
    base::CompositionId doc_id, base::TrackId track_id, base::EventId event_id)
{
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.delete_controller_event(*doc, track_id, event_id);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<base::EventId> CoreFacade::create_pitch_bend(
    base::CompositionId doc_id, base::TrackId track_id, int64_t tick, int16_t value)
{
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.create_pitch_bend(*doc, track_id, timeline::Tick{tick}, value);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::update_pitch_bend(
    base::CompositionId doc_id, base::TrackId track_id, base::EventId event_id,
    std::optional<int64_t> tick, std::optional<int16_t> value)
{
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    std::optional<timeline::Tick> at;
    if (tick) at = timeline::Tick{*tick};
    auto result = m_edit_service.update_pitch_bend(*doc, track_id, event_id, at, value);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::delete_pitch_bend(
    base::CompositionId doc_id, base::TrackId track_id, base::EventId event_id)
{
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.delete_pitch_bend(*doc, track_id, event_id);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

base::Result<void> CoreFacade::set_track_program(base::CompositionId doc_id, base::TrackId track_id, uint8_t program) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (!doc) return std::unexpected(base::Error{base::ErrorCode::NotFound, "Document not found"});
    const uint64_t base_revision = doc->revision();
    auto result = m_edit_service.set_track_program(*doc, track_id, program);
    if (result) refresh_playback_if_active(doc_id, *doc);
    publish_changes(doc_id, *doc, base_revision);
    return result;
}

void CoreFacade::ping() {
    MC_LOG_DEBUG("Ping received in CoreFacade");
}

void CoreFacade::play(base::CompositionId doc_id) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (doc) {
        m_transport_doc_id = doc_id;
        m_playback_engine.play(*doc);
    }
}

void CoreFacade::record(base::CompositionId doc_id) {
    std::lock_guard lock(m_doc_mutex);
    auto* doc = m_document_manager.get_document(doc_id);
    if (doc) {
        m_transport_doc_id = doc_id;
        m_playback_engine.record(*doc);
    }
}

void CoreFacade::stop() {
    m_playback_engine.stop();
}

void CoreFacade::pause() {
    m_playback_engine.pause();
}

void CoreFacade::shutdown() {
    m_playback_engine.shutdown();
}

void CoreFacade::seek(int64_t tick) {
    m_playback_engine.seek(timeline::Tick{tick});
}

void CoreFacade::set_metronome_enabled(bool enabled) {
    m_playback_engine.set_metronome_enabled(enabled);
}

bool CoreFacade::is_metronome_enabled() const {
    return m_playback_engine.is_metronome_enabled();
}

void CoreFacade::set_document_patched_callback(DocumentPatchedCallback cb) {
    std::lock_guard lock(m_doc_mutex);
    m_document_patched_callback = std::move(cb);
}

void CoreFacade::publish_changes(base::CompositionId doc_id, project::ProjectDocument& doc, uint64_t base_revision) {
    auto patch = build_patch(doc_id, doc, base_revision);
    if (!patch.is_null() && m_document_patched_callback) m_document_patched_callback(patch);
}

nlohmann::json CoreFacade::build_patch(base::CompositionId doc_id, project::ProjectDocument& doc, uint64_t base_revision) {
    auto changes = doc.take_pending_changes();

    // Revision unchanged: the operation failed, was a no-op, or a batch rolled
    // back. Anything the helpers recorded on the way describes state that no
    // longer exists, so drop it.
    if (doc.revision() == base_revision) return nullptr;

    nlohmann::json patch;
    patch["documentId"] = doc_id.value();
    patch["baseRevision"] = base_revision;
    patch["revision"] = doc.revision();
    patch["dirty"] = doc.dirty();
    patch["canUndo"] = doc.history().can_undo();
    patch["canRedo"] = doc.history().can_redo();

    const auto& comp = doc.composition();
    auto items = nlohmann::json::array();
    bool resync = changes.empty();   // committed but described nothing: play safe

    for (const auto& change : changes) {
        nlohmann::json item;
        switch (change.kind) {
            case project::ChangeKind::NoteCreated:
            case project::ChangeKind::NoteUpdated:
                item["kind"] = change.kind == project::ChangeKind::NoteCreated ? "noteCreated" : "noteUpdated";
                item["trackId"] = change.track_id.value();
                item["note"] = note_to_json(change.note);
                break;
            case project::ChangeKind::NoteDeleted:
                item["kind"] = "noteDeleted";
                item["trackId"] = change.track_id.value();
                item["noteId"] = change.note_id.value();
                break;
            case project::ChangeKind::MasterVolumeUpdated:
                item["kind"] = "masterVolumeUpdated";
                item["masterVolume"] = comp.master_volume();
                break;

            case project::ChangeKind::TrackPropsUpdated: {
                const auto* track = find_track_const(comp, change.track_id);
                if (!track) { resync = true; continue; }
                item["kind"] = "trackPropsUpdated";
                item["track"] = track_props_to_json(*track);
                break;
            }
            case project::ChangeKind::TrackProgramsUpdated: {
                const auto* track = find_track_const(comp, change.track_id);
                if (!track) { resync = true; continue; }
                item["kind"] = "trackProgramsUpdated";
                item["trackId"] = change.track_id.value();
                item["programChanges"] = program_changes_to_json(*track);
                break;
            }
            case project::ChangeKind::TrackControllersUpdated: {
                const auto* track = find_track_const(comp, change.track_id);
                if (!track) { resync = true; continue; }
                item["kind"] = "trackControllersUpdated";
                item["trackId"] = change.track_id.value();
                item["controllerEvents"] = controller_events_to_json(*track);
                break;
            }
            case project::ChangeKind::TrackPitchBendsUpdated: {
                const auto* track = find_track_const(comp, change.track_id);
                if (!track) { resync = true; continue; }
                item["kind"] = "trackPitchBendsUpdated";
                item["trackId"] = change.track_id.value();
                item["pitchBends"] = pitch_bends_to_json(*track);
                break;
            }
            case project::ChangeKind::TempoMapUpdated:
                item["kind"] = "tempoMapUpdated";
                item["tempoMap"] = tempo_map_to_json(comp);
                break;
            case project::ChangeKind::TimeSignatureMapUpdated:
                item["kind"] = "timeSignatureMapUpdated";
                item["timeSignatureMap"] = time_signature_map_to_json(comp);
                break;
            case project::ChangeKind::KeySignatureMapUpdated:
                item["kind"] = "keySignatureMapUpdated";
                item["keySignatureMap"] = key_signature_map_to_json(comp);
                break;
            case project::ChangeKind::ResyncRequired:
                resync = true;
                continue;
        }
        items.push_back(std::move(item));
    }

    // A resync supersedes the individual items: the UI will re-snapshot anyway.
    if (resync) {
        patch["resync"] = true;
        patch["changes"] = nlohmann::json::array();
    } else {
        patch["resync"] = false;
        patch["changes"] = std::move(items);
    }
    return patch;
}


void CoreFacade::on_recorded_note(uint8_t pitch, uint8_t velocity, int64_t start_tick, int64_t duration) {
    // Runs on the MIDI input thread. The patch is built under the lock but sent
    // outside it, so notifying the UI never holds up document access.
    nlohmann::json patch;
    DocumentPatchedCallback notify;
    {
        std::lock_guard lock(m_doc_mutex);
        if (!m_transport_doc_id) return;
        auto* doc = m_document_manager.get_document(*m_transport_doc_id);
        if (!doc) return;

        auto& tracks = doc->composition().tracks();
        auto it = std::find_if(tracks.begin(), tracks.end(), [](const auto& t) { return t.is_armed(); });
        if (it == tracks.end()) {
            MC_LOG_WARN("Recorded note dropped: no armed track");
            return;
        }

        const uint64_t base_revision = doc->revision();
        auto result = m_edit_service.create_note(
            *doc, it->id(), timeline::Tick{std::max<int64_t>(0, start_tick)},
            timeline::TickDuration{duration}, pitch,
            velocity == 0 ? uint8_t{1} : velocity,
            m_document_manager.get_next_note_id());
        if (!result) {
            MC_LOG_WARN("Recorded note rejected: {}", result.error().message);
            doc->take_pending_changes();
            return;
        }
        patch = build_patch(*m_transport_doc_id, *doc, base_revision);
        notify = m_document_patched_callback;
    }
    if (notify && !patch.is_null()) notify(patch);
}

} // namespace midi_composer::app
