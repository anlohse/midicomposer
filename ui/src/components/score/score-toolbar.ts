import { LitElement, html, css } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';

export type ScoreTool = 'select' | 'insert' | 'erase' | 'resize';
export type NoteDuration = 'quarter' | 'eighth' | 'sixteenth';
export type ScoreMode = 'edit' | 'play';

// Snap resolution. 'auto' follows the selected note duration.
export type ScoreGrid = 'auto' | '1/4' | '1/8' | '1/16' | '1/32' | 'off';

const GRID_OPTIONS: Array<{ value: ScoreGrid; label: string }> = [
    { value: 'auto', label: 'Auto (note value)' },
    { value: '1/4',  label: '1/4' },
    { value: '1/8',  label: '1/8' },
    { value: '1/16', label: '1/16' },
    { value: '1/32', label: '1/32' },
    { value: 'off',  label: 'Off' },
];

@customElement('mc-score-toolbar')
export class ScoreToolbar extends LitElement {
    @property({ type: Boolean }) canUndo = false;
    @property({ type: Boolean }) canRedo = false;

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

    private emit(name: string) {
        this.dispatchEvent(new CustomEvent(name, { bubbles: true, composed: true }));
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
                <label for="grid">Snap</label>
                <select id="grid" ?disabled=${!editing}
                        title="Grid the insert / move / resize gestures snap to"
                        @change=${(e: Event) => this.selectGrid(e)}>
                    ${GRID_OPTIONS.map(o => html`
                        <option value=${o.value} ?selected=${this.activeGrid === o.value}>${o.label}</option>
                    `)}
                </select>
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
