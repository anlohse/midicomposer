#pragma once

#include "base/error.hpp"
#include "music/composition.hpp"
#include <string>

namespace midi_composer::io {

// Standard MIDI File import/export built on libremidi's SMF reader/writer.
// Paths are UTF-8. Import assigns fresh sequential entity ids starting at 1;
// the document manager re-bases its counters when adopting the composition.
class MidiFile final {
public:
    static base::Result<void> export_file(const music::Composition& comp, const std::string& path);
    [[nodiscard]] static base::Result<music::Composition> import_file(const std::string& path);
};

} // namespace midi_composer::io
