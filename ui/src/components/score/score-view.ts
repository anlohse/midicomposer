import { LitElement, html, css } from 'lit';
import { customElement, property, state, query } from 'lit/decorators.js';
import { DocumentSnapshot } from '../../models/document';
import { NotationService, NotationValue, NoteFragment, MeasureLayout,
         RestFragment } from '../../services/notationService';
import { CoreBridge } from '../../bridge/coreBridge';
import { pitchName } from '../../models/pitch';
import { ClefDef, clefDef, middleLineStep, STEP_PX } from '../../models/clef';
import { ACCIDENTAL_TEXT, AccidentalGlyph, accidentalFor, cancellationCount,
         cancellationSteps, keyAt, signatureSteps, spellPitch,
         stepToKeyPitch } from '../../models/keySignature';
import { DENOMINATORS } from '../../models/timeSignature';
import { maxScroll, measureWindow, needsReveal, scrollStops, scrollThumb,
         snapToStop, stepStop, thumbDragToScroll } from '../../models/scoreScroll';
import type { ScoreTool, NoteDuration, ScoreMode, ScoreGrid } from './score-toolbar';

// ─── Layout constants ─────────────────────────────────────────────────────────

const TH               = 100;   // track height px
const STAFF_LABEL_W    = 100;   // fixed label column width
const PX_PER_TICK_BASE = 0.2;
const STEM_LEN         = 30;    // notehead-centre → stem tip
const BEAM_H           = 4;     // beam bar thickness
const BEAM_GAP         = 3;     // gap between beam bars (16th)

// ─── Measure geometry ────────────────────────────────────────────────────────
//
// A measure column is laid out as
//
//   │<lead>[clef][time-sig]<padL>········note area········<padR>│
//   ^ barline                                            barline ^
//
// The lead-in and the two pads are fixed pixel amounts (zoom-independent).
// They keep noteheads, accidentals and augmentation dots clear of the
// barlines, which is what makes measure boundaries readable at a glance.

const MEASURE_LEAD_IN = 10;   // barline → first header symbol / measure number
const MEASURE_PAD_L   = 20;   // header → first notehead (accidentals live here)
const MEASURE_PAD_R   = 22;   // end of the note area → next barline
// One width for every clef: a wider glyph must not shift its staff's measures
// out of alignment with the others.
const CLEF_W          = 36;
const KEY_ACC_W       = 9;    // per accidental in the key signature
const TIME_SIG_W      = 26;   // two stacked numerals
const MIN_CONTENT_W   = 110;  // floor on the note area so glyphs never collide

// A notehead occupies x … x+12; the augmentation dot reaches x+18.
const NOTE_GLYPH_W = 18;

// ─── Staff geometry ──────────────────────────────────────────────────────────
//
// The staff itself is fixed; only which pitch lands on which line depends on the
// track's clef, and that collapses to ClefDef.bottomLineStep. See models/clef.ts
// for the pitch ↔ step mapping.

const BOTTOM_LINE_Y = 80;   // y offset of the bottom staff line inside a lane
const TOP_LINE_Y    = 40;   // y offset of the top staff line inside a lane


// ─── Types ───────────────────────────────────────────────────────────────────

// Selection keys are always String(noteId): snapshot ids arrive as numbers at
// runtime even though the TS types say string.
function noteKey(id: unknown): string { return String(id); }

interface ResizeDrag {
    kind: 'resize';
    noteId: string;
    trackId: string;
    originTick: number;
    originDuration: number;
    curDuration: number;
}

// Group move of the whole selection; commits as one atomic batch_edit.
interface MoveDrag {
    kind: 'move';
    startCx: number;
    startCy: number;
    pointerStartTick: number;
    pointerStartPitch: number;
    deltaTick: number;      // snapped to the active grid
    deltaPitch: number;     // semitones, preserves intervals
    started: boolean;       // becomes true past the drag threshold
    pressedNoteId: string;
    trackIdx: number;       // lane the drag began in; fixes the clef in use
}

interface MarqueeDrag {
    kind: 'marquee';
    x0: number; y0: number;
    x1: number; y1: number;
    additive: boolean;      // ctrl/cmd held: add to selection
    moved: boolean;
}

type DragState = ResizeDrag | MoveDrag | MarqueeDrag;

const DRAG_THRESHOLD_PX = 4;

interface ClipboardEntry {
    relTick: number;
    trackOffset: number;
    pitch: number;
    durationTicks: number;
    velocity: number;
}

// Module-level so the clipboard survives document switches.
let noteClipboard: ClipboardEntry[] = [];

export type EditAction = 'undo' | 'redo' | 'cut' | 'copy' | 'paste' | 'delete' | 'selectAll';

interface NoteInfo {
    hollow: boolean;
    hasStem: boolean;
    flags: number;    // 0 = quarter+, 1 = eighth, 2 = sixteenth
    dotted: boolean;
}

// MeasureLayout augmented with pixel coordinates computed for the current
// zoom / scroll state.  noteAreaX is where actual note glyphs begin (after
// the clef / time-sig header and the left pad).
//
// pxPerTick is per-measure rather than global because a measure's note area is
// never allowed below MIN_CONTENT_W — at low zoom short measures get stretched
// and everything (rendering, hit testing, the playhead) must agree on the same
// scale, so it travels with the layout.
interface MeasureVisualLayout extends MeasureLayout {
    visualStartX: number;   // left barline of the measure column
    headerWidth: number;    // lead-in + clef + key-sig + time-sig + left pad
    noteAreaX: number;      // x of tick === startTick
    contentWidth: number;   // px width of the note area
    pxPerTick: number;      // contentWidth / durationTicks
    barlineX: number;       // right barline of the measure column
    showClef: boolean;
    showTimeSig: boolean;
    showKeySig: boolean;
    keySigWidth: number;
    /** Outgoing key, so the header can draw cancellation naturals; null at the start. */
    prevKeyFifths: number | null;
    /** x of the time-signature numerals; also its click target. */
    timeSigX: number;
}

// Insert-tool hover target, recomputed on every pointer move.
interface HoverTarget {
    trackIdx: number;
    tick: number;      // already snapped to the active grid
    pitch: number;
}

// How far the score scrolls, and how the scrollbar maps onto it, is worked out
// in models/scoreScroll.ts.
const HSCROLL_H  = 12;   // horizontal scrollbar strip height
const HTHUMB_MIN = 30;   // keep the thumb grabbable on a long composition
// Wheel delta that counts as one measure.  A mouse notch reports ~100; a
// trackpad sends a stream of small deltas, which accumulate to the same thing.
const WHEEL_NOTCH = 100;

// ─── Component ───────────────────────────────────────────────────────────────

@customElement('mc-score-view')
export class ScoreView extends LitElement {
    @property({ type: Object }) doc?: DocumentSnapshot;
    @property() tool: ScoreTool = 'select';
    @property() duration: NoteDuration = 'quarter';
    @property() mode: ScoreMode = 'edit';
    @property() grid: ScoreGrid = 'auto';

    @state() private currentTick = 0;
    @state() private selection = new Set<string>();
    // Paste target: set by clicking empty staff area with the select tool.
    private anchorTick: number | null = null;
    private anchorTrackIdx = 0;
    @state() private hs = 0;
    @state() private vs = 0;
    @state() private zoom = 1.0;

    @query('.canvas-container') private canvasContainer?: HTMLElement;
    @query('#main-canvas') private mainCanvas?: HTMLCanvasElement;
    @query('#overlay-canvas') private overlayCanvas?: HTMLCanvasElement;
    @query('#track-name-editor') private trackNameEditor?: HTMLInputElement;

    // Track label being renamed in place, if any.
    @state() private editingTrackIdx: number | null = null;
    private trackEditorNeedsFocus = false;

    private resizeObserver?: ResizeObserver;
    private animationFrameId?: number;
    private drag: DragState | null = null;

    // Measure layouts only depend on the document revision, not on scroll or
    // zoom — cache them so the render loop doesn't recompute fragments and
    // rests for every track on every animation frame.
    private measureCache = new Map<string, MeasureLayout[]>();

    private positionHandler = (payload: any) => {
        this.currentTick = payload?.tick ?? 0;
        // Keep the cursor on screen while the transport runs.  A drag in
        // progress owns the view, so leave the scroll where the user put it.
        if (!this.drag) this.followPlayhead();
    };

    static styles = css`
        :host {
            display: flex; flex-direction: column;
            width: 100%; height: 100%;
            position: relative;
            background: #1e1e1e; overflow: hidden;
        }
        /* min-height:0 so the flex item may shrink below its canvas content
           and actually leave room for the scrollbar strip. */
        .canvas-container { position: relative; flex: 1; min-height: 0; width: 100%; }
        .canvas-container.tool-select { cursor: default; }
        .canvas-container.tool-insert { cursor: crosshair; }
        .canvas-container.tool-resize { cursor: ew-resize; }
        .canvas-container.tool-erase  { cursor: not-allowed; }
        .canvas-container.mode-play   { cursor: pointer; }
        canvas { position: absolute; top: 0; left: 0; width: 100%; height: 100%; }
        /* Inline track rename, overlaid on the canvas label column. */
        .track-name-editor {
            position: absolute;
            z-index: 5;
            background: #2d2d2d;
            border: 1px solid #007acc;
            border-radius: 2px;
            color: #fff;
            font: bold 0.9rem sans-serif;
            padding: 2px 4px;
            box-sizing: border-box;
        }
        .track-name-editor:focus { outline: none; }
        /* Inline meter editor, anchored on the time signature it edits. */
        .meter-editor {
            position: absolute;
            z-index: 6;
            display: flex;
            align-items: center;
            gap: 3px;
            background: #2d2d2d;
            border: 1px solid #007acc;
            border-radius: 3px;
            padding: 2px 4px;
            box-shadow: 0 2px 8px rgba(0, 0, 0, 0.5);
        }
        .meter-editor .bar-label { color: #8a8a8a; font-size: 0.66rem; margin-right: 2px; }
        .meter-editor .slash { color: #777; }
        .meter-editor input, .meter-editor select {
            background: #1e1e1e;
            border: 1px solid #444;
            color: #eee;
            font-size: 0.78rem;
            padding: 1px 2px;
        }
        .meter-editor input { width: 32px; text-align: right; }
        .meter-editor input:focus, .meter-editor select:focus {
            outline: none; border-color: #007acc;
        }
        /* Horizontal scrollbar.  Custom rather than native overflow: the score
           is drawn on absolutely positioned canvases, so there is no laid-out
           content for a native scroller to measure.  It is a flex sibling rather
           than an overlay so it takes its height out of the canvas area instead
           of covering the bottom of a staff. */
        .hscroll {
            position: relative;
            flex: none;
            background: #191919;
            border-top: 1px solid #2b2b2b;
        }
        .hthumb {
            position: absolute;
            top: 2px; bottom: 2px;
            background: #4a4a4a;
            border-radius: 5px;
            cursor: grab;
        }
        .hthumb:hover { background: #5c5c5c; }
        .hthumb:active { cursor: grabbing; background: #007acc; }
        /* Whole score fits: leave the strip visible so the layout never jumps,
           but say plainly that there is nothing to drag. */
        .hthumb.idle { background: #2b2b2b; cursor: default; }
    `;

    // ─── Lifecycle ────────────────────────────────────────────────────────────

    private keyHandler = (e: KeyboardEvent) => this.handleKeyDown(e);

    async firstUpdated() {
        CoreBridge.on('playback_position', this.positionHandler);
        window.addEventListener('keydown', this.keyHandler);
        this.resizeObserver = new ResizeObserver(() => this.handleResize());
        this.resizeObserver.observe(this);
        this.startRendering();
    }

    disconnectedCallback() {
        super.disconnectedCallback();
        CoreBridge.off('playback_position', this.positionHandler);
        window.removeEventListener('keydown', this.keyHandler);
        this.resizeObserver?.disconnect();
        if (this.animationFrameId) cancelAnimationFrame(this.animationFrameId);
    }

    // ─── Keyboard shortcuts ───────────────────────────────────────────────────

    private handleKeyDown(e: KeyboardEvent) {
        // Don't steal keys from form fields (e.g. the BPM input).
        const target = e.composedPath()[0] as HTMLElement | undefined;
        const tag = target?.tagName;
        if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;

        if (e.key === 'Escape') {
            if (this.drag) this.drag = null;           // cancel drag without committing
            else this.selection = new Set();
            return;
        }

        if (e.code === 'Space') {
            e.preventDefault();
            this.togglePlayback();
            return;
        }

        if (e.key === 'Home' || e.key === 'End') {
            e.preventDefault();
            this.setHs(e.key === 'Home' ? 0 : this.maxHs);
            return;
        }
        if (e.key === 'PageUp' || e.key === 'PageDown') {
            e.preventDefault();
            const page = this.measuresPerPage;
            this.scrollByMeasures(e.key === 'PageUp' ? -page : page);
            return;
        }
        if (e.key === 'ArrowLeft' || e.key === 'ArrowRight') {
            // Only when nothing is selected; otherwise the arrows belong to the
            // selection and stealing them would move notes instead.
            if (this.selection.size > 0) return;
            e.preventDefault();
            this.scrollByMeasures(e.key === 'ArrowLeft' ? -1 : 1);
            return;
        }

        if (this.mode !== 'edit') return;

        if ((e.key === 'Delete' || e.key === 'Backspace') && this.selection.size > 0) {
            e.preventDefault();
            this.deleteSelection();
            return;
        }

        const ctrl = e.ctrlKey || e.metaKey;
        if (!ctrl) return;
        const k = e.key.toLowerCase();
        if (k === 'z' && !e.shiftKey) {
            e.preventDefault();
            this.sendHistoryCommand('undo');
        } else if (k === 'y' || (k === 'z' && e.shiftKey)) {
            e.preventDefault();
            this.sendHistoryCommand('redo');
        } else if (k === 'a') {
            e.preventDefault();
            this.selectAll();
        } else if (k === 'c') {
            e.preventDefault();
            this.copySelection();
        } else if (k === 'x') {
            e.preventDefault();
            if (this.copySelection()) this.deleteSelection();
        } else if (k === 'v') {
            e.preventDefault();
            this.pasteClipboard();
        }
    }

    // ─── Menu-facing API ──────────────────────────────────────────────────────

    // The same actions the keyboard shortcuts run, exposed so the Edit menu can
    // reach them. Keeping one implementation means the menu and the shortcuts can
    // never drift apart.
    runEditAction(action: EditAction) {
        if (this.mode !== 'edit') return;
        switch (action) {
            case 'undo':      this.sendHistoryCommand('undo'); break;
            case 'redo':      this.sendHistoryCommand('redo'); break;
            case 'copy':      this.copySelection(); break;
            case 'cut':       if (this.copySelection()) this.deleteSelection(); break;
            case 'paste':     this.pasteClipboard(); break;
            case 'delete':    this.deleteSelection(); break;
            case 'selectAll': this.selectAll(); break;
        }
    }

    get hasSelection(): boolean { return this.selection.size > 0; }
    get canPaste(): boolean { return noteClipboard.length > 0; }

    // ─── Selection / clipboard operations ─────────────────────────────────────

    // Every selected note with its owning track, resolved from the snapshot.
    private selectedNotes(): Array<{ note: any; track: any; trackIdx: number }> {
        if (!this.doc) return [];
        const result: Array<{ note: any; track: any; trackIdx: number }> = [];
        this.doc.tracks.forEach((track, trackIdx) => {
            for (const note of track.notes) {
                if (this.selection.has(noteKey(note.id))) result.push({ note, track, trackIdx });
            }
        });
        return result;
    }

    private async sendBatch(operations: any[]): Promise<number[]> {
        if (!this.doc || operations.length === 0) return [];
        try {
            const res = await CoreBridge.sendCommand<{ createdNoteIds: number[] }>('batch_edit', {
                documentId: this.doc.id, operations,
            });
            return res.createdNoteIds ?? [];
        } catch (err) {
            console.error('batch_edit failed', err);
            return [];
        }
    }

    private selectAll() {
        if (!this.doc) return;
        const s = new Set<string>();
        for (const track of this.doc.tracks) {
            for (const note of track.notes) s.add(noteKey(note.id));
        }
        this.selection = s;
    }

    private async deleteSelection() {
        const ops = this.selectedNotes().map(({ note, track }) => ({
            type: 'DeleteNote', trackId: parseInt(track.id), noteId: parseInt(note.id),
        }));
        this.selection = new Set();
        await this.sendBatch(ops);
    }

    // Returns true if something was copied.
    private copySelection(): boolean {
        const sel = this.selectedNotes();
        if (sel.length === 0) return false;
        const minTick = Math.min(...sel.map(s => s.note.startTick));
        const minTrack = Math.min(...sel.map(s => s.trackIdx));
        noteClipboard = sel.map(s => ({
            relTick: s.note.startTick - minTick,
            trackOffset: s.trackIdx - minTrack,
            pitch: s.note.pitch,
            durationTicks: s.note.durationTicks,
            velocity: s.note.velocity,
        }));
        return true;
    }

    private async pasteClipboard() {
        if (!this.doc || noteClipboard.length === 0) return;
        const baseTick = this.anchorTick ?? this.currentTick;
        const trackCount = this.doc.tracks.length;
        const ops = noteClipboard.map(c => {
            const trackIdx = Math.max(0, Math.min(trackCount - 1, this.anchorTrackIdx + c.trackOffset));
            return {
                type: 'CreateNote',
                trackId: parseInt(this.doc!.tracks[trackIdx].id),
                startTick: Math.max(0, baseTick + c.relTick),
                durationTicks: c.durationTicks,
                pitch: c.pitch,
                velocity: c.velocity,
            };
        });
        const created = await this.sendBatch(ops);
        this.selection = new Set(created.map(noteKey));
    }

    private async sendHistoryCommand(command: 'undo' | 'redo') {
        if (!this.doc) return;
        try {
            await CoreBridge.sendCommand(command, { documentId: this.doc.id });
        } catch { /* nothing to undo/redo */ }
    }

    private async togglePlayback() {
        if (!this.doc) return;
        try {
            const st = await CoreBridge.sendCommand<{state: string}>('get_transport_state');
            if (st.state === 'playing' || st.state === 'recording') {
                await CoreBridge.sendCommand('stop');
            } else {
                await CoreBridge.sendCommand('play', { documentId: this.doc.id });
            }
        } catch (err) { console.error('transport toggle failed', err); }
    }

    // ─── Measure-aware coordinate system ─────────────────────────────────────
    //
    // Visual x-position of a note is NOT simply tick * pxPerTick, because each
    // measure may have a fixed-width header (clef, time sig) that shifts the
    // note area right.  All rendering and hit-testing go through noteX().

    private get basePxPerTick() { return PX_PER_TICK_BASE * this.zoom; }

    private getMeasuresCached(trackIdx: number): MeasureLayout[] {
        const doc = this.doc;
        if (!doc) return [];
        const key = `${doc.id}:${doc.revision}:${trackIdx}`;
        let measures = this.measureCache.get(key);
        if (!measures) {
            if (this.measureCache.size > 64) this.measureCache.clear();
            measures = NotationService.getMeasures(doc, trackIdx, measureWindow(doc));
            this.measureCache.set(key, measures);
        }
        return measures;
    }

    // Lay measure columns out left to right.  Each column is
    // header + note area + right pad, so the accumulated lead-ins and pads push
    // every following measure further right than a plain tick × pxPerTick
    // mapping would — which is exactly why nothing may compute an x from ticks
    // alone.  Go through noteX() / tickToCanvasXAware() instead.
    private computeVisualLayouts(measures: MeasureLayout[]): MeasureVisualLayout[] {
        const result: MeasureVisualLayout[] = [];
        let x = STAFF_LABEL_W - this.hs;
        for (let i = 0; i < measures.length; i++) {
            const m    = measures[i];
            const prev = i > 0 ? measures[i - 1] : null;

            const showClef    = i === 0;
            const showTimeSig = i === 0 || !!(prev &&
                (prev.timeSignature.numerator   !== m.timeSignature.numerator ||
                 prev.timeSignature.denominator !== m.timeSignature.denominator));
            // A key change also needs naturals cancelling whatever the outgoing
            // signature had, so a change to C major is visible rather than blank.
            const keyChanged = !!(prev && prev.keySignature.fifths !== m.keySignature.fifths);
            const prevKeyFifths = keyChanged ? prev!.keySignature.fifths : null;
            const accidentals = Math.min(Math.abs(m.keySignature.fifths), 7);
            const cancels = prevKeyFifths === null
                ? 0
                : cancellationCount(prevKeyFifths, m.keySignature.fifths);
            const showKeySig  = (i === 0 && accidentals > 0) ||
                                (keyChanged && (accidentals > 0 || cancels > 0));
            const keySigWidth = showKeySig ? (cancels + accidentals) * KEY_ACC_W + 6 : 0;

            const headerWidth = MEASURE_LEAD_IN
                + (showClef    ? CLEF_W     : 0)
                + keySigWidth
                + (showTimeSig ? TIME_SIG_W : 0)
                + MEASURE_PAD_L;

            const timeSigX = x + MEASURE_LEAD_IN + (showClef ? CLEF_W : 0) + keySigWidth;
            const noteAreaX    = x + headerWidth;
            const contentWidth = Math.max(MIN_CONTENT_W, m.durationTicks * this.basePxPerTick);
            const barlineX     = noteAreaX + contentWidth + MEASURE_PAD_R;

            result.push({
                ...m,
                visualStartX: x, headerWidth, noteAreaX, contentWidth, barlineX,
                pxPerTick: contentWidth / Math.max(1, m.durationTicks),
                showClef, showTimeSig, showKeySig, keySigWidth, timeSigX, prevKeyFifths,
            });
            x = barlineX;
        }
        return result;
    }

    // Visual layouts depend on the document revision plus zoom and scroll.  The
    // render loop asks for them once per track per frame and hit testing asks
    // again on every pointer move, so memoise per (revision, zoom, hs).
    private visualCache = new Map<number, MeasureVisualLayout[]>();
    private visualCacheKey = '';

    private visualsFor(trackIdx: number): MeasureVisualLayout[] {
        const doc = this.doc;
        if (!doc) return [];
        const key = `${doc.id}:${doc.revision}:${this.zoom}:${this.hs}`;
        if (key !== this.visualCacheKey) {
            this.visualCache.clear();
            this.visualCacheKey = key;
        }
        let visuals = this.visualCache.get(trackIdx);
        if (!visuals) {
            visuals = this.computeVisualLayouts(this.getMeasuresCached(trackIdx));
            this.visualCache.set(trackIdx, visuals);
        }
        return visuals;
    }

    // Pixel X for a given tick inside a specific visual measure.
    private noteX(tick: number, vm: MeasureVisualLayout): number {
        return vm.noteAreaX + (tick - vm.startTick) * vm.pxPerTick;
    }

    // Convert a canvas X to a musical tick.  A click inside a measure's header
    // or either pad clamps to the nearest end of that measure's note area, so
    // the dead space around barlines never resolves to a foreign measure.
    private canvasXToTickAware(cx: number): number {
        const visuals = this.visualsFor(0);
        if (visuals.length === 0) {
            return Math.max(0, (cx - STAFF_LABEL_W + this.hs) / this.basePxPerTick);
        }
        for (const vm of visuals) {
            if (cx <= vm.barlineX) {
                const local = (cx - vm.noteAreaX) / vm.pxPerTick;
                return vm.startTick + Math.max(0, Math.min(vm.durationTicks, local));
            }
        }
        // Past all precomputed measures: extrapolate at the last measure's scale.
        const last = visuals[visuals.length - 1];
        return last.startTick + last.durationTicks +
               Math.max(0, (cx - last.barlineX) / last.pxPerTick);
    }

    // Pixel X for a tick, respecting measure headers and pads (playback cursor,
    // drag preview).  Falls back to the linear mapping when there is no layout.
    private tickToCanvasXAware(tick: number): number {
        const visuals = this.visualsFor(0);
        if (visuals.length === 0) return STAFF_LABEL_W + tick * this.basePxPerTick - this.hs;
        for (const vm of visuals) {
            if (tick < vm.startTick + vm.durationTicks) {
                return this.noteX(Math.max(tick, vm.startTick), vm);
            }
        }
        return this.noteX(tick, visuals[visuals.length - 1]);
    }

    // ─── Duration helpers ─────────────────────────────────────────────────────

    private get ticksForDuration(): number {
        const ppqn = this.doc?.ppqn ?? 480;
        switch (this.duration) {
            case 'quarter':   return ppqn;
            case 'eighth':    return Math.floor(ppqn / 2);
            case 'sixteenth': return Math.floor(ppqn / 4);
        }
    }

    // Active snap resolution in ticks; 0 means snapping is off.
    // 'auto' follows the selected note duration, which keeps the common case
    // (lay down a run of same-length notes) aligned without any extra setup.
    private get gridTicks(): number {
        const ppqn = this.doc?.ppqn ?? 480;
        switch (this.grid) {
            case 'off':  return 0;
            case '1/4':  return ppqn;
            case '1/8':  return Math.floor(ppqn / 2);
            case '1/16': return Math.floor(ppqn / 4);
            case '1/32': return Math.floor(ppqn / 8);
            default:     return this.ticksForDuration;
        }
    }

    // Snap to the *nearest* grid line, not the one before it: flooring meant a
    // click a hair past a beat still landed on the previous one, which reads as
    // notes being yanked backwards onto each other.
    private snapTick(tick: number): number {
        const g = this.gridTicks;
        if (g <= 0) return Math.max(0, Math.round(tick));
        return Math.max(0, Math.round(tick / g) * g);
    }

    // Grid step for drag deltas and resize quantisation.  With snapping off,
    // fall back to a 32nd so drags stay on sane tick values.
    private get dragGridTicks(): number {
        const g = this.gridTicks;
        return g > 0 ? g : Math.max(1, Math.floor((this.doc?.ppqn ?? 480) / 8));
    }

    // Glyph shape for a resolved notation value. Committed notes and rests always
    // come through here, so what is drawn matches what the notation service
    // decided rather than being re-derived from ticks.
    private noteInfoFor(value: NotationValue, dotted: boolean): NoteInfo {
        switch (value) {
            case 'whole':     return { hollow: true,  hasStem: false, flags: 0, dotted };
            case 'half':      return { hollow: true,  hasStem: true,  flags: 0, dotted };
            case 'quarter':   return { hollow: false, hasStem: true,  flags: 0, dotted };
            case 'eighth':    return { hollow: false, hasStem: true,  flags: 1, dotted };
            case 'sixteenth': return { hollow: false, hasStem: true,  flags: 2, dotted };
        }
    }

    // Tick-based fallback, used only for live drag previews where no fragment has
    // been re-notated yet. Picks the *nearest* value: rounding down would show a
    // note being resized to just under a quarter as a dotted eighth.
    private getNoteInfo(durationTicks: number): NoteInfo {
        const ppqn = this.doc?.ppqn ?? 480;
        const candidates: Array<[number, NotationValue, boolean]> = [
            [4, 'whole', false], [3, 'half', true], [2, 'half', false],
            [1.5, 'quarter', true], [1, 'quarter', false],
            [0.75, 'eighth', true], [0.5, 'eighth', false],
            [0.375, 'sixteenth', true], [0.25, 'sixteenth', false],
        ];
        let best: [number, NotationValue, boolean] = candidates[candidates.length - 1];
        let bestDiff = Infinity;
        for (const c of candidates) {
            const diff = Math.abs(c[0] * ppqn - durationTicks);
            if (diff < bestDiff) { bestDiff = diff; best = c; }
        }
        return this.noteInfoFor(best[1], best[2]);
    }

    // ─── Staff geometry ───────────────────────────────────────────────────────

    // The clef a track's staff is drawn in.
    private clefFor(trackIdx: number): ClefDef {
        return clefDef(this.doc?.tracks[trackIdx]?.clef);
    }

    // Position comes from the notation step, never from the pitch: F#4 and Gb4
    // are the same pitch on different lines.
    private stepY(step: number, yBase: number, clef: ClefDef): number {
        return yBase + BOTTOM_LINE_Y - (step - clef.bottomLineStep) * STEP_PX;
    }

    // ─── Beam grouping ────────────────────────────────────────────────────────

    private buildBeamGroups(sorted: NoteFragment[], vm: MeasureVisualLayout): Array<NoteFragment | NoteFragment[]> {
        const ppqn      = this.doc?.ppqn ?? 480;
        const beatTicks = Math.max(1, Math.floor(ppqn * 4 / vm.timeSignature.denominator));
        const singles: NoteFragment[]            = [];
        const byBeat   = new Map<number, NoteFragment[]>();

        for (const f of sorted) {
            const isDragged =
                (this.drag?.kind === 'move' && this.drag.started && this.selection.has(noteKey(f.noteId))) ||
                (this.drag?.kind === 'resize' && this.drag.noteId === f.noteId);
            if (isDragged || this.noteInfoFor(f.noteValue, f.dotted).flags === 0) {
                singles.push(f);
                continue;
            }
            const beat = Math.floor(f.startTick / beatTicks);
            if (!byBeat.has(beat)) byBeat.set(beat, []);
            byBeat.get(beat)!.push(f);
        }

        const result: Array<NoteFragment | NoteFragment[]> = [...singles];
        for (const [, group] of byBeat) {
            result.push(group.length >= 2 ? group : group[0]);
        }
        result.sort((a, b) => {
            const ta = Array.isArray(a) ? a[0].startTick : a.startTick;
            const tb = Array.isArray(b) ? b[0].startTick : b.startTick;
            return ta - tb;
        });
        return result;
    }

    // ─── Hit testing ──────────────────────────────────────────────────────────

    private trackIndexAt(cy: number) { return Math.floor((cy + this.vs) / TH); }

    private stepAtCanvasY(cy: number, trackIdx: number): number {
        const yt = (cy + this.vs) % TH;
        return this.clefFor(trackIdx).bottomLineStep + Math.round((BOTTOM_LINE_Y - yt) / STEP_PX);
    }

    // Inverse of stepY: which pitch the pointer is over. The key signature
    // decides it, so clicking the F line in G major inserts F#, not F.
    private canvasYtoPitch(cy: number, trackIdx: number, tick = 0): number {
        const key = keyAt(this.doc?.keySignatureMap, tick);
        return stepToKeyPitch(this.stepAtCanvasY(cy, trackIdx), key.fifths);
    }

    private hitTestNote(cx: number, cy: number): { fragment: NoteFragment; trackId: string } | null {
        if (!this.doc) return null;
        const trackIdx = this.trackIndexAt(cy);
        if (trackIdx < 0 || trackIdx >= this.doc.tracks.length) return null;

        const visualMeasures = this.visualsFor(trackIdx);
        const yBase = trackIdx * TH - this.vs;
        const clef  = this.clefFor(trackIdx);

        // Adjacent diatonic steps are only 5px apart, so hit boxes overlap and
        // "first match wins" reliably grabbed the wrong note of a cluster.
        // Collect every candidate and keep the closest, weighting the vertical
        // axis because that is the crowded one.
        let best: NoteFragment | null = null;
        let bestScore = Infinity;

        // Only called on mousedown, when no drag is in progress — fragments
        // are always at their committed positions here.
        for (const vm of visualMeasures) {
            for (const f of vm.fragments) {
                const dx = Math.abs(cx - (this.noteX(f.startTick, vm) + 6));
                const dy = Math.abs(cy - this.stepY(f.step, yBase, clef));
                if (dx > 10 || dy > 6) continue;
                const score = dy * 4 + dx;
                if (score < bestScore) { bestScore = score; best = f; }
            }
        }
        return best ? { fragment: best, trackId: this.doc.tracks[trackIdx].id } : null;
    }

    // ─── Core commands ────────────────────────────────────────────────────────

    private async createNote(trackId: string, tick: number, pitch: number) {
        if (!this.doc) return;
        try {
            await CoreBridge.sendCommand('create_note', {
                documentId: this.doc.id, trackId: parseInt(trackId),
                tick, pitch, velocity: 100, duration: this.ticksForDuration,
            });
        } catch (err) { console.error('create_note failed', err); }
    }

    private async deleteNote(trackId: string, noteId: string) {
        if (!this.doc) return;
        try {
            await CoreBridge.sendCommand('delete_note', {
                documentId: this.doc.id, trackId: parseInt(trackId), noteId: parseInt(noteId),
            });
            if (this.selection.has(noteKey(noteId))) {
                const s = new Set(this.selection);
                s.delete(noteKey(noteId));
                this.selection = s;
            }
        } catch (err) { console.error('delete_note failed', err); }
    }

    private async commitDrag() {
        if (!this.drag || !this.doc) return;
        const d = this.drag;
        this.drag = null;

        if (d.kind === 'resize') {
            if (d.curDuration !== d.originDuration) {
                try {
                    await CoreBridge.sendCommand('resize_note', {
                        documentId: this.doc.id, trackId: parseInt(d.trackId),
                        noteId: parseInt(d.noteId), duration: d.curDuration,
                    });
                } catch (err) { console.error('resize commit failed', err); }
            }
            return;
        }

        if (d.kind === 'move' && d.started && (d.deltaTick !== 0 || d.deltaPitch !== 0)) {
            // The whole selection moves as one atomic batch → one undo entry.
            const ops: any[] = [];
            for (const { note, track } of this.selectedNotes()) {
                if (d.deltaTick !== 0) {
                    ops.push({
                        type: 'MoveNote', trackId: parseInt(track.id), noteId: parseInt(note.id),
                        newStartTick: Math.max(0, note.startTick + d.deltaTick),
                    });
                }
                if (d.deltaPitch !== 0) {
                    ops.push({
                        type: 'UpdateNote', trackId: parseInt(track.id), noteId: parseInt(note.id),
                        pitch: Math.max(0, Math.min(127, note.pitch + d.deltaPitch)),
                    });
                }
            }
            await this.sendBatch(ops);
        }
    }

    // ─── Mouse events ─────────────────────────────────────────────────────────

    // The underlying note for a fragment hit: fragments of tied notes carry
    // measure-local start/duration, but edits operate on the real note.
    private findNote(trackIdx: number, noteId: string) {
        return this.doc?.tracks[trackIdx]?.notes.find(n => n.id === noteId);
    }

    private async seekTo(tick: number) {
        this.currentTick = Math.max(0, Math.round(tick));
        try {
            await CoreBridge.sendCommand('seek', { tick: this.currentTick });
        } catch (err) { console.error('seek failed', err); }
    }

    private handleMouseDown(e: MouseEvent) {
        if (!this.doc || e.button !== 0) return;
        const rect = this.viewportRect();
        const cx = e.clientX - rect.left;
        const cy = e.clientY - rect.top;
        if (cx < STAFF_LABEL_W) return;

        // Play mode: the score is read-only and clicking seeks the transport.
        if (this.mode === 'play') {
            this.seekTo(this.canvasXToTickAware(cx));
            return;
        }

        // The time signature is editable in place; it must be checked before the
        // tool logic, which would otherwise start a marquee over the header.
        if (this.hitTestTimeSignature(cx, cy)) return;

        const trackIdx = this.trackIndexAt(cy);
        if (trackIdx < 0 || trackIdx >= this.doc.tracks.length) return;

        if (this.tool === 'insert') {
            const tick  = this.snapTick(this.canvasXToTickAware(cx));
            const pitch = this.canvasYtoPitch(cy, trackIdx, tick);
            this.createNote(this.doc.tracks[trackIdx].id, tick, pitch);
            return;
        }

        const hit = this.hitTestNote(cx, cy);

        if (this.tool === 'erase') {
            if (hit) this.deleteNote(hit.trackId, hit.fragment.noteId);
            return;
        }

        if (this.tool === 'resize') {
            if (!hit) return;
            const note = this.findNote(trackIdx, hit.fragment.noteId);
            if (!note) return;
            this.selection = new Set([noteKey(note.id)]);
            this.drag = {
                kind: 'resize',
                noteId: hit.fragment.noteId, trackId: hit.trackId,
                originTick: note.startTick,
                originDuration: note.durationTicks, curDuration: note.durationTicks,
            };
            return;
        }

        // Select tool
        const additive = e.ctrlKey || e.metaKey;
        if (hit) {
            const k = noteKey(hit.fragment.noteId);
            if (additive) {
                const s = new Set(this.selection);
                s.has(k) ? s.delete(k) : s.add(k);
                this.selection = s;
                return;
            }
            if (!this.selection.has(k)) this.selection = new Set([k]);
            this.drag = {
                kind: 'move',
                startCx: cx, startCy: cy,
                pointerStartTick: this.canvasXToTickAware(cx),
                pointerStartPitch: this.canvasYtoPitch(cy, trackIdx),
                trackIdx,
                deltaTick: 0, deltaPitch: 0, started: false,
                pressedNoteId: k,
            };
        } else {
            this.drag = { kind: 'marquee', x0: cx, y0: cy, x1: cx, y1: cy, additive, moved: false };
        }
    }

    private handleMouseMove(e: MouseEvent) {
        const rect = this.viewportRect();
        const cx = e.clientX - rect.left;
        const cy = e.clientY - rect.top;

        this.updateHover(cx, cy);
        if (!this.drag) return;

        if (this.drag.kind === 'resize') {
            const g = this.dragGridTicks;
            const raw = this.canvasXToTickAware(cx) - this.drag.originTick;
            const newDuration = Math.max(g, Math.round(raw / g) * g);
            if (newDuration !== this.drag.curDuration) {
                this.drag = { ...this.drag, curDuration: newDuration };
            }
            return;
        }

        if (this.drag.kind === 'move') {
            const dist = Math.hypot(cx - this.drag.startCx, cy - this.drag.startCy);
            const started = this.drag.started || dist > DRAG_THRESHOLD_PX;
            const g = this.dragGridTicks;
            const rawDelta = this.canvasXToTickAware(cx) - this.drag.pointerStartTick;
            const deltaTick = Math.round(rawDelta / g) * g;
            // Read the pitch in the clef of the lane the drag started in, so
            // dragging over a staff with a different clef cannot skew the delta.
            const deltaPitch = this.canvasYtoPitch(cy, this.drag.trackIdx) - this.drag.pointerStartPitch;
            if (started !== this.drag.started || deltaTick !== this.drag.deltaTick || deltaPitch !== this.drag.deltaPitch) {
                this.drag = { ...this.drag, started, deltaTick, deltaPitch };
            }
            return;
        }

        // Marquee
        const moved = this.drag.moved || Math.hypot(cx - this.drag.x0, cy - this.drag.y0) > 3;
        this.drag = { ...this.drag, x1: cx, y1: cy, moved };
    }

    private handleMouseUp(e: MouseEvent) {
        const d = this.drag;
        if (!d) return;

        if (d.kind === 'resize' || (d.kind === 'move' && d.started)) {
            this.commitDrag();
            return;
        }

        if (d.kind === 'move') {
            // Plain click on a note inside a multi-selection collapses to it.
            this.selection = new Set([d.pressedNoteId]);
            this.drag = null;
            return;
        }

        // Marquee finished
        this.drag = null;
        if (d.moved) {
            this.applyMarqueeSelection(d);
        } else {
            // Plain click on empty staff: clear selection, set the paste anchor.
            if (!d.additive) this.selection = new Set();
            const rect = this.viewportRect();
            const cx = e.clientX - rect.left;
            const cy = e.clientY - rect.top;
            this.anchorTick = this.snapTick(this.canvasXToTickAware(cx));
            this.anchorTrackIdx = Math.max(0, Math.min((this.doc?.tracks.length ?? 1) - 1, this.trackIndexAt(cy)));
        }
    }

    // Notes whose noteheads intersect the marquee rectangle, across all tracks.
    private applyMarqueeSelection(d: MarqueeDrag) {
        if (!this.doc) return;
        const left = Math.min(d.x0, d.x1), right = Math.max(d.x0, d.x1);
        const top = Math.min(d.y0, d.y1), bottom = Math.max(d.y0, d.y1);

        const s = d.additive ? new Set(this.selection) : new Set<string>();
        this.doc.tracks.forEach((_track, trackIdx) => {
            const laneTop = trackIdx * TH - this.vs;
            if (laneTop > bottom || laneTop + TH < top) return;
            const visuals = this.visualsFor(trackIdx);
            const clef = this.clefFor(trackIdx);
            for (const vm of visuals) {
                for (const f of vm.fragments) {
                    const fx = this.noteX(f.startTick, vm);
                    const fy = this.stepY(f.step, laneTop, clef);
                    if (fx + 12 >= left && fx <= right && fy + 6 >= top && fy - 6 <= bottom) {
                        s.add(noteKey(f.noteId));
                    }
                }
            }
        });
        this.selection = s;
    }

    private handleMouseLeave(_e: MouseEvent) {
        this.hover = null;
        if (this.drag) this.commitDrag();
    }

    // ─── Insert preview ───────────────────────────────────────────────────────

    // Where the insert tool would drop a note, at the exact snapped position it
    // would use.  Plain field, not @state: the overlay redraws every frame
    // anyway, so a Lit update per pointer move would be pure overhead.
    private hover: HoverTarget | null = null;

    private updateHover(cx: number, cy: number) {
        if (!this.doc || this.mode !== 'edit' || this.tool !== 'insert' || cx < STAFF_LABEL_W) {
            this.hover = null;
            return;
        }
        const trackIdx = this.trackIndexAt(cy);
        if (trackIdx < 0 || trackIdx >= this.doc.tracks.length) {
            this.hover = null;
            return;
        }
        const tick = this.snapTick(this.canvasXToTickAware(cx));
        this.hover = {
            trackIdx,
            tick,
            pitch: this.canvasYtoPitch(cy, trackIdx, tick),
        };
    }

    // ─── Inline time signature editing ────────────────────────────────────────

    // Meter being edited in place, if any. The change applies from this bar on.
    @state() private editingMeter:
        { measureIndex: number; startTick: number; numerator: number; denominator: number;
          x: number; y: number } | null = null;

    /** True when the click landed on a drawn time signature, which opens the editor. */
    private hitTestTimeSignature(cx: number, cy: number): boolean {
        if (!this.doc) return false;
        const trackIdx = this.trackIndexAt(cy);
        if (trackIdx < 0 || trackIdx >= this.doc.tracks.length) return false;
        const yBase = trackIdx * TH - this.vs;
        // Only the staff band counts, so clicks above or below stay with the tools.
        if (cy < yBase + TOP_LINE_Y - 4 || cy > yBase + BOTTOM_LINE_Y + 4) return false;

        for (const vm of this.visualsFor(trackIdx)) {
            if (!vm.showTimeSig) continue;
            if (vm.timeSigX > cx || cx > vm.timeSigX + TIME_SIG_W) continue;
            this.editingMeter = {
                measureIndex: vm.index,
                startTick: vm.startTick,
                numerator: vm.timeSignature.numerator,
                denominator: vm.timeSignature.denominator,
                x: vm.timeSigX,
                y: yBase + TOP_LINE_Y - 2,
            };
            return true;
        }
        return false;
    }

    private async commitMeter(numerator: number, denominator: number) {
        const editing = this.editingMeter;
        this.editingMeter = null;
        if (!editing || !this.doc) return;
        if (!Number.isFinite(numerator) || !Number.isFinite(denominator)) return;
        if (numerator === editing.numerator && denominator === editing.denominator) return;
        try {
            // The core snaps to the measure containing this tick, so the bar the
            // user clicked is exactly the bar that changes.
            await CoreBridge.sendCommand('set_time_signature', {
                documentId: this.doc.id, tick: editing.startTick, numerator, denominator,
            });
        } catch (err) { console.error('set_time_signature failed', err); }
    }

    private renderMeterEditor() {
        const editing = this.editingMeter;
        if (!editing) return '';
        return html`
            <div class="meter-editor" style="left:${editing.x - 6}px; top:${editing.y}px"
                 @keydown=${(e: KeyboardEvent) => {
                     e.stopPropagation();
                     if (e.key === 'Escape') { e.preventDefault(); this.editingMeter = null; }
                 }}
                 @focusout=${(e: FocusEvent) => {
                     // Moving between the two fields must not close the editor.
                     const next = e.relatedTarget as Node | null;
                     if (!next || !(e.currentTarget as HTMLElement).contains(next)) {
                         this.editingMeter = null;
                     }
                 }}>
                <span class="bar-label">bar ${editing.measureIndex + 1}</span>
                <input id="meter-num" type="number" min="1" max="32" step="1"
                       .value=${String(editing.numerator)}
                       @change=${(e: Event) => this.commitMeter(
                           parseInt((e.target as HTMLInputElement).value), editing.denominator)}>
                <span class="slash">/</span>
                <select @change=${(e: Event) => this.commitMeter(
                            editing.numerator, parseInt((e.target as HTMLSelectElement).value))}>
                    ${DENOMINATORS.map(d => html`
                        <option value=${d} ?selected=${d === editing.denominator}>${d}</option>`)}
                </select>
            </div>
        `;
    }

    // ─── Inline track rename ──────────────────────────────────────────────────

    // Double-click the track label column to rename it in place.
    private handleDblClick(e: MouseEvent) {
        if (!this.doc || this.mode !== 'edit') return;
        const rect = this.viewportRect();
        const cx = e.clientX - rect.left;
        const cy = e.clientY - rect.top;
        if (cx >= STAFF_LABEL_W) return;
        const trackIdx = this.trackIndexAt(cy);
        if (trackIdx < 0 || trackIdx >= this.doc.tracks.length) return;

        this.editingTrackIdx = trackIdx;
        this.trackEditorNeedsFocus = true;
    }

    updated() {
        if (this.trackEditorNeedsFocus && this.trackNameEditor) {
            this.trackEditorNeedsFocus = false;
            this.trackNameEditor.focus();
            this.trackNameEditor.select();
        }
        // Focus the meter editor as it appears, so Escape and tabbing work without
        // a second click.
        const meterInput = this.shadowRoot?.querySelector('#meter-num') as HTMLInputElement | null;
        const focused = this.shadowRoot?.activeElement ?? null;
        if (meterInput && !(focused && meterInput.parentElement?.contains(focused))) {
            meterInput.focus();
            meterInput.select();
        }
    }

    private async commitTrackName(cancel = false) {
        const idx = this.editingTrackIdx;
        // Read the field before clearing the state that renders it.
        const typed = this.trackNameEditor?.value ?? '';
        this.editingTrackIdx = null;
        if (cancel || idx === null || !this.doc) return;

        const track = this.doc.tracks[idx];
        const name = typed.trim();
        if (!track || !name || name === track.name) return;
        try {
            await CoreBridge.sendCommand('rename_track', {
                documentId: this.doc.id, trackId: parseInt(track.id), name,
            });
        } catch (err) { console.error('rename_track failed', err); }
    }

    private renderTrackNameEditor() {
        const idx = this.editingTrackIdx;
        if (idx === null || !this.doc) return '';
        const track = this.doc.tracks[idx];
        if (!track) return '';
        // Sits over the label column, vertically centred in its lane — the same
        // place the canvas draws the name.
        const top = idx * TH - this.vs + TH / 2 - 12;
        return html`
            <input id="track-name-editor" class="track-name-editor"
                   style="top:${top}px; left:6px; width:${STAFF_LABEL_W - 12}px"
                   .value=${track.name}
                   @keydown=${(e: KeyboardEvent) => {
                       e.stopPropagation();
                       if (e.key === 'Enter')       { e.preventDefault(); this.commitTrackName(); }
                       else if (e.key === 'Escape') { e.preventDefault(); this.commitTrackName(true); }
                   }}
                   @blur=${() => this.commitTrackName()}>
        `;
    }

    private handleWheel(e: WheelEvent) {
        e.preventDefault();
        // Scrolling would leave the inline editors detached from what they anchor to.
        if (this.editingTrackIdx !== null) this.commitTrackName();
        if (this.editingMeter !== null) this.editingMeter = null;
        if (e.ctrlKey) {
            const rect   = this.viewportRect();
            const mouseX = Math.max(STAFF_LABEL_W, e.clientX - rect.left);
            // Keep the tick under the cursor stationary through the zoom.  hs
            // shifts every measure column uniformly, so correcting by the drift
            // the new zoom introduced is exact even with headers and pads.
            const tickAtMouse = this.canvasXToTickAware(mouseX);
            this.zoom = Math.max(0.1, Math.min(5.0, this.zoom - e.deltaY * 0.001));
            this.setHs(this.hs + (this.tickToCanvasXAware(tickAtMouse) - mouseX));
        } else if (e.shiftKey) {
            // Shift + wheel scrolls horizontally.  A plain mouse reports no
            // deltaX at all, so without this there was no way to reach later
            // measures other than a trackpad.
            this.scrollByWheel(e.deltaY || e.deltaX);
        } else if (e.deltaX === 0 && this.maxVs === 0) {
            // Nowhere to scroll vertically, so a plain wheel on a plain mouse
            // would otherwise do nothing at all.  Sideways is the only useful
            // reading of it here.
            this.scrollByWheel(e.deltaY);
        } else {
            this.scrollByWheel(e.deltaX);
            this.vs = this.clampVs(this.vs + e.deltaY);
        }
    }

    // The canvases fill .canvas-container, which is the host minus the
    // scrollbar strip.  Anything mapping pixels to music has to measure the
    // container, or the canvas backing store and the hit tests disagree.
    private viewportRect(): DOMRect {
        return (this.canvasContainer ?? this).getBoundingClientRect();
    }

    private get maxVs(): number {
        const tracks = this.doc?.tracks.length ?? 0;
        return Math.max(0, tracks * TH - this.viewportRect().height);
    }

    // Keep at least one lane in view instead of scrolling off into blank space.
    private clampVs(vs: number): number {
        return Math.max(0, Math.min(this.maxVs, vs));
    }

    // ─── Horizontal scroll extent ─────────────────────────────────────────────

    /** Total px the laid-out measures occupy, excluding the fixed label column. */
    private get contentWidth(): number {
        const visuals = this.visualsFor(0);
        if (visuals.length === 0) return 0;
        // barlineX already has hs subtracted out of it; add it back for an
        // absolute width that does not move as the view scrolls.
        return visuals[visuals.length - 1].barlineX + this.hs - STAFF_LABEL_W;
    }

    private get maxHs(): number {
        return maxScroll(this.contentWidth, this.scrollTrackWidth);
    }

    // Width the fixed clef gutter occupies under a given key.  The accidental
    // count comes from the key, not the clef, so every track's gutter is the
    // same width and one reservation covers all of them.  Both the reservation
    // in measureOffsets and the drawing in renderClefGutter go through here, so
    // the space set aside and the space used cannot drift apart.
    private gutterWidthFor(fifths: number): number {
        const accidentals = Math.min(Math.abs(fifths), 7);
        return MEASURE_LEAD_IN + CLEF_W + (accidentals ? accidentals * KEY_ACC_W + 6 : 0);
    }

    /** Leftmost measure fully inside the note area: the one the gutter restates. */
    private get leadMeasure(): MeasureVisualLayout | undefined {
        const visuals = this.visualsFor(0);
        return visuals.find(vm => vm.visualStartX >= STAFF_LABEL_W - 0.5)
            ?? visuals[visuals.length - 1];
    }

    /**
     * Scroll offsets that put each measure at the start of the staff.  From the
     * second measure on the gutter is showing, so its width is reserved: without
     * that the leftmost measure's first noteheads sit under the restated clef.
     */
    private get measureOffsets(): number[] {
        return this.visualsFor(0).map((vm, i) => {
            const absolute = vm.visualStartX + this.hs - STAFF_LABEL_W;
            return i === 0 ? 0
                           : Math.max(0, absolute - this.gutterWidthFor(vm.keySignature.fifths));
        });
    }

    private get scrollStops(): number[] {
        return scrollStops(this.measureOffsets, this.maxHs);
    }

    /** Scrolling rests on measure boundaries; see models/scoreScroll.ts. */
    private setHs(value: number) {
        const snapped = snapToStop(Math.max(0, Math.min(this.maxHs, value)), this.scrollStops);
        if (snapped !== this.hs) this.hs = snapped;
    }

    private scrollByMeasures(delta: number) {
        const target = stepStop(this.hs, this.scrollStops, delta);
        if (target !== this.hs) this.hs = target;
    }

    // Wheel deltas are continuous and measure steps are not, so quantise.
    private wheelAccum = 0;
    private scrollByWheel(delta: number) {
        if (this.wheelAccum !== 0 && Math.sign(delta) !== Math.sign(this.wheelAccum)) {
            this.wheelAccum = 0;   // reversing direction should not need to unwind
        }
        this.wheelAccum += delta;
        const steps = Math.trunc(this.wheelAccum / WHEEL_NOTCH);
        if (steps === 0) return;
        this.wheelAccum -= steps * WHEEL_NOTCH;
        this.scrollByMeasures(steps);
    }

    /** Whole measures currently fitting in the note area, at least one. */
    private get measuresPerPage(): number {
        const width = this.viewportRect().width;
        let count = 0;
        for (const vm of this.visualsFor(0)) {
            if (vm.barlineX <= STAFF_LABEL_W) continue;
            if (vm.barlineX > width) break;
            count++;
        }
        return Math.max(1, count);
    }

    /** Scroll the playback cursor into view, used while the transport runs. */
    private followPlayhead() {
        const rect = this.viewportRect();
        if (rect.width <= STAFF_LABEL_W) return;
        const x = this.tickToCanvasXAware(this.currentTick);
        if (!needsReveal(x, STAFF_LABEL_W, rect.width)) return;
        // Turn the page: the measure the cursor is in becomes the leftmost one.
        // Nudging by a fraction of the view instead would leave the cursor
        // mid-measure at the edge, which measure-snapped scrolling then undoes.
        const offsets = this.measureOffsets;
        const index = this.visualsFor(0)
            .findIndex(vm => this.currentTick < vm.startTick + vm.durationTicks);
        if (index < 0) return;
        this.setHs(offsets[index]);
    }

    // ─── Horizontal scrollbar ─────────────────────────────────────────────────

    private get scrollTrackWidth(): number {
        return Math.max(0, this.viewportRect().width - STAFF_LABEL_W);
    }

    private get thumb() {
        return scrollThumb(this.scrollTrackWidth, this.contentWidth, this.hs, HTHUMB_MIN);
    }

    private handleThumbDown(e: PointerEvent) {
        e.preventDefault();
        e.stopPropagation();     // not a track click, which would page instead
        const track   = this.scrollTrackWidth;
        const width   = this.thumb.width;
        const maxHs   = this.maxHs;
        const startX  = e.clientX;
        const startHs = this.hs;
        if (maxHs <= 0) return;
        const thumb = e.currentTarget as HTMLElement;
        // Capture keeps the drag alive past the edge of the thumb.  A synthetic
        // event has no live pointer to capture, which throws; the listeners
        // below still work, so it is not worth failing the drag over.
        try { thumb.setPointerCapture(e.pointerId); } catch { /* not capturable */ }
        const move = (ev: PointerEvent) => this.setHs(
            startHs + thumbDragToScroll(ev.clientX - startX, track, width, maxHs));
        const up = () => {
            thumb.removeEventListener('pointermove', move);
            thumb.removeEventListener('pointerup', up);
            thumb.removeEventListener('pointercancel', up);
        };
        thumb.addEventListener('pointermove', move);
        thumb.addEventListener('pointerup', up);
        thumb.addEventListener('pointercancel', up);
    }

    private handleScrollTrackDown(e: PointerEvent) {
        const rect = (e.currentTarget as HTMLElement).getBoundingClientRect();
        const page = Math.max(1, this.scrollTrackWidth - 40);
        this.setHs(e.clientX - rect.left < this.thumb.left ? this.hs - page : this.hs + page);
    }

    private renderScrollBar() {
        // Always rendered, even before the first layout gives it a width: the
        // strip must not appear and disappear under the staves.
        const { width, left } = this.thumb;
        const scrollable = this.maxHs > 0;
        return html`
            <div class="hscroll" style="margin-left:${STAFF_LABEL_W}px; height:${HSCROLL_H}px"
                 @pointerdown=${this.handleScrollTrackDown}>
                <div class="hthumb ${scrollable ? '' : 'idle'}"
                     style="left:${left}px; width:${width}px"
                     @pointerdown=${this.handleThumbDown}></div>
            </div>
        `;
    }

    // ─── Canvas resize ────────────────────────────────────────────────────────

    private handleResize() {
        const dpr = window.devicePixelRatio || 1;
        if (this.mainCanvas && this.overlayCanvas) {
            const rect = this.viewportRect();
            for (const c of [this.mainCanvas, this.overlayCanvas]) {
                c.width  = rect.width  * dpr;
                c.height = rect.height * dpr;
                c.getContext('2d')?.scale(dpr, dpr);
            }
        }
        // The scroll extent and the thumb are both sized off the viewport.
        this.requestUpdate();
    }

    private startRendering() {
        const render = () => {
            // The scroll extent moves with zoom, the viewport size and the
            // amount of material, so re-clamp rather than only on scroll.
            // setHs assigns only on a real change, so this costs nothing.
            this.setHs(this.hs);
            this.renderCanvas();
            this.renderOverlay();
            this.animationFrameId = requestAnimationFrame(render);
        };
        this.animationFrameId = requestAnimationFrame(render);
    }

    // ─── Main canvas ──────────────────────────────────────────────────────────

    private renderCanvas() {
        if (!this.mainCanvas || !this.doc) return;
        const ctx  = this.mainCanvas.getContext('2d');
        if (!ctx)  return;
        const rect = this.viewportRect();
        ctx.clearRect(0, 0, rect.width, rect.height);

        const first = Math.max(0, Math.floor(this.vs / TH));
        const last  = Math.min(this.doc.tracks.length - 1, Math.floor((this.vs + rect.height) / TH));
        for (let i = first; i <= last; i++) this.renderTrack(ctx, i, rect.width);
    }

    private renderTrack(ctx: CanvasRenderingContext2D, trackIdx: number, width: number) {
        if (!this.doc) return;
        const track  = this.doc.tracks[trackIdx];
        const yBase  = trackIdx * TH - this.vs;
        const rect   = this.viewportRect();

        // Staff lines
        ctx.strokeStyle = '#333';
        ctx.lineWidth   = 1;
        for (let l = 0; l < 5; l++) {
            const y = yBase + TOP_LINE_Y + l * 10;
            ctx.beginPath();
            ctx.moveTo(STAFF_LABEL_W, y); ctx.lineTo(width, y);
            ctx.stroke();
        }

        // Label column
        ctx.fillStyle = '#1e1e1e';
        ctx.fillRect(0, yBase, STAFF_LABEL_W, TH);
        ctx.fillStyle    = '#888';
        ctx.font         = 'bold 0.9rem sans-serif';
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillText(track.name, 10, yBase + TH / 2);

        const visuals = this.visualsFor(trackIdx);

        const clef = this.clefFor(trackIdx);
        // Notation is confined to the staff.  Without this, scrolling drags
        // noteheads, stems and beams across the track-name column, and a note
        // near a bar line spills over the barline into the fixed clef gutter.
        this.withNoteAreaClip(ctx, () => {
            for (const vm of visuals) {
                if (vm.barlineX > STAFF_LABEL_W && vm.visualStartX < rect.width) {
                    this.renderMeasure(ctx, vm, yBase, clef);
                }
            }
        });

        // Drawn last: it sits on top of the notation it covers.
        this.renderClefGutter(ctx, yBase, clef);
    }

    // Restricts drawing to the scrolling note area — everything right of the
    // fixed label column.  Only horizontal: a high note's ledger lines and stem
    // legitimately reach outside its lane box.
    private withNoteAreaClip(ctx: CanvasRenderingContext2D, draw: () => void) {
        const rect = this.viewportRect();
        ctx.save();
        ctx.beginPath();
        ctx.rect(STAFF_LABEL_W, 0, Math.max(0, rect.width - STAFF_LABEL_W), rect.height);
        ctx.clip();
        draw();
        ctx.restore();
    }

    // Once the score is scrolled, measure 1's clef and key signature are off
    // screen and there is nothing left saying what staff you are reading. Restate
    // them in a fixed column over the scrolling content — Notation spec §25.2's
    // viewport-friendly mode.
    //
    // It has to be an overlay rather than an inset: the gutter's width depends on
    // the key signature, so insetting the note area would reflow the whole layout
    // as the user scrolls past a key change.
    private renderClefGutter(ctx: CanvasRenderingContext2D, yBase: number, clef: ClefDef) {
        // While the real clef is still visible there is nothing to restate.
        if (this.hs <= MEASURE_LEAD_IN) return;

        // The key of the measure the gutter belongs to, not of whatever happens
        // to sit under x = STAFF_LABEL_W: the scroll reserves this gutter's width
        // ahead of that measure, so x there is still inside the previous one.
        const lead = this.leadMeasure;
        const fifths = lead ? lead.keySignature.fifths
                            : keyAt(this.doc?.keySignatureMap,
                                    this.canvasXToTickAware(STAFF_LABEL_W)).fifths;
        const marks = signatureSteps(fifths, clef.bottomLineStep);
        const width = this.gutterWidthFor(fifths);

        // Opaque backing so the notation underneath does not show through.
        ctx.fillStyle = '#1e1e1e';
        ctx.fillRect(STAFF_LABEL_W, yBase + 1, width, TH - 2);

        // Staff lines across the gutter, so the clef sits on a staff.
        ctx.strokeStyle = '#333';
        ctx.lineWidth   = 1;
        for (let l = 0; l < 5; l++) {
            const y = yBase + TOP_LINE_Y + l * 10;
            ctx.beginPath();
            ctx.moveTo(STAFF_LABEL_W, y);
            ctx.lineTo(STAFF_LABEL_W + width, y);
            ctx.stroke();
        }

        this.drawClef(ctx, STAFF_LABEL_W + MEASURE_LEAD_IN, yBase, clef);
        if (marks.length) {
            ctx.fillStyle    = '#b4b4b4';
            ctx.font         = '15px serif';
            ctx.textAlign    = 'center';
            ctx.textBaseline = 'middle';
            marks.forEach((mark, i) => {
                ctx.fillText(ACCIDENTAL_TEXT[mark.glyph],
                             STAFF_LABEL_W + MEASURE_LEAD_IN + CLEF_W + 4 + i * KEY_ACC_W,
                             this.stepY(mark.step, yBase, clef));
            });
            ctx.textAlign    = 'left';
            ctx.textBaseline = 'alphabetic';
        }

        // Edge marking the column as fixed rather than part of the music.
        ctx.strokeStyle = '#4a4a4a';
        ctx.beginPath();
        ctx.moveTo(STAFF_LABEL_W + width, yBase + 1);
        ctx.lineTo(STAFF_LABEL_W + width, yBase + TH - 1);
        ctx.stroke();
    }

    private renderMeasure(ctx: CanvasRenderingContext2D, vm: MeasureVisualLayout, yBase: number, clef: ClefDef) {
        // ── Grid guides ───────────────────────────────────────────────────────
        // Drawn first so notation sits on top of them.
        this.renderGrid(ctx, vm, yBase);

        // ── Bar lines ─────────────────────────────────────────────────────────
        ctx.strokeStyle = '#5a5a5a';
        ctx.lineWidth   = 1;
        ctx.beginPath();
        ctx.moveTo(vm.barlineX, yBase + TOP_LINE_Y);
        ctx.lineTo(vm.barlineX, yBase + BOTTOM_LINE_Y);
        // The very first measure also needs the opening line; every other
        // measure's left edge is the previous measure's bar line.
        if (vm.index === 0) {
            ctx.moveTo(vm.visualStartX, yBase + TOP_LINE_Y);
            ctx.lineTo(vm.visualStartX, yBase + BOTTOM_LINE_Y);
        }
        ctx.stroke();

        // Measure number, in the lead-in above the staff. Kept clear of the
        // octave marker an 8va clef draws in the same band, which otherwise
        // reads as one number ("1 8").
        ctx.fillStyle = '#6b6b6b';
        ctx.font      = '0.7rem sans-serif';
        ctx.textAlign = 'left';
        ctx.fillText(String(vm.index + 1), vm.visualStartX + 3, yBase + TOP_LINE_Y - 15);

        // ── Header symbols ────────────────────────────────────────────────────
        let cursorX = vm.visualStartX + MEASURE_LEAD_IN;

        if (vm.showClef) {
            this.drawClef(ctx, cursorX, yBase, clef);
            cursorX += CLEF_W;
        }

        if (vm.showKeySig) {
            this.drawKeySignature(ctx, cursorX, yBase, clef, vm.keySignature.fifths, vm.prevKeyFifths);
            cursorX += vm.keySigWidth;
        }

        if (vm.showTimeSig) {
            ctx.fillStyle    = '#bbb';
            ctx.font         = 'bold 19px serif';
            ctx.textAlign    = 'center';
            ctx.textBaseline = 'middle';
            const tsX = vm.timeSigX + TIME_SIG_W / 2;
            ctx.fillText(String(vm.timeSignature.numerator),   tsX, yBase + 51);
            ctx.fillText(String(vm.timeSignature.denominator), tsX, yBase + 70);
        }

        // ── Notes ─────────────────────────────────────────────────────────────
        ctx.textBaseline = 'alphabetic';
        const sorted = [...vm.fragments].sort((a, b) => a.startTick - b.startTick);
        for (const item of this.buildBeamGroups(sorted, vm)) {
            if (Array.isArray(item)) this.renderBeamGroup(ctx, item, yBase, vm, clef);
            else                     this.renderNote(ctx, item, yBase, false, vm, clef);
        }

        // ── Rests ─────────────────────────────────────────────────────────────
        for (const r of vm.rests) this.renderRest(ctx, r, yBase, vm);
    }

    // The clef glyph plus, for the transposing clefs, the 8 above or below it.
    // The 8 is drawn as text rather than using the ottava-alta/bassa codepoints,
    // which render as missing-glyph boxes in common system fonts.
    private drawClef(ctx: CanvasRenderingContext2D, x: number, yBase: number, clef: ClefDef) {
        ctx.fillStyle    = '#b4b4b4';
        ctx.font         = `${clef.fontPx}px serif`;
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'alphabetic';
        ctx.fillText(clef.glyph, x + 2, yBase + TOP_LINE_Y + clef.baselineOffset);

        if (clef.octaveMarker) {
            ctx.font      = 'bold 11px serif';
            ctx.textAlign = 'center';
            const y = clef.octaveMarker === 'above'
                ? yBase + TOP_LINE_Y - 4            // just above the top staff line
                : yBase + BOTTOM_LINE_Y + 15;       // just below the bottom staff line
            ctx.fillText('8', x + 12, y);
            ctx.textAlign = 'left';
        }
    }

    // The key signature accidentals, at their conventional staff positions for
    // this clef.
    private drawKeySignature(ctx: CanvasRenderingContext2D, x: number, yBase: number,
                             clef: ClefDef, fifths: number, prevFifths: number | null) {
        ctx.fillStyle    = '#b4b4b4';
        ctx.font         = '15px serif';
        ctx.textAlign    = 'center';
        ctx.textBaseline = 'middle';

        // Cancellations come first, then the incoming signature.
        const marks = [
            ...(prevFifths === null ? [] : cancellationSteps(prevFifths, fifths, clef.bottomLineStep)),
            ...signatureSteps(fifths, clef.bottomLineStep),
        ];
        marks.forEach((mark, i) => {
            ctx.fillText(ACCIDENTAL_TEXT[mark.glyph],
                         x + 4 + i * KEY_ACC_W, this.stepY(mark.step, yBase, clef));
        });
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'alphabetic';
    }

    // Faint beat and snap-subdivision markers.  Without them the snap result is
    // invisible until the note is already committed, which is what made note
    // placement feel arbitrary.
    private renderGrid(ctx: CanvasRenderingContext2D, vm: MeasureVisualLayout, yBase: number) {
        const ppqn      = this.doc?.ppqn ?? 480;
        const beatTicks = Math.max(1, Math.round(ppqn * 4 / vm.timeSignature.denominator));
        const top       = yBase + TOP_LINE_Y;
        const bottom    = yBase + BOTTOM_LINE_Y;

        const line = (t: number) => {
            const x = vm.noteAreaX + t * vm.pxPerTick;
            ctx.moveTo(x, top);
            ctx.lineTo(x, bottom);
        };

        // Snap subdivisions, only while they stay far enough apart to read.
        const g = this.gridTicks;
        if (g > 0 && g < beatTicks && g * vm.pxPerTick >= 8) {
            ctx.strokeStyle = 'rgba(255, 255, 255, 0.045)';
            ctx.lineWidth   = 1;
            ctx.beginPath();
            for (let t = g; t < vm.durationTicks; t += g) {
                if (t % beatTicks !== 0) line(t);
            }
            ctx.stroke();
        }

        // Beat markers (the measure start is already the bar line).
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.085)';
        ctx.lineWidth   = 1;
        ctx.beginPath();
        for (let t = beatTicks; t < vm.durationTicks; t += beatTicks) line(t);
        ctx.stroke();
    }

    // ─── Note rendering ───────────────────────────────────────────────────────

    private drawLedgerLines(ctx: CanvasRenderingContext2D, x: number, y: number, yBase: number) {
        ctx.strokeStyle = '#555';
        ctx.lineWidth   = 1;
        const drawLine = (ly: number) => {
            ctx.beginPath();
            ctx.moveTo(x - 3, ly);
            ctx.lineTo(x + 15, ly);
            ctx.stroke();
        };
        // Above the staff
        for (let ly = yBase + TOP_LINE_Y - 10; ly >= y - 2; ly -= 10) drawLine(ly);
        // Below the staff
        for (let ly = yBase + BOTTOM_LINE_Y + 10; ly <= y + 2; ly += 10) drawLine(ly);
    }

    private drawAccidental(ctx: CanvasRenderingContext2D, x: number, y: number,
                           color: string, glyph: AccidentalGlyph) {
        ctx.fillStyle    = color;
        ctx.font         = '14px serif';
        ctx.textAlign    = 'right';
        ctx.textBaseline = 'middle';
        ctx.fillText(ACCIDENTAL_TEXT[glyph], x - 2, y);
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'alphabetic';
    }

    // Semitones → diatonic steps, for the live drag preview only.
    private stepDelta(semitones: number): number {
        return Math.round(semitones * 7 / 12);
    }

    private renderNote(
        ctx: CanvasRenderingContext2D,
        f: NoteFragment,
        yBase: number,
        beamed: boolean,
        vm: MeasureVisualLayout,
        clef: ClefDef,
    ) {
        const k = noteKey(f.noteId);
        const isSelected = this.selection.has(k);

        // Active drag previews: group move offsets every selected note;
        // resize previews the new duration of its single target.
        const moveDrag   = this.drag?.kind === 'move' && this.drag.started && isSelected ? this.drag : null;
        const resizeDrag = this.drag?.kind === 'resize' && this.drag.noteId === f.noteId ? this.drag : null;
        const isDragged  = !!moveDrag || !!resizeDrag;

        const displayPitch    = moveDrag ? Math.max(0, Math.min(127, f.pitch + moveDrag.deltaPitch)) : f.pitch;
        const displayDuration = resizeDrag ? resizeDrag.curDuration : f.durationTicks;
        const x = moveDrag
            ? this.tickToCanvasXAware(Math.max(0, f.startTick + moveDrag.deltaTick))
            : this.noteX(f.startTick, vm);
        // While dragging by pitch there is no re-spelled fragment yet: approximate
        // by shifting the notated step, which is exact for whole-octave and
        // diatonic moves and off by at most a step otherwise. The commit
        // re-spells properly.
        const displayStep = moveDrag ? f.step + this.stepDelta(moveDrag.deltaPitch) : f.step;
        const y = this.stepY(displayStep, yBase, clef);

        // A resize drag has no re-notated fragment yet, so fall back to ticks.
        const info  = resizeDrag ? this.getNoteInfo(displayDuration)
                                 : this.noteInfoFor(f.noteValue, f.dotted);
        const color = isDragged ? '#ffaa33' : isSelected ? '#4da6ff' : '#fff';
        const stemDown = displayStep >= middleLineStep(clef);

        // Resize preview: show the note's time extent as a translucent bar.
        if (resizeDrag) {
            ctx.fillStyle = 'rgba(255, 170, 51, 0.25)';
            ctx.fillRect(x, y - 8, displayDuration * vm.pxPerTick, 16);
        }

        this.drawLedgerLines(ctx, x, y, yBase);
        if (f.accidental && !isDragged) this.drawAccidental(ctx, x, y, color, f.accidental);

        ctx.fillStyle = ctx.strokeStyle = color;

        // Notehead (centre sits on the line/space for this pitch)
        ctx.beginPath();
        ctx.ellipse(x + 6, y, 6, 4.5, -20 * Math.PI / 180, 0, 2 * Math.PI);
        if (info.hollow) { ctx.lineWidth = 2; ctx.stroke(); }
        else             { ctx.fill(); }

        // Augmentation dot
        if (info.dotted) {
            ctx.fillStyle = color;
            ctx.beginPath();
            ctx.arc(x + 16, y - 3, 2, 0, Math.PI * 2);
            ctx.fill();
        }

        // Stem: up from the right side below the middle line, down from the
        // left side on or above it.
        const stemX   = stemDown ? x : x + 12;
        const stemTip = stemDown ? y + STEM_LEN : y - STEM_LEN;
        if (info.hasStem) {
            ctx.strokeStyle = color;
            ctx.lineWidth   = 1;
            ctx.beginPath();
            ctx.moveTo(stemX, stemDown ? y + 2 : y - 2);
            ctx.lineTo(stemX, stemTip);
            ctx.stroke();
        }

        // Flags (suppressed when part of a beam group)
        if (!beamed && info.flags >= 1) this.drawFlag(ctx, stemX, stemTip, color, stemDown);
        if (!beamed && info.flags >= 2) this.drawFlag(ctx, stemX, stemTip + (stemDown ? -(BEAM_GAP + BEAM_H) : BEAM_GAP + BEAM_H), color, stemDown);

        // Tie
        if (f.tieStart && !isDragged) {
            this.drawTie(ctx, x, this.tieTargetX(f, vm), y);
        }
    }

    // Where a tie should land: the next fragment of the same note in this measure.
    // Null when the tie crosses the bar line, where the continuation is laid out in
    // the next measure and a fixed stub is the best we can draw.
    private tieTargetX(f: NoteFragment, vm: MeasureVisualLayout): number | null {
        let best: NoteFragment | null = null;
        for (const other of vm.fragments) {
            if (other.noteId !== f.noteId || other.startTick <= f.startTick) continue;
            if (!best || other.startTick < best.startTick) best = other;
        }
        return best ? this.noteX(best.startTick, vm) : null;
    }

    private drawTie(ctx: CanvasRenderingContext2D, fromX: number, toX: number | null, y: number) {
        const endX = toX ?? fromX + 50;
        ctx.strokeStyle = '#007acc';
        ctx.lineWidth   = 1.5;
        ctx.setLineDash([2, 2]);
        ctx.beginPath();
        ctx.moveTo(fromX + 6, y + 3);
        ctx.quadraticCurveTo((fromX + endX) / 2 + 6, y + 13, endX + 6, y + 3);
        ctx.stroke();
        ctx.setLineDash([]);
    }

    private drawFlag(ctx: CanvasRenderingContext2D, x: number, stemTip: number, color: string, stemDown: boolean) {
        const dir = stemDown ? -1 : 1;
        ctx.strokeStyle = color;
        ctx.lineWidth   = 1.5;
        ctx.beginPath();
        ctx.moveTo(x, stemTip);
        ctx.bezierCurveTo(
            x + 12, stemTip + dir * 4,
            x + 12, stemTip + dir * 12,
            x + 5,  stemTip + dir * 18,
        );
        ctx.stroke();
    }

    private renderBeamGroup(
        ctx: CanvasRenderingContext2D,
        group: NoteFragment[],
        yBase: number,
        vm: MeasureVisualLayout,
        clef: ClefDef,
    ) {
        const positions = group.map(f => ({
            f,
            x: this.noteX(f.startTick, vm),
            noteY: this.stepY(f.step, yBase, clef),
        }));

        // Majority stem direction for the whole group.
        const downCount = group.filter(f => f.step >= middleLineStep(clef)).length;
        const stemsDown = downCount * 2 >= group.length;

        const beamY = stemsDown
            ? Math.max(...positions.map(p => p.noteY)) + STEM_LEN
            : Math.min(...positions.map(p => p.noteY)) - STEM_LEN;
        const maxFlags = Math.max(...group.map(f => this.noteInfoFor(f.noteValue, f.dotted).flags));
        const stemOffset = stemsDown ? 0 : 12;

        for (const { f, x, noteY } of positions) {
            const isSelected = this.selection.has(noteKey(f.noteId));
            const color      = isSelected ? '#4da6ff' : '#fff';

            this.drawLedgerLines(ctx, x, noteY, yBase);
            if (f.accidental) this.drawAccidental(ctx, x, noteY, color, f.accidental);

            ctx.fillStyle = ctx.strokeStyle = color;

            // Filled notehead
            ctx.beginPath();
            ctx.ellipse(x + 6, noteY, 6, 4.5, -20 * Math.PI / 180, 0, 2 * Math.PI);
            ctx.fill();

            // Augmentation dot
            if (f.dotted) {
                ctx.beginPath();
                ctx.arc(x + 16, noteY - 3, 2, 0, Math.PI * 2);
                ctx.fill();
            }

            // Stem to beam height
            ctx.lineWidth = 1;
            ctx.beginPath();
            ctx.moveTo(x + stemOffset, stemsDown ? noteY + 2 : noteY - 2);
            ctx.lineTo(x + stemOffset, beamY);
            ctx.stroke();

            // Tie
            if (f.tieStart) this.drawTie(ctx, x, this.tieTargetX(f, vm), noteY);
        }

        // Beam bars
        const x0 = positions[0].x + stemOffset;
        const x1 = positions[positions.length - 1].x + stemOffset;
        const barTop = stemsDown ? beamY - BEAM_H : beamY;
        ctx.fillStyle = '#fff';
        ctx.fillRect(x0, barTop, Math.max(1, x1 - x0), BEAM_H);
        if (maxFlags >= 2) {
            const secondTop = stemsDown ? barTop - BEAM_GAP - BEAM_H : barTop + BEAM_H + BEAM_GAP;
            ctx.fillRect(x0, secondTop, Math.max(1, x1 - x0), BEAM_H);
        }
    }

    // ─── Rest rendering ───────────────────────────────────────────────────────

    private renderRest(
        ctx: CanvasRenderingContext2D,
        r: RestFragment,
        yBase: number,
        vm: MeasureVisualLayout,
    ) {
        const staffMid = yBase + 60;   // 3rd (middle) staff line; lines are 10px apart

        // The notation service has already resolved the exact rest value, so
        // this is a lookup rather than a duration guess.
        //
        // A rest covering the whole measure is drawn as one whole rest centred
        // in the note area whatever the meter — the conventional "this bar is
        // empty" notation, and far quicker to read than a glyph jammed against
        // the left bar line.
        const x = r.fullMeasure
            ? vm.noteAreaX + (vm.contentWidth - NOTE_GLYPH_W) / 2
            : this.noteX(r.startTick, vm);

        ctx.fillStyle = ctx.strokeStyle = '#666';
        ctx.textAlign = 'left';

        switch (r.restValue) {
            case 'whole':
                // Hangs from the 4th line, filling the space above the middle line.
                ctx.fillRect(x + 2, staffMid - 10, 14, 7);
                break;
            case 'half':
                // Sits on the middle line.
                ctx.fillRect(x + 2, staffMid - 7, 14, 7);
                break;
            case 'quarter':
                ctx.font = '22px serif'; ctx.textBaseline = 'middle';
                ctx.fillText('𝄽', x + 2, staffMid);
                break;
            case 'eighth':
                ctx.font = '18px serif'; ctx.textBaseline = 'middle';
                ctx.fillText('𝄾', x + 2, staffMid);
                break;
            case 'sixteenth':
                ctx.font = '15px serif'; ctx.textBaseline = 'middle';
                ctx.fillText('𝄿', x + 2, staffMid);
                break;
        }

        if (r.dotted) {
            ctx.fillStyle = '#666';
            ctx.beginPath();
            ctx.arc(x + 20, staffMid - 14, 2, 0, Math.PI * 2);
            ctx.fill();
        }
        ctx.textBaseline = 'alphabetic';
    }

    // ─── Overlay (playback cursor, previews) ──────────────────────────────────

    // Translucent notehead plus a snap marker at the exact tick/pitch a click
    // would commit, so the snap result is visible before anything is created.
    private renderInsertPreview(ctx: CanvasRenderingContext2D, h: HoverTarget) {
        const yBase = h.trackIdx * TH - this.vs;
        const clef  = this.clefFor(h.trackIdx);
        const x     = this.tickToCanvasXAware(h.tick);
        const spelling = spellPitch(h.pitch, keyAt(this.doc?.keySignatureMap, h.tick).fifths);
        const y     = this.stepY(spelling.step, yBase, clef);
        if (x < STAFF_LABEL_W) return;

        const info = this.getNoteInfo(this.ticksForDuration);

        ctx.save();
        ctx.globalAlpha = 0.55;

        // Snap marker spanning the staff at the target tick
        ctx.strokeStyle = '#e0c060';
        ctx.lineWidth   = 1;
        ctx.setLineDash([3, 3]);
        ctx.beginPath();
        ctx.moveTo(x + 6, yBase + TOP_LINE_Y - 12);
        ctx.lineTo(x + 6, yBase + BOTTOM_LINE_Y + 12);
        ctx.stroke();
        ctx.setLineDash([]);

        this.drawLedgerLines(ctx, x, y, yBase);
        const previewAccidental = accidentalFor(spelling, keyAt(this.doc?.keySignatureMap, h.tick).fifths);
        if (previewAccidental) this.drawAccidental(ctx, x, y, '#ffd166', previewAccidental);

        ctx.fillStyle = ctx.strokeStyle = '#ffd166';
        ctx.beginPath();
        ctx.ellipse(x + 6, y, 6, 4.5, -20 * Math.PI / 180, 0, 2 * Math.PI);
        ctx.fill();

        const stemDown = spelling.step >= middleLineStep(clef);
        const stemX    = stemDown ? x : x + 12;
        const stemTip  = stemDown ? y + STEM_LEN : y - STEM_LEN;
        ctx.lineWidth = 1;
        ctx.beginPath();
        ctx.moveTo(stemX, stemDown ? y + 2 : y - 2);
        ctx.lineTo(stemX, stemTip);
        ctx.stroke();
        if (info.flags >= 1) this.drawFlag(ctx, stemX, stemTip, '#ffd166', stemDown);
        if (info.flags >= 2) {
            this.drawFlag(ctx, stemX, stemTip + (stemDown ? -(BEAM_GAP + BEAM_H) : BEAM_GAP + BEAM_H),
                          '#ffd166', stemDown);
        }

        // Pitch readout, kept clear of the notehead and the stem
        ctx.globalAlpha  = 0.9;
        ctx.fillStyle    = '#ffd166';
        ctx.font         = 'bold 11px sans-serif';
        ctx.textAlign    = 'left';
        ctx.textBaseline = 'middle';
        ctx.fillText(pitchName(h.pitch), x + 20, y - 10);

        ctx.restore();
        ctx.textBaseline = 'alphabetic';
    }

    private renderOverlay() {
        if (!this.overlayCanvas || !this.doc) return;
        const ctx  = this.overlayCanvas.getContext('2d');
        if (!ctx)  return;
        const rect = this.viewportRect();
        ctx.clearRect(0, 0, rect.width, rect.height);

        // Same rule as the notation: nothing the overlay draws may stray over the
        // label column.  The playhead and the caret check their own x below, but
        // the marquee and the insert-tool ghost would otherwise spill.
        this.withNoteAreaClip(ctx, () => this.renderOverlayContent(ctx, rect));
    }

    private renderOverlayContent(ctx: CanvasRenderingContext2D, rect: DOMRect) {
        const cursorX = this.tickToCanvasXAware(this.currentTick);
        if (cursorX >= STAFF_LABEL_W && cursorX <= rect.width) {
            ctx.strokeStyle = '#007acc';
            ctx.lineWidth   = 2;
            ctx.shadowBlur  = 8;
            ctx.shadowColor = '#007acc';
            ctx.beginPath();
            ctx.moveTo(cursorX, 0); ctx.lineTo(cursorX, rect.height);
            ctx.stroke();
            ctx.shadowBlur = 0;
        }

        // Insert-tool ghost note.  Re-check the tool here: it can change while
        // the pointer sits still over the canvas, leaving a stale hover.
        if (this.hover && !this.drag && this.tool === 'insert' && this.mode === 'edit') {
            this.renderInsertPreview(ctx, this.hover);
        }

        // Marquee selection rectangle
        if (this.drag?.kind === 'marquee' && this.drag.moved) {
            const d = this.drag;
            const x = Math.min(d.x0, d.x1), y = Math.min(d.y0, d.y1);
            const w = Math.abs(d.x1 - d.x0), h = Math.abs(d.y1 - d.y0);
            ctx.fillStyle = 'rgba(77, 166, 255, 0.12)';
            ctx.fillRect(x, y, w, h);
            ctx.strokeStyle = '#4da6ff';
            ctx.lineWidth = 1;
            ctx.setLineDash([4, 3]);
            ctx.strokeRect(x, y, w, h);
            ctx.setLineDash([]);
        }

        // Paste anchor caret in its track lane
        if (this.anchorTick !== null && this.mode === 'edit') {
            const ax = this.tickToCanvasXAware(this.anchorTick);
            const ay = this.anchorTrackIdx * TH - this.vs;
            if (ax >= STAFF_LABEL_W && ax <= rect.width) {
                ctx.strokeStyle = '#e0c060';
                ctx.lineWidth = 1.5;
                ctx.beginPath();
                ctx.moveTo(ax, ay + TOP_LINE_Y - 8);
                ctx.lineTo(ax, ay + BOTTOM_LINE_Y + 8);
                ctx.stroke();
                ctx.fillStyle = '#e0c060';
                ctx.beginPath();
                ctx.moveTo(ax - 4, ay + TOP_LINE_Y - 8);
                ctx.lineTo(ax + 4, ay + TOP_LINE_Y - 8);
                ctx.lineTo(ax, ay + TOP_LINE_Y - 2);
                ctx.closePath();
                ctx.fill();
            }
        }
    }

    // ─── Template ─────────────────────────────────────────────────────────────

    render() {
        if (!this.doc) return html`<div>No document</div>`;
        return html`
            <div class="canvas-container tool-${this.tool} ${this.mode === 'play' ? 'mode-play' : ''}"
                 @wheel=${this.handleWheel}
                 @mousedown=${this.handleMouseDown}
                 @mousemove=${this.handleMouseMove}
                 @mouseup=${this.handleMouseUp}
                 @mouseleave=${this.handleMouseLeave}
                 @dblclick=${this.handleDblClick}>
                <canvas id="main-canvas"></canvas>
                <canvas id="overlay-canvas"></canvas>
            </div>
            ${this.renderScrollBar()}
            ${this.renderTrackNameEditor()}
            ${this.renderMeterEditor()}
        `;
    }
}
