import { LitElement, html, css } from 'lit';
import { customElement, property, state, query } from 'lit/decorators.js';
import { repeat } from 'lit/directives/repeat.js';
import { DocumentSnapshot, TrackSnapshot } from '../../models/document';
import { CoreBridge } from '../../bridge/coreBridge';
import { loadOutputInfo, loadedFileName, type OutputChoice } from '../shell/output-settings';
import { pitchName } from '../../models/pitch';
import { GM_FAMILIES, gmProgramName, isPercussionChannel } from '../../models/gmPrograms';
import { CLEFS, CLEF_ORDER, clefDef } from '../../models/clef';
import { BEND_MAX, BEND_MIN, DEFAULT_CONTROLLER, DEFAULT_CONTROLLER_VALUE,
         bendLabel, controllerName } from '../../models/midiController';
import { fieldCoarseStep, fieldStep } from '../../models/snap';
import '../common/value-field';

type EventKind = 'Note' | 'CC' | 'PitchBend' | 'ProgramChange';

interface PanelEvent {
    kind: EventKind;
    tick: number;
    trackId: string;
    trackName: string;
    trackIdx: number;
    // Note only
    noteId?: string;
    pitch?: number;
    velocity?: number;
    durationTicks?: number;
    // CC / pitch bend / program change. `eventId` is the core's id for the
    // event, which is what makes these rows addressable for editing.
    eventId?: string;
    controller?: number;
    value?: number;
    program?: number;
}

const ALL_TRACKS = -1;

@customElement('mc-midi-events-panel')
export class MidiEventsPanel extends LitElement {
    @property({ type: Object }) doc?: DocumentSnapshot;
    /**
     * The score view's snap step in ticks, 0 when snapping is off. Tick and
     * duration fields step by the same amount the score does, so the two ways
     * of moving an event agree — and with snapping off they step by one tick,
     * which is the only way to reach a position the score can now place.
     */
    @property({ type: Number }) snapTicks = 0;

    // Which track's events are listed, and whose parameters the header edits.
    @state() private trackFilter = ALL_TRACKS;

    @query('#instrument') private instrumentSelect?: HTMLSelectElement;
    @query('#clef') private clefSelect?: HTMLSelectElement;

    // Which field held focus going into this update, so it can be restored if
    // the update moved its row. See restoreFocus().
    private focusMemo: { key: string; index: number } | null = null;

    willUpdate() {
        this.focusMemo = this.findFocusedCell();
    }

    updated() {
        this.syncInstrumentSelect();
        this.restoreFocus();
    }

    // Once a <select> has a user-set value, re-rendering its <option> list with
    // different `selected` attributes does not move the selection. Undo/redo and
    // any other external change therefore need an explicit resync, or the
    // dropdown keeps showing the instrument the user last picked by hand.
    private syncInstrumentSelect() {
        const track = this.selectedTrack;
        if (!track) return;
        if (this.instrumentSelect) {
            const want = String(this.trackProgram(track));
            if (this.instrumentSelect.value !== want) this.instrumentSelect.value = want;
        }
        if (this.clefSelect) {
            const want = clefDef(track.clef).name;
            if (this.clefSelect.value !== want) this.clefSelect.value = want;
        }
    }

    private findFocusedCell(): { key: string; index: number } | null {
        const active = this.shadowRoot?.activeElement as HTMLElement | null;
        if (!active || active.tagName !== 'MC-VALUE-FIELD') return null;
        const row = active.closest('tr');
        const key = row?.dataset.key;
        if (!row || !key) return null;
        return { key, index: [...row.querySelectorAll('mc-value-field')].indexOf(active) };
    }

    // Editing a tick re-sorts the table, and relocating a row in the DOM blurs
    // whatever it contains — so the caret would be dropped after every step that
    // crossed another event. Put it back on the same field of the same event.
    private restoreFocus() {
        const memo = this.focusMemo;
        this.focusMemo = null;
        if (!memo || memo.index < 0) return;

        const row = this.shadowRoot?.querySelector(`tr[data-key="${memo.key}"]`);
        const field = row?.querySelectorAll('mc-value-field')[memo.index] as
            (HTMLElement & { shadowRoot: ShadowRoot | null }) | undefined;
        const input = field?.shadowRoot?.querySelector('input');
        if (input && field!.shadowRoot!.activeElement !== input) input.focus();
    }

    static styles = css`
        :host {
            display: flex;
            flex-direction: column;
            height: 100%;
            background: #1e1e1e;
            min-height: 0;
        }

        /* ── Track parameter bar ─────────────────────────────────────────── */
        .track-bar {
            display: flex;
            align-items: center;
            gap: 10px;
            padding: 5px 10px;
            background: #252526;
            border-bottom: 1px solid #333;
            font-size: 0.78rem;
            color: #bbb;
            flex-wrap: wrap;
        }
        .track-bar label { color: #8a8a8a; }
        .field-group { display: flex; align-items: center; gap: 4px; }
        select, input.name {
            background: #3c3c3c;
            border: 1px solid #4a4a4a;
            color: #ddd;
            font: inherit;
            padding: 2px 4px;
            border-radius: 2px;
        }
        input.name { width: 140px; }
        input.name:focus, select:focus { outline: none; border-color: #007acc; }
        select.instrument { max-width: 230px; }
        select.clef { max-width: 190px; }
        .chan-field { width: 46px; }
        .hint {
            color: #6f6f6f;
            font-size: 0.72rem;
            margin-left: auto;
            white-space: nowrap;
        }
        kbd {
            background: #333;
            border: 1px solid #4a4a4a;
            border-radius: 2px;
            padding: 0 3px;
            font-family: inherit;
            font-size: 0.95em;
        }

        /* ── Event table ─────────────────────────────────────────────────── */
        .table-container { flex: 1; overflow-y: auto; min-height: 0; }
        table {
            width: 100%;
            border-collapse: collapse;
            font-size: 0.8rem;
            color: #ccc;
            text-align: left;
        }
        th {
            background: #2d2d2d;
            padding: 5px 10px;
            border-bottom: 1px solid #444;
            position: sticky;
            top: 0;
            z-index: 10;
            color: #eee;
            font-weight: 600;
            white-space: nowrap;
        }
        td {
            padding: 2px 10px;
            border-bottom: 1px solid #2a2a2a;
            white-space: nowrap;
        }
        tr:hover { background: #262829; }
        tr:hover td { color: #fff; }
        .type-note { color: #4ec9b0; }
        .type-cc   { color: #dcdcaa; }
        .type-pb   { color: #c586c0; }
        .type-pc   { color: #569cd6; }
        .read-only { color: #8a8a8a; }
        .empty { padding: 16px; color: #666; font-size: 0.85rem; }

        /* ── Add / delete ────────────────────────────────────────────────── */
        .add-group { gap: 6px; }
        button.add {
            background: #333;
            border: 1px solid #4a4a4a;
            color: #ddd;
            font: inherit;
            padding: 2px 8px;
            border-radius: 2px;
            cursor: pointer;
        }
        button.add:hover:not(:disabled) { background: #3d3d3d; border-color: #007acc; }
        button.add:disabled { color: #666; cursor: default; }
        .source { color: #8a8a8a; font-size: 0.9em; }
        button.listen {
            background: #333;
            border: 1px solid #4a4a4a;
            color: #ddd;
            font: inherit;
            padding: 2px 7px;
            border-radius: 2px;
            cursor: pointer;
            line-height: 1;
        }
        button.listen:hover:not(:disabled) { background: #3d3d3d; border-color: #007acc; }
        button.listen:disabled { color: #666; cursor: default; }
        button.row-delete {
            background: none;
            border: none;
            color: #6f6f6f;
            font: inherit;
            cursor: pointer;
            padding: 0 4px;
            border-radius: 2px;
        }
        button.row-delete:hover { color: #ff8080; background: #3a2a2a; }

        col.c-tick  { width: 96px; }
        col.c-track { width: 130px; }
        col.c-type  { width: 110px; }
        col.c-d1    { width: 140px; }
        col.c-d2    { width: 110px; }
        col.c-dur   { width: 96px; }
        col.c-del   { width: 28px; }
    `;

    // ─── Event collection ────────────────────────────────────────────────────

    private collectEvents(): PanelEvent[] {
        if (!this.doc) return [];
        const events: PanelEvent[] = [];

        this.doc.tracks.forEach((track, trackIdx) => {
            if (this.trackFilter !== ALL_TRACKS && this.trackFilter !== trackIdx) return;
            const base = { trackId: track.id, trackName: track.name, trackIdx };

            for (const n of track.notes) {
                events.push({
                    ...base, kind: 'Note', tick: n.startTick, noteId: String(n.id),
                    pitch: n.pitch, velocity: n.velocity, durationTicks: n.durationTicks,
                });
            }
            for (const cc of track.controllerEvents) {
                events.push({ ...base, kind: 'CC', tick: cc.tick, eventId: String(cc.id),
                              controller: cc.controller, value: cc.value });
            }
            for (const pb of track.pitchBends) {
                events.push({ ...base, kind: 'PitchBend', tick: pb.tick, eventId: String(pb.id),
                              value: pb.value });
            }
            for (const pc of track.programChanges) {
                events.push({ ...base, kind: 'ProgramChange', tick: pc.tick, eventId: String(pc.id),
                              program: pc.program });
            }
        });

        events.sort((a, b) => a.tick - b.tick || a.trackIdx - b.trackIdx);
        return events;
    }

    // ─── Core commands ───────────────────────────────────────────────────────

    private async send(command: string, payload: Record<string, unknown>) {
        if (!this.doc) return;
        try {
            await CoreBridge.sendCommand(command, { documentId: this.doc.id, ...payload });
        } catch (err) {
            console.error(`${command} failed`, err);
        }
    }

    private editNote(ev: PanelEvent, field: 'tick' | 'duration' | 'pitch' | 'velocity', value: number) {
        const target = { trackId: parseInt(ev.trackId), noteId: parseInt(ev.noteId!) };
        switch (field) {
            case 'tick':     return this.send('move_note',   { ...target, tick: value });
            case 'duration': return this.send('resize_note', { ...target, duration: value });
            case 'pitch':    return this.send('update_note', { ...target, pitch: value });
            case 'velocity': return this.send('update_note', { ...target, velocity: value });
        }
    }

    // Only the changed field is sent; the core leaves the rest alone.
    private editEvent(ev: PanelEvent, patch: Record<string, number>) {
        const target = { trackId: parseInt(ev.trackId), eventId: parseInt(ev.eventId!) };
        const command = ev.kind === 'CC' ? 'update_controller_event' : 'update_pitch_bend';
        return this.send(command, { ...target, ...patch });
    }

    private deleteEvent(ev: PanelEvent) {
        const target = { trackId: parseInt(ev.trackId), eventId: parseInt(ev.eventId!) };
        const command = ev.kind === 'CC' ? 'delete_controller_event' : 'delete_pitch_bend';
        return this.send(command, target);
    }

    // Where a new event lands: one beat after the last one of its kind on the
    // track, or the start if there are none. Stacking them all on tick 0 would
    // make several identical rows that are impossible to tell apart, and the
    // tick is editable anyway.
    private nextTickFor(track: TrackSnapshot, kind: 'CC' | 'PitchBend'): number {
        const ticks = kind === 'CC'
            ? track.controllerEvents.map(e => e.tick)
            : track.pitchBends.map(e => e.tick);
        if (ticks.length === 0) return 0;
        return Math.max(...ticks) + (this.doc?.ppqn ?? 480);
    }

    private addEvent(kind: 'CC' | 'PitchBend') {
        const track = this.selectedTrack;
        if (!track) return;
        const trackId = parseInt(track.id);
        const tick = this.nextTickFor(track, kind);
        if (kind === 'CC') {
            return this.send('create_controller_event', {
                trackId, tick, controller: DEFAULT_CONTROLLER, value: DEFAULT_CONTROLLER_VALUE,
            });
        }
        // Centre: a bend of zero is audible as nothing, which is the right
        // starting point for something the user is about to shape.
        return this.send('create_pitch_bend', { trackId, tick, value: 0 });
    }

    // ─── Track parameter bar ─────────────────────────────────────────────────

    private get selectedTrack(): TrackSnapshot | undefined {
        if (!this.doc || this.trackFilter === ALL_TRACKS) return undefined;
        return this.doc.tracks[this.trackFilter];
    }

    // The track's instrument is the program change at tick 0; absent means the
    // MIDI default (program 0).
    private trackProgram(track: TrackSnapshot): number {
        return track.programChanges.find(pc => pc.tick === 0)?.program ?? 0;
    }

    private renderTrackBar() {
        const doc = this.doc!;
        const track = this.selectedTrack;

        return html`
            <div class="track-bar">
                <div class="field-group">
                    <label for="track-filter">Track</label>
                    <select id="track-filter" @change=${(e: Event) => {
                        this.trackFilter = parseInt((e.target as HTMLSelectElement).value);
                    }}>
                        <option value=${ALL_TRACKS} ?selected=${this.trackFilter === ALL_TRACKS}>All tracks</option>
                        ${doc.tracks.map((t, i) => html`
                            <option value=${i} ?selected=${this.trackFilter === i}>${t.name}</option>
                        `)}
                    </select>
                </div>

                ${track ? this.renderTrackParams(track) : html`
                    <span class="read-only">Pick a track to edit its name, channel and instrument.</span>
                `}

                <div class="field-group add-group">
                    <!-- Disabled with "All tracks" chosen: a new event has to go
                         on one track, and silently picking one would be worse
                         than saying so. -->
                    <button class="add" ?disabled=${!track}
                            title=${track ? 'Add a control change to this track'
                                          : 'Pick a track first'}
                            @click=${() => this.addEvent('CC')}>+ CC</button>
                    <button class="add" ?disabled=${!track}
                            title=${track ? 'Add a pitch bend to this track'
                                          : 'Pick a track first'}
                            @click=${() => this.addEvent('PitchBend')}>+ Pitch Bend</button>
                </div>

                <span class="hint">
                    Click a value, then <kbd>↑</kbd><kbd>↓</kbd> — hold <kbd>Shift</kbd> for bigger steps
                </span>
            </div>
        `;
    }

    private renderTrackParams(track: TrackSnapshot) {
        const trackId = parseInt(track.id);
        const percussion = isPercussionChannel(track.midiChannel);

        return html`
            <div class="field-group">
                <label for="track-name">Name</label>
                <input id="track-name" class="name" type="text" .value=${track.name}
                       @change=${(e: Event) => {
                           const name = (e.target as HTMLInputElement).value.trim();
                           if (name && name !== track.name) this.send('rename_track', { trackId, name });
                       }}>
            </div>

            <div class="field-group">
                <label>Ch</label>
                <mc-value-field class="chan-field" label="MIDI channel"
                    .value=${track.midiChannel + 1} .min=${1} .max=${16} .step=${1} .coarse=${4}
                    @value-change=${(e: CustomEvent<{ value: number }>) =>
                        this.send('set_track_channel', { trackId, channel: e.detail.value - 1 })}>
                </mc-value-field>
            </div>

            <div class="field-group">
                <label for="track-output">Output</label>
                <select id="track-output" class="clef"
                        title="Which output plays this track. The route belongs to its MIDI channel, so two tracks sharing a channel share it."
                        @change=${(e: Event) => this.send('set_track_output', {
                            trackId, outputId: (e.target as HTMLSelectElement).value,
                        })}>
                    <option value="" ?selected=${!track.outputId}>Project output</option>
                    ${this.availableOutputs.map(o => html`
                        <option value=${o.id} ?selected=${o.id === track.outputId}>${o.name}</option>`)}
                </select>
            </div>

            <div class="field-group">
                <label for="clef">Clef</label>
                <select id="clef" class="clef"
                        title="Staff clef for this track — display only, pitches are unchanged"
                        @change=${(e: Event) => this.send('set_track_clef', {
                            trackId, clef: (e.target as HTMLSelectElement).value,
                        })}>
                    ${CLEF_ORDER.map(name => html`
                        <option value=${name} ?selected=${clefDef(track.clef).name === name}>
                            ${CLEFS[name].label}
                        </option>`)}
                </select>
            </div>

            <div class="field-group">
                <label for="instrument">
                    Instrument${this.programSource
                        ? html`<span class="source" title="Where these instrument names come from"
                               > · ${this.programSource}</span>`
                        : ''}
                </label>
                <select id="instrument" class="instrument"
                        title=${this.programs.length
                            ? this.programs.some(p => p.name)
                                ? 'Instruments the selected output has loaded'
                                : 'This plugin decides what a program change means; it offers no names'
                            : percussion
                                ? 'Channel 10 is the percussion channel — programs select a drum kit'
                                : 'General MIDI program for this track'}
                        @change=${(e: Event) => this.send('set_track_program', {
                            trackId, program: parseInt((e.target as HTMLSelectElement).value),
                        })}>
                    ${this.programs.length
                        ? this.programs.map(({ program, name }) => html`
                            <option value=${program}
                                ?selected=${this.trackProgram(track) === program}
                            >${program + 1}${name ? `. ${name}` : ''}</option>`)
                        : GM_FAMILIES.map(family => html`
                            <optgroup label=${family.name}>
                                ${family.programs.map((name, i) => {
                                    const program = family.firstProgram + i;
                                    return html`<option value=${program}
                                        ?selected=${this.trackProgram(track) === program}>${program + 1}. ${name}</option>`;
                                })}
                            </optgroup>
                        `)}
                </select>
                <!-- Two dozen numbered unknowns in a ripped bank is what makes
                     this worth a button: picking one to find out what it is
                     otherwise means putting it on a track and playing. -->
                <button class="listen" title="Hear this instrument"
                        @click=${() => this.audition(track)}>&#9654;</button>
            </div>
        `;
    }

    /**
     * Sounds the track's instrument once, without changing anything.
     *
     * Middle C rather than the track's own notes: an audition is for comparing
     * instruments, and comparing them at different pitches compares the pitches.
     */
    private audition(track: TrackSnapshot) {
        void this.send('audition_program', { program: this.trackProgram(track) });
    }

    // ─── Rows ────────────────────────────────────────────────────────────────

    /** What can be routed to. Fixed for a build, so read once. */
    @state() private availableOutputs: OutputChoice[] = [];

    /**
     * The programs the selected output declares, or empty for General MIDI.
     *
     * Three cases, and they read differently: no entries means General MIDI --
     * a port, or the internal synth whose families really are the GM ones.
     * Entries with names are a sampler's bank. Entries without names are a
     * hosted plugin, which decides for itself what a program change means and
     * offers no way to ask, so the number is all there is to show.
     *
     * Re-read whenever the output changes, because loading a bank is exactly
     * what changes this.
     */
    @state() private programs: Array<{ program: number; name: string }> = [];

    /**
     * What the instrument names came from, shown beside the picker.
     *
     * Without this the list is 128 names with no provenance, and loading the
     * wrong bank looks exactly like the application being wrong: instruments
     * that should exist are missing, and the ones that remain sound like
     * something else. That cost an afternoon once.
     */
    @state() private programSource = '';

    async connectedCallback() {
        super.connectedCallback();
        const info = await loadOutputInfo();
        this.availableOutputs = info?.available ?? [];
        this.programSource = loadedFileName(info);
        await this.loadProgramNames();
        window.addEventListener('mc-output-changed', this.onOutputChanged);
    }

    disconnectedCallback() {
        window.removeEventListener('mc-output-changed', this.onOutputChanged);
        super.disconnectedCallback();
    }

    private onOutputChanged = () => {
        void this.loadProgramNames();
        void loadOutputInfo().then(info => { this.programSource = loadedFileName(info); });
    };

    private async loadProgramNames() {
        try {
            this.programs = await CoreBridge.sendCommand<
                Array<{ program: number; name: string }>>('get_program_names') ?? [];
        } catch (err) {
            // The General MIDI names are the fallback, and they are what was
            // being shown before this existed.
            console.error('Failed to read the instrument list', err);
            this.programs = [];
        }
    }

    private renderRow(ev: PanelEvent) {
        const ppqn = this.doc?.ppqn ?? 480;
        const tickStep = fieldStep(this.snapTicks);
        const coarseStep = fieldCoarseStep(this.snapTicks, ppqn);

        // A program change is the track's instrument, edited through the
        // selector in the bar above; editing it here as a raw number would give
        // two controls for one thing that could disagree.
        if (ev.kind === 'ProgramChange') {
            return html`
                <tr data-key=${this.eventKey(ev)}>
                    <td class="read-only">${ev.tick}</td>
                    <td class="read-only">${ev.trackName}</td>
                    <td class="type-pc">Program</td>
                    <td class="read-only">${ev.program! + 1}. ${gmProgramName(ev.program!)}</td>
                    <td></td>
                    <td></td>
                    <td></td>
                </tr>
            `;
        }

        if (ev.kind === 'CC' || ev.kind === 'PitchBend') {
            const isCC = ev.kind === 'CC';
            return html`
                <tr data-key=${this.eventKey(ev)}>
                    <td>
                        <mc-value-field label="Tick"
                            .value=${ev.tick} .min=${0} .max=${100_000_000}
                            .step=${tickStep} .coarse=${coarseStep}
                            @value-change=${(e: CustomEvent<{ value: number }>) =>
                                this.editEvent(ev, { tick: e.detail.value })}>
                        </mc-value-field>
                    </td>
                    <td class="read-only">${ev.trackName}</td>
                    <td class=${isCC ? 'type-cc' : 'type-pb'}>${isCC ? 'CC' : 'Pitch Bend'}</td>
                    <td>
                        ${isCC ? html`
                            <!-- The number is already in the field, so the
                                 formatter's suffix carries only the name. -->
                            <mc-value-field label="Controller number"
                                .value=${ev.controller!} .min=${0} .max=${127} .step=${1} .coarse=${8}
                                .formatter=${(n: number) => controllerName(n) ?? ''}
                                @value-change=${(e: CustomEvent<{ value: number }>) =>
                                    this.editEvent(ev, { controller: e.detail.value })}>
                            </mc-value-field>
                        ` : html`
                            <mc-value-field label="Bend amount"
                                .value=${ev.value!} .min=${BEND_MIN} .max=${BEND_MAX}
                                .step=${64} .coarse=${1024}
                                .formatter=${bendLabel}
                                @value-change=${(e: CustomEvent<{ value: number }>) =>
                                    this.editEvent(ev, { value: e.detail.value })}>
                            </mc-value-field>
                        `}
                    </td>
                    <td>
                        ${isCC ? html`
                            <mc-value-field label="Controller value"
                                .value=${ev.value!} .min=${0} .max=${127} .step=${1} .coarse=${10}
                                @value-change=${(e: CustomEvent<{ value: number }>) =>
                                    this.editEvent(ev, { value: e.detail.value })}>
                            </mc-value-field>
                        ` : ''}
                    </td>
                    <td>
                        <button class="row-delete" title="Delete this event"
                                @click=${() => this.deleteEvent(ev)}>✕</button>
                    </td>
                </tr>
            `;
        }

        return html`
            <tr data-key=${this.eventKey(ev)}>
                <td>
                    <mc-value-field label="Start tick"
                        .value=${ev.tick} .min=${0} .max=${100_000_000}
                        .step=${tickStep} .coarse=${coarseStep}
                        @value-change=${(e: CustomEvent<{ value: number }>) =>
                            this.editNote(ev, 'tick', e.detail.value)}>
                    </mc-value-field>
                </td>
                <td class="read-only">${ev.trackName}</td>
                <td class="type-note">Note</td>
                <td>
                    <mc-value-field label="Pitch"
                        .value=${ev.pitch!} .min=${0} .max=${127} .step=${1} .coarse=${12}
                        .formatter=${pitchName}
                        @value-change=${(e: CustomEvent<{ value: number }>) =>
                            this.editNote(ev, 'pitch', e.detail.value)}>
                    </mc-value-field>
                </td>
                <td>
                    <mc-value-field label="Velocity"
                        .value=${ev.velocity!} .min=${1} .max=${127} .step=${1} .coarse=${10}
                        @value-change=${(e: CustomEvent<{ value: number }>) =>
                            this.editNote(ev, 'velocity', e.detail.value)}>
                    </mc-value-field>
                </td>
                <td>
                    <mc-value-field label="Duration"
                        .value=${ev.durationTicks!} .min=${1} .max=${100_000_000}
                        .step=${tickStep} .coarse=${coarseStep}
                        @value-change=${(e: CustomEvent<{ value: number }>) =>
                            this.editNote(ev, 'duration', e.detail.value)}>
                    </mc-value-field>
                </td>
                <!-- Notes are deleted from the score view, where they can be
                     selected in groups. -->
                <td></td>
            </tr>
        `;
    }

    // Stable identity for a row, so Lit moves a row rather than rebuilding it
    // when an edit reorders the table — which is what keeps the focused field
    // under the caret. Every event kind has a real id from the core.
    private eventKey(ev: PanelEvent): string {
        if (ev.kind === 'Note') return `n:${ev.trackId}:${ev.noteId}`;
        return `${ev.kind}:${ev.trackId}:${ev.eventId}`;
    }

    render() {
        if (!this.doc) return html`<div class="empty">No document</div>`;
        const events = this.collectEvents();

        return html`
            ${this.renderTrackBar()}
            <div class="table-container">
                <table>
                    <colgroup>
                        <col class="c-tick"><col class="c-track"><col class="c-type">
                        <col class="c-d1"><col class="c-d2"><col class="c-dur"><col class="c-del">
                    </colgroup>
                    <thead>
                        <tr>
                            <th>Tick</th>
                            <th>Track</th>
                            <th>Type</th>
                            <th>Pitch / Data</th>
                            <th>Velocity / Value</th>
                            <th>Duration</th>
                            <th></th>
                        </tr>
                    </thead>
                    <!-- Keyed: editing a tick reorders the list, and an unkeyed
                         repeat would rebind the focused field to a different
                         event, so further arrow presses would edit the wrong
                         note. Keying makes Lit move the row instead. -->
                    <tbody>${repeat(events, ev => this.eventKey(ev), ev => this.renderRow(ev))}</tbody>
                </table>
                ${events.length === 0 ? html`<div class="empty">No events on this track.</div>` : ''}
            </div>
        `;
    }
}
