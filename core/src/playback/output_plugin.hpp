#pragma once

#include "base/error.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace midi_composer::playback {

// ── Configuration ────────────────────────────────────────────────────────────
//
// A plugin declares what it can be configured with; the host renders it. The
// host never learns what a sample bank is -- it learns that an output has a
// file parameter, and draws a file picker.
//
// There is deliberately no way for a plugin to draw its own dialog. Once that
// exists it becomes the path of least resistance and the application ends up
// with two UI toolkits; while it does not, every time a plugin appears to need
// one it is a signal that this list is missing a type, and adding the type is
// the correct fix.

enum class ParameterType {
    Enum,     // one of `choices`
    Int,      // bounded by min/max/step, labelled with `unit`
    Bool,
    String,
    File,     // a path, narrowed by `filter`
};

struct EnumChoice {
    // Stable, and what gets stored. Never an index: unplug a device and every
    // index shifts, so a remembered choice would silently become a different
    // one.
    std::string value;
    std::string label;   // shown
};

struct Parameter {
    std::string   name;                              // stable key
    std::string   label;                             // shown
    ParameterType type{ParameterType::String};

    // At most one parameter should set this. The status bar shows it beside the
    // plugin's name, which is what pays back the step this design adds: the
    // port used to be picked straight from a list, and is now one dialog away.
    bool headline{false};

    int         min{0};                              // Int
    int         max{0};
    int         step{1};
    std::string unit;

    std::vector<EnumChoice> choices;                 // Enum
    std::string             filter;                  // File, e.g. "*.spc"
};

// std::monostate means "not set". A variant because the plugin is compiled in;
// across a real ABI boundary this becomes a tagged union.
using ParameterValue = std::variant<std::monostate, std::string, int, bool>;

/**
 * Where playback sends what it plays.
 *
 * The system MIDI output is one implementation of this rather than the
 * privileged path, so an internal instrument can take its place without the
 * engine, the document or the UI knowing what it is. If SystemMidiOutput's
 * methods are not essentially the calls the OS already offers, the abstraction
 * is wrong -- that is what having the hardware behind the same interface is for.
 *
 * Compiled into the application, but written as if it were a DLL boundary: no
 * STL across it once it becomes one, no assumption of a shared allocator. See
 * docs/Output Plugin Spec.md.
 *
 * The method set is scoped to what MIDI Composer emits, not to the MIDI
 * specification. That is what makes typed methods safe: the set is closed by
 * the document model -- notes, program changes, controller events, pitch bends
 * -- and can only grow if the document does.
 *
 * The declared parameter schema in the spec is not here yet. Today's only
 * plugin has one setting, which port, and it is still reached through the
 * bridge commands that were already there.
 */
class OutputPlugin {
public:
    virtual ~OutputPlugin() = default;

    /** Stable across versions, because a project will store it. Not shown. */
    [[nodiscard]] virtual std::string_view id() const = 0;
    /** Shown to the user. */
    [[nodiscard]] virtual std::string_view name() const = 0;

    // ── Configuration. Called from the command thread. ───────────────────────
    //
    // Values are re-read after every set, and whenever the dialog opens, so a
    // parameter may change another's choices freely -- there is no notification
    // to send. The first plugin already needs that: MIDI ports appear and
    // disappear as controllers are plugged in.
    //
    // Defaulted, because a plugin with nothing to configure should not have to
    // say so three times.

    [[nodiscard]] virtual std::vector<Parameter> parameters() const { return {}; }

    [[nodiscard]] virtual ParameterValue get_parameter(std::string_view /*name*/) const {
        return {};
    }

    virtual base::Result<void> set_parameter(std::string_view name, const ParameterValue&) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "Unknown parameter: " + std::string(name)});
    }

    // ── Lifecycle. Called from the command thread. ───────────────────────────

    /**
     * Called as the transport starts. Anything expensive belongs here -- opening
     * a device, loading samples -- because it is the moment the user pressed
     * Play and is waiting for an answer.
     *
     * The error is what they are shown instead of unexplained silence, so it
     * should say what is missing rather than that something failed.
     */
    virtual base::Result<void> start() = 0;

    /** Called as the transport stops. Whether that closes anything is the
        implementation's business; the engine only stops sending. */
    virtual void stop() = 0;

    // ── Events. Called from the playback thread. ─────────────────────────────
    //
    // `when_us` is the instant the event was due, in microseconds on
    // steady_clock -- absolute rather than a delta, because an implementation
    // cannot know when the host computed a delta, and any delay between the two
    // would corrupt it silently.
    //
    // It is normally a little in the past: the engine sends when it notices an
    // event is due, on a ~5ms loop. What it carries is therefore not absolute
    // accuracy but the spacing *between* events of one iteration, which is what
    // a chord staying together depends on. An implementation that plays
    // immediately can ignore it, as the hardware one does.

    virtual void note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us) = 0;
    virtual void note_off(uint8_t channel, uint8_t pitch, int64_t when_us) = 0;
    virtual void controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t when_us) = 0;
    virtual void program_change(uint8_t channel, uint8_t program, int64_t when_us) = 0;
    virtual void pitch_bend(uint8_t channel, int16_t value, int64_t when_us) = 0;

    // ── Health. ──────────────────────────────────────────────────────────────

    /**
     * A latched failure, or nothing. Cleared by the next successful start().
     *
     * Latched rather than returned per event because a send fails for one
     * reason -- the device went away -- and that is a sticky condition, not a
     * per-message one. The engine reads this once per loop iteration, not once
     * per note.
     *
     * Read from the playback thread *and* the command thread, so it is the one
     * method an implementation has to make thread-safe on its own.
     */
    [[nodiscard]] virtual std::optional<base::Error> failure() const = 0;
};

} // namespace midi_composer::playback
