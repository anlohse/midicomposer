#pragma once

#include "base/strong_id.hpp"
#include "timeline/tick.hpp"
#include <vector>

namespace midi_composer::music {

struct TempoEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint32_t microseconds_per_quarter{500000}; // 120 BPM

    [[nodiscard]] double bpm() const noexcept {
        return 60'000'000.0 / static_cast<double>(microseconds_per_quarter);
    }
};

class TempoMap final {
public:
    TempoMap();
    [[nodiscard]] const std::vector<TempoEvent>& events() const noexcept { return events_; }
    std::vector<TempoEvent>& events() noexcept { return events_; }

private:
    std::vector<TempoEvent> events_;
};

} // namespace midi_composer::music
