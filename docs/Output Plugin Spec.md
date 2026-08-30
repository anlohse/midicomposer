# Output Plugin Spec

Status: **design agreed, not implemented.** Two decisions are still open (§10).

## 1. What this is for

Playback output is hardcoded to `MidiService`, a concrete class wrapping libremidi.
`PlaybackEngine` calls five methods on it and there is no way to put anything else
behind them.

An `OutputPlugin` is the thing playback sends its events to. The system MIDI
output becomes one implementation of it rather than the privileged path, so an
internal instrument — an SPC-700 model, a soft synth — can be selected in its
place without the engine, the document or the UI knowing what it is.

Making the hardware output a plugin is the point, not symmetry for its own sake.
If `SystemMidiOutput::note_on` is not essentially a `midiOutShortMsg`, the
abstraction is wrong, and that stays true for as long as the code exists.

Two things fall out of it that are worth having on their own:

- **A recording plugin for tests.** `MidiService` talks to a real port and has no
  seam, which is why the last three changes to playback shipped with "not
  verified audibly" in their commit messages. CC 7, the master volume scaling and
  the state restored on seek all become assertable.
- **A place for per-track output routing** to land later, without redesigning
  anything at that point.

## 2. Scope

**Output only.** MIDI input keeps working exactly as it does now, through
`MidiService`. It has the same shape and could follow later; nothing here should
be named as if it already had.

The plugin interface is scoped to **what MIDI Composer emits**, not to the MIDI
specification. That is what makes typed methods safe: the method set is closed by
the document model, which has notes, program changes, controller events and pitch
bends — no aftertouch, no sysex. It can only grow if the document grows, and that
is a change of its own.

## 3. The interface

Compiled into the application, but written as if it were a DLL boundary: no STL
across it once it becomes one, no assumption of a shared allocator. That keeps the
option open at close to zero cost, and closes nothing today.

```cpp
class OutputPlugin {
public:
    virtual ~OutputPlugin() = default;

    // ── Identity ─────────────────────────────────────────────────────────────
    // `id` is stable across versions: the project stores it. `name` is shown.
    virtual std::string_view id() const = 0;      // "system-midi"
    virtual std::string_view name() const = 0;    // "System MIDI"

    // ── Configuration (§4) ───────────────────────────────────────────────────
    virtual std::vector<Parameter> parameters() const = 0;
    virtual base::Result<void> set_parameter(std::string_view name, const ParameterValue&) = 0;
    virtual ParameterValue      get_parameter(std::string_view name) const = 0;

    // ── Lifecycle ────────────────────────────────────────────────────────────
    // Called when the transport starts. The plugin opens its device, loads what
    // it needs and reports why it cannot. Anything expensive belongs here.
    virtual base::Result<void> start() = 0;
    virtual void stop() = 0;

    // ── Events ───────────────────────────────────────────────────────────────
    virtual void note_on(uint8_t ch, uint8_t pitch, uint8_t velocity, int64_t when_us) = 0;
    virtual void note_off(uint8_t ch, uint8_t pitch, int64_t when_us) = 0;
    virtual void controller(uint8_t ch, uint8_t cc, uint8_t value, int64_t when_us) = 0;
    virtual void program_change(uint8_t ch, uint8_t program, int64_t when_us) = 0;
    virtual void pitch_bend(uint8_t ch, int16_t value, int64_t when_us) = 0;

    // ── Health (§8) ──────────────────────────────────────────────────────────
    // A latched failure, cleared by the next successful start().
    virtual std::optional<base::Error> failure() const = 0;
};
```

### 3.0 The audio capability

An output that makes sound itself rather than handing MIDI to something that
does also implements `AudioSource`, and answers `audio()` with itself. A MIDI
port answers null and always will.

The host **pulls**: it delivers the events of a block, then asks for that
block's frames. That is the shape an offline render needs and the shape a
real-time callback needs, so there is one of it rather than two. It is also the
capability query — it decides whether rendering to a file is offered at all,
and it is what will decide whether the application needs an audio device.

### 3.1 The timestamp

`when_us` is **the instant the event was due**, in microseconds on a monotonic
clock shared with the host (`steady_clock`). An `int64_t` rather than a
`chrono` type, because it has to survive the DLL boundary the interface is
shaped for.

Absolute, not a delta: the plugin has no way to know when the host computed a
delta, so any delay between computing and delivering corrupts it silently. Not
ticks either — that would push a tempo-map lookup onto a plugin that does not
have the tempo map.

The engine sends events when it notices they are due, on its ~5ms loop, so
`when_us` is normally a little in the past. That is still worth having: every
event in one iteration is late by about the same amount, so **relative spacing
survives** — a chord stays together and a fast run keeps its internal spacing
instead of clumping onto the loop boundary. `SystemMidiOutput` ignores the value
entirely.

Removing the absolute lateness needs **look-ahead** — emitting events a few
milliseconds early, stamped with the true time, for the plugin to queue. That is
deliberately not in the first version: it would force the hardware side onto
`midiStreamOut` (or its own timer), because a plugin that played look-ahead
events on arrival would sound *early*, which is worse than today. Revisit when a
plugin exists that benefits.

### 3.2 Threading contract

To be repeated on the declarations themselves. This is where heisenbugs come
from, and the codebase already documents this kind of thing well elsewhere
(`all_notes_off_locked`, "requires m_state_mutex held").

| Method | Called from |
|---|---|
| `parameters`, `set_parameter`, `get_parameter` | command thread |
| `start`, `stop` | command thread |
| `note_on` … `pitch_bend` | playback thread |
| `failure` | playback thread, once per loop iteration, **and** command thread |

`failure()` is therefore the one method a plugin must make thread-safe on its
own.

## 4. Configuration

A plugin declares its parameters; the host renders them in Lit. The host never
knows what a sample bank is — it knows the output has a file parameter.

```cpp
enum class ParameterType { Enum, Int, Bool, String, File };

struct EnumChoice {
    std::string value;   // stable, stored in the project
    std::string label;   // shown
};

struct Parameter {
    std::string   name;             // stable key, stored in the project
    std::string   label;
    ParameterType type;
    bool          headline = false; // §4.3

    // Int
    int min = 0, max = 0, step = 1;
    std::string unit;

    // Enum
    std::vector<EnumChoice> choices;

    // File
    std::string filter;             // "*.spc;*.brr"
};
```

`Bool` and `File` are not decoration. The SPC needs a file for its sample bank —
as a `String` the user would be typing a path — and echo/interpolation toggles as
an `Int` or `String` are clumsy. `min`/`max`/`step`/`unit` on `Int` are what let
the host draw a sensible control and put validation somewhere; they map onto the
existing `mc-value-field`, which already takes a step and a coarse step.

### 4.1 Enum values are dynamic

**The first plugin already needs this.** MIDI ports appear and disappear when a
USB controller is plugged in. Later, the SPC's instrument list depends on which
file is loaded — one parameter changing the value set of another.

Rule: **the host re-queries `parameters()` after every `set_parameter`, and
whenever the configuration dialog is opened.** No notification mechanism, no
subscription. A plugin whose choices changed simply returns different ones.

### 4.2 No foreign dialogs

There is no escape hatch for a plugin to draw its own window, and that is
deliberate. Once one exists it becomes the path of least resistance and the
application ends up with two UI toolkits. While it does not exist, every time a
plugin "needs" one it is a signal that the schema is missing a type — and adding
the type is the correct fix.

If something genuinely does not fit a parameter list one day — a BRR sample
browser with a waveform, say — it should be **HTML rendered in the WebView**,
never a native window. One toolkit, either way.

### 4.3 The headline parameter

A plugin may mark one parameter `headline`. The status bar shows
`System MIDI — Microsoft GS Wavetable Synth` rather than just the plugin name,
and the value can be offered inline instead of only behind the dialog.

It shows the name *and* the value. The first draft had the headline replace the
name, which reads correctly for a port — `Microsoft GS Wavetable Synth` says
everything — and became nonsense with the second plugin, where it read
`OUT: Saw`. Designing that against one implementation was the mistake this
document warns about elsewhere, made here.

This exists to pay back the UX this design costs: today the user picks a port
directly from a list, and under this model they pick a plugin and then a port
inside its configuration — one step further away. The headline parameter puts the
part they actually care about back in front of them.

## 5. A plugin is a type, not a port

Selection arrived with the second plugin rather than being designed against the
first, which is what stage 2's single-output dialog was deliberately holding
off for. Switching is refused while the transport runs: the change would leave
notes sounding on an output nothing is going to send their note-offs to.


The host has no concept of a port. A port is an implementation detail of
`SystemMidiOutput`, surfaced as a dynamic enum parameter.

One plugin is selected at a time and holds one configuration. Two tracks pointing
at two different ports is therefore **not possible**, and that is an accepted
consequence: tracks already separate by MIDI channel, and there are sixteen of
them on one port. Per-track routing would mean revisiting this — configuration
would have to move from the type to an instance — and that is the day to do it,
not now.

## 6. Persistence

Stored in the project (see §10.2), as a **map of plugin id to its parameter
values**, not as one blob for whichever plugin is selected:

```json
"output": {
  "selected": "system-midi",
  "plugins": {
    "system-midi": { "port": "Microsoft GS Wavetable Synth" },
    "spc700":      { "bank": "C:/samples/strings.brr", "echo": true }
  }
}
```

Keeping every plugin's settings means switching to the SPC to try it and back
does not discard the port that was configured. With one flat blob, each switch
overwrites the last one's settings.

The host serializes these values without understanding any of them, which is the
main practical win of the declared schema.

**Ports are stored by name, never by index.** Unplug a controller and every index
shifts; the project would reopen pointing at a different device with nothing to
indicate it. Names resolve on open, with an explicit failure (§8) when the device
is gone. This is why `EnumChoice` separates a stable `value` from a display
`label`.

## 7. Defaults and first run

`CoreFacade::initialize()` currently auto-opens the first available MIDI output.
That is why a fresh install makes sound with nobody configuring anything, and it
is easy to lose in a refactor of exactly this shape. The regression would be:
install, press play, silence.

**The plugin chooses its own default** when it has no stored configuration —
`SystemMidiOutput` takes the first available port, as today. Consistent with the
rest of this design: the host has no idea what a reasonable default port is, and
the plugin does.

## 8. Failure has to be legible

The failure mode of this whole design is silence with no explanation, and there
are at least three ways to reach it: an SPC selected with no bank loaded, a saved
port that no longer exists, and — depending on §10.1 — a metronome routed through
a plugin that has no percussion. All three produce the same symptom: press play,
nothing happens.

Two mechanisms, deliberately split by cost:

**Pre-flight, on `start()`.** Returns `Result`, so the plugin can say
*why* it cannot start: "no sample bank loaded", "port 'X' not found". This is the
moment the user pressed Play and is waiting for an answer, and it is also where
the plugin should do its expensive setup.

**Runtime, latched and polled.** Event methods return nothing. A MIDI send fails
for essentially one reason — the device went away — and that is a sticky
condition, not a per-message one. Rich errors on the hot path would cost a return
value hundreds of times a second for information the engine cannot act on there
anyway: it cannot log at that rate, and it cannot stop from inside the loop,
which already holds `m_state_mutex`. So the plugin latches the failure, and the
engine reads `failure()` **once per loop iteration**.

No `GetLastError`. Thread-local shared error state decouples the error from the
call, and two threads genuinely touch this interface — the playback thread
sending and the command thread configuring. `base::Result` carries the error back
from the call that produced it. When the interface becomes a real ABI boundary,
that becomes a returned code plus a `describe_error(code)`, which keeps the same
property.

**Reporting is already built.** The engine stops itself at the end of the
composition and notifies the UI through `transport_state`. Losing a device is the
same path with a different trigger, plus a message.

## 9. What changes in the existing code

Not an internal-only refactor. The first slice touches:

- **`MidiService`** shrinks to MIDI input. `SystemMidiOutput` takes its own
  libremidi output handle rather than sharing one instance across input and
  output — sharing works until it does not, and the two now have different
  lifetimes.
- **`PlaybackEngine`** holds an `OutputPlugin&` instead of a `MidiService&`. Its
  five `send_*` helpers become calls on the plugin, plus the per-iteration
  `failure()` check.
- **Bridge commands.** `get_midi_output_devices` and `open_midi_output` are
  replaced by listing plugins, reading and setting parameters. `get_midi_input_devices`
  and `open_midi_input` are untouched.
- **UI.** The output picker becomes plugin selection plus a generic
  schema-rendered configuration dialog. The status bar shows the plugin name and
  its headline parameter.

## 10. Open decisions

### 10.1 Does the metronome go through the plugin?

The engine plays the click as `note_on` on channel 9. Under this design it
reaches the selected plugin — so an SPC with no percussion samples means the
metronome silently stops working, and it will be reported as a metronome bug
rather than an output one.

Either the click goes through the plugin and every plugin has to deal with it, or
the metronome keeps its own path, always on system MIDI, independent of the
selected output.

This decides whether `OutputPlugin` is *the* output or *the musical* output. No
recommendation; it needs a call.

### 10.2 Does the selection live in the project or on the machine?

Saving it in the project means opening someone else's project changes your output
device, and a MIDI port is a property of your machine, not of the composition.
But the SPC's sample bank defines **how the piece sounds**, which is clearly the
composition's. The plugin being device and instrument at once is what blurs this.

Leaning towards the project, for two reasons:

- The application has **no preferences store at all** today. Everything lives in
  the document. Choosing "machine setting" means inventing a settings file, with a
  location, a format and migration — a real cost that does not exist yet.
- The failure mode is already designed. A port that does not exist on the other
  machine is exactly §8's "port 'X' not found", with the plugin falling back to a
  default and saying so.

Written up as project storage in §6, but flagged here because it is visible to
the user and should be a decision rather than something that emerges from the
code.

## 11. Staging

1. **Extract `OutputPlugin`; reimplement today's behaviour as `SystemMidiOutput`.**
   No new feature. If this works without contortion the design is proven. Add the
   recording plugin and the tests it unblocks.
2. **The configuration schema and its generic dialog**, with the port as
   `SystemMidiOutput`'s only parameter — which also exercises dynamic enums, since
   ports come and go.
3. **A second real plugin.** ✅ Done, with one deliberate change: it ships as
   *Internal Synth*, not as an SPC-700.

   The voice chain is shaped after the S-DSP — eight voices at 32kHz, playback
   driven by a pitch *rate*, per-voice left/right levels, an envelope, voice
   stealing — and it is driven straight from the plugin's events and rendered
   offline to a WAV. But the chip's character lives in three tables: the
   gaussian interpolation kernel, the ADSR rate table, and BRR decoding.
   Reproducing those from memory would mean inventing numbers that look
   authentic and are not, so Hermite interpolation and a millisecond envelope
   stand in for the first two, and the third still needs an answer to where a
   sample bank comes from. **A real SPC-700 plugin is those three things added
   to this shape**, which is why it is not called one yet.

   Waveforms are generated in code rather than shipped, so the question of whose
   samples these are does not arise.
4. **Real-time audio backend** ✅ Done, through miniaudio — a single header, no
   system dependency, the same reasoning that chose miniz.

   The device is opened when an output that makes sound is selected, not when
   the transport starts: opening one costs tens of milliseconds and can glitch,
   and with nothing playing the source renders silence.

   Events cross into the audio thread through a fixed-capacity queue whose
   consumer takes no lock. Producers serialise among themselves with a mutex,
   which costs them nothing they cannot afford. A burst that overflows is
   counted rather than swallowed. Waveforms are all built at construction so
   changing one is an atomic store rather than a rebuild under the reader.

   Look-ahead is still not implemented. Events arrive up to a loop period late
   and are applied at the first frame of the block they land in, which is why
   "overdue means now" is a tested property rather than an accident.

## 12. Deliberately not doing

- **Dynamically loaded plugins.** A stable C ABI, versioning, crash isolation and
  installer packaging buy nothing while every plugin ships with the application.
  The interface is shaped so this can be promoted later.
- **Hosting VST3 or CLAP.** If the goal ever becomes *many* instruments, CLAP is
  the answer rather than an in-house ABI — but hosting is a project the size of
  this application.
- **A system-wide virtual MIDI port**, so other applications could receive from
  MIDI Composer. Windows has no user-space API for it; it needs a kernel driver.
  macOS and Linux give it away, which would make it a feature that works
  everywhere except the primary platform.
- **Per-track output routing** (§5) and **MIDI input** (§2).
