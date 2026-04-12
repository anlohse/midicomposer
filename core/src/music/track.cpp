#include "track.hpp"

namespace midi_composer::music {

Track::Track() = default;

Track::Track(base::TrackId id, std::string name) 
    : id_(id), name_(std::move(name)) {}

} // namespace midi_composer::music
