import { LitElement, html, css } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';
import { CoreBridge } from '../../bridge/coreBridge';
import { KEY_CHOICES, keyAt } from '../../models/keySignature';
import { DENOMINATORS, timeSignatureAt } from '../../models/timeSignature';
import { DocumentSnapshot } from '../../models/document';

@customElement('mc-transport-bar')
export class TransportBar extends LitElement {
    @property({ type: Number }) documentId?: number;
    @property({ type: Object }) doc?: DocumentSnapshot;
    @state() private metronomeEnabled = false;
    @state() private transportState: 'stopped' | 'playing' | 'paused' | 'recording' = 'stopped';

    private syncTimer?: number;

    static styles = css`
        :host {
            display: flex;
            align-items: center;
            padding: 4px 12px;
            gap: 12px;
            height: 40px;
            background: #252526;
        }
        .btn-group { display: flex; gap: 4px; }
        button {
            background: #3c3c3c;
            border: none;
            color: #ccc;
            padding: 4px 12px;
            cursor: pointer;
            border-radius: 2px;
            font-size: 0.8rem;
            font-weight: bold;
        }
        button:hover { background: #454545; }
        button:active { background: #007acc; }
        button.active {
            background: #007acc;
            color: white;
        }
        button.recording {
            background: #ab0000;
            color: white;
        }
        button.recording.active {
            background: #ff0000;
            box-shadow: 0 0 8px rgba(255, 0, 0, 0.5);
        }
        .display {
            font-family: 'Courier New', Courier, monospace;
            background: #000;
            color: #0f0;
            padding: 2px 12px;
            border-radius: 2px;
            min-width: 120px;
            text-align: center;
            font-size: 1.1rem;
        }
        .tempo {
            display: flex;
            align-items: center;
            gap: 4px;
            color: #888;
            font-size: 0.8rem;
        }
        .key {
            display: flex;
            align-items: center;
            gap: 4px;
            margin-left: 14px;
            color: #888;
            font-size: 0.8rem;
        }
        .key select {
            background: #1e1e1e;
            border: 1px solid #444;
            color: #ccc;
            font-size: 0.8rem;
            padding: 1px 2px;
            max-width: 150px;
        }
        .key select:focus, .key input:focus { outline: none; border-color: #007acc; }
        .ts-num {
            width: 34px;
            background: #1e1e1e;
            border: 1px solid #444;
            color: #ccc;
            font-size: 0.8rem;
            padding: 1px 2px;
            text-align: right;
        }
        .ts-slash { color: #666; }
        .ts-den { max-width: 48px; }
        .tempo input {
            width: 64px;
            background: #1e1e1e;
            border: 1px solid #444;
            color: #ccc;
            font-family: 'Courier New', Courier, monospace;
            font-size: 0.9rem;
            padding: 2px 4px;
            text-align: right;
        }
    `;

    // A <select> keeps a user-set value even when its options re-render, so undo
    // and any external change need an explicit resync.
    updated() {
        const keySelect = this.renderRoot.querySelector('#key-sig') as HTMLSelectElement | null;
        if (keySelect) {
            const key = keyAt(this.doc?.keySignatureMap, 0);
            const want = String(KEY_CHOICES.findIndex(c => c.fifths === key.fifths && c.minor === key.minor));
            if (want !== '-1' && keySelect.value !== want) keySelect.value = want;
        }
        const denSelect = this.renderRoot.querySelector('#ts-den') as HTMLSelectElement | null;
        if (denSelect) {
            const want = String(timeSignatureAt(this.doc?.timeSignatureMap, 0).denominator);
            if (denSelect.value !== want) denSelect.value = want;
        }
    }

    render() {
        const bpm = this.doc?.tempoMap?.[0]?.bpm ?? 120;
        const key = keyAt(this.doc?.keySignatureMap, 0);
        const meter = timeSignatureAt(this.doc?.timeSignatureMap, 0);
        return html`
            <div class="btn-group">
                <button title="Stop" @click=${() => this.handleStop()}>STOP</button>
                <button
                    title="Play"
                    @click=${() => this.handlePlay()}
                    class=${this.transportState === 'playing' ? 'active' : ''}
                >PLAY</button>
                <button
                    title="Record"
                    @click=${() => this.handleRecord()}
                    class=${`recording ${this.transportState === 'recording' ? 'active' : ''}`}
                >REC</button>
            </div>
            <div class="display" id="tick-display">000000</div>
            <div class="btn-group">
                <button
                    title="Metronome"
                    @click=${() => this.handleMetronomeToggle()}
                    class=${this.metronomeEnabled ? 'active' : ''}
                >METRO</button>
            </div>
            <div style="flex: 1"></div>
            <div class="tempo">
                <input type="number" min="20" max="400" step="1" .value=${bpm.toFixed(2)}
                       title="Tempo (BPM)"
                       @change=${(e: Event) => this.handleTempoChange(e)}>
                <span>BPM</span>
            </div>
            <div class="key">
                <label for="ts-num">Time</label>
                <input id="ts-num" class="ts-num" type="number" min="1" max="32" step="1"
                       .value=${String(meter.numerator)}
                       title="Beats per bar, at the start of the composition"
                       @change=${(e: Event) => this.handleMeterChange(
                           parseInt((e.target as HTMLInputElement).value), meter.denominator)}>
                <span class="ts-slash">/</span>
                <select id="ts-den" class="ts-den"
                        title="Beat unit — click a bar's time signature in the score to change it from there on"
                        @change=${(e: Event) => this.handleMeterChange(
                            meter.numerator, parseInt((e.target as HTMLSelectElement).value))}>
                    ${DENOMINATORS.map(d => html`
                        <option value=${d} ?selected=${d === meter.denominator}>${d}</option>`)}
                </select>
            </div>
            <div class="key">
                <label for="key-sig">Key</label>
                <select id="key-sig" title="Key signature at the start of the composition"
                        @change=${(e: Event) => this.handleKeyChange(e)}>
                    ${KEY_CHOICES.map((c, i) => html`
                        <option value=${i} ?selected=${c.fifths === key.fifths && c.minor === key.minor}>
                            ${c.label}
                        </option>`)}
                </select>
            </div>
        `;
    }

    async firstUpdated() {
        CoreBridge.on('playback_position', (payload: any) => {
            const display = this.shadowRoot?.getElementById('tick-display');
            if (display) {
                display.innerText = String(payload.tick).padStart(6, '0');
            }
        });

        // Pushed on every transition, so the buttons follow the transport even
        // when it changes on its own — the automatic stop at the end of the
        // composition has no command behind it to react to.
        CoreBridge.on('transport_state', (payload: any) => {
            this.transportState = (payload?.state as any) ?? 'stopped';
            if (this.transportState === 'stopped') {
                const display = this.shadowRoot?.getElementById('tick-display');
                if (display) display.innerText = String(payload?.tick ?? 0).padStart(6, '0');
            }
        });

        if (CoreBridge.isNative()) {
            this.metronomeEnabled = await CoreBridge.sendCommand('is_metronome_enabled');
            // Backstop for the pushed events above (a missed notification, or a
            // transport driven from somewhere that predates them).
            this.syncTimer = window.setInterval(() => this.syncTransportState(), 1000);
        }
    }

    disconnectedCallback() {
        super.disconnectedCallback();
        if (this.syncTimer !== undefined) clearInterval(this.syncTimer);
    }

    private async syncTransportState() {
        try {
            const st = await CoreBridge.sendCommand<{state: string, tick: number}>('get_transport_state');
            this.transportState = (st.state as any) ?? 'stopped';
            if (st.state === 'stopped') {
                const display = this.shadowRoot?.getElementById('tick-display');
                if (display) display.innerText = String(st.tick).padStart(6, '0');
            }
        } catch { /* bridge unavailable */ }
    }

    async handleTempoChange(e: Event) {
        if (this.documentId === undefined) return;
        const input = e.target as HTMLInputElement;
        const bpm = parseFloat(input.value);
        if (!isFinite(bpm)) return;
        try {
            await CoreBridge.sendCommand('set_tempo', { documentId: this.documentId, bpm });
        } catch (err) {
            console.error('set_tempo failed', err);
            input.value = (this.doc?.tempoMap?.[0]?.bpm ?? 120).toFixed(2);
        }
    }

    async handleMeterChange(numerator: number, denominator: number) {
        if (this.documentId === undefined) return;
        if (!Number.isFinite(numerator) || !Number.isFinite(denominator)) return;
        try {
            // tick 0: this control governs the composition's opening meter. Later
            // changes are made from the score, on the bar they apply to.
            await CoreBridge.sendCommand('set_time_signature', {
                documentId: this.documentId, tick: 0, numerator, denominator,
            });
        } catch (err) {
            console.error('set_time_signature failed', err);
        }
    }

    async handleKeyChange(e: Event) {
        if (this.documentId === undefined) return;
        const choice = KEY_CHOICES[parseInt((e.target as HTMLSelectElement).value)];
        if (!choice) return;
        try {
            await CoreBridge.sendCommand('set_key_signature', {
                documentId: this.documentId, tick: 0,
                fifths: choice.fifths, minor: choice.minor,
            });
        } catch (err) {
            console.error('set_key_signature failed', err);
        }
    }

    async handleMetronomeToggle() {
        this.metronomeEnabled = !this.metronomeEnabled;
        await CoreBridge.sendCommand('set_metronome_enabled', { enabled: this.metronomeEnabled });
    }

    async handlePlay() {
        if (this.documentId === undefined) return;
        this.transportState = 'playing';
        await CoreBridge.sendCommand('play', { documentId: this.documentId });
    }

    async handleRecord() {
        if (this.documentId === undefined) return;
        this.transportState = 'recording';
        await CoreBridge.sendCommand('record', { documentId: this.documentId });
    }

    async handleStop() {
        this.transportState = 'stopped';
        await CoreBridge.sendCommand('stop');
        const display = this.shadowRoot?.getElementById('tick-display');
        if (display) display.innerText = '000000';
    }
}
