#pragma once

#include "base/error.hpp"
#include "document_manager.hpp"
#include "edit/edit_service.hpp"
#include "playback/playback_engine.hpp"
#include "device/audio_device.hpp"
#include "playback/internal_synth_output.hpp"
#include "playback/clap_library.hpp"
#include "playback/routing_output.hpp"
#include "playback/system_midi_output.hpp"
#include "preferences.hpp"
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace midi_composer::app {

class CoreFacade {
public:
    // Fired after every committed document mutation with a ready-to-send patch:
    // { documentId, baseRevision, revision, dirty, canUndo, canRedo, changes[] }.
    // The UI applies the changes to its mirror instead of re-fetching a whole
    // snapshot. May be invoked from the MIDI input thread (recorded notes).
    using DocumentPatchedCallback = std::function<void(const nlohmann::json&)>;

    CoreFacade();
    ~CoreFacade();

    void initialize();

    [[nodiscard]] std::string get_version() const;
    void exit_application();

    // Document management
    base::CompositionId new_project();
    bool close_project(base::CompositionId id);
    void set_active_document(base::CompositionId id);

    [[nodiscard]] nlohmann::json get_document_snapshot(base::CompositionId id) const;
    [[nodiscard]] std::vector<base::CompositionId> get_open_documents() const;

    // Persistence (paths are UTF-8; dialogs live at the bridge/shell layer)
    base::Result<void> save_project(base::CompositionId id, const std::string& path);
    base::Result<base::CompositionId> open_project(const std::string& path);
    base::Result<void> export_midi(base::CompositionId id, const std::string& path);
    base::Result<base::CompositionId> import_midi(const std::string& path);
    [[nodiscard]] std::string get_project_path(base::CompositionId id) const;

    // Note editing
    base::Result<base::NoteId> create_note(base::CompositionId doc_id, base::TrackId track_id, int64_t tick, int64_t duration, uint8_t pitch, uint8_t velocity);
    base::Result<void> delete_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id);
    base::Result<void> move_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id, int64_t new_tick);
    base::Result<void> resize_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id, int64_t new_duration);
    base::Result<void> update_note(base::CompositionId doc_id, base::TrackId track_id, base::NoteId note_id, std::optional<uint8_t> pitch, std::optional<uint8_t> velocity);

    // Atomic multi-operation edit; returns ids of created notes.
    base::Result<std::vector<base::NoteId>> batch_edit(base::CompositionId doc_id, std::vector<edit::BatchOperation> operations);

    // Undo / redo
    base::Result<void> undo(base::CompositionId doc_id);
    base::Result<void> redo(base::CompositionId doc_id);

    // Track management
    base::Result<base::TrackId> create_track(base::CompositionId doc_id, const std::string& name);
    base::Result<void> rename_track(base::CompositionId doc_id, base::TrackId track_id, const std::string& name);
    base::Result<void> delete_track(base::CompositionId doc_id, base::TrackId track_id);

    // Timeline
    base::Result<void> set_tempo(base::CompositionId doc_id, double bpm);
    // Applies at the start of the measure containing `tick`, since a meter change
    // relocates every following bar line.
    base::Result<void> set_time_signature(base::CompositionId doc_id, int64_t tick, uint8_t numerator, uint8_t denominator);
    // fifths is the position on the circle of fifths (-7..+7); minor only names
    // the key in the UI and never affects notation.
    base::Result<void> set_key_signature(base::CompositionId doc_id, int64_t tick, int8_t fifths, bool minor);

    // Track parameters
    base::Result<void> set_track_volume(base::CompositionId doc_id, base::TrackId track_id, uint8_t volume);
    base::Result<void> set_master_volume(base::CompositionId doc_id, uint8_t volume);
    base::Result<void> set_track_pan(base::CompositionId doc_id, base::TrackId track_id, uint8_t pan);
    base::Result<void> set_track_mute(base::CompositionId doc_id, base::TrackId track_id, bool mute);
    base::Result<void> set_track_solo(base::CompositionId doc_id, base::TrackId track_id, bool solo);
    base::Result<void> set_track_arm(base::CompositionId doc_id, base::TrackId track_id, bool arm);

    // Channel and instrument are musical content rather than mixer state, so
    // unlike volume/pan/mute/solo/arm these go through the edit service and are
    // undoable.
    base::Result<void> set_track_channel(base::CompositionId doc_id, base::TrackId track_id, uint8_t channel);
    base::Result<void> set_track_program(base::CompositionId doc_id, base::TrackId track_id, uint8_t program);

    // Notation display only; never alters stored pitches.
    base::Result<void> set_track_clef(base::CompositionId doc_id, base::TrackId track_id, music::Clef clef);

    // Controller events and pitch bends. Every field is editable; `update` only
    // touches the fields it is given.
    base::Result<base::EventId> create_controller_event(base::CompositionId doc_id, base::TrackId track_id,
                                                        int64_t tick, uint8_t controller, uint8_t value);
    base::Result<void> update_controller_event(base::CompositionId doc_id, base::TrackId track_id,
                                               base::EventId event_id, std::optional<int64_t> tick,
                                               std::optional<uint8_t> controller, std::optional<uint8_t> value);
    base::Result<void> delete_controller_event(base::CompositionId doc_id, base::TrackId track_id,
                                               base::EventId event_id);

    base::Result<base::EventId> create_pitch_bend(base::CompositionId doc_id, base::TrackId track_id,
                                                  int64_t tick, int16_t value);
    base::Result<void> update_pitch_bend(base::CompositionId doc_id, base::TrackId track_id,
                                         base::EventId event_id, std::optional<int64_t> tick,
                                         std::optional<int16_t> value);
    base::Result<void> delete_pitch_bend(base::CompositionId doc_id, base::TrackId track_id,
                                         base::EventId event_id);

    // Transport
    base::Result<void> play(base::CompositionId doc_id);
    base::Result<void> record(base::CompositionId doc_id);
    void stop();
    void pause();
    void shutdown();
    void seek(int64_t tick);
    void set_metronome_enabled(bool enabled);
    [[nodiscard]] bool is_metronome_enabled() const;

    void set_document_patched_callback(DocumentPatchedCallback cb);

    [[nodiscard]] playback::PlaybackEngine& playback_engine() { return m_playback_engine; }
    [[nodiscard]] device::MidiService& midi_service() { return m_midi_service; }
    /** The selected output. */
    [[nodiscard]] playback::OutputPlugin& output() { return *m_selected_output; }
    /** The audio device, open only while an output that makes sound is
        selected. Exposed so the shell can report what it is doing. */
    [[nodiscard]] const device::AudioDevice& audio_device() const { return m_audio_device; }
    /** Everything that can be selected, in the order it is offered. */
    [[nodiscard]] std::vector<playback::OutputPlugin*> outputs();
    base::Result<void> select_output(std::string_view id);

    /**
     * Change a setting on the selected output and remember it.
     *
     * Goes through the facade rather than straight to the plugin because the
     * remembering is the point: a port chosen once should still be chosen
     * tomorrow, and the plugin has no idea a preferences file exists.
     */
    base::Result<void> set_output_parameter(const std::string& name,
                                            const playback::ParameterValue& value);

    /** What this installation remembers between runs. */
    [[nodiscard]] const Preferences& preferences() const { return m_preferences; }

    /** Every folder scanned for plugins: the application's own, then the
        user's. Not the standard install locations, which ClapLibrary adds. */
    [[nodiscard]] std::vector<std::string> plugin_search_paths() const;

    /**
     * Replace the folders scanned for `.clap` files, and rescan.
     *
     * A rescan can only add: plugins already found stay, because one of them
     * may be the selected output or be named by a track, and dropping it would
     * silence a project to tidy up a list.
     */
    base::Result<void> set_clap_search_paths(std::vector<std::string> paths);
    /** Point one track at an output, or pass an empty id to follow the
        project's. */
    base::Result<void> set_track_output(base::CompositionId doc_id, base::TrackId track_id,
                                        const std::string& output_id);

    /** Render the composition to a WAV file through the selected output. */
    base::Result<void> export_audio(base::CompositionId doc_id, const std::string& path);

    void ping();

private:
    // Requires m_doc_mutex held. Applies a mutation to a track and refreshes
    // the playback snapshot when the edited document is being played.
    // What a track edit has to tell the playback engine. Rebuild is the safe
    // default; MixOnly exists because a fader move changes no note and arrives
    // once per pixel of the drag.
    enum class PlaybackSync { Rebuild, MixOnly };

    template <typename Fn>
    base::Result<void> with_track(base::CompositionId doc_id, base::TrackId track_id, Fn&& fn,
                                  PlaybackSync sync = PlaybackSync::Rebuild);

    void refresh_playback_if_active(base::CompositionId doc_id, const project::ProjectDocument& doc);
    void on_recorded_note(uint8_t pitch, uint8_t velocity, int64_t start_tick, int64_t duration);

    // Drains the changes the mutation recorded and pushes them as one patch.
    // Requires m_doc_mutex held. `base_revision` is the revision read before the
    // mutation: if it did not move, nothing was committed (failed command or a
    // rolled-back batch) and the recorded changes are discarded.
    void publish_changes(base::CompositionId doc_id, project::ProjectDocument& doc, uint64_t base_revision);

    // Builds the patch JSON without sending it, so callers on other threads can
    // release m_doc_mutex before notifying. Returns null when there is nothing
    // to publish. Requires m_doc_mutex held.
    nlohmann::json build_patch(base::CompositionId doc_id, project::ProjectDocument& doc, uint64_t base_revision);


    device::MidiService m_midi_service;
    // Declared before the engine: it is constructed with one of them.
    playback::SystemMidiOutput   m_system_output;
    playback::InternalSynthOutput m_synth_output;
    playback::OutputPlugin*      m_selected_output{&m_system_output};
    // What the engine actually plays into. The selected output is the default
    // behind it, and is what the UI configures and reports.
    playback::RoutingOutput      m_routing;

    // Plugins found on disk. The libraries are held because an instance's
    // destructor runs code that lives in them, and declared before the
    // instances so they are torn down after.
    std::vector<std::shared_ptr<playback::ClapLibrary>> m_clap_libraries;
    std::vector<std::unique_ptr<playback::ClapInstance>> m_clap_outputs;

    // Opens whatever CLAP plugins are installed, in the standard folders plus
    // whatever the preferences add. Failures are logged, never fatal: a plugin
    // that will not load should cost you that plugin.
    void discover_clap_plugins();

    // What this installation remembers. Declared after the outputs it names, so
    // nothing is restored onto an output that no longer exists.
    Preferences m_preferences;

    // Applies the remembered output and its parameters, after discovery has
    // decided which outputs there are to apply them to.
    void restore_preferred_output();

    // Best-effort: preferences failing to save must not fail the command that
    // changed them, because the change itself worked.
    void save_preferences();
    device::AudioDevice          m_audio_device;

    // Rebuilds the channel routes from a document's tracks.
    void refresh_routes(const project::ProjectDocument& doc);

    // What the device is currently pulling, so opening it again can be skipped.
    playback::AudioSource* m_audio_source{nullptr};

    // Opens the device when the selected output makes its own sound, closes it
    // when it does not.
    void follow_output_audio();
    DocumentManager m_document_manager;
    edit::EditService m_edit_service;
    playback::PlaybackEngine m_playback_engine;

    // Serializes all access to documents. Commands arrive on the bridge
    // thread while recorded notes are committed from the MIDI input thread.
    mutable std::mutex m_doc_mutex;
    std::optional<base::CompositionId> m_transport_doc_id;
    DocumentPatchedCallback m_document_patched_callback;
};

} // namespace midi_composer::app
