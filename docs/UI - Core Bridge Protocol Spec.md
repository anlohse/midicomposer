# MIDI Composer — UI ↔ Core Bridge Protocol Spec

## 1. Purpose

This document defines the communication protocol between:

* the **native C++ core**
* the **HTML/TypeScript UI** running inside **Saucer WebView**

The bridge protocol must allow the UI to:

* send user commands to the core
* request data and state snapshots
* receive notifications from the core
* stay synchronized during editing, playback, and recording

The bridge must also allow the core to:

* validate and execute commands
* send structured success/error responses
* push model and transport updates
* maintain multiple open documents/windows safely

This protocol is intended for MVP, but should be extensible.

---

# 2. Design Goals

The bridge protocol shall be:

* simple to debug
* strongly structured
* versioned
* document-aware
* safe against malformed messages
* asynchronous by default
* stable enough for UI and core to evolve independently

For MVP, the protocol will use **JSON messages**.

That is the right tradeoff for:

* fast development
* easy inspection in dev tools
* easy encoding/decoding in both TypeScript and C++

---

# 3. Communication Model

## 3.1 Direction types

There are three message directions:

### UI → Core

Commands and queries from the frontend.

### Core → UI

Responses and notifications.

### Optional Core → UI snapshot push

Full or partial document snapshots sent after load/open or reconnect.

---

## 3.2 Interaction styles

The protocol supports two interaction styles:

### Request/response

Used for:

* create note
* open file
* save file
* query document snapshot
* transport actions

### Push notifications

Used for:

* playback position changed
* note updated from recording
* document changed
* device list changed
* transport state changed

---

# 4. Protocol Versioning

Every message shall include a protocol version.

## 4.1 Initial version

```json
{
  "protocolVersion": 1
}
```

## 4.2 Rule

If a message arrives with an unsupported protocol version:

* it must be rejected
* the receiver should return a structured protocol error if possible

## 4.3 Compatibility policy

For MVP:

* UI and core are expected to use the same protocol version
* backward compatibility is not required initially

But the field must exist from day one.

---

# 5. Transport Format

All bridge messages shall be JSON objects with a common envelope.

## 5.1 Common envelope fields

Every message should include:

* `protocolVersion`
* `messageId`
* `type`
* `timestamp`
* `windowId` if applicable
* `documentId` if applicable
* `payload`

## 5.2 Base message shape

```json
{
  "protocolVersion": 1,
  "messageId": "msg-12345",
  "type": "CreateNoteCommand",
  "timestamp": 1712345678901,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {}
}
```

### Field notes

#### `messageId`

Unique per message from sender.
Used for tracing request/response pairs.

#### `type`

Logical message type string.

#### `timestamp`

Unix epoch milliseconds or other clearly documented format.

#### `windowId`

Identifies the UI window instance.
Useful when multiple windows are open.

#### `documentId`

Identifies the target document.
Required for document-scoped operations.

#### `payload`

Type-specific message data.

---

# 6. Message Categories

The protocol defines four top-level categories:

* **commands**
* **queries**
* **responses**
* **events**

---

# 7. Command Messages

Commands request that the core perform an action that may mutate state or trigger behavior.

## 7.1 Command naming convention

Use verb-first names:

* `CreateNoteCommand`
* `MoveNoteCommand`
* `DeleteNoteCommand`
* `PlayCommand`
* `StopCommand`
* `OpenProjectCommand`

This is verbose, but crystal clear.

---

## 7.2 Generic command envelope

```json
{
  "protocolVersion": 1,
  "messageId": "msg-1001",
  "type": "CreateNoteCommand",
  "timestamp": 1712345678901,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "trackId": "track-3",
    "startTick": 1920,
    "durationTicks": 480,
    "pitch": 64,
    "velocity": 100
  }
}
```

---

## 7.3 Core command groups

### Document/file commands

* `NewProjectCommand`
* `OpenProjectCommand`
* `SaveProjectCommand`
* `SaveProjectAsCommand`
* `CloseProjectCommand`
* `ImportMidiCommand`
* `ExportMidiCommand`

### Edit commands

* `CreateNoteCommand`
* `UpdateNoteCommand`
* `MoveNoteCommand`
* `ResizeNoteCommand`
* `DeleteNoteCommand`
* `BatchEditCommand`
* `UndoCommand`
* `RedoCommand`

### Track commands

* `CreateTrackCommand`
* `DeleteTrackCommand`
* `RenameTrackCommand`
* `SetTrackChannelCommand`
* `SetTrackVolumeCommand`
* `SetTrackPanCommand`
* `SetTrackMuteCommand`
* `SetTrackSoloCommand`
* `SetTrackArmCommand`

### Timeline commands

* `InsertTempoEventCommand`
* `UpdateTempoEventCommand`
* `DeleteTempoEventCommand`
* `InsertTimeSignatureEventCommand`
* `UpdateTimeSignatureEventCommand`
* `DeleteTimeSignatureEventCommand`

### Transport commands

* `PlayCommand`
* `StopCommand`
* `PauseCommand`
* `SeekCommand`
* `SetLoopRegionCommand`
* `ClearLoopRegionCommand`

### Recording/metronome commands

* `StartRecordingCommand`
* `StopRecordingCommand`
* `SetMetronomeEnabledCommand`
* `SetCountInEnabledCommand`

### Device commands

* `RefreshMidiDevicesCommand`
* `SelectMidiInputDeviceCommand`
* `SelectMidiOutputDeviceCommand`

### UI/session-oriented commands that still require core cooperation

* `SetEditModeCommand`
* `SetPlayModeCommand`

These may or may not remain core commands later. For MVP, only send them to core if they affect core behavior.

---

# 8. Query Messages

Queries request state but do not mutate the model.

## 8.1 Naming convention

Use names like:

* `GetDocumentSnapshotQuery`
* `GetTransportStateQuery`
* `GetMidiDevicesQuery`

## 8.2 Example

```json
{
  "protocolVersion": 1,
  "messageId": "msg-2001",
  "type": "GetDocumentSnapshotQuery",
  "timestamp": 1712345678901,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {}
}
```

## 8.3 Initial query set

* `GetDocumentSnapshotQuery`
* `GetTransportStateQuery`
* `GetPreferencesSnapshotQuery`
* `GetMidiDevicesQuery`
* `GetWindowStateQuery` if needed later
* `GetDocumentListQuery` for multi-document support

---

# 9. Response Messages

Responses are sent by the core to the UI for commands and queries.

## 9.1 Response rules

Every command or query must result in exactly one terminal response:

* success
* error

Even if the command also triggers later async events.

## 9.2 Generic response envelope

```json
{
  "protocolVersion": 1,
  "messageId": "msg-3001",
  "type": "CommandResponse",
  "timestamp": 1712345678910,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "requestMessageId": "msg-1001",
    "success": true,
    "result": {
      "noteId": "note-42",
      "revision": 18
    }
  }
}
```

---

## 9.3 Error response example

```json
{
  "protocolVersion": 1,
  "messageId": "msg-3002",
  "type": "CommandResponse",
  "timestamp": 1712345678910,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "requestMessageId": "msg-1001",
    "success": false,
    "error": {
      "code": "InvalidArgument",
      "message": "Pitch must be between 0 and 127",
      "details": {
        "field": "pitch"
      }
    }
  }
}
```

---

## 9.4 Response payload shape

Recommended standard fields:

* `requestMessageId`
* `success`
* `result` on success
* `error` on failure

---

# 10. Event Messages

Events are asynchronous notifications pushed from core to UI.

## 10.1 Event naming convention

Use past-tense or state-change style:

* `NoteCreatedEvent`
* `NoteUpdatedEvent`
* `TrackUpdatedEvent`
* `TransportStateChangedEvent`
* `PlaybackPositionChangedEvent`

That makes them read naturally as facts.

---

## 10.2 Event categories

### Document/model events

* `DocumentOpenedEvent`
* `DocumentClosedEvent`
* `DocumentRevisionChangedEvent`
* `DocumentPatchedEvent`
* `TrackCreatedEvent`
* `TrackUpdatedEvent`
* `TrackDeletedEvent`
* `NoteCreatedEvent`
* `NoteUpdatedEvent`
* `NoteDeletedEvent`
* `TempoMapChangedEvent`
* `TimeSignatureMapChangedEvent`

### Transport/playback events

* `TransportStateChangedEvent`
* `PlaybackStartedEvent`
* `PlaybackStoppedEvent`
* `PlaybackPositionChangedEvent`
* `LoopRegionChangedEvent`

### Recording events

* `RecordingStartedEvent`
* `RecordingStoppedEvent`
* `RecordingNotePreviewEvent` optional
* `RecordingTakeCommittedEvent`

### Device events

* `MidiDeviceListChangedEvent`
* `MidiDeviceErrorEvent`

### Error/warning events

* `CoreWarningEvent`
* `CoreErrorEvent`

---

## 10.3 Example event

```json
{
  "protocolVersion": 1,
  "messageId": "msg-4001",
  "type": "NoteCreatedEvent",
  "timestamp": 1712345678920,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "revision": 19,
    "trackId": "track-3",
    "note": {
      "id": "note-42",
      "startTick": 1920,
      "durationTicks": 480,
      "pitch": 64,
      "velocity": 100
    }
  }
}
```

---

# 11. Snapshot and Patch Strategy

This is one of the most important architectural decisions.

## 11.1 Recommended model for MVP

Use a **snapshot + incremental patch** model.

### Snapshot

Sent when:

* a document is opened
* a window is initialized
* the UI reconnects or requests resync

### Patch/event updates

Sent after:

* edits
* transport changes
* device changes
* recording changes

This avoids rebuilding the entire document state after every edit.

---

## 11.2 Document snapshot shape

A snapshot should contain the current full state needed by the UI to render the document.

Example:

```json
{
  "protocolVersion": 1,
  "messageId": "msg-5001",
  "type": "DocumentSnapshotEvent",
  "timestamp": 1712345678930,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "revision": 19,
    "document": {
      "id": "doc-7",
      "title": "Untitled",
      "ppqn": 480,
      "tracks": [],
      "tempoMap": [],
      "timeSignatureMap": [],
      "transport": {
        "state": "Stopped",
        "currentTick": 0
      }
    }
  }
}
```

---

## 11.3 Patch event recommendation

For MVP, do not jump immediately to a generic JSON Patch engine unless you really want that complexity.

Better option:

* use typed domain events such as `NoteCreatedEvent`, `NoteUpdatedEvent`, `TrackUpdatedEvent`

Later, you can introduce generalized patch batches if necessary.

---

# 12. Document Identity and Routing

The bridge must support multiple open documents.

## 12.1 Required IDs

Use these IDs consistently:

* `windowId`
* `documentId`
* entity IDs inside payloads

## 12.2 Rules

### `windowId`

Identifies which UI window or view instance is sending/receiving the message.

### `documentId`

Identifies the target project document in the core.

Commands that mutate a document must include `documentId`.

Global commands such as `GetMidiDevicesQuery` may omit `documentId`.

---

# 13. Message Ordering Rules

Because playback and recording are asynchronous, ordering matters.

## 13.1 Command responses

A response must refer to the exact originating `requestMessageId`.

## 13.2 Event ordering

Within a single document, the core should preserve logical ordering as much as possible.

Examples:

* `NoteCreatedEvent` should not arrive before the success response for the command that created it, unless explicitly documented
* revision numbers must increase monotonically

## 13.3 Revision-based consistency

Every model-changing event should include a `revision`.

The UI should use revisions to:

* detect stale messages
* request resync if out-of-order or missing updates are suspected

---

# 14. Error Model

The bridge must use structured errors.

## 14.1 Standard error shape

```json
{
  "code": "InvalidArgument",
  "message": "Pitch must be between 0 and 127",
  "details": {
    "field": "pitch"
  }
}
```

## 14.2 Recommended error codes

* `InvalidArgument`
* `InvalidState`
* `NotFound`
* `Conflict`
* `UnsupportedOperation`
* `IoFailure`
* `DeviceFailure`
* `ParseFailure`
* `ProtocolError`
* `InternalError`

## 14.3 Bridge-level protocol errors

Examples:

* missing required field
* unknown message type
* wrong payload shape
* unsupported protocol version
* invalid document ID

These should return an error response when possible.

---

# 15. Data Transfer Objects

The bridge should use DTOs separate from the internal core domain model.

That is very important.

## 15.1 Why separate DTOs

Because internal entities may evolve differently from UI-facing state.

DTO separation helps:

* versioning
* serialization stability
* avoiding accidental leakage of internal implementation details

## 15.2 Main DTO categories

### Document DTOs

* `DocumentDto`
* `TrackDto`
* `NoteDto`
* `ControllerEventDto`
* `PitchBendEventDto`
* `TempoEventDto`
* `TimeSignatureEventDto`

### Transport DTOs

* `TransportStateDto`
* `PlaybackPositionDto`

### Device DTOs

* `MidiInputDeviceDto`
* `MidiOutputDeviceDto`

### Preferences/session DTOs

* `PreferencesDto`
* `WindowLayoutDto`

---

# 16. Recommended DTO Shapes

## 16.1 Note DTO

```json
{
  "id": "note-42",
  "startTick": 1920,
  "durationTicks": 480,
  "pitch": 64,
  "velocity": 100
}
```

## 16.2 Track DTO

```json
{
  "id": "track-3",
  "name": "Piano",
  "midiChannel": 0,
  "muted": false,
  "solo": false,
  "armed": false,
  "volume": 100,
  "pan": 64,
  "notes": []
}
```

## 16.3 Tempo event DTO

```json
{
  "id": "tempo-1",
  "tick": 0,
  "microsecondsPerQuarter": 500000,
  "bpm": 120.0
}
```

---

# 17. Bridge Command Examples

## 17.1 Create note

### UI → Core

```json
{
  "protocolVersion": 1,
  "messageId": "msg-1001",
  "type": "CreateNoteCommand",
  "timestamp": 1712345678901,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "trackId": "track-3",
    "startTick": 1920,
    "durationTicks": 480,
    "pitch": 64,
    "velocity": 100
  }
}
```

### Core → UI response

```json
{
  "protocolVersion": 1,
  "messageId": "msg-1001-r",
  "type": "CommandResponse",
  "timestamp": 1712345678905,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "requestMessageId": "msg-1001",
    "success": true,
    "result": {
      "noteId": "note-42",
      "revision": 22
    }
  }
}
```

### Core → UI event

```json
{
  "protocolVersion": 1,
  "messageId": "msg-evt-9",
  "type": "NoteCreatedEvent",
  "timestamp": 1712345678906,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "revision": 22,
    "trackId": "track-3",
    "note": {
      "id": "note-42",
      "startTick": 1920,
      "durationTicks": 480,
      "pitch": 64,
      "velocity": 100
    }
  }
}
```

---

## 17.2 Play

### UI → Core

```json
{
  "protocolVersion": 1,
  "messageId": "msg-play-1",
  "type": "PlayCommand",
  "timestamp": 1712345679000,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {}
}
```

### Core → UI response

```json
{
  "protocolVersion": 1,
  "messageId": "msg-play-1-r",
  "type": "CommandResponse",
  "timestamp": 1712345679001,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "requestMessageId": "msg-play-1",
    "success": true,
    "result": {
      "state": "Playing"
    }
  }
}
```

### Core → UI async position updates

```json
{
  "protocolVersion": 1,
  "messageId": "msg-pos-1",
  "type": "PlaybackPositionChangedEvent",
  "timestamp": 1712345679020,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "tick": 240,
    "measure": 0,
    "beat": 1,
    "subTick": 0,
    "state": "Playing"
  }
}
```

---

# 18. Batch Edit Protocol

The UI will often produce gesture-based edits, not single atomic note changes.

## 18.1 Requirement

The bridge should support batching multiple edit operations into one transaction.

## 18.2 Message shape

```json
{
  "protocolVersion": 1,
  "messageId": "msg-batch-1",
  "type": "BatchEditCommand",
  "timestamp": 1712345679100,
  "windowId": "win-1",
  "documentId": "doc-7",
  "payload": {
    "operations": [
      {
        "type": "MoveNote",
        "trackId": "track-3",
        "noteId": "note-10",
        "newStartTick": 1440
      },
      {
        "type": "ResizeNote",
        "trackId": "track-3",
        "noteId": "note-11",
        "newDurationTicks": 960
      }
    ]
  }
}
```

## 18.3 Behavior

* all operations should be applied atomically if possible
* one success response returns the final revision
* events may be emitted individually or as one batch-changed event

For MVP, either approach is okay, but document it clearly.

Recommended MVP behavior:

* one success response
* one `DocumentPatchedEvent` with a list of changes or a revision-only invalidation event

---

# 19. Transport Update Throttling

Playback can generate too many updates if not controlled.

## 19.1 Requirement

`PlaybackPositionChangedEvent` must be throttled.

## 19.2 Recommended rate

Use something like:

* 20 Hz minimum acceptable
* 30–60 Hz ideal depending on UI smoothness and cost

## 19.3 Rule

The UI must never assume it receives every tick.
It should treat transport updates as sampled state.

---

# 20. Recording-Specific Bridge Behavior

Recording introduces live state.

## 20.1 Recording flow

When recording starts:

* core sends `RecordingStartedEvent`
* transport updates continue
* optionally live note preview events are sent
* completed recorded notes are committed as normal note events or a take-commit event

## 20.2 Recommendation for MVP

Keep this simple:

* do not send ultra-chatty raw MIDI input messages to UI
* send only meaningful editor updates

Examples:

* `RecordingStartedEvent`
* `RecordingStoppedEvent`
* `NoteCreatedEvent` for newly committed notes

Optional:

* `LiveInputActivityEvent` for UI indicators

---

# 21. Core-to-UI State Sync Policy

## 21.1 Single source of truth

The **core** is the source of truth for document and transport state.

The UI may keep a local mirror/cache for rendering, but it must treat core messages as authoritative.

## 21.2 Resync mechanism

If the UI detects inconsistency, it should be able to issue:

* `GetDocumentSnapshotQuery`
* `GetTransportStateQuery`

This is important for robustness.

---

# 22. TypeScript-Side Protocol Model

On the UI side, define discriminated unions for all messages.

Example shape:

```ts
type BridgeMessage =
  | CreateNoteCommand
  | MoveNoteCommand
  | CommandResponse
  | NoteCreatedEvent
  | PlaybackPositionChangedEvent
  | DocumentSnapshotEvent;
```

Each message type should have:

* a literal `type`
* strongly typed `payload`

This makes the UI side much safer.

---

# 23. C++-Side Protocol Model

On the C++ side, do not parse raw JSON everywhere.

Recommended structure:

* parse JSON into DTO structs
* validate DTOs
* dispatch DTOs to handlers
* map handler results back into response/event DTOs
* serialize DTOs to JSON at the boundary

This keeps bridge code clean and testable.

---

# 24. Recommended Bridge Modules

In `core/src/ui_bridge/`, use something like:

```text
ui_bridge/
  bridge_protocol.hpp
  bridge_protocol.cpp
  bridge_dispatcher.hpp
  bridge_dispatcher.cpp
  message_parser.hpp
  message_parser.cpp
  message_serializer.hpp
  message_serializer.cpp
  command_handlers/
  dto/
    common.hpp
    commands.hpp
    events.hpp
    responses.hpp
    snapshots.hpp
```

## 24.1 Responsibilities

### `message_parser`

JSON → DTOs

### `bridge_dispatcher`

route DTOs to handler functions

### `command_handlers`

execute specific command/query logic

### `message_serializer`

DTOs → JSON

---

# 25. Security and Validation Rules

Even though this is an in-process desktop app, the bridge must still validate inputs.

## 25.1 Validate at the bridge boundary

Check:

* protocol version
* message type
* required fields
* integer ranges
* ID presence
* document/window existence

## 25.2 Never trust UI payloads blindly

The UI is not the authority for:

* note validity
* transport state validity
* document existence
* device availability

The core must validate everything.

---

# 26. Logging and Diagnostics

The bridge should be easy to debug.

## 26.1 Log categories

Recommended:

* `bridge.inbound`
* `bridge.outbound`
* `bridge.error`
* `bridge.dispatch`

## 26.2 Logging policy

In debug builds:

* log message type, IDs, and summary
* avoid dumping extremely noisy position updates unless enabled

In release builds:

* log warnings/errors and important lifecycle transitions

---

# 27. Recommended Initial Protocol Surface

To keep MVP manageable, I would start with this minimum set.

## 27.1 Commands

* `NewProjectCommand`
* `OpenProjectCommand`
* `SaveProjectCommand`
* `ImportMidiCommand`
* `ExportMidiCommand`
* `CreateNoteCommand`
* `MoveNoteCommand`
* `ResizeNoteCommand`
* `DeleteNoteCommand`
* `CreateTrackCommand`
* `RenameTrackCommand`
* `SetTrackVolumeCommand`
* `SetTrackPanCommand`
* `SetTrackMuteCommand`
* `SetTrackSoloCommand`
* `SetTrackArmCommand`
* `PlayCommand`
* `StopCommand`
* `PauseCommand`
* `SeekCommand`
* `StartRecordingCommand`
* `StopRecordingCommand`
* `SetMetronomeEnabledCommand`

## 27.2 Queries

* `GetDocumentSnapshotQuery`
* `GetTransportStateQuery`
* `GetMidiDevicesQuery`

## 27.3 Events

* `DocumentSnapshotEvent`
* `DocumentRevisionChangedEvent`
* `NoteCreatedEvent`
* `NoteUpdatedEvent`
* `NoteDeletedEvent`
* `TrackUpdatedEvent`
* `TransportStateChangedEvent`
* `PlaybackPositionChangedEvent`
* `RecordingStartedEvent`
* `RecordingStoppedEvent`
* `MidiDeviceListChangedEvent`

That is enough to get a real MVP UI working.

---

# 28. Final Recommended Protocol Decisions

Here’s the version I’d lock in now:

## 28.1 Protocol style

* JSON message envelopes
* versioned from day one

## 28.2 Identity

* include `messageId`
* include `windowId`
* include `documentId` where applicable
* use stable entity IDs

## 28.3 Sync model

* snapshot on open/load/resync
* incremental typed events after changes

## 28.4 Ownership of truth

* core is authoritative
* UI maintains a mirror only

## 28.5 Error handling

* structured error responses
* explicit error codes

## 28.6 MVP rendering support

* transport updates throttled
* model changes pushed as typed events
* no raw MIDI spam to UI

---
