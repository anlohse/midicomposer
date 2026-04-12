#include "tempo_map.hpp"

namespace midi_composer::music {

TempoMap::TempoMap() {
    TempoEvent ev;
    ev.id = base::EventId{0};
    ev.tick = timeline::Tick{0};
    ev.microseconds_per_quarter = 500000;
    events_.push_back(ev);
}

} // namespace midi_composer::music
