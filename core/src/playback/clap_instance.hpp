#pragma once

#include "base/error.hpp"
#include "playback/output_plugin.hpp"

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace midi_composer::playback {

/**
 * Drives one CLAP plugin: an OutputPlugin that hands its events to a plugin
 * built to someone else's ABI, and an AudioSource that gives back what the
 * plugin renders.
 *
 * ── Why CLAP rather than an interface of our own ─────────────────────────────
 *
 * An in-house ABI is one nobody else ever writes a plugin for, and the whole
 * point of hosting is the instruments that already exist. CLAP is MIT, headers
 * only, and its event model turns out to be the one already built here: an
 * event carries a *sample offset within the block*, which is what `when_us` was
 * approximating, and activate() takes the sample rate from the host, which is
 * what §9a.5 said had to happen.
 *
 * ── Deliberately not the whole specification ─────────────────────────────────
 *
 * Enough to play a composition: notes, controllers, programs, bends, audio out.
 * No GUI (§4.2 -- and a plugin window over a WebView is its own project), no
 * parameter automation, no state, no latency compensation. Those are additions
 * to this shape rather than changes to it.
 *
 * Deliberately split from loading a `.clap` file: this half can be driven by a
 * plugin written in a test, which is the only way any of it could be verified
 * on a machine with no CLAP plugin installed.
 */
class ClapInstance final : public OutputPlugin, public AudioSource {
public:
    /**
     * Takes a plugin that has been created but not initialised.
     *
     * `id` and `name` are what this output is called in the application; they
     * come from the plugin's descriptor when there is a real one.
     */
    ClapInstance(const clap_plugin_t* plugin, std::string id, std::string name);
    ~ClapInstance() override;

    ClapInstance(const ClapInstance&) = delete;
    ClapInstance& operator=(const ClapInstance&) = delete;

    /** init(), and read the extensions that decide how events are sent. */
    base::Result<void> initialise();

    /**
     * The host descriptor to pass to create_plugin.
     *
     * It has to exist before the plugin does and outlive it, which is why the
     * instance is built first and the plugin handed to it afterwards.
     */
    [[nodiscard]] const clap_host_t* host() const { return &m_host; }

    /**
     * Take ownership of a created plugin, and of whatever has to outlive it.
     *
     * `owner` is the library the plugin came from: unloading it before the
     * plugin is destroyed would pull the code out from under the destructor, so
     * the instance holds it rather than the caller being trusted with the
     * order.
     */
    void adopt(const clap_plugin_t* plugin, std::shared_ptr<void> owner);

    [[nodiscard]] std::string_view id() const override { return m_id; }
    [[nodiscard]] std::string_view name() const override { return m_name; }

    [[nodiscard]] AudioSource* audio() override { return this; }

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
     * All 128, unnamed.
     *
     * A plugin does something of its own with a program change and there is no
     * way to ask what: `clap.preset-load` loads a preset from a path, and the
     * preset-discovery factory indexes preset files on disk, neither of which
     * says what program 12 is. Offering the General MIDI names instead would
     * label a JC-303 "Acoustic Grand Piano", which is worse than a number.
     */
    [[nodiscard]] std::vector<ProgramInfo> programs() const override;

    // ── AudioSource ──────────────────────────────────────────────────────────

    [[nodiscard]] int sample_rate() const override { return m_sample_rate; }
    void begin_block(int64_t start_us) override;
    void render(float* interleaved, int frames) override;

    /** The host decides the rate; the plugin is told. Before start(). */
    void set_sample_rate(int rate) { m_sample_rate = rate; }

    /** Whether the plugin takes raw MIDI. When it does not, only notes can be
        sent: a controller has nowhere to go that is not parameter automation. */
    [[nodiscard]] bool accepts_midi() const { return m_accepts_midi; }
    [[nodiscard]] bool accepts_clap_notes() const { return m_accepts_clap_notes; }

    /** Events this instance had nowhere to send. Silence with a reason. */
    [[nodiscard]] uint64_t dropped_events() const {
        return m_dropped.load(std::memory_order_relaxed);
    }

private:
    // The largest block the plugin is told to expect. Bigger than any device
    // period this host asks for, so activate() never has to be redone.
    static constexpr uint32_t kMaxBlock = 4096;

    // One queued event, in the union CLAP delivers. Kept as the widest of the
    // two rather than allocating per event: the audio thread reads these.
    struct Queued {
        union {
            clap_event_note_t note;
            clap_event_midi_t midi;
        } event{};
        int64_t when_us{0};
    };

    void push(const Queued& q);
    void flush_expired(int64_t block_start_us, int frames);

    static uint32_t input_size(const clap_input_events_t* list);
    static const clap_event_header_t* input_get(const clap_input_events_t* list, uint32_t index);
    static bool output_try_push(const clap_output_events_t* list,
                                const clap_event_header_t* event);

    const clap_plugin_t* m_plugin{nullptr};
    std::shared_ptr<void> m_owner;   // destroyed last: see adopt()
    std::string m_id;
    std::string m_name;

    clap_host_t m_host{};
    int  m_sample_rate{48000};
    bool m_initialised{false};
    bool m_active{false};
    bool m_processing{false};
    bool m_accepts_midi{false};
    bool m_accepts_clap_notes{false};

    // Producer side is serialised; the audio thread reads without locking, the
    // same split InternalSynthOutput uses and for the same reason.
    std::mutex m_producer;
    std::vector<Queued> m_pending;      // guarded by m_producer
    std::vector<Queued> m_block;        // audio thread only: this block's events
    std::vector<const clap_event_header_t*> m_block_headers;

    clap_input_events_t  m_in_events{};
    clap_output_events_t m_out_events{};

    // CLAP gives audio out as separate channel buffers, not interleaved.
    std::vector<float> m_left;
    std::vector<float> m_right;

    int64_t m_block_start_us{0};
    int64_t m_steady_time{0};
    std::atomic<uint64_t> m_dropped{0};
    mutable std::mutex m_failure_mutex;
    std::optional<base::Error> m_failure;
};

} // namespace midi_composer::playback
