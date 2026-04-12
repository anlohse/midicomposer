#pragma once

#include <cstdint>

namespace midi_composer::playback {

enum class TransportState {
    Stopped,
    Playing,
    Paused,
    Recording,
    Seeking
};

// The wire name the UI sees. Lives here so the command reply and the pushed
// notification cannot drift apart into two spellings of the same state.
[[nodiscard]] inline const char* transport_state_name(TransportState state) {
    switch (state) {
        case TransportState::Playing:   return "playing";
        case TransportState::Paused:    return "paused";
        case TransportState::Recording: return "recording";
        case TransportState::Seeking:   return "seeking";
        default:                        return "stopped";
    }
}

} // namespace midi_composer::playback
