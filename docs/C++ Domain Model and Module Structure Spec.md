# MIDI Composer — C++ Domain Model and Module Structure Spec

## 1. Scope

This document defines:

* the **core C++ domain model**
* the **responsibilities of each major module**
* the **ownership and lifetime rules**
* the **boundaries between editing, playback, recording, persistence, and UI integration**

This is for the **native core only**, not the HTML/TypeScript UI.

The target is **C++20 or later**.

---

# 2. Core Design Principles

## 2.1 General principles

The core shall be designed around these rules:

* domain objects represent **musical concepts**, not UI widgets
* musical time is represented canonically in **ticks**
* the model must support both:

  * **fast editing/querying**
  * **fast playback scheduling**
* ownership must be explicit and safe
* modules should be **loosely coupled** and communicate through clear interfaces
* the UI must not depend on internal storage details

## 2.2 C++ principles

The codebase shall prefer:

* value types for domain data
* `std::unique_ptr` for exclusive ownership of polymorphic/infrastructural objects
* `std::shared_ptr` only when shared lifetime is truly required
* `std::span`, references, and raw pointers only as non-owning views
* `std::optional` for optional values
* `std::variant` where a closed set of alternatives makes sense
* `std::expected<T, Error>` or equivalent for recoverable failures
* RAII for all external resources

---

# 3. Top-Level Module Structure

Recommended top-level namespace:

```cpp
namespace midi_composer
```

Recommended submodules:

```cpp
midi_composer::base
midi_composer::music
midi_composer::timeline
midi_composer::project
midi_composer::edit
midi_composer::playback
midi_composer::recording
midi_composer::metronome
midi_composer::midi
midi_composer::io
midi_composer::persistence
midi_composer::device
midi_composer::notify
midi_composer::app
```

## 3.1 Module summary

### `base`

Shared foundational utilities and types.

### `music`

Musical domain entities: notes, tracks, controllers, tempo events, etc.

### `timeline`

Time conversion, measure mapping, tempo map traversal, transport positions.

### `project`

Composition/project aggregate and project-level services.

### `edit`

Editing commands, transactions, undo/redo infrastructure.

### `playback`

Playback engine and scheduler.

### `recording`

Recording engine and live MIDI capture logic.

### `metronome`

Metronome scheduling and click generation.

### `midi`

MIDI protocol-level types and helpers.

### `io`

MIDI file import/export.

### `persistence`

Native project serialization/deserialization.

### `device`

MIDI input/output device abstraction.

### `notify`

Typed event notifications from core to outer layers.

### `app`

High-level façade API used by UI/native shell.

---

# 4. Foundational Types

These should be small, strongly typed, and easy to pass around.

## 4.1 Entity identifiers

All important entities should have stable IDs.

```cpp
namespace midi_composer::base {

using EntityId = std::uint64_t;
using TrackId = EntityId;
using NoteId = EntityId;
using EventId = EntityId;
using CompositionId = EntityId;

}
```

A stronger approach is preferred:

```cpp
template<typename Tag>
class StrongId {
public:
    using value_type = std::uint64_t;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(value_type v) noexcept : value_(v) {}

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }

    auto operator<=>(const StrongId&) const = default;

private:
    value_type value_{0};
};

struct TrackIdTag {};
struct NoteIdTag {};
struct EventIdTag {};
struct CompositionIdTag {};

using TrackId = StrongId<TrackIdTag>;
using NoteId = StrongId<NoteIdTag>;
using EventId = StrongId<EventIdTag>;
using CompositionId = StrongId<CompositionIdTag>;
```

That avoids accidental mixing.

---

## 4.2 Time types

Use strong types, not plain integers everywhere.

```cpp
namespace midi_composer::timeline {

class Tick {
public:
    constexpr Tick() noexcept = default;
    explicit constexpr Tick(std::int64_t v) noexcept : value_(v) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

    auto operator<=>(const Tick&) const = default;

private:
    std::int64_t value_{0};
};

class TickDuration {
public:
    constexpr TickDuration() noexcept = default;
    explicit constexpr TickDuration(std::int64_t v) noexcept : value_(v) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

private:
    std::int64_t value_{0};
};

struct TickRange {
    Tick start;
    Tick end; // half-open [start, end)
};

}
```

Also define:

```cpp
struct MeasureIndex { std::int32_t value{}; };
struct BeatIndex { std::int32_t value{}; };

struct MeasurePosition {
    MeasureIndex measure;
    BeatIndex beat;
    std::int32_t tick_offset_in_beat{};
};
```

---

## 4.3 Result and error model

Recommended:

```cpp
namespace midi_composer::base {

enum class ErrorCode {
    InvalidArgument,
    InvalidState,
    NotFound,
    IoFailure,
    ParseFailure,
    DeviceFailure,
    UnsupportedFormat,
    Conflict,
    InternalError
};

struct Error {
    ErrorCode code{};
    std::string message;
};

template<typename T>
using Result = std::expected<T, Error>;

}
```

---

# 5. Domain Model Overview

The main aggregate is:

* **ProjectDocument**

  * owns one **Composition**
  * owns editor-independent metadata
  * owns revision state
  * coordinates change notifications

Within the composition:

* **Composition**

  * owns tracks
  * owns tempo map
  * owns time signature map
  * owns markers/regions if included
  * defines the canonical musical data

This is the key design decision:

## 5.1 Canonical storage model

* Notes and performance events are stored **per track**
* Time is stored as **absolute ticks**
* Measures are **derived**, not primary storage containers
* Playback uses a **derived playback event stream**, not the editing model directly

That keeps the domain model simple and efficient.

---

# 6. Detailed Domain Model

## 6.1 Composition

Represents the entire musical document.

```cpp
namespace midi_composer::music {

class Composition final {
public:
    using TrackContainer = std::vector<class Track>;

    [[nodiscard]] base::CompositionId id() const noexcept;
    [[nodiscard]] std::string_view title() const noexcept;
    [[nodiscard]] std::uint16_t ppqn() const noexcept;

    [[nodiscard]] const TrackContainer& tracks() const noexcept;
    [[nodiscard]] TrackContainer& tracks() noexcept;

    [[nodiscard]] const class TempoMap& tempo_map() const noexcept;
    [[nodiscard]] TempoMap& tempo_map() noexcept;

    [[nodiscard]] const class TimeSignatureMap& time_signature_map() const noexcept;
    [[nodiscard]] TimeSignatureMap& time_signature_map() noexcept;

private:
    base::CompositionId id_{};
    std::string title_;
    std::uint16_t ppqn_{480};

    TrackContainer tracks_;
    TempoMap tempo_map_;
    TimeSignatureMap time_signature_map_;
};

}
```

### Responsibilities

* own the canonical musical content
* provide access to tracks and global timeline maps
* enforce composition-level invariants
* provide lookup helpers

### Invariants

* `ppqn > 0`
* there is always an effective tempo at tick 0
* there is always an effective time signature at tick 0
* track IDs are unique within the composition

---

## 6.2 Track

A track owns note and controller-like events.

```cpp
namespace midi_composer::music {

class Track final {
public:
    [[nodiscard]] base::TrackId id() const noexcept;
    [[nodiscard]] std::string_view name() const noexcept;

    [[nodiscard]] std::uint8_t midi_channel() const noexcept;
    [[nodiscard]] bool muted() const noexcept;
    [[nodiscard]] bool solo() const noexcept;
    [[nodiscard]] bool armed() const noexcept;

    [[nodiscard]] const std::vector<class Note>& notes() const noexcept;
    [[nodiscard]] std::vector<Note>& notes() noexcept;

    [[nodiscard]] const std::vector<class ControllerEvent>& controller_events() const noexcept;
    [[nodiscard]] std::vector<ControllerEvent>& controller_events() noexcept;

    [[nodiscard]] const std::vector<class PitchBendEvent>& pitch_bends() const noexcept;
    [[nodiscard]] std::vector<PitchBendEvent>& pitch_bends() noexcept;

    [[nodiscard]] const std::vector<class ProgramChangeEvent>& program_changes() const noexcept;
    [[nodiscard]] std::vector<ProgramChangeEvent>& program_changes() noexcept;

private:
    base::TrackId id_{};
    std::string name_;

    std::uint8_t midi_channel_{0};

    bool muted_{false};
    bool solo_{false};
    bool armed_{false};

    std::uint8_t volume_{100};
    std::uint8_t pan_{64};
    std::uint8_t expression_{127};
    std::uint8_t reverb_{0};
    std::uint8_t chorus_{0};

    std::vector<Note> notes_;
    std::vector<ControllerEvent> controller_events_;
    std::vector<PitchBendEvent> pitch_bends_;
    std::vector<ProgramChangeEvent> program_changes_;
};

}
```

### Responsibilities

* own performance events belonging to the track
* expose track-level playback/recording state
* maintain sorted event collections or a dirty-sort contract

### Important note

A track should own the data directly as vectors unless a strong reason emerges to heap-allocate every event. Contiguous storage helps performance and simplifies serialization.

---

## 6.3 Note

```cpp
namespace midi_composer::music {

struct Note final {
    base::NoteId id{};
    timeline::Tick start{};
    timeline::TickDuration duration{};

    std::uint8_t pitch{60};
    std::uint8_t velocity{100};
    std::optional<std::uint8_t> release_velocity{};

    [[nodiscard]] timeline::Tick end() const noexcept {
        return timeline::Tick{start.value() + duration.value()};
    }
};

}
```

### Responsibilities

* represent one musical note in editor-friendly form
* support exact start and duration
* remain independent of raw MIDI note-on/off event pairing once imported

### Invariants

* pitch in `0..127`
* velocity in `1..127` for normal note presence
* duration > 0
* start >= 0

---

## 6.4 ControllerEvent

```cpp
namespace midi_composer::music {

struct ControllerEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint8_t controller{};
    std::uint8_t value{};
};

}
```

### Invariants

* controller in `0..127`
* value in `0..127`

---

## 6.5 PitchBendEvent

```cpp
namespace midi_composer::music {

struct PitchBendEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::int16_t value{}; // e.g. -8192..8191
};

}
```

### Recommendation

Store pitch bend in centered signed form in the domain model, and convert to raw MIDI format in protocol/output layers.

---

## 6.6 ProgramChangeEvent

```cpp
namespace midi_composer::music {

struct ProgramChangeEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint8_t program{};
};

}
```

---

## 6.7 TempoEvent and TempoMap

```cpp
namespace midi_composer::music {

struct TempoEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint32_t microseconds_per_quarter{500000};

    [[nodiscard]] double bpm() const noexcept {
        return 60'000'000.0 / static_cast<double>(microseconds_per_quarter);
    }
};

class TempoMap final {
public:
    [[nodiscard]] const std::vector<TempoEvent>& events() const noexcept;
    [[nodiscard]] std::vector<TempoEvent>& events() noexcept;

private:
    std::vector<TempoEvent> events_;
};

}
```

### Invariants

* sorted by tick
* first effective event at tick 0
* no zero tempo values

---

## 6.8 TimeSignatureEvent and TimeSignatureMap

```cpp
namespace midi_composer::music {

struct TimeSignatureEvent final {
    base::EventId id{};
    timeline::Tick tick{};
    std::uint8_t numerator{4};
    std::uint8_t denominator{4};
};

class TimeSignatureMap final {
public:
    [[nodiscard]] const std::vector<TimeSignatureEvent>& events() const noexcept;
    [[nodiscard]] std::vector<TimeSignatureEvent>& events() noexcept;

private:
    std::vector<TimeSignatureEvent> events_;
};

}
```

### Invariants

* sorted by tick
* effective event at tick 0
* denominator should normally be a power of two

---

## 6.9 Measure as a derived view

A measure should not own notes. It should be a computed view.

```cpp
namespace midi_composer::timeline {

struct MeasureInfo final {
    MeasureIndex index{};
    Tick start{};
    Tick end{};
    std::uint8_t numerator{};
    std::uint8_t denominator{};
};

}
```

This object is created by timeline services, not stored canonically in the composition.

---

## 6.10 ProjectDocument

This is the document-level aggregate used by higher layers.

```cpp
namespace midi_composer::project {

class ProjectDocument final {
public:
    [[nodiscard]] music::Composition& composition() noexcept;
    [[nodiscard]] const music::Composition& composition() const noexcept;

    [[nodiscard]] std::uint64_t revision() const noexcept;
    [[nodiscard]] bool dirty() const noexcept;

    void mark_dirty() noexcept;
    void clear_dirty() noexcept;
    void bump_revision() noexcept;

private:
    music::Composition composition_;
    std::uint64_t revision_{0};
    bool dirty_{false};
};

}
```

### Why separate `ProjectDocument` from `Composition`

Because the document owns concerns like:

* revision number
* dirty state
* file path
* persistence metadata
* future editor/session data hooks

without polluting the music model.

---

# 7. Domain Query Helpers

The core will need efficient querying for UI and playback. Keep querying logic out of raw entity classes when possible.

Recommended service classes:

* `TrackQueryService`
* `TimelineQueryService`
* `CompositionQueryService`

Example:

```cpp
namespace midi_composer::music {

class CompositionQueryService final {
public:
    [[nodiscard]] const Track* find_track(
        const Composition& composition,
        base::TrackId id) const noexcept;

    [[nodiscard]] std::span<const Note> notes_in_range(
        const Track& track,
        timeline::TickRange range) const noexcept;
};

}
```

For MVP, these may start as free functions.

---

# 8. Indexing Strategy

The domain model should stay simple. Secondary indexing should be handled by dedicated helpers.

## 8.1 Canonical storage

Canonical storage stays in vectors inside tracks and maps.

## 8.2 Secondary indexes

Recommended optional index types:

* entity ID to vector position
* cached measure boundaries
* flattened playback event stream
* note range lookup acceleration

These should be rebuildable and never be the sole source of truth.

Example:

```cpp
namespace midi_composer::music {

struct TrackIndex final {
    std::unordered_map<base::NoteId, std::size_t> note_positions;
    std::unordered_map<base::EventId, std::size_t> controller_positions;
    bool valid{false};
};

}
```

Keep indexes separate from the entity types.

---

# 9. Module Responsibilities in Detail

## 9.1 `base`

Contains:

* strong IDs
* error/result types
* logging interfaces
* assertions
* utility helpers
* immutable small helper classes if needed

Should not depend on musical modules.

---

## 9.2 `music`

Contains canonical music-domain types:

* `Composition`
* `Track`
* `Note`
* `ControllerEvent`
* `PitchBendEvent`
* `ProgramChangeEvent`
* `TempoMap`
* `TimeSignatureMap`

Should not contain:

* device handles
* UI selection state
* WebView or JSON-RPC details
* transport thread logic

---

## 9.3 `timeline`

Contains:

* tick/time conversions
* measure calculations
* tempo traversal
* mapping between:

  * tick ↔ measure position
  * tick ↔ wall time

Suggested main classes:

```cpp
class MeasureMap;
class TimeConverter;
class TransportPosition;
```

Example:

```cpp
namespace midi_composer::timeline {

struct TransportPosition final {
    Tick current_tick{};
    MeasurePosition measure_position{};
    double current_bpm{};
};

}
```

This module depends on `music`, but `music` should not depend on `timeline` implementations beyond shared types.

---

## 9.4 `project`

Contains:

* `ProjectDocument`
* document-level metadata
* save/load session state hooks
* document lifecycle services

This module owns the current composition document used by the app.

---

## 9.5 `edit`

Contains editing commands and transactions.

Recommended core abstractions:

```cpp
namespace midi_composer::edit {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual base::Result<void> apply(project::ProjectDocument&) = 0;
    virtual base::Result<void> undo(project::ProjectDocument&) = 0;
};

class CommandTransaction;
class UndoStack;
class EditService;

}
```

For performance and simplicity, you may later replace runtime polymorphism with `std::variant` commands, but the concept remains the same.

### `EditService` responsibilities

* validate edits
* apply mutations
* bump revision
* emit notifications
* integrate undo/redo

---

## 9.6 `playback`

Contains playback-specific derived models and engines.

Suggested classes:

```cpp
namespace midi_composer::playback {

enum class TransportState {
    Stopped,
    Playing,
    Paused,
    Recording,
    PreRoll,
    Seeking
};

struct ScheduledMidiEvent final {
    timeline::Tick tick{};
    std::chrono::steady_clock::time_point due_time{};
    // midi payload fields...
};

class PlaybackEventBuilder;
class PlaybackScheduler;
class PlaybackEngine;

}
```

### Responsibilities

* flatten domain model into playable event stream
* schedule outgoing MIDI with timing accuracy
* manage transport state
* emit transport notifications

### Must not do

* directly mutate the composition during playback except tightly defined cases
* depend on UI code

---

## 9.7 `recording`

Contains live MIDI capture and conversion to domain edits.

Suggested classes:

```cpp
namespace midi_composer::recording {

struct LiveMidiMessage final {
    std::chrono::steady_clock::time_point timestamp;
    std::vector<std::uint8_t> bytes;
};

class RecordingBuffer;
class NoteCaptureState;
class RecordingEngine;

}
```

### Responsibilities

* consume incoming MIDI input
* timestamp events
* pair note-on/note-off into notes
* translate captured input into edit operations
* support overdub/replace policies

---

## 9.8 `metronome`

Contains click generation logic.

Suggested classes:

```cpp
namespace midi_composer::metronome {

struct MetronomeConfig final {
    bool enabled{true};
    bool count_in_enabled{true};
    std::uint8_t accent_note{76};
    std::uint8_t regular_note{77};
    std::uint8_t velocity{100};
};

class MetronomePatternBuilder;
class MetronomeEngine;

}
```

### Responsibilities

* generate beat clicks from tempo/time signature context
* distinguish downbeat from regular beats
* work with playback and recording

---

## 9.9 `midi`

Contains protocol-level definitions.

Suggested contents:

* MIDI status constants
* channel voice message helpers
* encoding/decoding utilities
* Standard MIDI File event helpers

Example:

```cpp
namespace midi_composer::midi {

enum class MessageType {
    NoteOff,
    NoteOn,
    PolyAftertouch,
    ControlChange,
    ProgramChange,
    ChannelAftertouch,
    PitchBend,
    Meta,
    SysEx
};

struct ShortMessage final {
    std::uint8_t status{};
    std::uint8_t data1{};
    std::uint8_t data2{};
};

}
```

This module is protocol-oriented, not editor-oriented.

---

## 9.10 `io`

Contains MIDI file import/export.

Suggested classes:

```cpp
namespace midi_composer::io {

class MidiFileReader;
class MidiFileWriter;
class MidiImporter;
class MidiExporter;

}
```

### Responsibilities

* parse/write Standard MIDI Files
* map raw MIDI file events into domain model
* handle import policies and malformed files robustly

---

## 9.11 `persistence`

Contains native project format save/load.

Suggested classes:

```cpp
namespace midi_composer::persistence {

class ProjectSerializer;
class ProjectDeserializer;
class ProjectRepository;

}
```

### Responsibilities

* serialize `ProjectDocument`
* include schema versioning
* preserve stable IDs and editor-oriented metadata

This is intentionally distinct from MIDI import/export.

---

## 9.12 `device`

Abstracts MIDI input/output backends.

Suggested interfaces:

```cpp
namespace midi_composer::device {

struct MidiInputDeviceInfo final {
    std::string id;
    std::string name;
};

struct MidiOutputDeviceInfo final {
    std::string id;
    std::string name;
};

class IMidiInput {
public:
    virtual ~IMidiInput() = default;
    virtual base::Result<void> open(std::string_view device_id) = 0;
    virtual void close() noexcept = 0;
};

class IMidiOutput {
public:
    virtual ~IMidiOutput() = default;
    virtual base::Result<void> open(std::string_view device_id) = 0;
    virtual base::Result<void> send(const midi::ShortMessage& msg) = 0;
    virtual void close() noexcept = 0;
};

class IMidiDeviceManager {
public:
    virtual ~IMidiDeviceManager() = default;
    virtual std::vector<MidiInputDeviceInfo> list_inputs() = 0;
    virtual std::vector<MidiOutputDeviceInfo> list_outputs() = 0;
};

}
```

Use platform-specific implementations behind these interfaces.

---

## 9.13 `notify`

Contains typed notifications emitted by the core.

Suggested model:

```cpp
namespace midi_composer::notify {

enum class EventType {
    NoteCreated,
    NoteUpdated,
    NoteDeleted,
    TrackCreated,
    TrackUpdated,
    TrackDeleted,
    TempoMapChanged,
    TimeSignatureMapChanged,
    PlaybackStarted,
    PlaybackStopped,
    PlaybackPositionChanged,
    RecordingStarted,
    RecordingStopped,
    DeviceListChanged
};

struct Notification final {
    EventType type{};
    std::uint64_t revision{};
    std::string payload_json; // MVP option
};

class IEventSink {
public:
    virtual ~IEventSink() = default;
    virtual void publish(const Notification& event) = 0;
};

}
```

For a better typed model later, replace `payload_json` with a `std::variant` payload set.

---

## 9.14 `app`

This is the façade layer the UI/shell should use.

Suggested main class:

```cpp
namespace midi_composer::app {

class CoreFacade final {
public:
    base::Result<void> new_project();
    base::Result<void> open_project(const std::filesystem::path& path);
    base::Result<void> save_project(const std::filesystem::path& path);

    base::Result<void> import_midi(const std::filesystem::path& path);
    base::Result<void> export_midi(const std::filesystem::path& path);

    base::Result<void> play();
    base::Result<void> stop();
    base::Result<void> pause();
    base::Result<void> seek(timeline::Tick tick);

    base::Result<void> create_note(base::TrackId track_id, music::Note note);
    base::Result<void> delete_note(base::TrackId track_id, base::NoteId note_id);

private:
    project::ProjectDocument document_;
    // services...
};

}
```

This keeps UI integration clean and avoids letting the UI call directly into many subsystems.

---

# 10. Ownership and Lifetime Rules

This needs to be explicit.

## 10.1 Owned by value

Prefer by-value ownership for:

* `Composition`
* `Track`
* `Note`
* `ControllerEvent`
* `PitchBendEvent`
* `TempoEvent`
* `TimeSignatureEvent`
* configuration structs
* query result structs

## 10.2 Owned by `unique_ptr`

Use `std::unique_ptr` for:

* device backend implementations
* playback scheduler internals
* file readers/writers if polymorphic
* façade-internal service objects with stable identity
* thread-owning engine components

## 10.3 Avoid shared ownership by default

`std::shared_ptr` should be rare. Use it only if you truly have cross-module shared lifetime that cannot be represented more cleanly.

## 10.4 Non-owning references

Use:

* references for required short-lived access
* raw pointers for optional non-owning access
* `std::span` for array views

Avoid storing long-lived non-owning pointers unless lifetime is guaranteed by design.

---

# 11. Suggested Folder Layout

A practical source layout:

```text
src/
  base/
    error.hpp
    result.hpp
    strong_id.hpp
    assertions.hpp

  music/
    composition.hpp
    composition.cpp
    track.hpp
    track.cpp
    note.hpp
    controller_event.hpp
    pitch_bend_event.hpp
    tempo_map.hpp
    tempo_map.cpp
    time_signature_map.hpp
    time_signature_map.cpp

  timeline/
    tick.hpp
    measure_map.hpp
    measure_map.cpp
    time_converter.hpp
    time_converter.cpp
    transport_position.hpp

  project/
    project_document.hpp
    project_document.cpp

  edit/
    command.hpp
    edit_service.hpp
    edit_service.cpp
    undo_stack.hpp
    undo_stack.cpp
    commands/

  playback/
    playback_engine.hpp
    playback_engine.cpp
    playback_scheduler.hpp
    playback_scheduler.cpp
    playback_event_builder.hpp
    playback_event_builder.cpp

  recording/
    recording_engine.hpp
    recording_engine.cpp
    note_capture_state.hpp

  metronome/
    metronome_engine.hpp
    metronome_engine.cpp

  midi/
    midi_message.hpp
    midi_constants.hpp
    midi_encoding.cpp

  io/
    midi_file_reader.hpp
    midi_file_reader.cpp
    midi_file_writer.hpp
    midi_file_writer.cpp
    midi_importer.hpp
    midi_importer.cpp
    midi_exporter.hpp
    midi_exporter.cpp

  persistence/
    project_serializer.hpp
    project_serializer.cpp
    project_deserializer.hpp
    project_deserializer.cpp

  device/
    midi_device_manager.hpp
    midi_input.hpp
    midi_output.hpp
    backend/

  notify/
    notification.hpp
    event_sink.hpp

  app/
    core_facade.hpp
    core_facade.cpp
```

---

# 12. Dependency Rules

To keep the codebase healthy, define allowed dependency directions.

## 12.1 Low-level to high-level

Allowed:

* `base` → nothing
* `music` → `base`, `timeline` shared primitive types only if needed
* `timeline` → `base`, `music`
* `project` → `base`, `music`
* `edit` → `base`, `music`, `project`, `notify`
* `playback` → `base`, `music`, `timeline`, `device`, `notify`
* `recording` → `base`, `music`, `timeline`, `project`, `edit`, `device`, `notify`
* `metronome` → `base`, `timeline`, `music`, `midi`
* `io` → `base`, `music`, `midi`
* `persistence` → `base`, `project`, `music`
* `app` → all public service modules

## 12.2 Forbidden dependencies

Examples:

* `music` must not depend on `playback`
* `music` must not depend on `device`
* `music` must not depend on `notify`
* `io` must not depend on UI or WebView code
* `device` must not depend on `app`

That preserves separation of concerns.

---

# 13. Service Layer Recommendations

A clean implementation benefits from explicit services instead of “fat entities.”

Recommended services:

* `CompositionValidator`
* `EditService`
* `TimelineService`
* `PlaybackEventBuilder`
* `PlaybackEngine`
* `RecordingEngine`
* `ProjectRepository`
* `MidiImportService`
* `MidiExportService`

Entities stay mostly data-centric; services hold behavior that spans multiple entities.

---

# 14. Example Class Relationship Summary

## 14.1 Aggregate structure

```text
ProjectDocument
  └── Composition
        ├── TempoMap
        ├── TimeSignatureMap
        └── vector<Track>
              ├── vector<Note>
              ├── vector<ControllerEvent>
              ├── vector<PitchBendEvent>
              └── vector<ProgramChangeEvent>
```

## 14.2 Runtime service structure

```text
CoreFacade
  ├── ProjectDocument
  ├── EditService
  ├── PlaybackEngine
  ├── RecordingEngine
  ├── MetronomeEngine
  ├── MidiImportService
  ├── MidiExportService
  ├── ProjectRepository
  └── NotificationSink
```

That’s a clean and workable arrangement.

---

# 15. Recommended MVP Interfaces

Here is a more concrete MVP-facing set of core interfaces.

## 15.1 Edit service

```cpp
class EditService final {
public:
    base::Result<base::NoteId> create_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        timeline::Tick start,
        timeline::TickDuration duration,
        std::uint8_t pitch,
        std::uint8_t velocity);

    base::Result<void> move_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id,
        timeline::Tick new_start);

    base::Result<void> resize_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id,
        timeline::TickDuration new_duration);

    base::Result<void> delete_note(
        project::ProjectDocument& doc,
        base::TrackId track_id,
        base::NoteId note_id);
};
```

## 15.2 Timeline service

```cpp
class TimelineService final {
public:
    [[nodiscard]] timeline::MeasurePosition tick_to_measure_position(
        const music::Composition& composition,
        timeline::Tick tick) const;

    [[nodiscard]] base::Result<timeline::Tick> measure_position_to_tick(
        const music::Composition& composition,
        timeline::MeasurePosition position) const;
};
```

## 15.3 Playback engine

```cpp
class PlaybackEngine final {
public:
    base::Result<void> play(const project::ProjectDocument& doc);
    base::Result<void> stop();
    base::Result<void> pause();
    base::Result<void> seek(timeline::Tick tick);

    [[nodiscard]] playback::TransportState state() const noexcept;
};
```

## 15.4 Recording engine

```cpp
class RecordingEngine final {
public:
    base::Result<void> arm_track(project::ProjectDocument& doc, base::TrackId track_id, bool armed);
    base::Result<void> start(project::ProjectDocument& doc);
    base::Result<void> stop(project::ProjectDocument& doc);
};
```

---

# 16. Validation Placement

Do not scatter validation randomly.

Recommended validation layers:

## 16.1 Entity-level validation

Cheap invariant checks near data constructors or edit points:

* pitch range
* duration > 0
* controller range

## 16.2 Service-level validation

Cross-entity checks:

* track exists
* note ID exists
* tempo map remains valid
* edit allowed in current transport state

## 16.3 Import-time validation

More permissive:

* accept weird but usable MIDI where possible
* normalize into valid domain objects
* report warnings for anomalies

---

# 17. Threading Boundaries in the Module Design

Even in the domain spec, define which modules are thread-sensitive.

## 17.1 Mostly single-threaded modules

Prefer single-threaded ownership for:

* `music`
* `project`
* `edit`
* `persistence`

## 17.2 Concurrent/runtime modules

Thread-sensitive:

* `playback`
* `recording`
* `device`

These should communicate with the main document owner through queues or command dispatch, not unrestricted direct mutation.

## 17.3 Recommendation

Treat `ProjectDocument` as owned by a single orchestration thread or guarded access point. That keeps correctness manageable.

---

# 18. What Not to Put in the Domain Model

This is just as important.

Do **not** put these in canonical music entities:

* UI selection state
* screen coordinates
* HTML/CSS concerns
* WebView bridge DTOs
* raw platform device handles
* command history internals
* playback scheduler internals
* temporary drag gesture state

Keep the domain model clean.

---

# 19. Suggested First-Pass Implementation Order

A sensible implementation sequence:

1. `base` strong types and error model
2. `music` entities and composition aggregate
3. `timeline` measure/time conversion
4. `project` document wrapper
5. `edit` service for notes/tracks/tempo/time signature
6. `persistence` native save/load
7. `io` MIDI import/export
8. `device` MIDI backend abstraction
9. `playback`
10. `recording`
11. `metronome`
12. `app` façade

That order reduces rework.

---

# 20. Final Recommended Baseline

If you want a practical baseline to lock now, I’d recommend this exact architectural stance:

* **Canonical model**: value-based `Composition` with per-track vectors of events
* **Measures**: derived from the time-signature map
* **Editing**: service-based, command-capable
* **Playback**: separate derived scheduling stream
* **Recording**: input-to-edit translation layer
* **UI integration**: only through `CoreFacade` + typed notifications
* **Ownership**: by-value data model, `unique_ptr` for infrastructural engines/backends
