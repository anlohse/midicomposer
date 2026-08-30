#pragma once

#include "base/strong_id.hpp"
#include "music/track.hpp"
#include "music/key_signature_map.hpp"
#include "music/tempo_map.hpp"
#include "music/time_signature_map.hpp"
#include <string>
#include <vector>

namespace midi_composer::music {

class Composition final {
public:
    using TrackContainer = std::vector<Track>;

    Composition();
    explicit Composition(base::CompositionId id);

    [[nodiscard]] base::CompositionId id() const noexcept { return id_; }
    [[nodiscard]] std::string_view title() const noexcept { return title_; }
    void set_title(std::string title) { title_ = std::move(title); }

    [[nodiscard]] std::uint16_t ppqn() const noexcept { return ppqn_; }
    void set_ppqn(std::uint16_t ppqn) noexcept { if (ppqn > 0) ppqn_ = ppqn; }

    // Scales every track's volume on the way to its channel. MIDI has no master
    // level of its own, so it is a property of the composition rather than of
    // the device, and it is saved with the project like the track faders are.
    [[nodiscard]] std::uint8_t master_volume() const noexcept { return master_volume_; }
    void set_master_volume(std::uint8_t volume) noexcept { master_volume_ = volume > 127 ? 127 : volume; }

    [[nodiscard]] const TrackContainer& tracks() const noexcept { return tracks_; }
    [[nodiscard]] TrackContainer& tracks() noexcept { return tracks_; }

    [[nodiscard]] const TempoMap& tempo_map() const noexcept { return tempo_map_; }
    [[nodiscard]] TempoMap& tempo_map() noexcept { return tempo_map_; }

    [[nodiscard]] const TimeSignatureMap& time_signature_map() const noexcept { return time_signature_map_; }
    [[nodiscard]] TimeSignatureMap& time_signature_map() noexcept { return time_signature_map_; }

    [[nodiscard]] const KeySignatureMap& key_signature_map() const noexcept { return key_signature_map_; }
    [[nodiscard]] KeySignatureMap& key_signature_map() noexcept { return key_signature_map_; }

private:
    base::CompositionId id_{};
    std::string title_{"Untitled"};
    std::uint16_t ppqn_{480};
    std::uint8_t master_volume_{127};   // unity: track faders reach the channel as written
    TrackContainer tracks_;
    TempoMap tempo_map_;
    TimeSignatureMap time_signature_map_;
    KeySignatureMap key_signature_map_;
};

} // namespace midi_composer::music
