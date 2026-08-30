import { LitElement, html, css } from 'lit';
import { customElement, state, query } from 'lit/decorators.js';
import { CoreBridge } from '../bridge/coreBridge';
import { DocumentSnapshot } from '../models/document';
import { applyDocumentPatch, DocumentPatch } from '../bridge/documentPatch';
import type { ScoreTool, NoteDuration, ScoreMode, ScoreGrid } from './score/score-toolbar';
import type { EditAction, ScoreView } from './score/score-view';
import { pitchName } from '../models/pitch';
import { snapTicks } from '../models/snap';
import { clampZoom } from './score/score-view';

import './transport/transport-bar';
import './score/score-toolbar';
import './score/score-view';
import './shell/output-settings';
import './mixer/mixer-panel';
import './midi-events/midi-events-panel';
import './shell/status-bar';

@customElement('mc-app-root')
export class AppRoot extends LitElement {
    @state() private version = '';
    @state() private showAbout = false;
    @state() private showHelp = false;
    @state() private showOutputSettings = false;
    @state() private documents: DocumentSnapshot[] = [];
    @state() private activeDocumentId: number | null = null;
    
    @state() private mixerCollapsed = false;
    @state() private eventsCollapsed = false;
    @state() private activeTool: ScoreTool = 'select';
    @state() private activeDuration: NoteDuration = 'quarter';
    @state() private activeGrid: ScoreGrid = 'auto';
    /** Whether the grid above applies at all. Separate from the resolution, so
        turning snap off and back on returns to the grid that was in use. */
    @state() private snapEnabled = true;
    /** Triplet mode. Held here rather than in the toolbar because it changes the
        value being inserted, the grid it snaps to and the step the events panel
        uses, and those three must never be reading different answers. */
    @state() private tripletMode = false;
    @state() private zoom = 1;
    @state() private showRuler = true;
    @state() private activeMode: ScoreMode = 'edit';
    /** Which menu is dropped down, if any. */
    @state() private openMenu: 'file' | 'edit' | 'help' | null = null;

    // ── Transpose dialog ─────────────────────────────────────────────────────
    // The selection cannot change while the dialog is up, so its pitch span is
    // read once on opening and the preview works off that.
    @state() private transposeRange: { min: number; max: number; count: number } | null = null;
    @state() private transposeUnit: 'semitones' | 'octaves' = 'semitones';
    @state() private transposeAmount = 1;

    @query('mc-score-view') private scoreView?: ScoreView;

    static styles = [
        css`
            :host {
                display: flex;
                flex-direction: column;
                height: 100vh;
                background-color: #1e1e1e;
                color: #d4d4d4;
                font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif;
            }
            nav {
                background-color: #333;
                display: flex;
                align-items: center;
                height: 35px;
                font-size: 0.9rem;
                border-bottom: 1px solid #222;
            }
            .menu-container {
                display: flex;
                height: 100%;
                border-right: 1px solid #444;
                margin-right: 0.5rem;
            }
            .menu-item {
                padding: 0 0.75rem;
                cursor: pointer;
                height: 100%;
                display: flex;
                align-items: center;
            }
            .menu-item:hover, .menu-item.open {
                background-color: #444;
            }
            .menu-root { position: relative; height: 100%; }
            /* Catches the click that dismisses the menu, without a global listener. */
            .menu-backdrop {
                position: fixed;
                inset: 0;
                z-index: 900;
            }
            .menu-dropdown {
                position: absolute;
                top: 100%;
                left: 0;
                z-index: 901;
                min-width: 190px;
                padding: 4px 0;
                background: #2d2d2d;
                border: 1px solid #454545;
                border-radius: 3px;
                box-shadow: 0 6px 18px rgba(0, 0, 0, 0.55);
            }
            .dropdown-item {
                display: flex;
                align-items: center;
                justify-content: space-between;
                gap: 24px;
                padding: 4px 12px;
                cursor: pointer;
                white-space: nowrap;
            }
            .dropdown-item:hover { background: #04395e; color: #fff; }
            .dropdown-item.disabled {
                color: #666;
                cursor: default;
            }
            .dropdown-item.disabled:hover { background: transparent; }
            .dropdown-item .accel { color: #7a7a7a; font-size: 0.8rem; }
            .dropdown-item.disabled .accel { color: #555; }
            .dropdown-sep {
                height: 1px;
                margin: 4px 6px;
                background: #454545;
            }
            .tabs-container {
                display: flex;
                height: 100%;
                overflow-x: auto;
            }
            .tab {
                padding: 0 1rem;
                height: 100%;
                display: flex;
                align-items: center;
                background-color: #2d2d2d;
                border-right: 1px solid #1e1e1e;
                cursor: pointer;
                min-width: 120px;
                max-width: 200px;
                position: relative;
            }
            .tab.active {
                background-color: #1e1e1e;
                color: #fff;
            }
            .tab .title {
                flex: 1;
                white-space: nowrap;
                overflow: hidden;
                text-overflow: ellipsis;
                margin-right: 0.5rem;
            }
            .tab .close-btn {
                font-size: 1.2rem;
                line-height: 1;
                padding: 0 4px;
                border-radius: 3px;
            }
            .tab .close-btn:hover {
                background-color: #444;
            }
            main {
                flex: 1;
                display: flex;
                flex-direction: column;
                overflow: hidden;
            }
            .empty-state {
                flex: 1;
                display: flex;
                flex-direction: column;
                justify-content: center;
                align-items: center;
                font-size: 1.5rem;
                color: #444;
            }
            .modal-overlay {
                position: fixed;
                top: 0;
                left: 0;
                right: 0;
                bottom: 0;
                background: rgba(0,0,0,0.7);
                display: flex;
                justify-content: center;
                align-items: center;
                z-index: 1000;
            }
            .modal-content {
                background: #2d2d2d;
                padding: 2rem;
                border-radius: 4px;
                min-width: 300px;
                box-shadow: 0 4px 15px rgba(0,0,0,0.5);
            }
            .modal-footer {
                margin-top: 1.5rem;
                text-align: right;
            }
            button {
                padding: 0.4rem 1rem;
                cursor: pointer;
                background: #007acc;
                color: white;
                border: none;
                border-radius: 2px;
            }
            button:hover:not(:disabled) {
                background: #0062a3;
            }
            button:disabled {
                background: #3a3a3a;
                color: #777;
                cursor: default;
            }

            /* ── Transpose dialog ─────────────────────────────────────────── */
            .modal-content.transpose { min-width: 340px; }
            .modal-content.transpose h3 { margin: 0 0 0.6rem; }
            .transpose .subject { color: #bbb; margin: 0 0 1.1rem; font-size: 0.9rem; }
            .transpose-row {
                display: flex;
                align-items: center;
                gap: 8px;
            }
            .transpose-row label { color: #8a8a8a; font-size: 0.9rem; }
            .transpose-row input, .transpose-row select {
                background: #1e1e1e;
                border: 1px solid #4a4a4a;
                color: #ddd;
                font: inherit;
                padding: 4px 6px;
                border-radius: 2px;
            }
            .transpose-row input { width: 72px; text-align: right; }
            .transpose-row input:focus, .transpose-row select:focus {
                outline: none;
                border-color: #007acc;
            }
            .transpose .hint { color: #6f6f6f; font-size: 0.78rem; margin: 0.45rem 0 0; }
            .transpose .preview {
                margin: 1rem 0 0;
                font-size: 0.9rem;
                color: #bbb;
                min-height: 1.2em;
            }
            .transpose .preview .delta { color: #6f6f6f; }
            .transpose .preview.bad { color: #ff8080; }
            .transpose .modal-footer { display: flex; gap: 8px; justify-content: flex-end; }
            .transpose button.secondary { background: #3a3a3a; }
            .transpose button.secondary:hover { background: #474747; }

            /* Layout Grid */
            .main-layout {
                display: grid;
                grid-template-areas:
                    "transport transport transport"
                    "toolbar toolbar toolbar"
                    "score score mixer"
                    "events events mixer";
                grid-template-rows: auto auto 1fr 200px;
                grid-template-columns: 1fr 1fr 300px;
                height: 100%;
                overflow: hidden;
            }

            .main-layout.mixer-collapsed {
                grid-template-areas:
                    "transport transport"
                    "toolbar toolbar"
                    "score score"
                    "events events";
                grid-template-columns: 1fr 1fr 0px;
            }

            .main-layout.events-collapsed {
                grid-template-rows: auto auto 1fr 0px;
            }

            .transport-area { grid-area: transport; border-bottom: 1px solid #111; }
            .toolbar-area { grid-area: toolbar; border-bottom: 1px solid #111; }
            .score-area { grid-area: score; overflow: auto; border-right: 1px solid #333; position: relative; }
            .mixer-area { grid-area: mixer; background: #252526; border-left: 1px solid #333; display: flex; flex-direction: column; overflow: hidden; }
            .events-area { grid-area: events; background: #252526; border-top: 1px solid #333; overflow: hidden; display: flex; flex-direction: column; }

            .panel-header {
                background: #37373d;
                padding: 4px 8px;
                font-size: 0.75rem;
                text-transform: uppercase;
                font-weight: bold;
                display: flex;
                justify-content: space-between;
                align-items: center;
                user-select: none;
            }

            .collapse-btn { cursor: pointer; opacity: 0.6; }
            .collapse-btn:hover { opacity: 1; }
        `
    ];

    private refreshTimer: number | undefined;

    connectedCallback() {
        super.connectedCallback();
        // Capture phase: the score view also listens for Escape (to clear its
        // selection) and registers first, so bubbling would reach it either way.
        window.addEventListener('keydown', this.handleMenuKey, true);
    }

    disconnectedCallback() {
        super.disconnectedCallback();
        window.removeEventListener('keydown', this.handleMenuKey, true);
    }

    private handleMenuKey = (e: KeyboardEvent) => {
        if (this.openMenu === null || e.key !== 'Escape') return;
        e.preventDefault();
        e.stopPropagation();
        this.openMenu = null;
    };

    async firstUpdated() {
        // Wait for the native bridge to be injected
        const isNative = await CoreBridge.waitForBridge(2000);
        console.log(`[AppRoot] Bridge initialization finished. Native: ${isNative}`);

        // Every committed mutation arrives here as an incremental patch.
        CoreBridge.on('document_patched', (patch: DocumentPatch) => this.handlePatch(patch));

        try {
            this.version = await CoreBridge.sendCommand<string>('get_version');
            await this.refreshDocuments();
        } catch (e) {
            console.error('Failed to initialize AppRoot', e);
        }
    }

    // Coalesce bursts of change notifications into one snapshot fetch.
    private scheduleRefresh() {
        if (this.refreshTimer !== undefined) return;
        this.refreshTimer = window.setTimeout(() => {
            this.refreshTimer = undefined;
            this.refreshDocuments();
        }, 100);
    }

    // ─── Incremental mirror updates ───────────────────────────────────────────

    /**
     * Applies a core patch to the mirrored document. Falls back to re-snapshotting
     * that one document when the patch cannot be applied — a structural change, a
     * revision gap, or an unknown change kind. Whole-snapshot fetches are the
     * exception now, not the rule.
     */
    private handlePatch(patch: DocumentPatch) {
        const index = this.documents.findIndex(d => d.id === patch.documentId);
        if (index < 0) {
            // A document we don't mirror yet (just created by open/import).
            this.scheduleRefresh();
            return;
        }

        const patched = applyDocumentPatch(this.documents[index], patch);
        if (!patched) {
            this.resyncDocument(patch.documentId);
            return;
        }

        const documents = this.documents.slice();
        documents[index] = patched;
        this.documents = documents;
    }

    /** Re-fetches a single document's snapshot, leaving the other tabs alone. */
    private async resyncDocument(id: number) {
        try {
            const snap = await CoreBridge.sendCommand<DocumentSnapshot>('get_document_snapshot', { id });
            if (!snap) { await this.refreshDocuments(); return; }
            const index = this.documents.findIndex(d => d.id === id);
            if (index < 0) { await this.refreshDocuments(); return; }
            const documents = this.documents.slice();
            documents[index] = snap;
            this.documents = documents;
        } catch (e) {
            console.error('Failed to resync document', id, e);
            await this.refreshDocuments();
        }
    }

    async refreshDocuments() {
        try {
            const ids = await CoreBridge.sendCommand<number[]>('get_open_documents');
            const snapshots: DocumentSnapshot[] = [];
            for (const id of ids) {
                const snap = await CoreBridge.sendCommand<DocumentSnapshot>('get_document_snapshot', { id });
                snapshots.push(snap);
            }
            this.documents = snapshots;

            if (snapshots.length === 0) {
                this.activeDocumentId = null;
            } else if (this.activeDocumentId === null ||
                       !snapshots.some(d => d.id === this.activeDocumentId)) {
                // The active document was closed (or never set): fall back to
                // the first open one instead of showing the empty state.
                this.activeDocumentId = snapshots[0].id;
            }
        } catch (e) {
            console.error('Failed to refresh documents', e);
        }
    }

    render() {
        const activeDoc = this.documents.find(d => d.id === this.activeDocumentId);

        return html`
            <nav>
                <div class="menu-container">
                    ${this.renderMenu('file', 'File')}
                    ${this.renderMenu('edit', 'Edit')}
                    ${this.renderMenu('help', 'Help')}
                </div>
                <div class="tabs-container">
                    ${this.documents.map(doc => html`
                        <div class="tab ${doc.id === this.activeDocumentId ? 'active' : ''}" 
                             @click=${() => this.handleTabClick(doc.id)}>
                            <span class="title">${doc.title}${doc.dirty ? ' *' : ''}</span>
                            <span class="close-btn" @click=${(e: Event) => this.handleClose(e, doc.id)}>&times;</span>
                        </div>
                    `)}
                </div>
            </nav>
            <main>
                ${activeDoc ? this.renderDocument(activeDoc) : this.renderEmptyState()}
            </main>
            
            <mc-status-bar .version=${this.version}
                    @open-output-settings=${() => { this.showOutputSettings = true; }}></mc-status-bar>

            ${this.showAbout ? this.renderAbout() : ''}
            ${this.showHelp ? this.renderHelp() : ''}
            ${this.showOutputSettings
                ? html`<mc-output-settings
                        @close=${() => { this.showOutputSettings = false; }}></mc-output-settings>`
                : ''}
            ${this.transposeRange ? this.renderTranspose() : ''}
        `;
    }

    // ─── Menu bar ─────────────────────────────────────────────────────────────

    private renderMenu(id: 'file' | 'edit' | 'help', label: string) {
        const open = this.openMenu === id;
        return html`
            <div class="menu-root">
                <div class="menu-item ${open ? 'open' : ''}"
                     @click=${() => { this.openMenu = open ? null : id; }}>${label}</div>
                ${open ? html`
                    <div class="menu-backdrop" @click=${() => { this.openMenu = null; }}></div>
                    <div class="menu-dropdown">${this.renderMenuItems(id)}</div>
                ` : ''}
            </div>
        `;
    }

    private renderMenuItems(id: 'file' | 'edit' | 'help') {
        const item = (label: string, run: () => void, enabled = true, accel = '') => html`
            <div class="dropdown-item ${enabled ? '' : 'disabled'}"
                 @click=${() => { if (!enabled) return; this.openMenu = null; run(); }}>
                <span>${label}</span>${accel ? html`<span class="accel">${accel}</span>` : ''}
            </div>`;
        const separator = html`<div class="dropdown-sep"></div>`;

        if (id === 'file') {
            const hasDoc = this.activeDocumentId !== null;
            return html`
                ${item('New', () => this.handleNew(), true, 'Ctrl+N')}
                ${item('Open…', () => this.handleOpen())}
                ${separator}
                ${item('Save', () => this.handleSave(false), hasDoc, 'Ctrl+S')}
                ${item('Save As…', () => this.handleSave(true), hasDoc)}
                ${separator}
                ${item('Output Settings…', () => { this.showOutputSettings = true; })}
                ${separator}
                ${item('Import MIDI…', () => this.handleImportMidi())}
                ${item('Export MIDI…', () => this.handleExportMidi(), hasDoc)}
                ${separator}
                ${item('Exit', () => this.handleExit())}
            `;
        }

        if (id === 'help') {
            return html`
                ${item('Contents', () => { this.showHelp = true; })}
                ${item('About', () => { this.showAbout = true; })}
            `;
        }

        // Edit. Enablement is read off the score view as the menu opens, so it
        // reflects the live selection and clipboard rather than a stale copy.
        const doc = this.documents.find(d => d.id === this.activeDocumentId);
        const view = this.scoreView;
        const editing = this.activeMode === 'edit';
        const selected = editing && !!view?.hasSelection;
        const act = (a: EditAction) => () => view?.runEditAction(a);
        return html`
            ${item('Undo', act('undo'), editing && !!doc?.canUndo, 'Ctrl+Z')}
            ${item('Redo', act('redo'), editing && !!doc?.canRedo, 'Ctrl+Y')}
            ${separator}
            ${item('Cut', act('cut'), selected, 'Ctrl+X')}
            ${item('Copy', act('copy'), selected, 'Ctrl+C')}
            ${item('Paste', act('paste'), editing && !!view?.canPaste, 'Ctrl+V')}
            ${item('Delete', act('delete'), selected, 'Del')}
            ${separator}
            ${item('Select All', act('selectAll'), editing, 'Ctrl+A')}
            ${separator}
            ${item('Transpose…', () => this.openTranspose(), selected)}
        `;
    }

    renderEmptyState() {
        return html`
            <div class="empty-state">
                <p>No document open</p>
                <button @click=${() => this.handleNew()}>Create New Project</button>
            </div>
        `;
    }

    renderDocument(doc: DocumentSnapshot) {
        return html`
            <div class="main-layout ${this.mixerCollapsed ? 'mixer-collapsed' : ''} ${this.eventsCollapsed ? 'events-collapsed' : ''}"
                 @document-updated=${() => this.refreshDocuments()}>
                <div class="transport-area">
                    <mc-transport-bar .documentId=${doc.id} .doc=${doc}></mc-transport-bar>
                </div>
                <div class="toolbar-area">
                    <mc-score-toolbar
                        .canUndo=${doc.canUndo}
                        .canRedo=${doc.canRedo}
                        .snapEnabled=${this.snapEnabled}
                        .tripletMode=${this.tripletMode}
                        .showRuler=${this.showRuler}
                        .zoom=${this.zoom}
                        @tool-change=${(e: CustomEvent) => { this.activeTool = e.detail.tool; }}
                        @duration-change=${(e: CustomEvent) => { this.activeDuration = e.detail.duration; }}
                        @grid-change=${(e: CustomEvent) => { this.activeGrid = e.detail.grid; }}
                        @snap-toggle=${(e: CustomEvent) => { this.snapEnabled = e.detail.enabled; }}
                        @triplet-toggle=${(e: CustomEvent) => { this.tripletMode = e.detail.triplet; }}
                        @ruler-toggle=${(e: CustomEvent) => { this.showRuler = e.detail.showRuler; }}
                        @zoom-step=${(e: CustomEvent) => { this.zoom = clampZoom(this.zoom * e.detail.factor); }}
                        @zoom-set=${(e: CustomEvent) => { this.zoom = clampZoom(e.detail.zoom); }}
                        @mode-change=${(e: CustomEvent) => { this.activeMode = e.detail.mode; }}
                        @score-undo=${() => this.handleUndo()}
                        @score-redo=${() => this.handleRedo()}
                        @add-track=${() => this.handleAddTrack()}>
                    </mc-score-toolbar>
                </div>
                <div class="score-area">
                    <mc-score-view
                        .doc=${doc}
                        .tool=${this.activeTool}
                        .duration=${this.activeDuration}
                        .grid=${this.activeGrid}
                        .snapEnabled=${this.snapEnabled}
                        .triplet=${this.tripletMode}
                        .showRuler=${this.showRuler}
                        .zoom=${this.zoom}
                        .mode=${this.activeMode}
                        @zoom-change=${(e: CustomEvent) => { this.zoom = e.detail.zoom; }}>
                    </mc-score-view>
                </div>
                <div class="mixer-area">
                    <div class="panel-header">
                        Mixer
                        <span class="collapse-btn" @click=${() => this.mixerCollapsed = !this.mixerCollapsed}>
                            ${this.mixerCollapsed ? '«' : '»'}
                        </span>
                    </div>
                    ${!this.mixerCollapsed ? html`<mc-mixer-panel .doc=${doc}></mc-mixer-panel>` : ''}
                </div>
                <div class="events-area">
                    <div class="panel-header">
                        MIDI Events
                        <span class="collapse-btn" @click=${() => this.eventsCollapsed = !this.eventsCollapsed}>
                            ${this.eventsCollapsed ? '▲' : '▼'}
                        </span>
                    </div>
                    ${!this.eventsCollapsed ? html`<mc-midi-events-panel .doc=${doc}
                        .snapTicks=${snapTicks(this.snapEnabled, this.activeGrid,
                                               this.activeDuration, doc.ppqn,
                                               this.tripletMode)}></mc-midi-events-panel>` : ''}
                </div>
            </div>
        `;
    }

    // ─── Transpose ────────────────────────────────────────────────────────────

    private openTranspose() {
        // Nothing selected means nothing to transpose, and the menu item is
        // disabled in that case — this is the guard for every other route in.
        const range = this.scoreView?.selectionPitchRange ?? null;
        if (!range) return;
        this.transposeRange = range;
        // autofocus only applies to markup present at page load, so put the
        // caret in the amount field once Lit has rendered the dialog.
        this.updateComplete.then(() => {
            const input = this.shadowRoot?.getElementById('transpose-amount') as HTMLInputElement | null;
            input?.focus();
            input?.select();
        });
    }

    private closeTranspose() {
        this.transposeRange = null;
    }

    /** The shift in semitones, which is what the core is told either way. */
    private get transposeSemitones(): number {
        const step = this.transposeUnit === 'octaves' ? 12 : 1;
        return Math.trunc(this.transposeAmount) * step;
    }

    private get transposeResult(): { min: number; max: number; fits: boolean } | null {
        const range = this.transposeRange;
        if (!range) return null;
        const min = range.min + this.transposeSemitones;
        const max = range.max + this.transposeSemitones;
        return { min, max, fits: min >= 0 && max <= 127 };
    }

    private async applyTranspose() {
        const result = this.transposeResult;
        if (!result?.fits || this.transposeSemitones === 0) return;
        const view = this.scoreView;
        this.closeTranspose();
        await view?.transposeSelection(this.transposeSemitones);
    }

    renderTranspose() {
        const range = this.transposeRange!;
        const result = this.transposeResult!;
        const semitones = this.transposeSemitones;
        // The limit is what still fits: an octave step past the edge of the MIDI
        // range is not a useful thing to offer.
        const limit = this.transposeUnit === 'octaves' ? 10 : 127;

        return html`
            <div class="modal-overlay" @click=${() => this.closeTranspose()}>
                <div class="modal-content transpose" @click=${(e: Event) => e.stopPropagation()}
                     @keydown=${(e: KeyboardEvent) => {
                         // The dialog owns these keys; the score view listens on
                         // window and would otherwise act on them too.
                         e.stopPropagation();
                         if (e.key === 'Enter') { e.preventDefault(); this.applyTranspose(); }
                         if (e.key === 'Escape') { e.preventDefault(); this.closeTranspose(); }
                     }}>
                    <h3>Transpose</h3>
                    <p class="subject">
                        ${range.count} ${range.count === 1 ? 'note' : 'notes'} selected —
                        ${range.min === range.max
                            ? pitchName(range.min)
                            : `${pitchName(range.min)} to ${pitchName(range.max)}`}
                    </p>

                    <div class="transpose-row">
                        <label for="transpose-amount">By</label>
                        <input id="transpose-amount" type="number"
                               .value=${String(this.transposeAmount)}
                               min=${-limit} max=${limit} step="1"
                               @input=${(e: Event) => {
                                   // Only commit a parseable value: a lone "-"
                                   // on the way to "-5" would otherwise be
                                   // rewritten to 0 under the user's cursor.
                                   const raw = parseInt((e.target as HTMLInputElement).value, 10);
                                   if (Number.isFinite(raw)) this.transposeAmount = raw;
                               }}>
                        <select id="transpose-unit"
                                @change=${(e: Event) => {
                                    this.transposeUnit =
                                        (e.target as HTMLSelectElement).value as 'semitones' | 'octaves';
                                }}>
                            <option value="semitones"
                                ?selected=${this.transposeUnit === 'semitones'}>semitones</option>
                            <option value="octaves"
                                ?selected=${this.transposeUnit === 'octaves'}>octaves</option>
                        </select>
                    </div>
                    <p class="hint">Negative values transpose down.</p>

                    <p class="preview ${result.fits ? '' : 'bad'}">
                        ${semitones === 0
                            ? 'No change.'
                            : result.fits
                                ? html`Result:
                                       ${result.min === result.max
                                           ? pitchName(result.min)
                                           : `${pitchName(result.min)} to ${pitchName(result.max)}`}
                                       <span class="delta">(${semitones > 0 ? '+' : ''}${semitones} st)</span>`
                                : `Out of range — a note would land outside MIDI 0-127.`}
                    </p>

                    <div class="modal-footer">
                        <button class="secondary" @click=${() => this.closeTranspose()}>Cancel</button>
                        <button ?disabled=${!result.fits || semitones === 0}
                                @click=${() => this.applyTranspose()}>OK</button>
                    </div>
                </div>
            </div>
        `;
    }

    renderAbout() {
        return html`
            <div class="modal-overlay" @click=${() => this.showAbout = false}>
                <div class="modal-content" @click=${(e: Event) => e.stopPropagation()}>
                    <h3>About MIDI Composer</h3>
                    <p>Version: ${this.version}</p>
                    <p>A modern MIDI editor with a C++ core.</p>
                    <div class="modal-footer">
                        <button @click=${() => this.showAbout = false}>Close</button>
                    </div>
                </div>
            </div>
        `;
    }

    renderHelp() {
        return html`
            <div class="modal-overlay" @click=${() => this.showHelp = false}>
                <div class="modal-content" @click=${(e: Event) => e.stopPropagation()}>
                    <h3>Help Contents</h3>
                    <ul>
                        <li><b>File -> New</b>: Create a new project.</li>
                        <li><b>File -> Exit</b>: Close the application.</li>
                        <li><b>Help -> About</b>: Version information.</li>
                    </ul>
                    <div class="modal-footer">
                        <button @click=${() => { this.showAbout = true; this.showHelp = false; }}>About</button>
                        <button @click=${() => this.showHelp = false}>Close</button>
                    </div>
                </div>
            </div>
        `;
    }

    async handleNew() {
        try {
            const result = await CoreBridge.sendCommand<{id: number}>('new_project');
            await this.refreshDocuments();
            this.activeDocumentId = result.id;
        } catch (e) {
            console.error('Failed to create new project', e);
        }
    }

    async handleClose(e: Event, id: number) {
        e.stopPropagation();
        try {
            await CoreBridge.sendCommand('close_project', { id });
            await this.refreshDocuments();
        } catch (e) {
            console.error('Failed to close project', e);
        }
    }

    async handleTabClick(id: number) {
        this.activeDocumentId = id;
        try {
            await CoreBridge.sendCommand('set_active_document', { id });
        } catch (e) {
            console.error('Failed to set active document', e);
        }
    }

    async handleOpen() {
        try {
            const result = await CoreBridge.sendCommand<{id?: number, cancelled?: boolean}>('open_project');
            if (result.cancelled) return;
            await this.refreshDocuments();
            if (result.id !== undefined) this.activeDocumentId = result.id;
        } catch (e) {
            console.error('Failed to open project', e);
            alert(`Failed to open project: ${e}`);
        }
    }

    async handleSave(saveAs: boolean) {
        if (this.activeDocumentId === null) return;
        try {
            await CoreBridge.sendCommand(saveAs ? 'save_project_as' : 'save_project', { id: this.activeDocumentId });
            await this.refreshDocuments();
        } catch (e) {
            console.error('Failed to save project', e);
            alert(`Failed to save project: ${e}`);
        }
    }

    async handleImportMidi() {
        try {
            const result = await CoreBridge.sendCommand<{id?: number, cancelled?: boolean}>('import_midi');
            if (result.cancelled) return;
            await this.refreshDocuments();
            if (result.id !== undefined) this.activeDocumentId = result.id;
        } catch (e) {
            console.error('Failed to import MIDI', e);
            alert(`Failed to import MIDI: ${e}`);
        }
    }

    async handleExportMidi() {
        if (this.activeDocumentId === null) return;
        try {
            await CoreBridge.sendCommand('export_midi', { id: this.activeDocumentId });
        } catch (e) {
            console.error('Failed to export MIDI', e);
            alert(`Failed to export MIDI: ${e}`);
        }
    }

    async handleUndo() {
        if (this.activeDocumentId === null) return;
        try {
            await CoreBridge.sendCommand('undo', { documentId: this.activeDocumentId });
        } catch { /* nothing to undo */ }
        await this.refreshDocuments();
    }

    async handleRedo() {
        if (this.activeDocumentId === null) return;
        try {
            await CoreBridge.sendCommand('redo', { documentId: this.activeDocumentId });
        } catch { /* nothing to redo */ }
        await this.refreshDocuments();
    }

    async handleAddTrack() {
        if (this.activeDocumentId === null) return;
        try {
            await CoreBridge.sendCommand('create_track', { documentId: this.activeDocumentId });
            await this.refreshDocuments();
        } catch (e) {
            console.error('Failed to add track', e);
        }
    }

    async handleExit() {
        if (confirm('Are you sure you want to exit?')) {
            await CoreBridge.sendCommand('exit_application');
        }
    }
}
