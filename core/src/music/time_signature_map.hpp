#pragma once

#include "base/strong_id.hpp"
#include "timeline/tick.hpp"
#include <vector>

namespace midi_composer::music {

struct TimeSignatureEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint8_t numerator{4};
    std::uint8_t denominator{4};

    bool operator==(const TimeSignatureEvent&) const = default;
};

class TimeSignatureMap final {
public:
    TimeSignatureMap();
    [[nodiscard]] const std::vector<TimeSignatureEvent>& events() const noexcept { return events_; }
    std::vector<TimeSignatureEvent>& events() noexcept { return events_; }

private:
    std::vector<TimeSignatureEvent> events_;
};

} // namespace midi_composer::music
