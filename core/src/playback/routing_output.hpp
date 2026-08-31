#pragma once

#include "playback/audio_mixer.hpp"
#include "playback/output_plugin.hpp"

#include <array>
#include <mutex>
#include <vector>

namespace midi_composer::playback {

/**
 * Sends each channel's events to whichever output that channel is routed to.
 *
 * This is how a track names its own output without the playback engine
 * learning what a track is. The engine deliberately has no such notion --
 * build_snapshot flattens tracks into channels, because that is what MIDI is --
 * and adding track identity to the playback path purely to route would put a
 * concept in the domain that the domain does not have.
 *
 * So a route belongs to a channel. Two tracks sharing one already share their
 * volume, pan, instrument and bend, because all of those are channel state; the
 * last one wins, exactly as it already does for the mixer. Which output plays
 * them is one more thing in that list rather than a new kind of conflict.
 *
 * Not offered to the user as an output itself: it has no settings and no name
 * worth showing. The facade hands it to the engine and keeps reporting the
 * selected output to the UI.
 */
class RoutingOutput final : public OutputPlugin {
public:
    [[nodiscard]] std::string_view id() const override { return "routing"; }

    /**
     * The name of whatever a channel falls back to.
     *
     * This layer is never chosen by anyone, so naming it in a message names
     * something the reader has never heard of: "the selected output does not
     * produce audio (Routing)" tells them nothing. What they picked is the
     * default, so that is what an error should call it.
     */
    [[nodiscard]] std::string_view name() const override;

    /** Where channels go when nothing routes them. Never null in practice. */
    void set_default_target(OutputPlugin* target);

    /**
     * Replace the whole map at once; null in a slot means the default.
     *
     * Returns whether anything actually moved, which the caller needs: the
     * routes are rebuilt on every document change, and only a real move should
     * make the engine forget what it has already sent.
     */
    bool set_routes(const std::array<OutputPlugin*, 16>& routes);

    /** The output a channel's events would reach. */
    [[nodiscard]] OutputPlugin* target_for(uint8_t channel) const;

    /**
     * Where the metronome clicks, for the sake of its lifecycle rather than its
     * events.
     *
     * The click does not go through this layer -- the engine holds the plugin
     * and sends to it directly, because a metronome has no channel to be routed
     * by. But the output still has to be *started*, told the sample rate, and
     * added to the mixer if it makes sound, and none of that happens for a
     * plugin no track points at. A user choosing an instrument nothing else
     * uses is the normal case here, not the odd one.
     *
     * Returns whether it moved.
     */
    bool set_metronome_target(OutputPlugin* target);

    /** Every distinct output currently reachable, default and metronome
        included. */
    [[nodiscard]] std::vector<OutputPlugin*> targets() const;

    // ── OutputPlugin ─────────────────────────────────────────────────────────

    base::Result<void> start() override;
    void stop() override;

    void note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us) override;
    void note_off(uint8_t channel, uint8_t pitch, int64_t when_us) override;
    void controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t when_us) override;
    void program_change(uint8_t channel, uint8_t program, int64_t when_us) override;
    void pitch_bend(uint8_t channel, int16_t value, int64_t when_us) override;

    [[nodiscard]] std::optional<base::Error> failure() const override;

    /**
     * Everything that makes sound, summed.
     *
     * Null only when nothing does. Several sources go through the mixer rather
     * than one of them being picked and the rest silently dropped, which is
     * what routing one track to the internal synth and another to a plugin
     * used to do.
     */
    [[nodiscard]] AudioSource* audio() override;

    /** The rate every audio target is asked to run at. */
    void set_host_sample_rate(int rate);

private:
    mutable std::mutex m_mutex;
    OutputPlugin* m_default{nullptr};
    OutputPlugin* m_metronome{nullptr};
    std::array<OutputPlugin*, 16> m_routes{};
    AudioMixer m_mixer;
    int m_host_sample_rate{48000};
};

} // namespace midi_composer::playback
