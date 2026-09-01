#pragma once

#include "playback/sample_bank.hpp"

#include <memory>
#include <utility>

namespace midi_composer::testing {

/**
 * Building banks for tests, now that a bank has two halves.
 *
 * A `Sample` is audio and a `Zone` is how to play it, so almost every test
 * needs both and almost none cares about the distinction. These keep the split
 * out of the way of tests that are about something else.
 */

/** Audio at a constant level. Easy to find a peak in. */
inline playback::Sample flat_audio(float value, int frames, int rate) {
    playback::Sample s;
    s.data.assign(static_cast<size_t>(frames), value);
    s.source_rate = rate;
    return s;
}

/** A zone that plays a sample plainly: whole keyboard, immediate, held. */
inline playback::Zone plain_zone(int sample_index = 0, int root_key = 60) {
    playback::Zone z;
    z.sample = sample_index;
    z.root_key = root_key;
    z.attack = 0.0f;
    z.decay = 0.0f;
    z.sustain = 1.0f;
    z.release = 0.01f;
    return z;
}

/** One recording, one zone, one program. */
inline std::shared_ptr<playback::SampleBank> one_zone_bank(playback::Sample sample,
                                                           playback::Zone zone,
                                                           int program = 0) {
    auto bank = std::make_shared<playback::SampleBank>();
    bank->samples.push_back(std::move(sample));
    zone.sample = 0;
    bank->programs[static_cast<size_t>(program)].name = "Test";
    bank->programs[static_cast<size_t>(program)].zones.push_back(zone);
    return bank;
}

} // namespace midi_composer::testing
