#pragma once

#include "base/strong_id.hpp"
#include "timeline/tick.hpp"
#include <cstdint>

namespace midi_composer::music {

struct ControllerEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint8_t controller{};
    std::uint8_t value{};
};

struct PitchBendEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::int16_t value{}; // -8192..8191
};

struct ProgramChangeEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint8_t program{};
};

} // namespace midi_composer::music
