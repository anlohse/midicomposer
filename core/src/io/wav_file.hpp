#pragma once

#include "base/error.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace midi_composer::io {

/**
 * Writes 16-bit stereo PCM. Enough for rendering a composition to a file and
 * nothing more: no float formats, no metadata chunks, no compression.
 */
base::Result<void> write_wav(const std::string& utf8_path,
                             const std::vector<float>& interleaved_stereo,
                             int sample_rate);

} // namespace midi_composer::io
