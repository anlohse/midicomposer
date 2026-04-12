#include "time_signature_map.hpp"

namespace midi_composer::music {

TimeSignatureMap::TimeSignatureMap() {
    TimeSignatureEvent ev;
    ev.id = base::EventId{0};
    ev.tick = timeline::Tick{0};
    ev.numerator = 4;
    ev.denominator = 4;
    events_.push_back(ev);
}

} // namespace midi_composer::music
