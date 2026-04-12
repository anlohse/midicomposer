import { LitElement, html, css } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import { TrackSnapshot } from '../../models/document';
import { CoreBridge } from '../../bridge/coreBridge';

@customElement('mc-mixer-channel-strip')
export class MixerChannelStrip extends LitElement {
    @property({ type: Object }) track?: TrackSnapshot;
    @property({ type: Number }) documentId?: number;

    static styles = css`
        :host {
            width: 70px;
            background: #333;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 8px 0;
            gap: 8px;
            border-radius: 2px;
            border: 1px solid #444;
        }
        .label {
            font-size: 0.7rem;
            text-align: center;
            white-space: nowrap;
            overflow: hidden;
            text-overflow: ellipsis;
            width: 90%;
            font-weight: bold;
            color: #aaa;
        }
        .fader-container {
            flex: 1;
            width: 20px;
            background: #222;
            position: relative;
            border-radius: 2px;
            display: flex;
            justify-content: center;
        }
        input[type="range"].fader {
            writing-mode: bt-lr; /* Bottom to top */
            appearance: slider-vertical;
            width: 10px;
            height: 100%;
            background: transparent;
        }
        .pan-knob {
            width: 40px;
        }
        .controls {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 2px;
            width: 90%;
        }
        button {
            font-size: 0.6rem;
            padding: 2px 0;
            background: #444;
            border: 1px solid #555;
            color: #ccc;
            cursor: pointer;
        }
        button.active.mute { background: #c00; color: white; }
        button.active.solo { background: #cc0; color: black; }
        button.active.arm { background: #f00; color: white; }
    `;

    render() {
        if (!this.track) return html``;

        return html`
            <div class="label">${this.track.name}</div>
            
            <div class="pan-container">
                <input type="range" class="pan-knob" min="0" max="127" .value=${this.track.pan}
                    @input=${(e: any) => this.handleUpdate('set_track_pan', { pan: parseInt(e.target.value) })}>
                <div style="font-size: 0.5rem; text-align: center;">PAN</div>
            </div>

            <div class="fader-container">
                <input type="range" class="fader" min="0" max="127" .value=${this.track.volume}
                    @input=${(e: any) => this.handleUpdate('set_track_volume', { volume: parseInt(e.target.value) })}>
            </div>

            <div class="controls">
                <button class="${this.track.muted ? 'active mute' : ''}" 
                    @click=${() => this.handleUpdate('set_track_mute', { muted: !this.track!.muted })}>M</button>
                <button class="${this.track.solo ? 'active solo' : ''}"
                    @click=${() => this.handleUpdate('set_track_solo', { solo: !this.track!.solo })}>S</button>
                <button class="${this.track.armed ? 'active arm' : ''}" style="grid-column: span 2"
                    @click=${() => this.handleUpdate('set_track_arm', { armed: !this.track!.armed })}>ARM</button>
            </div>
        `;
    }

    async handleUpdate(command: string, payload: any) {
        if (!this.track || this.documentId === undefined) return;
        try {
            await CoreBridge.sendCommand(command, {
                documentId: this.documentId,
                trackId: parseInt(this.track.id),
                ...payload
            });
        } catch (err) {
            console.error(`Failed to update track: ${command}`, err);
        }
    }
}
