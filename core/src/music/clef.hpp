#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace midi_composer::music {

// Notation clef for a track's staff. Purely a display property — it never
// changes the stored pitches, only where the renderer draws them.
//
// The octave variants are transposing clefs: Treble8va sounds an octave above
// what is written, Bass8vb an octave below, so for the same MIDI pitch they put
// the notehead an octave's worth of staff steps away from their plain form.
enum class Clef : std::uint8_t {
    Treble,      // G clef on line 2
    Bass,        // F clef on line 4
    Alto,        // C clef on line 3
    Tenor,       // C clef on line 4
    Treble8va,   // G clef with 8 above
    Bass8vb,     // F clef with 8 below
};

inline std::string_view clef_to_string(Clef clef) noexcept {
    switch (clef) {
        case Clef::Bass:      return "bass";
        case Clef::Alto:      return "alto";
        case Clef::Tenor:     return "tenor";
        case Clef::Treble8va: return "treble8va";
        case Clef::Bass8vb:   return "bass8vb";
        case Clef::Treble:    break;
    }
    return "treble";
}

// Unknown names fall back to treble rather than failing: a project written by a
// newer build should still open.
inline Clef clef_from_string(std::string_view name) noexcept {
    if (name == "bass")      return Clef::Bass;
    if (name == "alto")      return Clef::Alto;
    if (name == "tenor")     return Clef::Tenor;
    if (name == "treble8va") return Clef::Treble8va;
    if (name == "bass8vb")   return Clef::Bass8vb;
    return Clef::Treble;
}

} // namespace midi_composer::music
