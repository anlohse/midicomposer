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
    TrackContainer tracks_;
    TempoMap tempo_map_;
    TimeSignatureMap time_signature_map_;
    KeySignatureMap key_signature_map_;
};

} // namespace midi_composer::music
