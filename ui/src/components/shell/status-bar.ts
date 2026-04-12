import { LitElement, html, css } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { CoreBridge } from '../../bridge/coreBridge';

@customElement('mc-status-bar')
export class StatusBar extends LitElement {
    @state() private isNative = false;
    @state() private midiOutDevice: string | null = null;
    @state() private midiInDevice: string | null = null;

    static styles = css`
        :host {
            display: flex;
            align-items: center;
            height: 25px;
            background: #007acc;
            color: white;
            font-size: 0.75rem;
            padding: 0 10px;
            justify-content: space-between;
        }
        .status-item { display: flex; align-items: center; gap: 10px; }
        .indicator {
            width: 8px;
            height: 8px;
            border-radius: 50%;
            background: #4ec9b0;
        }
        .indicator.mock { background: #ce9178; }
        .midi-status {
            color: #dcdcdc;
            font-weight: 500;
        }
    `;

    firstUpdated() {
        this.isNative = CoreBridge.isNative();
        this.checkMidiDevices();
        
        // Check again after a bit in case of race condition
        setTimeout(() => {
            this.isNative = CoreBridge.isNative();
            this.checkMidiDevices();
        }, 500);

        // Poll for MIDI status occasionally
        setInterval(() => this.checkMidiDevices(), 5000);
    }

    async checkMidiDevices() {
        if (!CoreBridge.isNative()) return;
        
        try {
            // Check Output
            const isOutOpen = await CoreBridge.sendCommand('is_midi_output_open');
            if (isOutOpen) {
                const devices = await CoreBridge.sendCommand('get_midi_output_devices');
                if (devices && devices.length > 0) {
                    this.midiOutDevice = devices[0].name;
                }
            } else {
                this.midiOutDevice = null;
            }

            // Check Input
            const isInOpen = await CoreBridge.sendCommand('is_midi_input_open');
            if (isInOpen) {
                const devices = await CoreBridge.sendCommand('get_midi_input_devices');
                // Note: This logic assumes the first one is the one we opened if we had auto-open for input too, 
                // but for now we'll just show it if any port is open.
                if (devices && devices.length > 0) {
                    this.midiInDevice = devices[0].name;
                }
            } else {
                this.midiInDevice = null;
            }
        } catch (e) {
            console.error('Failed to check MIDI devices', e);
        }
    }

    render() {
        return html`
            <div class="status-item">
                <div class="indicator ${this.isNative ? '' : 'mock'}"></div>
                <span>${this.isNative ? 'NATIVE CORE CONNECTED' : 'MOCK MODE (BROWSER)'}</span>
                ${this.midiOutDevice ? html`<span class="midi-status">| OUT: ${this.midiOutDevice}</span>` : ''}
                ${this.midiInDevice ? html`<span class="midi-status">| IN: ${this.midiInDevice}</span>` : ''}
            </div>
            <div class="status-item">
                <span>MIDI Composer v0.1.0</span>
            </div>
        `;
    }
}
