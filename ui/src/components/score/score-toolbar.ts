import { LitElement, html, css } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { SNAP_GRIDS, type SnapGrid } from '../../models/snap';

// One click of the zoom buttons. A ratio rather than an increment, so zooming
// out and back in returns to where it started.
const ZOOM_STEP = 1.5;

export type ScoreTool = 'select' | 'insert' | 'erase' | 'resize';
export type NoteDuration = 'quarter' | 'eighth' | 'sixteenth';
export type ScoreMode = 'edit' | 'play';

// Snap resolution. Whether snapping happens at all is a separate toggle, so
// turning it off and back on returns to the resolution that was in use.
export type ScoreGrid = SnapGrid;

@customElement('mc-score-toolbar')
export class ScoreToolbar extends LitElement {
    @property({ type: Boolean }) canUndo = false;
    @property({ type: Boolean }) canRedo = false;
    @property({ type: Boolean }) snapEnabled = true;
    @property({ type: Boolean }) showRuler = true;
    /** Horizontal zoom, 1 = the default scale. Owned by the parent, because the
        score view also changes it (ctrl + wheel) and both must show the same. */
    @property({ type: Number }) zoom = 1;

    static styles = css`
        :host {
            display: flex;
            align-items: center;
            padding: 0 12px;
            gap: 12px;
            height: 100%;
        }
        .tool-group { display: flex; align-items: center; gap: 4px; border-right: 1px solid #444; padding-right: 8px; }
        button {
            background: #3c3c3c;
            border: none;
            color: #ccc;
            padding: 2px 8px;
            cursor: pointer;
            font-size: 0.8rem;
        }
        button:hover:not(.active):not(:disabled) { background: #505050; }
        button.active { background: #007acc; color: white; }
        button:disabled { opacity: 0.4; cursor: default; }
        .spacer { flex: 1; }
        .mode-btn.play-mode { background: #2e7d32; color: white; }
        label { font-size: 0.8rem; color: #999; }
        select {
            background: #3c3c3c;
            border: 1px solid #4a4a4a;
            color: #ccc;
            font-size: 0.8rem;
            padding: 1px 2px;
        }
        select:disabled { opacity: 0.4; }
        .zoom-group { gap: 2px; }
        .zoom-readout {
            font-size: 0.8rem;
            color: #ccc;
            min-width: 42px;
            text-align: center;
            cursor: pointer;
            user-select: none;
        }
        .zoom-readout:hover { color: #fff; text-decoration: underline; }
    `;

    @state() private activeTool: ScoreTool = 'select';
    @state() private activeDuration: NoteDuration = 'quarter';
    @state() private activeGrid: ScoreGrid = 'auto';
    @state() private mode: ScoreMode = 'edit';

    private selectTool(tool: ScoreTool) {
        this.activeTool = tool;
        this.dispatchEvent(new CustomEvent<{ tool: ScoreTool }>('tool-change', {
            detail: { tool },
            bubbles: true,
            composed: true,
        }));
    }

    private selectDuration(duration: NoteDuration) {
        this.activeDuration = duration;
        this.dispatchEvent(new CustomEvent<{ duration: NoteDuration }>('duration-change', {
            detail: { duration },
            bubbles: true,
            composed: true,
        }));
    }

    private selectGrid(e: Event) {
        this.activeGrid = (e.target as HTMLSelectElement).value as ScoreGrid;
        this.dispatchEvent(new CustomEvent<{ grid: ScoreGrid }>('grid-change', {
            detail: { grid: this.activeGrid },
            bubbles: true,
            composed: true,
        }));
    }

    private toggleMode() {
        this.mode = this.mode === 'edit' ? 'play' : 'edit';
        this.dispatchEvent(new CustomEvent<{ mode: ScoreMode }>('mode-change', {
            detail: { mode: this.mode },
            bubbles: true,
            composed: true,
        }));
    }

    private emit(name: string, detail?: unknown) {
        this.dispatchEvent(new CustomEvent(name, { detail, bubbles: true, composed: true }));
    }

    render() {
        const t = this.activeTool;
        const d = this.activeDuration;
        const editing = this.mode === 'edit';
        return html`
            <div class="tool-group">
                <button ?disabled=${!editing} class=${t === 'select' ? 'active' : ''} @click=${() => this.selectTool('select')}>Select</button>
                <button ?disabled=${!editing} class=${t === 'insert' ? 'active' : ''} @click=${() => this.selectTool('insert')}>Insert</button>
                <button ?disabled=${!editing} class=${t === 'resize' ? 'active' : ''} @click=${() => this.selectTool('resize')}>Resize</button>
                <button ?disabled=${!editing} class=${t === 'erase'  ? 'active' : ''} @click=${() => this.selectTool('erase')}>Erase</button>
            </div>
            <div class="tool-group">
                <button ?disabled=${!editing} class=${d === 'quarter'   ? 'active' : ''} @click=${() => this.selectDuration('quarter')}>1/4</button>
                <button ?disabled=${!editing} class=${d === 'eighth'    ? 'active' : ''} @click=${() => this.selectDuration('eighth')}>1/8</button>
                <button ?disabled=${!editing} class=${d === 'sixteenth' ? 'active' : ''} @click=${() => this.selectDuration('sixteenth')}>1/16</button>
            </div>
            <div class="tool-group">
                <button title="Undo (Ctrl+Z)" ?disabled=${!this.canUndo || !editing} @click=${() => this.emit('score-undo')}>↩ Undo</button>
                <button title="Redo (Ctrl+Y)" ?disabled=${!this.canRedo || !editing} @click=${() => this.emit('score-redo')}>↪ Redo</button>
            </div>
            <div class="tool-group">
                <button title="Add a new track" ?disabled=${!editing} @click=${() => this.emit('add-track')}>＋ Track</button>
            </div>
            <div class="tool-group">
                <button id="snap-toggle" class=${this.snapEnabled ? 'active' : ''}
                        ?disabled=${!editing}
                        title=${this.snapEnabled
                            ? 'Snapping is on - click to place notes at any tick'
                            : 'Snapping is off - notes land where you point'}
                        @click=${() => this.emit('snap-toggle', { enabled: !this.snapEnabled })}>
                    Snap
                </button>
                <select id="grid" ?disabled=${!editing || !this.snapEnabled}
                        title="Grid the insert / move / resize gestures snap to"
                        @change=${(e: Event) => this.selectGrid(e)}>
                    ${SNAP_GRIDS.map(o => html`
                        <option value=${o.value} ?selected=${this.activeGrid === o.value}>${o.label}</option>
                    `)}
                </select>
            </div>
            <div class="tool-group">
                <button id="ruler-toggle" class=${this.showRuler ? 'active' : ''}
                        title="Show or hide the ruler above the staves"
                        @click=${() => this.emit('ruler-toggle', { showRuler: !this.showRuler })}>
                    Ruler
                </button>
            </div>
            <div class="tool-group zoom-group">
                <label>Zoom</label>
                <button id="zoom-out"
                        title="Zoom out (Ctrl + wheel over the score)"
                        @click=${() => this.emit('zoom-step', { factor: 1 / ZOOM_STEP })}>-</button>
                <span class="zoom-readout" title="Click to reset to 100%"
                      @click=${() => this.emit('zoom-set', { zoom: 1 })}>
                    ${Math.round(this.zoom * 100)}%
                </span>
                <button id="zoom-in" title="Zoom in (Ctrl + wheel over the score)"
                        @click=${() => this.emit('zoom-step', { factor: ZOOM_STEP })}>+</button>
            </div>
            <div class="spacer"></div>
            <button class="mode-btn ${this.mode === 'play' ? 'play-mode' : ''}"
                    title="Toggle edit / play mode (click on the score seeks in play mode)"
                    @click=${() => this.toggleMode()}>
                ${this.mode === 'edit' ? 'Mode: Edit' : 'Mode: Play'}
            </button>
        `;
    }
}
