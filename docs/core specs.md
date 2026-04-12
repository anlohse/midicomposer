# MIDI Composer — Core Detailed Specification

## 1. Purpose of the Core

The core is the native engine responsible for all music-domain logic, MIDI data modeling, playback, recording, editing operations, persistence, and communication with the UI layer.

It must:

* provide a musically meaningful internal model
* support efficient editing and playback
* import/export standard MIDI files
* play to MIDI output devices
* record from MIDI input devices
* notify the UI about playback and model changes
* remain independent from the HTML/TypeScript UI implementation

The core must be written in **modern C++20 or later**, using RAII and strong ownership rules to minimize memory and lifetime bugs.

---

## 2. Core Design Goals

### 2.1 Functional goals

The core shall support:

* loading and saving compositions
* importing and exporting MIDI files
* note editing with precise temporal placement
* track, measure, note, controller, and pitch bend management
* tempo and time signature configuration
* playback with real-time scheduling
* recording from MIDI input
* metronome generation for recording and playback assistance
* undo/redo-ready editing operations
* efficient synchronization with the UI

### 2.2 Non-functional goals

The core shall be:

* deterministic where possible
* thread-safe at subsystem boundaries
* low-latency for playback and recording
* designed for incremental updates rather than full reloads
* portable across major desktop platforms
* testable without the UI
* extensible for later notation, quantization, automation, and plugin-like features

---

## 3. Architectural Overview

The core should be organized into clear modules.

## 3.1 Proposed modules

### A. Domain Model

Represents the musical composition in memory.

### B. MIDI File I/O

Imports from and exports to Standard MIDI Files.

### C. Playback Engine

Schedules and sends MIDI events to output devices in real time.

### D. Recording Engine

Receives MIDI input, timestamps events, and writes them into the composition.

### E. Metronome Engine

Generates click events during playback/recording.

### F. Edit Engine

Applies editing commands to the composition.

### G. Transport / Timeline Engine

Controls play, stop, pause, loop, position, tempo map traversal, and timing conversions.

### H. Event/Notification Bus

Sends model and playback updates to the UI-facing adapter.

### I. Persistence Layer

Stores compositions in the project’s native format.

### J. Device Abstraction Layer

Abstracts MIDI input and output device enumeration and communication.

---

# 4. Fundamental Core Principles

## 4.1 Ownership and memory

The core must follow strict ownership rules:

* use stack allocation by default
* use `std::unique_ptr` for exclusive ownership
* use `std::shared_ptr` only where shared lifetime is truly required
* avoid raw owning pointers
* use raw pointers or references only as non-owning views
* all resources must be acquired and released via RAII wrappers

Examples of resources that must be RAII-managed:

* MIDI device handles
* file handles
* worker threads
* mutex locks
* timers
* OS-specific playback/recording resources

## 4.2 Error handling

Use a clear and consistent strategy:

* recoverable domain and I/O failures should return structured error types, such as `std::expected<T, Error>`
* unrecoverable programming errors should use assertions in debug builds
* exceptions may be allowed internally for infrastructure or third-party boundaries, but the public core API should expose predictable error results

## 4.3 Identity and stability

All user-visible composition entities should have stable IDs so the UI can track them reliably.

Entities that should have IDs:

* composition
* track
* measure
* note
* controller event
* pitch bend event
* tempo event
* marker or region, if added

IDs should remain stable across edits unless the entity is deleted.

---

# 5. Internal Music Representation

This is the most important part.

You mentioned an “intelligent format” that supports quick note placement and playback. The right answer is usually a **hybrid representation**, not just one structure.

## 5.1 Why a hybrid model is better

A single representation rarely serves all needs well:

* playback wants a time-ordered event stream
* editing wants direct access to measures, tracks, and notes
* score/piano-roll views want grouped musical structure
* recording wants append-friendly timestamped insertion

So the core should maintain:

### A. Structural model

For editing and UI semantics.

### B. Time-ordered event index

For fast playback, seeking, and iteration.

The event index can be rebuilt incrementally after edits.

---

## 5.2 Canonical time model

The composition shall use **ticks** as the canonical musical time unit.

### Requirements

* define a pulses-per-quarter-note value, for example 480 or 960 PPQN
* all note positions and durations are stored in integer ticks
* conversion to/from wall-clock time depends on tempo map traversal
* measure-relative and absolute times should both be derivable

### Why ticks

* avoids floating point drift
* aligns with MIDI conventions
* makes quantization and measure calculations easier

---

## 5.3 Core temporal objects

### Tick

Absolute musical time from composition start.

### Duration

Length in ticks.

### MeasurePosition

A structured form:

* measure index
* beat within measure
* subdivision / tick offset within beat

### TimeRange

A half-open interval:

* start tick inclusive
* end tick exclusive

This is useful for playback, selection, editing, and collision checks.

---

# 6. Domain Model Specification

## 6.1 Composition

Represents one MIDI composition/project in memory.

### Properties

* composition ID
* title
* PPQN resolution
* global metadata
* track list
* tempo map
* time signature map
* markers or regions, optional for MVP but recommended
* loop settings
* playback state snapshot
* project-specific extra data

### Responsibilities

* own all musical data
* provide APIs for querying and editing
* support serialization/deserialization
* expose a change notification stream

---

## 6.2 Track

Represents one musical lane / instrument / MIDI channel stream.

### Properties

* track ID
* track name
* track order index
* enabled / muted / solo state
* armed-for-recording state
* MIDI channel
* output device routing
* program / instrument metadata
* volume
* pan
* expression
* reverb send
* chorus send
* custom metadata
* visual/UI hints if needed later, but preferably separated from core domain data

### Contents

A track contains:

* notes
* controller events
* pitch bend events
* program changes
* channel pressure / aftertouch, optional for MVP but recommended
* polyphonic key pressure, optional for post-MVP
* other meta-musical annotations if needed later

### Notes on measure ownership

Measures should not “own” notes. Tracks should own note/event data; measures are timeline partitions used for interpretation and display.

That avoids ugly duplication and makes cross-measure notes easier.

---

## 6.3 Measure

A measure is primarily a derived timeline segment defined by the time signature map, but you may still expose it as an object-like concept for editing and UI use.

### Measure-related data

* measure index
* start tick
* end tick
* time signature effective in that measure
* optional measure metadata:

  * rehearsal mark
  * section label
  * comments
  * local annotations

### Important design choice

Do not make measures the primary storage container for notes.

Instead:

* measures are computed from the timeline and time signature map
* notes reference absolute time
* queries can ask for “all events in measure X”

This makes playback and editing much cleaner.

---

## 6.4 Note

Represents a note event pair conceptually composed of note-on and note-off.

### Properties

* note ID
* track ID
* MIDI pitch 0–127
* start tick
* duration in ticks
* velocity
* release velocity, optional
* channel override, optional
* tied-from / tied-to markers, optional for notation support
* articulation or accidental metadata, optional
* selected state should not live in core domain unless there is a strong reason; better in UI/session state
* extra metadata bag

### Constraints

* duration must be positive
* pitch must be valid MIDI pitch
* start tick must be non-negative
* note end must be representable

### Recommended derived values

* end tick
* measure position
* pitch class
* octave

---

## 6.5 Controller Event

Represents MIDI Control Change data.

### Properties

* event ID
* track ID
* controller number 0–127
* value 0–127
* tick
* optional interpolation/group metadata

### MVP scope

At minimum support storage, editing, playback, and MIDI import/export.

Recommended initial supported controllers in UI-aware API:

* volume
* pan
* expression
* modulation
* sustain pedal
* reverb send
* chorus send

---

## 6.6 Pitch Bend Event

### Properties

* event ID
* track ID
* tick
* value in MIDI pitch bend range, typically 14-bit centered
* optional normalized representation for convenience

### Recommendation

Internally store the raw 14-bit value or a signed centered integer. Provide helper conversions.

---

## 6.7 Tempo Event

Represents tempo changes over time.

### Properties

* event ID
* tick
* tempo in microseconds per quarter note
* derived BPM accessor

### Requirements

* composition must support multiple tempo changes
* there must always be an effective initial tempo at tick 0

---

## 6.8 Time Signature Event

### Properties

* event ID
* tick
* numerator
* denominator
* optional metronome click hints
* optional notational subdivision hints

### Requirements

* must support changes over time
* there must always be an effective initial time signature at tick 0

---

# 7. Internal Data Structures

## 7.1 Track event storage

For fast editing and retrieval, each track should store event categories separately.

Recommended per track:

* `std::vector<Note>`
* `std::vector<ControllerEvent>`
* `std::vector<PitchBendEvent>`
* `std::vector<ProgramChangeEvent>`
* optionally other event vectors

These should be kept sorted by tick, or maintain sortable dirty flags and sort lazily when needed.

## 7.2 Secondary indexes

For efficient playback and UI queries, maintain indexes such as:

* events sorted by start tick
* notes by measure
* notes by pitch range, if piano-roll editing grows later
* event lookup by stable ID

### Recommended index strategy

Canonical storage in vectors plus:

* `unordered_map<EntityId, IndexReference>`
* lightweight time index rebuilt incrementally after edits

This gives good cache locality and simpler ownership.

## 7.3 Measure lookup

Measures should be computed via the time signature map and cached.

Provide APIs like:

* get measure by index
* get measure at tick
* get tick range for measure
* iterate events intersecting a measure

---

# 8. Timing and Conversion Rules

The core must support conversion between:

* ticks
* measure/beat/subdivision
* milliseconds
* audio-clock-like monotonic playback time

## 8.1 Transport timing services

Provide a dedicated timing service that can answer:

* tick to real time
* real time to tick
* measure position to tick
* tick to measure position

These conversions must honor:

* tempo changes
* time signature changes
* playback start offset
* loop regions

---

# 9. Editing Model

The core should expose edits as commands rather than arbitrary mutation where possible.

## 9.1 Why command-based edits

This makes it easier to support:

* undo/redo
* validation
* consistent notifications
* future macro operations
* event batching

## 9.2 Command examples

* create note
* delete note
* move note
* resize note
* change note velocity
* insert controller event
* delete controller event
* change tempo
* insert tempo event
* change time signature
* create track
* delete track
* reorder track
* rename track
* set track channel
* set track volume/pan/effects
* split note
* merge notes, maybe later
* quantize selection, maybe later MVP+

## 9.3 Batch edits

Support atomic edit transactions:

* begin transaction
* apply N edits
* commit
* rollback on failure

The UI will need this for gestures like drag-select and move multiple notes.

---

# 10. Playback Engine Specification

## 10.1 Purpose

The playback engine is responsible for scheduling events from the composition timeline to MIDI output with accurate timing.

## 10.2 Capabilities

* play from current position
* stop
* pause
* continue
* seek to arbitrary tick
* loop playback over a region
* optionally play count-in before recording
* send all-notes-off / reset on stop

## 10.3 Event scheduling

Playback should not iterate raw notes directly in real time if avoidable. It should use a prepared event stream or scheduling queue.

### Recommended approach

At playback start or after major edits:

* build or refresh a flattened playback event stream
* each note becomes note-on and note-off scheduled events
* include controller, pitch bend, program changes, tempo events if relevant to scheduling context

Use a look-ahead scheduler:

* scheduler thread wakes periodically
* sends events due within a near-future window
* uses a monotonic clock
* compensates for drift as much as practical

## 10.4 Playback states

Define an explicit transport state machine:

* stopped
* playing
* paused
* recording
* preroll
* seeking
* looping

State transitions must be explicit and validated.

## 10.5 UI notifications during playback

The playback engine shall emit periodic status updates such as:

* current tick
* current measure/beat
* playback state
* active loop region
* metronome state
* optionally currently sounding notes for highlighting

Notification rate should be configurable and throttled, for example 20–60 Hz.

---

# 11. Recording Engine Specification

## 11.1 Purpose

Capture MIDI input from a device, timestamp incoming events, and convert them into composition edits.

## 11.2 Requirements

* enumerate MIDI input devices
* arm one or more tracks for recording
* start recording at current transport position
* optionally support count-in
* record notes
* record controller changes
* record pitch bends
* support overdub and replace modes
* support metronome during recording

## 11.3 Timestamping model

Incoming MIDI events must be timestamped against a stable monotonic clock and converted to composition ticks based on current transport timing.

This is crucial for accuracy.

## 11.4 Note pairing

For recording notes:

* note-on creates a pending live note
* note-off closes the note and materializes a `Note` entity
* if recording stops with hanging notes, close them according to policy

## 11.5 Recording modes

At minimum:

* overdub: add recorded notes over existing data
* replace in armed range: replace events in a defined region on armed tracks
* step record is optional, not MVP

---

# 12. Metronome Specification

## 12.1 Purpose

Provide click guidance for playback and recording.

## 12.2 Requirements

* configurable enabled/disabled state
* configurable count-in before recording
* distinct accent for first beat of measure
* tempo-aware
* time-signature-aware
* route to MIDI output or internal click abstraction

## 12.3 MVP implementation options

For MVP, simplest path is MIDI metronome events to a dedicated channel or device.
Later, this can be extended to audio clicks.

---

# 13. MIDI File Import Specification

## 13.1 Supported format

Must support Standard MIDI Files:

* Format 0
* Format 1

Format 2 can be out of MVP unless easy to support.

## 13.2 Imported data

Import:

* tracks
* note events
* tempo changes
* time signatures
* track names
* program changes
* controller changes
* pitch bends
* basic meta events relevant to composition

## 13.3 Import behavior

The importer must:

* parse delta times into absolute ticks
* pair note-on/note-off into note entities
* preserve track/channel information where possible
* extract tempo map and time signature map
* preserve unknown or unsupported events in an extensible raw-event container if feasible

## 13.4 Edge cases

Must define behavior for:

* overlapping same-pitch notes on same channel
* note-on with zero velocity
* malformed or truncated files
* missing end-of-track events
* tempo changes in non-primary tracks
* files with inconsistent metadata

## 13.5 Import policy recommendation

Be permissive in reading, conservative in writing.

That is usually the right choice for MIDI.

---

# 14. MIDI File Export Specification

## 14.1 Requirements

Must export the composition to a valid Standard MIDI File.

## 14.2 Export behavior

* flatten note entities into note-on/note-off events
* write delta times correctly
* include tempo and time signature events
* include track names and supported track metadata
* include controller and pitch bend events
* ensure correct ordering when multiple events share a tick

## 14.3 Event ordering at same tick

Define a stable policy, for example:

1. tempo/time signature/meta
2. program changes
3. controller changes
4. note-off
5. note-on
6. pitch bend or as appropriate by desired semantics

You should define this explicitly because it affects playback correctness.

---

# 15. Native Project Persistence Format

MIDI import/export is not enough as the native save format, because MIDI does not preserve all editor semantics cleanly.

## 15.1 Requirement

The application should have its own project format for saving editable compositions.

## 15.2 Suggested format

For MVP:

* a JSON-based project file is acceptable for readability and debugging

For a more robust design:

* zipped project package with JSON metadata and optional binary chunks

## 15.3 What native format should preserve

* composition metadata
* track metadata
* notes with stable IDs
* controllers and pitch bends
* tempo map
* time signature map
* editor-oriented extra data
* future-compatible custom metadata
* app version / schema version

## 15.4 Recommendation

Even if export/import MIDI is supported, users should save into the native project format to avoid losing editor-specific information.

---

# 16. Change Notification Model

The UI needs updates when the core changes.

## 16.1 Notification categories

### Document/model changes

Examples:

* note created
* note updated
* note deleted
* track added
* track reordered
* tempo changed
* measure map invalidated

### Transport/playback changes

Examples:

* play started
* play stopped
* current position changed
* loop changed
* recording started/stopped
* metronome enabled/disabled

### Device changes

Examples:

* MIDI input devices changed
* MIDI output devices changed
* device connection failed

## 16.2 Notification style

Prefer structured event payloads, not string messages.

Example style:

* event type enum
* entity ID
* affected time range
* old/new values when relevant
* transaction ID or revision number

## 16.3 Revision tracking

The composition should maintain a monotonically increasing revision number.
This helps the UI detect stale updates and coalesce re-renders.

---

# 17. Threading Model

This part matters a lot.

## 17.1 Recommended threads

At minimum:

* UI thread
* core/control thread or main app thread
* playback scheduler thread
* MIDI input callback/thread depending on platform/backend

## 17.2 Rules

* UI must not mutate the composition directly across threads
* playback thread must not perform heavy allocations or blocking operations
* recording callbacks must be minimal and push events into lock-safe queues
* document edits should happen on a controlled thread or behind synchronization

## 17.3 Recommended pattern

Use message passing between subsystems:

* UI sends commands to core
* core applies edits
* core emits notifications
* playback and recording communicate through lock-minimized queues

Avoid broad shared mutable state.

---

# 18. Public Core API Shape

The UI should not talk to raw internals. The core should expose an application service layer.

## 18.1 Example service groups

### Project service

* create composition
* open/save composition
* import/export MIDI

### Edit service

* create/move/delete notes
* edit tracks
* insert controllers
* update tempo/time signature

### Transport service

* play
* stop
* pause
* seek
* loop control

### Recording service

* arm track
* start/stop recording
* enable count-in
* metronome settings

### Device service

* enumerate MIDI inputs/outputs
* select active devices

### Query service

* get tracks
* get notes in measure/range
* get transport status
* get current tempo/time signature at tick

---

# 19. Validation Rules

The core should validate edits and imported data.

## 19.1 Examples

* track channel must be valid
* note pitch must be 0–127
* note duration must be > 0
* controller number and value must be 0–127
* tempo must be within sane bounds
* time signature denominator should be musically valid, preferably power-of-two unless intentionally relaxed
* entity references must be valid

## 19.2 Validation policy

* reject invalid commands with structured errors
* sanitize imported data where safe
* log recoverable anomalies during import

---

# 20. Suggested MVP Features You Did Not Mention But Should Strongly Consider

These are worth adding to the core spec now.

## 20.1 Loop region support

Very useful for editing and recording.

## 20.2 Count-in before recording

Almost mandatory with a metronome.

## 20.3 Mute / solo / arm track states

Needed for practical playback and recording.

## 20.4 Undo/redo foundation

Even if first implementation is limited, design for command history now.

## 20.5 Quantization hooks

Even if full quantize UI comes later, design recording/edit APIs so quantization can be added cleanly.

## 20.6 Snap/grid services

Useful for UI editing and core note placement rules.

## 20.7 All-notes-off/reset on stop

Prevents stuck notes.

## 20.8 Dirty-state tracking

Needed for save prompts and multi-document UI.

## 20.9 Selection-independent domain model

Keep transient UI selection out of core where possible.

## 20.10 Schema versioning

Needed from day one for the native project format.

---

# 21. Out of Scope for MVP

It helps to be explicit.

Likely out of MVP:

* full professional notation engraving
* VST/AU instrument hosting
* audio rendering and audio tracks
* advanced articulation libraries
* humanization engine
* advanced score layout rules
* collaborative editing
* plugin system
* full DAW-style automation lanes
* full MusicXML import/export

---

# 22. Acceptance Criteria for the Core MVP

A first MVP of the core is acceptable when it can:

* create a composition with tracks, tempo, and time signature
* add, move, resize, and delete notes
* store notes in a structure efficient enough for interactive editing
* import a standard MIDI file into the internal model
* export the internal model back to MIDI
* play the composition through a MIDI output device
* record notes from a MIDI input device into an armed track
* run a metronome during playback/recording
* notify the UI of transport position and composition changes
* save/load the project in a native editable format
* avoid common lifetime/resource bugs through RAII and clean ownership

---

# 23. Recommended Concrete Internal Model Decision

If I had to lock one now for your project, I’d choose this:

## Canonical model

* `Composition`

  * owns `Track` objects
  * owns global `TempoMap` and `TimeSignatureMap`

* each `Track`

  * owns sorted vectors of `Note`, `ControllerEvent`, `PitchBendEvent`, etc.

* all event times stored as absolute ticks

* measures are derived from time signature map, not primary storage containers

* playback uses a flattened scheduled event stream derived from the canonical model

That gives you:

* clean editing
* good playback performance
* easy MIDI import/export
* future room for score and piano-roll style UIs

---

# 24. Suggested Next Step in the Spec

The next useful artifact would be one of these:

1. a **C++ domain model spec** with class/struct responsibilities
2. a **core architecture document** with modules and thread boundaries
3. a **public API contract** between UI and core
4. a **native project file format spec**
5. a **playback/recording state machine spec**
