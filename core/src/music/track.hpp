#pragma once

#include "base/strong_id.hpp"
#include "music/clef.hpp"
#include "music/note.hpp"
#include "music/midi_events.hpp"
#include <string>
#include <vector>

namespace midi_composer::music {

class Track final {
public:
    Track();
    explicit Track(base::TrackId id, std::string name = "New Track");

    [[nodiscard]] base::TrackId id() const noexcept { return id_; }
    [[nodiscard]] std::string_view name() const noexcept { return name_; }
    void set_name(std::string name) { name_ = std::move(name); }

    [[nodiscard]] std::uint8_t midi_channel() const noexcept { return midi_channel_; }
    void set_midi_channel(std::uint8_t channel) { midi_channel_ = channel; }

    /**
     * Which output plays this track, or empty to follow the project's.
     *
     * Empty by default and empty for every project written before this existed,
     * which is what keeps one choice covering a whole composition: naming an
     * output per track would be the common case paying for the rare one.
     */
    [[nodiscard]] std::string_view output_id() const noexcept { return output_id_; }
    void set_output_id(std::string id) { output_id_ = std::move(id); }

    // Notation-only: which clef the score view draws this track's staff in.
    [[nodiscard]] Clef clef() const noexcept { return clef_; }
    void set_clef(Clef clef) noexcept { clef_ = clef; }

    [[nodiscard]] const std::vector<Note>& notes() const noexcept { return notes_; }
    [[nodiscard]] std::vector<Note>& notes() noexcept { return notes_; }

    [[nodiscard]] const std::vector<ControllerEvent>& controller_events() const noexcept { return controller_events_; }
    [[nodiscard]] std::vector<ControllerEvent>& controller_events() noexcept { return controller_events_; }

    [[nodiscard]] const std::vector<PitchBendEvent>& pitch_bends() const noexcept { return pitch_bends_; }
    [[nodiscard]] std::vector<PitchBendEvent>& pitch_bends() noexcept { return pitch_bends_; }

    [[nodiscard]] const std::vector<ProgramChangeEvent>& program_changes() const noexcept { return program_changes_; }
    [[nodiscard]] std::vector<ProgramChangeEvent>& program_changes() noexcept { return program_changes_; }

    [[nodiscard]] std::uint8_t volume() const noexcept { return volume_; }
    void set_volume(std::uint8_t v) { volume_ = v; }

    [[nodiscard]] std::uint8_t pan() const noexcept { return pan_; }
    void set_pan(std::uint8_t p) { pan_ = p; }

    [[nodiscard]] bool is_muted() const noexcept { return muted_; }
    void set_muted(bool m) { muted_ = m; }

    [[nodiscard]] bool is_solo() const noexcept { return solo_; }
    void set_solo(bool s) { solo_ = s; }

    [[nodiscard]] bool is_armed() const noexcept { return armed_; }
    void set_armed(bool a) { armed_ = a; }

    void add_note(const Note& note) { notes_.push_back(note); }

private:
    base::TrackId id_{};
    std::string name_;
    std::uint8_t midi_channel_{0}; // 0-based wire channel (channel "1" on the panel)
    std::string output_id_;
    Clef clef_{Clef::Treble};

    std::uint8_t volume_{100};
    std::uint8_t pan_{64};
    bool muted_{false};
    bool solo_{false};
    bool armed_{false};

    std::vector<Note> notes_;
    std::vector<ControllerEvent> controller_events_;
    std::vector<PitchBendEvent> pitch_bends_;
    std::vector<ProgramChangeEvent> program_changes_;
};

} // namespace midi_composer::music
