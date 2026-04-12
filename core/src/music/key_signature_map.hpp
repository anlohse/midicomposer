#pragma once

#include "base/strong_id.hpp"
#include "timeline/tick.hpp"
#include <cstdint>
#include <vector>

namespace midi_composer::music {

// A key signature is stored as its position on the circle of fifths, which is
// all the notation needs: it gives both the accidentals to draw in the signature
// and the enharmonic spelling of every pitch. -7 = Cb (7 flats) … 0 = C … +7 =
// C# (7 sharps). `minor` only affects how the key is named in the UI, never the
// spelling, since a minor key shares its signature with its relative major.
struct KeySignatureEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::int8_t fifths{0};
    bool minor{false};

    bool operator==(const KeySignatureEvent&) const = default;
};

class KeySignatureMap final {
public:
    KeySignatureMap() {
        // Every composition has an effective key from tick 0: C major.
        events_.push_back(KeySignatureEvent{base::EventId{1}, timeline::Tick{0}, 0, false});
    }

    [[nodiscard]] const std::vector<KeySignatureEvent>& events() const noexcept { return events_; }
    std::vector<KeySignatureEvent>& events() noexcept { return events_; }

private:
    std::vector<KeySignatureEvent> events_;
};

} // namespace midi_composer::music
