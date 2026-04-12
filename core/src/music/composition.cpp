#include "composition.hpp"

namespace midi_composer::music {

Composition::Composition() = default;

Composition::Composition(base::CompositionId id) : id_(id) {}

} // namespace midi_composer::music
