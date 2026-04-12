#pragma once

#include "base/strong_id.hpp"
#include "timeline/tick.hpp"
#include <optional>

namespace midi_composer::music {

struct Note final {
    base::NoteId id{};
    timeline::Tick start{};
    timeline::TickDuration duration{};

    std::uint8_t pitch{60};
    std::uint8_t velocity{100};
    std::optional<std::uint8_t> release_velocity{};

    [[nodiscard]] timeline::Tick end() const noexcept {
        return timeline::Tick{start.value() + duration.value()};
    }
};

} // namespace midi_composer::music
