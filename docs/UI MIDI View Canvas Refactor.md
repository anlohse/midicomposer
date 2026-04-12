# MIDI View Canvas Refactor Spec

## 1. Purpose

Refactor the MIDI score view UI from DOM-based rendering to Canvas-based rendering.

This refactor affects only the UI layer. No core model, bridge protocol, MIDI logic, playback engine, or persistence changes are required unless later discovered.

## 2. Goals

The MIDI view shall:

* render tracks, measures, notes, selections, cursors, and tool previews using Canvas
* support horizontal and vertical scrolling
* support zoom
* support multiple vertical tracks
* support edit mode and view mode
* provide coordinate conversion between screen/canvas space and musical track/measure space
* support smooth visual feedback for tools such as selection, note insertion, paste preview, and note cursor

## 3. Rendering Model

The MIDI view shall use a Canvas-based renderer instead of DOM elements for note and measure display.

Recommended structure:

* one visible canvas for final output
* one offscreen canvas for double buffering
* optional overlay canvas for transient tool feedback

The renderer should draw the full visible viewport based on current:

* scroll position
* zoom level
* active document
* active tracks
* visible measure range
* current tool state

## 4. Coordinate Spaces

The implementation shall define at least two coordinate spaces.

### 4.1 Canvas Space

Canvas coordinates represent visible screen pixels inside the canvas.

```text
C = (x, y)
```

### 4.2 Track Space

Track coordinates represent logical coordinates inside a track lane.

```text
T = (xt, yt)
```

Track space is relative to the track’s own origin.

### 4.3 Required Constants and Variables

The renderer shall use:

```text
TH = Track Height
TO = Track Origin for each track
hs = horizontal scroll offset
vs = vertical scroll offset
zoom = current zoom factor
```

The staff should be vertically centered inside each track height `TH`.

## 5. Canvas Point to Track Point

Given a canvas point:

```text
C = (x, y)
```

The renderer must determine whether the point belongs to a visible track.

A point belongs to a track if:

```text
TO.y - vs <= y < TO.y - vs + TH
```

Once the target track is found, convert the canvas point to track space:

```text
xt = (x + hs - TO.x) / zoom
yt = (y + vs - TO.y) / zoom
```

Conceptually:

```text
TrackPoint = CanvasPoint * CanvasToTrackMatrix
```

The resulting point should include:

```ts
interface TrackHitPoint {
  trackId: string;
  trackIndex: number;
  xt: number;
  yt: number;
}
```

## 6. Track Point to Canvas Point

Given a track point:

```text
T = (xt, yt)
```

Convert it back to canvas space:

```text
x = (xt * zoom) + TO.x - hs
y = (yt * zoom) + TO.y - vs
```

Conceptually:

```text
CanvasPoint = TrackPoint * TrackToCanvasMatrix
```

This conversion is required for rendering notes, measure lines, selections, previews, cursors, and playback position.

## 7. Measure and Note Positioning

Measure positions should be derived from tick values and zoom.

The renderer shall provide helpers such as:

```ts
tickToTrackX(tick: number): number
trackXToTick(x: number): number
snapTick(tick: number, noteGrid: NoteGrid): number
```

Snap rules must support common note durations:

* whole
* half
* quarter
* eighth
* sixteenth

When inserting or pasting notes, the target tick should be snapped before sending commands to the core.

## 8. Multiple Tracks

Tracks are rendered vertically.

The active tool must resolve the target track based on:

```text
trackIndex = floor((canvasY + vs) / TH)
```

Then validate the point against the actual visible track list.

Each track shall have:

* track origin
* track height
* staff vertical center
* visible measure range
* note layout data

## 9. Double Buffering

The canvas renderer shall use double buffering.

Recommended flow:

1. draw full scene to offscreen canvas
2. draw tool overlays and transient previews
3. copy offscreen canvas to visible canvas

This prevents flicker and allows smooth visual feedback.

Transient visuals include:

* selection rectangle
* paste preview
* insert-note cursor
* hover note preview
* drag preview
* playback cursor

## 10. XOR / Overlay Rendering

Selection rectangles and cursors may use XOR-like visual behavior.

Since Canvas does not provide classic native XOR drawing in the same way old graphics APIs did, the implementation may use:

* `globalCompositeOperation = "difference"`
* `globalCompositeOperation = "xor"` where useful
* or a dedicated overlay canvas cleared/redrawn every frame

Recommended approach:

* use a separate overlay canvas for transient interactions
* avoid destructive drawing over the main rendered score
* use composite operations only when they improve clarity

## 11. Interaction Modes

The MIDI view shall support two modes.

### 11.1 Edit Mode

Edit mode enables active tools.

Tools may include:

* select
* insert note
* paste
* erase
* resize note
* move note

Pointer events are converted into semantic actions and then sent to the core.

Example:

```text
canvas click → track point → snapped tick/pitch → CreateNoteCommand
```

### 11.2 View Mode

View mode is read-only.

Allowed actions:

* scroll
* zoom
* inspect
* seek playback position, if enabled
* view playback cursor

Disabled actions:

* note insertion
* note deletion
* paste
* drag editing
* resize editing

## 12. Tool Interaction Requirements

The active tool must receive normalized interaction input:

```ts
interface CanvasToolContext {
  canvasPoint: Point;
  trackHit?: TrackHitPoint;
  snappedTick?: number;
  pitch?: number;
  activeDocumentId: string;
  zoom: number;
  hs: number;
  vs: number;
}
```

Tools should not directly manipulate the document state. They should create UI previews and send bridge commands on commit.

## 13. Rendering Loop

The renderer should redraw when:

* document snapshot changes
* notes change
* tracks change
* scroll changes
* zoom changes
* active tool changes
* hover state changes
* selection changes
* playback position changes

For playback cursor animation, use `requestAnimationFrame` or throttled core playback events.

## 14. Hit Testing

Canvas hit testing must replace DOM hit testing.

The renderer shall provide hit test methods for:

* track lane
* measure
* note
* selection rectangle
* resize handles
* staff area
* empty area

Recommended API:

```ts
hitTestCanvasPoint(point: Point): HitTestResult
```

Example result:

```ts
type HitTestResult =
  | { kind: 'note'; trackId: string; noteId: string }
  | { kind: 'measure'; trackId: string; measureIndex: number; tick: number }
  | { kind: 'track'; trackId: string }
  | { kind: 'empty' };
```

## 15. Performance Requirements

The canvas renderer should render only the visible viewport plus a small buffer.

Recommended optimizations:

* calculate visible track range from `vs` and `TH`
* calculate visible measure range from `hs`, zoom, and measure width
* cache measure layout where possible
* cache note glyph paths or SVG-to-canvas assets
* avoid full document traversal on every mouse move

## 16. Acceptance Criteria

This refactor is complete when:

* the MIDI view no longer depends on DOM elements for score rendering
* tracks and measures render on Canvas
* notes render on Canvas
* horizontal and vertical scrolling work
* zoom affects rendering and hit testing correctly
* canvas-to-track and track-to-canvas transformations work correctly
* active tools resolve the correct track using `TH`
* edit mode allows tool previews and note actions
* view mode disables editing behavior
* double buffering or overlay buffering prevents flicker during interaction

## 17. Recommended Additional Items

Add these to make the refactor safer:

* debug overlay showing track bounds and coordinates
* unit tests for coordinate conversion formulas
* visual test screen with fixed tracks/measures/notes
* feature flag to switch between old DOM view and new Canvas view during migration
* logging for hit test results while developing

