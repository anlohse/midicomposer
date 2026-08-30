import { LitElement, html, css } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import { DocumentSnapshot } from '../../models/document';
import { CoreBridge } from '../../bridge/coreBridge';
import './mixer-channel-strip';

/** Full scale on a MIDI controller, and so on every fader here. */
const MAX_LEVEL = 127;

@customElement('mc-mixer-panel')
export class MixerPanel extends LitElement {
    @property({ type: Object }) doc?: DocumentSnapshot;

    static styles = css`
        :host {
            display: flex;
            flex-direction: column;
            height: 100%;
            background: #252526;
        }
        .strips-container {
            flex: 1;
            display: flex;
            padding: 10px;
            gap: 8px;
            overflow-x: auto;
            align-items: stretch;
        }
        .master-strip {
            border-left: 2px solid #111;
            padding-left: 8px;
        }
        /* Deliberately the same shape as a channel strip: it is the same kind of
           control, and one that looked different read as decoration. */
        .master {
            width: 70px;
            /* Full height, like the channel strips it sits beside: a shorter
               fader beside a taller one reads as a different kind of control. */
            height: 100%;
            box-sizing: border-box;
            background: #2b2b2b;
            display: flex;
            flex-direction: column;
            align-items: center;
            padding: 8px 0;
            gap: 8px;
            border-radius: 2px;
            border: 1px solid #4a4a4a;
        }
        .label {
            font-size: 0.7rem;
            font-weight: bold;
            color: #aaa;
        }
        .fader-container {
            flex: 1;
            width: 20px;
            background: #222;
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
        .readout {
            font-size: 0.6rem;
            color: #999;
            font-variant-numeric: tabular-nums;
        }
    `;

    render() {
        if (!this.doc) return html``;

        // Always present: a project saved before there was a master fader is
        // read back at unity by the core, not left undefined here.
        const master = this.doc.masterVolume;

        return html`
            <div class="strips-container">
                ${this.doc.tracks.map(track => html`
                    <mc-mixer-channel-strip .track=${track} .documentId=${this.doc!.id}></mc-mixer-channel-strip>
                `)}

                <div class="master-strip">
                    <div class="master">
                        <div class="label">MASTER</div>
                        <div class="fader-container">
                            <input type="range" class="fader" id="master-fader"
                                   min="0" max=${MAX_LEVEL} .value=${String(master)}
                                   title="Scales every track's volume"
                                   @input=${(e: Event) =>
                                       this.setMaster(parseInt((e.target as HTMLInputElement).value))}>
                        </div>
                        <div class="readout">${Math.round(master * 100 / MAX_LEVEL)}%</div>
                    </div>
                </div>
            </div>
        `;
    }

    private async setMaster(volume: number) {
        if (!this.doc) return;
        try {
            await CoreBridge.sendCommand('set_master_volume', {
                documentId: this.doc.id, volume,
            });
        } catch (err) {
            console.error('Failed to set master volume', err);
        }
    }
}
