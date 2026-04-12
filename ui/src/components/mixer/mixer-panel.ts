import { LitElement, html, css } from 'lit';
import { customElement, property } from 'lit/decorators.js';
import { DocumentSnapshot } from '../../models/document';
import './mixer-channel-strip';

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
    `;

    render() {
        if (!this.doc) return html``;

        return html`
            <div class="strips-container">
                ${this.doc.tracks.map(track => html`
                    <mc-mixer-channel-strip .track=${track} .documentId=${this.doc!.id}></mc-mixer-channel-strip>
                `)}
                
                <div class="master-strip">
                    <!-- Master Strip Placeholder -->
                    <div style="width: 70px; background: #222; height: 100%; border-radius: 2px; display: flex; flex-direction: column; align-items: center; padding: 8px 0; gap: 8px; opacity: 0.5;">
                        <div style="font-size: 0.7rem; font-weight: bold;">MASTER</div>
                        <div style="flex: 1; width: 15px; background: #111; border-radius: 2px;"></div>
                        <div style="font-size: 0.6rem;">0.0 dB</div>
                    </div>
                </div>
            </div>
        `;
    }
}
