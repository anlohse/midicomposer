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
at two different ports is therefore **not possible**, and that was an accepted
consequence: tracks already separate by MIDI channel, and there are sixteen of
them on one port.

That day has arrived — see §9a. What reopened it was not the routing itself but
CLAP, whose instruments are one per track.

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

## 9a. Per-track routing

Status: **designed, not implemented.** This is §5's deferred decision, reopened
because two things arrived that need it.

### 9a.1 Why now

**CLAP instruments are one per track.** The convention is a mono-timbral plugin
instance per track, not one instance answering sixteen channels. Hosting CLAP at
all therefore means a track can name its own output.

**And a project mixes kinds.** A composer wants the bass on a hardware synth
over MIDI and the lead on a plugin, in the same piece. One selected output
cannot express that.

Per-track routing subsumes what exists today: a global output is every track
pointing at the same one.

### 9a.2 The mixer is where this gets uncomfortable

A track routed to an audio plugin can be mixed by the host: its frames are
right there, so the fader is a gain and the pan is a real pan.

A track routed to a system MIDI port cannot. The sound is being made on the
other side of a cable, and the only thing the host can do is **ask** — CC 7 for
volume, CC 10 for pan — and hope the device honours it. Most do. It is still a
request, not attenuation.

So there are two mixing domains behind one row of faders, and pretending
otherwise would be the dishonest option:

| | Audio output | MIDI port |
|---|---|---|
| Volume | Gain on the frames | CC 7, a request |
| Pan | Constant-power pan on the frames | CC 10, a request |
| Master | Gain on the sum | Scales the CC 7s, as today |
| Mute / solo | Notes are not scheduled | Notes are not scheduled |

Mute and solo are the same in both, because they work by not scheduling the
notes at all rather than by silencing anything. That part needs no thought.

The master is the one that reads worst: pulled to zero it genuinely silences
audio tracks and *asks* MIDI devices to go quiet. Every DAW has this asymmetry
with external gear and nobody is surprised by it — but only because the strip
says which kind it is. **A channel strip has to show what it is driving**, or the
fader silently means two different things.

### 9a.3 The fader stops being CC 7 for audio outputs

Today the mixer's volume reaches every output as CC 7, including the internal
synth, which applies it inside. Once the host has a real mixer that would apply
twice.

For an output the host mixes, the fader becomes **host gain** and is no longer
sent as CC 7. A CC 7 written into the *score* still reaches the plugin, as
automation, which is what it always was.

That is a cleaner split than the precedence rule §8 needed while the fader and
the automation were the same controller, and it is how a DAW divides them: the
strip is the host's, the controller is the music's.

### 9a.4 One instance per track, unless the output says otherwise

CLAP's convention is an instance per track. Applied blindly it would give four
instances of the internal synth for four tracks, when one of it already plays
sixteen channels.

So an output type declares whether it is multi-timbral. Tracks routed to a
multi-timbral output **share one instance** and keep their channels; tracks
routed to any other get **one instance each**, and each sees a single channel.

Without the flag one of the two cases is wrong: either instances are wasted, or
the multi-timbral behaviour that already works is thrown away.

### 9a.5 The host owns the sample rate

`AudioSource::sample_rate()` currently has the source *declare* its rate, which
works because there is exactly one of them. With several, the device runs at one
rate and everything else has to accept it.

It becomes the other direction: the host prepares each source with the rate and
the maximum block size, which is also what CLAP's `activate()` does. An output
that cannot run at the offered rate says so and is not used, rather than
silently running at the wrong speed.

### 9a.6 The graph

Per block, the host: dispatches each track's events to the output that track
names; pulls frames from every audio output; applies each track's gain and pan;
sums; applies the master; hands the result to the device. MIDI tracks are
dispatched the same way and contribute nothing to the sum, having already left.

### 9a.7 Deliberately deferred again

**Latency compensation.** A plugin can report its latency and a host is supposed
to delay everything else to match. Worth doing only once something reports one.

**Plugin GUIs** (§4.2), for the same reason as before, and now with a second: a
plugin window would have to live over a WebView.

**Per-track output persistence** waits on §10.2, like everything else about
where output settings live.

### 9a.8 What has to be decided before writing it

- Does a track *have* to name an output, or does it inherit a project default?
  Inheriting keeps existing documents working and keeps the common case one
  choice rather than one per track.
- What happens to a track whose output is missing when the project opens — the
  §8 failure path, but now per track rather than once.

## 9b. A bridge defect that was not one

Recorded here because it was written up as real and acted on, and the retraction
belongs next to the claim.

The bridge was said to lose backslashes from any string a command carries --
`C:\Users\alanl` arriving as `C:Usersalanl`. It does not. The transport is
lossless: JSON.stringify on the page, one JSON string through `postMessage`,
`TryGetWebMessageAsString` on the other side, glaze into the exposed function.
There is no step that could unescape twice.

What actually happened is that the probe was driven over CDP, and the probe's
own JavaScript went through a template literal on the way. That literal ate one
level of escaping before the string ever reached the page, so the corruption was
measured on the way *into* the measurement.

Verified by building the backslash from `String.fromCharCode(92)`, where no
layer can eat an escape: `select_output` echoed `C:\Users\alanl\Desktop`
back intact, and `export_audio` wrote byte-identical files given the same
directory with `\` and with `/`.

The lesson is about the harness, not the bridge: a probe that has to escape its
own payload is measuring itself as much as its subject.

## 10. Decisions

### 10.1 Does the metronome go through the plugin? -- decided

**It goes through an output the user chooses. The decision is not ours.**

The engine plays the click as `note_on` on channel 9, which left "where does the
click go" answered by whoever happened to own that channel -- a percussion track
routed elsewhere took the metronome with it.

An earlier draft answered this by following the first track's output. That was
wrong, and worth recording as wrong rather than quietly replacing: it made the
metronome move whenever the user rearranged their tracks, which is an edit about
the composition producing a change in something that is not part of the
composition. Which instrument a click comes out of is a matter of taste, and
taste is not derivable from a document.

So it is a preference, `metronomeOutput`, beside the others (§10.2):

- **The default is the first available MIDI device** -- the System MIDI output,
  which always exists, so the default can never itself be missing. Stored as an
  *empty* value rather than as `"system-midi"`: the preferences do not know what
  outputs exist, and writing a default would record a choice the user never made
  that nothing could later tell apart from one they did.
- **A choice that no longer exists is reset**, not remembered: the value is
  cleared, saved, and the default takes over. This is deliberately the opposite
  of what a missing *project* output does (§10.2 keeps that one, so plugging an
  interface back in brings it back). An output you cannot hear a click through
  is one you would go and change anyway, so keeping a dead name only means
  finding it still dead next time.

Two consequences of the click having an output of its own:

- **It keeps channel 9.** For a MIDI port that is still General MIDI percussion;
  for a plugin it is simply another channel, and the wood-block pitches produce
  whatever the instrument has there. The click stays off any track's channel, so
  it never inherits a program or a fader and never steals that channel's state.
- **The chosen output has to be alive even when nothing else uses it**, which is
  the normal case here rather than the odd one -- picking an instrument no track
  points at is exactly what this setting is for. `RoutingOutput` therefore counts
  it among its targets, so it gets started, told the sample rate, and added to
  the mixer, while the click itself goes straight from the engine to the plugin
  rather than through channel routing.

### 10.2 Does the selection live in the project or on the machine? -- decided

**Both, split by what the thing actually is.** The machine keeps the device; the
project keeps the instrument.

The question was hard only because a plugin is device and instrument at once. It
stops being hard once a *track* can name its own output (§9a), because that is
the instrument half and it is already saved in the project. What is left over --
which output a fresh project plays through, and how that output is configured --
is the device half, and a MIDI port is a property of the computer it is plugged
into. Opening someone else's project must not repoint your sound at hardware
they happen to own.

So the project-storage lean recorded above is reversed for the selection, and
the reason it was recorded is gone with it: "the application has no preferences
store at all" was the honest objection, and building one was the answer. It is
`app::Preferences`, at `%APPDATA%\MIDI Composer\preferences.json`, holding:

- `selectedOutput` -- the output a new project plays through.
- `outputParameters` -- keyed by output id, because two outputs may both have a
  "port" and they are not the same port. Read back from the plugin after a set
  rather than stored as given: a plugin is entitled to normalise what it was
  handed, and remembering the request instead of the result would restore
  something the plugin already declined to be.
- `clapSearchPaths` -- see §10.3.

Three rules the implementation follows, each of which is a way of not making
preferences more important than they are:

- **Reading is best-effort.** Missing, unreadable, corrupt, or written by a
  newer build: the defaults stay and the application starts. Preferences save
  the user a few clicks, and losing them must never cost more than those clicks.
- **Writing is atomic.** A temporary file beside the real one, then a rename.
  That is what stops a crash halfway through a save from turning "a few clicks"
  into a file that will not parse again.
- **The file says whose it is.** `"application": "MIDI Composer"` -- because
  `preferences.json` is a name many programs use and the path it sits at proves
  nothing: a sync tool, a restored backup or a hand edit can leave someone
  else's file there, and that file parses cleanly as ours with every field
  missing, which is indistinguishable from a first run. A marker that says
  something else means the file is not ours and the defaults stand. A marker
  that is *absent* is accepted and added on the next save: the field was
  introduced after the format was, so discarding such a file would throw away
  settings to enforce a key that did not exist when they were written.
- **A setting that no longer applies costs only itself.** A remembered output
  that is gone leaves the default playing and is *not* erased from the file --
  plugging the interface back in should bring the choice back with it. A
  parameter that will not apply is logged and skipped.

### 10.3 Where plugins are looked for -- decided

`ClapLibrary` scanned the standard install folders plus `CLAP_PATH`, and this
document argued that a setting of our own would be a second answer to a settled
question. That held while there was nowhere to put the setting.

What `CLAP_PATH` cannot be is *changed*. It has to be exported before launch, so
a user who downloads a plugin into a folder of their own cannot point a running
application at it -- and downloading a plugin into a folder of your own is the
normal way to try one. `clapSearchPaths` is where that folder goes. Both are
read; neither replaces the other, and the extra folders are scanned last so one
of them cannot hide a properly installed plugin.

Adding a folder rescans immediately, and only for files not already open:
creating a second instance of a plugin that is currently playing would be a new
output with the same name and none of the sound. Removing a folder does *not*
unload what came from it, because that would silence a project to tidy up a
list; it takes effect on the next run.

**And one folder the application owns**, at `%LOCALAPPDATA%\MIDI
Composer\Plugins`, created at startup, always scanned, and not removable from
the list. A list of folders answers "I installed plugins somewhere already"; it
does not answer "I have a plugin and nowhere to put it". Somewhere to put one
has to exist before a user can be told to put one somewhere, so the folder is
created whether or not anyone uses it, and the settings dialog opens it in the
file manager -- a path printed in a dialog is something to retype, a folder that
opens is somewhere to drop a file.

It sits under Local rather than Roaming, unlike the preferences beside it, and
deliberately: these are native binaries, and a roaming profile would carry one
machine's build onto another and count against a quota besides. It is scanned
first among the extras, so a deliberately pasted copy wins over the same plugin
found somewhere else.

### 10.4 Still open

Two things the preferences file could reasonably hold and deliberately does not
yet: whether the metronome is on, and the window's size and position. Neither
was needed to answer §10.2, and a preferences file grows best one answered
question at a time.

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
5. **The SPC-700 itself** ✅ Done, and large enough to have its own section:
   see §11a. Stage 3 deferred it for want of three tables -- the gaussian
   kernel, the ADSR rates and BRR decoding -- and all three now have real
   answers.

## 11a. The SPC-700

Stage 3 shipped as *Internal Synth* rather than as an SPC-700, and said why: the
chip's character lives in the gaussian interpolation kernel, the ADSR rate
table, and BRR decoding, and inventing those from memory would mean numbers that
look authentic and are not. This is what happened when they stopped being
invented.

### 11a.1 A separate output, not an evolution of the synth

Internal Synth generates its waveforms in code and takes no files. The sampler
takes nothing else. Merging them would give one output whose behaviour depended
on whether a bank happened to be loaded, and whose settings dialog was half
irrelevant either way. They share a shape and nothing else, so the SPC-700 is
its own `OutputPlugin` and the synth is left as the thing that always makes a
sound with nobody configuring anything (§7).

### 11a.2 The bank is immutable, and that is the whole concurrency answer

`SampleBank` is built once and never mutated. The audio thread takes a
`shared_ptr<const SampleBank>` at the top of each block, and **every sounding
voice holds its own reference**. Loading a bank builds a new one and publishes
it; the old one lives until the last block and the last note reading it are
finished.

That is the entire answer to "replace the instruments while sound is playing",
and it is worth stating as one sentence because the alternatives are all worse:
locking the audio thread against the loader, double-buffering with a swap flag,
or refusing to load while anything sounds. A note started on the old bank
finishes on it, which is also the musically correct behaviour.

The one cost: the last reference is often released *by the audio thread*, so a
bank's destructor can run there. For a few megabytes of vectors that is a cheap
operation, and the alternative -- handing the corpse to another thread to bury
-- is a queue and a thread for a problem nothing has measured.

### 11a.2a A recording and how to play it are different things

`Sample` is audio and nothing else. Everything about *how* to play it -- root
key, loop points, envelope, whether it feeds the echo -- lives on a `Zone`,
and a program is a list of zones covering parts of the keyboard.

The split is not tidiness. It is what a SoundFont actually states: one recording
is reached by several zones, each with its own root key and envelope, and a
piano sampled at three octaves is a handful of recordings and a dozen zones.
Fusing the two makes that unrepresentable -- two zones sharing a recording
cannot disagree about its root key if the root key belongs to the recording.

`zone_for(program, key, velocity)` takes the first zone whose ranges contain the
note. First rather than best: a SoundFont's zones are meant to partition the
keyboard, and where they overlap the format says the first applies. Ranking them
would invent a rule the file did not state. A note outside every zone plays
nothing, which is what a bank that says nothing about a key means.

### 11a.3 It renders at the host's rate

The chip runs at 32kHz and the authentic arrangement would be to run the DSP
there and resample at the edge. A sampler resamples inherently -- reading a
sample at a rate *is* resampling -- so what 32kHz would actually change is where
the interpolation kernel's cutoff lands. That was a hypothetical while the
kernel was a stand-in; now that the kernel is real (§11a.6) it is a real
consideration, and still not one anything has been heard to need. Left alone
until there is something to hear.

### 11a.4 Where the samples come from

**SoundFont for instruments, `.spc` for ripping.** They answer different
questions, and the difference is not one of quality.

An `.spc` is a savestate of the audio subsystem frozen mid-song: 64KB of chip
RAM, the SPC700's registers, and the 128 DSP registers. The samples are in
there and are findable without guessing -- DSP register `$5D` names the page
holding the sample directory, four bytes an entry giving start and loop
addresses. What is *not* in there is which sample is which instrument, what
pitch each was recorded at, or how each is meant to fade. That lives in the
game's own music driver, which differs per game and is code rather than data.
The DSP's per-voice ADSR registers describe eight voices at one instant, not a
table of instruments.

SF2 answers what a composition actually asks: presets addressed by bank and
program -- exactly what a program change selects -- with loop points, root keys,
tuning and envelopes already stated.

Read from SF2: a preset's first instrument, **every** zone of that instrument
with its key and velocity ranges, loop points, root key, tuning and volume
envelope. Skipped: presets that layer several instruments, stereo pairs,
modulators, the filter.

Zones are the reason `Sample` and `Zone` are separate types (§11a.2a). A first
version fused them, which worked exactly as long as every instrument was one
recording stretched across the keyboard -- and stopped working the moment a
real General MIDI bank turned up. ExpressiveSNES.sf2 goes from 108 samples in
128 single-zone programs to 135 samples in 193 zones, 32 of them multi-sampled:
27 recordings that were in the file all along and never reached, because they
were in the second zone of an instrument.

### 11a.5 Telling a ripped instrument from a coincidence

A sample directory is 256 entries whether or not the game filled them, and the
rest of the page is whatever was in memory. Around ninety stale entries per file
happen to point at bytes with a BRR end flag somewhere near, so **"it decodes"
turns out to be no test at all** -- it produced about a hundred instruments per
rip where a SNES game of that size loads around twenty-five, most of them a
millisecond of noise.

The loop address is the test, and it is a hardware constraint rather than a
heuristic: on real hardware the loop has to point at a BRR block *inside the
sample it belongs to*. So it must fall within the bytes just decoded, and a
whole number of nine-byte blocks from the start. Stale entries fail instantly --
their loops point tens of kilobytes away at no boundary in particular. Across
ninety-two Chrono Trigger rips this takes 9221 candidates down to 2237, an
average of 24 a file. Entries aliasing one sample are collapsed too.

**Pitch is measured, not assumed.** A rip states no root key, and treating every
sample as C4 leaves each instrument transposed by its own interval -- the
loudest thing wrong with a ripped bank, because it is wrong *per instrument*
rather than wrong overall, and a sample recorded at note 83 plays two octaves
low. A pitched instrument's loop is periodic by construction, so the period is
measurable and the period is the pitch: normalised autocorrelation, preferring
the shortest lag within a margin of the best to avoid the octave error, with the
peak interpolated so the answer is not quantised to whole samples. The
fractional MIDI note is kept and its remainder becomes the sample's fine tuning.

Two calibrations the real files corrected, both of which had been reasoned
backwards:

- **The window.** Measuring over the loop is right when the loop is long enough
  to hold cycles. Most ripped loops are 32 frames and cannot; falling back to
  the whole sample took detection from 27% of samples to 46%.
- **The threshold.** A high confidence gate looked prudent -- a made-up pitch
  seems worse than none. It is not, here, because the two mistakes are different
  sizes: a loosely measured pitch is a semitone out, while *no* measurement
  leaves the sample at C4 and octaves away. Percussion, which has no right
  answer, costs nothing either way. The gate is now the detector's own floor.

710 of the 792 samples long enough to hold a pitch get one; the remaining tenth
is choirs, wind and sound effects with no fundamental to find.

### 11a.6 Which tables are real, and their provenance

**The gaussian kernel is the chip's**: 512 twelve-bit values from the S-DSP's
mask ROM. So is the ADSR rate table, though that took a correction to notice --
see §11a.6a.

Provenance mattered more than availability. Every SNES emulator has both tables,
but blargg's `snes_spc` is LGPL and the ports are derivatives of it; one
BSD-licensed project's own README says its envelope code came from blargg's, so
the permissive licence is on paper rather than in the history. Attaching a
licence to this project for two tables would be a heavy price for data that is a
measurement of a chip rather than anyone's expression. The SnesLab wiki
publishes the same dump under Creative Commons Attribution, which asks for
credit and gets it in `gaussian_table.hpp`.

Checked before believed, because a transcription cannot be reviewed by reading
it: the four taps the chip uses at any fractional position sum to 2048 -- unity
in its fixed point -- across all 256 positions, within the one count that
twelve-bit quantisation costs. The tap ordering was derived from the table's own
shape rather than assumed. Both are tests, since this is data an edit could
break silently.

Worth recording: the computed kernel it replaced was close. Where the chip
weights its taps (0.181, 0.637, 0.183, 0) the approximation had (0.176, 0.644,
0.176, 0.003). What changed is that the error is now zero rather than unknown.

BRR is decoded with the documented predictor coefficients written as the
hardware's shifts, because the rounding of a right shift on a negative number is
part of the result. The tests check the decoder against those coefficients,
which is not the same as checking it against a console; without a reference rip
to compare with, "bit-exact" is a claim this cannot make.

### 11a.6a The ADSR registers, and a wrong call corrected

This section exists because §11a.10 used to say the chip's ADSR rate table had
no consumer, and that was wrong.

The reasoning was: an `.spc` is a snapshot, so its per-voice ADSR registers
describe eight voices at one instant rather than a table of instruments. True as
far as it goes, and it skipped register `$x4` -- **SRCN**, which says which
sample each voice is set to play. With that, the eight voices are eight
(sample, envelope) pairs the game itself wrote, and converting them needs
exactly the table that was said to have no use.

Measured across ninety-two rips: every file carries an envelope for between two
and eight distinct samples, mean 4.9, in 76 distinct combinations -- varied
per-instrument settings rather than one default repeated. 442 of 2237 samples
come back with the game's own shape, and the ones they cover are the long
instrument samples rather than the percussion hits.

The tables are Sony's, transcribed on the Super Famicom Development Wiki, and
they are published as *times* rather than as the counter periods an emulator
works in. That happens to be the better form here: the envelope is time-based
and a SoundFont states times, so a rip and a SoundFont end up speaking the same
language.

Two things this cannot give and does not pretend to:

- **A rip gives *an* envelope for a sample, never *the* envelope.** Wind Scene
  plays sample 33 on four voices, one instant-and-held and three with a 260ms
  attack fading over 24 seconds -- one sample, several articulations, which is
  a normal thing for a driver to do. The most common shape among the voices is
  taken, because letting voice order decide would be arbitrary.
- **A voice running GAIN instead of ADSR is skipped.** Its envelope is being
  shaped by driver code, which a snapshot cannot hand over.

### 11a.7 The echo is data, not a table

Unlike the kernel and the rates, the echo is *per game* and sits in the DSP
registers of any `.spc` -- delay, feedback, volume, and the eight FIR taps. A
rip therefore carries the reverb the piece was written to sound through.

The chip's arrangement, not a reverb of our own: read the delay line, run the
taps across what comes out, add that to the output, write the dry signal plus
the filtered echo back in. **The filter being inside the feedback path** is what
makes repeats darken rather than merely fade, and is most of why this sounds
like a room instead of a delay pedal.

The line is sized once for the longest delay the chip allows, so changing rips
never allocates on the audio thread, and it is cleared when the length changes
rather than replaying what the previous piece left in it. A filter with gain
above one inside a feedback path runs away, so a rip declaring one is loaded
with the echo off and a line saying why.

**Not everything reaches it.** Register `$4D` picks which voices feed the echo,
and games use it: across ninety-two rips the count runs from none to all eight,
mean five, and 207 of the 442 configured samples are kept out. A dry lead over a
wet accompaniment is a mix decision somebody made, and sending everything erases
it. The bit is taken from the same voice the envelope came from (§11a.6a), since
taking one voice's envelope and another's echo bit would describe a voice that
never existed. A sample no voice names still sends, which is what a SoundFont
means by saying nothing.

Worth knowing when this looks like it is doing nothing: Wind Scene sets `$4D` to
`0xFF`. That game really did send everything.

Ninety-one of ninety-two rips declare an echo, with delays spread from 48 to
240ms and a mean feedback of 0.59. Random bytes do not average to a musical
feedback across ninety-two independent files, which was the evidence the
register map was right before documentation confirmed it.

### 11a.8 The envelope

Decay and release are exponential; attack is linear. Both authorities agree and
they agree with each other: the chip's decay subtracts a proportion of what is
left rather than a fixed step, and the SoundFont specification states its
envelope times in decibels, which is the same curve said differently. A linear
fall holds a note up too long and then arrives at the sustain level abruptly.

Release is the one place the chip is linear and this deliberately is not: a note
that slides straight to zero clicks, and the SoundFont curve is the better
behaviour to share with the banks that ask for it.

**Sustain decays when the source says so, and holds otherwise.** The chip has no
"hold": its sustain rate always falls, and a driver asks for a held note by
setting the rate to zero. A SoundFont holds and says nothing about a rate, so it
gets zero; a rip states one per voice and it is read (§11a.6a). Falling is
exponential like the other stages, so a held note thins out rather than ramping.

This paragraph previously said the opposite -- that sustain held because nothing
could know the rate. That was wrong, and how it was wrong is worth keeping.

### 11a.9 What an output's programs are called

The instrument list was the 128 General MIDI names always. Right for a MIDI
port, a lie for anything else: a sampler's program 11 is whatever its bank put
there, and calling it "Music Box" tells the user something false rather than
nothing.

Three answers are needed and they are all different, which is why an output
declares *entries* -- a number and a possibly-blank name -- rather than a list
of names:

- **No entries** means General MIDI. A port, and the internal synth, whose
  instrument families really are the GM ones.
- **Entries with names** are a sampler's bank: only the programs it filled, so a
  rip offers its two dozen rather than 128 slots with two dozen useful.
- **Entries without names** are a hosted plugin. CLAP has no way to ask:
  `clap.preset-load` loads a preset from a path and the preset-discovery factory
  indexes preset files on disk, and neither maps onto the 128 program slots. A
  number is not helpful, but it is true.

A SoundFont names a program after its *preset* -- the file calls the sample
"Piano C4" and the preset "Grand Piano", and the second is what somebody is
choosing between. A rip has no names at all, so the label is built from what was
measured: `Sample 32 (B5, 415ms, looped)`. Length separates a percussion hit
from a held instrument at a glance, which is what turns auditioning two dozen
unknowns into reading a list.

### 11a.9a Zones stack, and the eight voices pay for it

A program can answer one note with more than one zone, and two different things
in a SoundFont produce that. A preset may name several instruments to sound
together; an instrument may cover the same key at the same velocity twice. Both
are layering, and until this section existed the loader took the first match and
dropped the rest.

That is not a thinner sound so much as a different one. Honky-tonk is two pianos
a few cents apart, and one of the two is a piano. Measured across the 128
programs of ExpressiveSNES.sf2: **seven presets name two instruments**, and in
every one of the seven both halves cover the whole keyboard at every velocity --
genuine stacking rather than a split keyboard. Reading them takes the bank from
193 zones to 207, and from 135 samples to 136: one instrument was named only by
a second layer, so its recording never arrived at all.

**A note now starts every zone that answers it**, and voice stealing is left to
do what it already did. Layering costs voices on a chip that has eight, and that
is the honest price: the alternative is playing one layer and calling the result
the instrument. The measured worst case in that bank is four zones for one note,
so a two-note chord on the worst program is already most of the machine. Zones
past the eighth are dropped when the note is built rather than left to steal one
of the note's own layers a moment later.

Three consequences that had to be handled together, because layering does not
work without them:

- **Attenuation is now read.** Two instruments sounding together arrive at twice
  the level of one, and a bank that stacks on purpose states the attenuation
  that pays for it. Ignoring it while honouring the layers would have made the
  layered presets the loudest things in the bank.
- **A preset zone narrows its instrument rather than replacing it.** The preset
  says over what part of the keyboard its instrument applies; the instrument
  says which of its samples covers what. The narrower of the two wins on each
  side, and a zone the preset excludes is absent rather than silent.
- **A generator stated at both levels is added, not chosen between.** The format
  says the preset's value is an offset. Adding happens before conversion,
  because timecents and centibels are logarithmic -- adding seconds or
  amplitudes afterwards would mean something else entirely, and quietly.

Worth knowing when this looks like it is doing nothing: a rip has no second
level to offset from. Everything here is a SoundFont's doing, and `.spc` files
come through untouched, one whole-keyboard zone per sample.

### 11a.10 Still missing

- **A fuller envelope for a rip.** Only the samples a voice happened to name get
  the game's own -- about a fifth. The rest keep defaults.
- **Auditioning.** A program still has to be assigned to a track and played to
  be heard. With two dozen unknowns in a rip, the naming in §11a.9 helps and
  does not finish the job.

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
