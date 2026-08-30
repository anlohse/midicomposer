import { LitElement, html, css } from 'lit';
import { customElement, state } from 'lit/decorators.js';
import { CoreBridge } from '../../bridge/coreBridge';

/**
 * The output's settings, drawn from what the plugin declares rather than from
 * anything this file knows about MIDI ports or sample banks.
 *
 * There is no way for a plugin to supply its own dialog. Every time one appears
 * to need it, the schema is missing a type — and adding the type here is the
 * fix, because it keeps one toolkit and gets persistence for free.
 */

export type ParameterType = 'enum' | 'int' | 'bool' | 'string' | 'file';

export interface ParameterChoice { value: string; label: string }

export interface OutputParameter {
    name: string;
    label: string;
    type: ParameterType;
    headline: boolean;
    value: string | number | boolean | null;
    choices?: ParameterChoice[];
    min?: number;
    max?: number;
    step?: number;
    unit?: string;
    filter?: string;
}

export interface OutputChoice { id: string; name: string }

export interface OutputInfo {
    id: string;
    name: string;
    /** Whether this output makes sound itself; gates rendering to a file. */
    producesAudio: boolean;
    available: OutputChoice[];
    parameters: OutputParameter[];
}

/** The whole schema, values included, in one call. */
export async function loadOutputInfo(): Promise<OutputInfo | null> {
    try {
        return await CoreBridge.sendCommand<OutputInfo>('get_output_info');
    } catch (err) {
        console.error('Failed to read the output settings', err);
        return null;
    }
}

/** The value worth showing next to the output's name. */
export function headlineValue(info: OutputInfo | null): string | null {
    const p = info?.parameters.find(p => p.headline);
    if (!p || p.value === null || p.value === '') return null;
    if (p.type === 'enum') {
        return p.choices?.find(c => c.value === p.value)?.label ?? String(p.value);
    }
    return String(p.value);
}

@customElement('mc-output-settings')
export class OutputSettings extends LitElement {
    @state() private info: OutputInfo | null = null;
    @state() private error: string | null = null;

    static styles = css`
        .modal-overlay {
            position: fixed; inset: 0;
            background: rgba(0, 0, 0, 0.5);
            display: flex; align-items: center; justify-content: center;
            z-index: 100;
        }
        .modal-content {
            background: #2d2d2d;
            border: 1px solid #454545;
            border-radius: 4px;
            padding: 16px 20px;
            min-width: 380px;
            color: #ccc;
            font-size: 0.85rem;
        }
        h3 { margin: 0 0 4px 0; font-size: 1rem; }
        .plugin-name { color: #888; font-size: 0.8rem; margin-bottom: 14px; }
        .row { display: flex; align-items: center; gap: 10px; margin-bottom: 10px; }
        .row label { flex: 0 0 110px; color: #aaa; }
        .row .control { flex: 1; display: flex; align-items: center; gap: 6px; }
        select, input[type="text"], input[type="number"] {
            flex: 1;
            background: #3c3c3c;
            border: 1px solid #4a4a4a;
            color: #ccc;
            padding: 3px 4px;
            font-size: 0.85rem;
        }
        .unit { color: #888; font-size: 0.8rem; }
        .empty { color: #888; font-style: italic; margin-bottom: 12px; }
        .error {
            color: #f48771;
            background: #3a2020;
            border: 1px solid #5a2d2d;
            padding: 6px 8px;
            margin-bottom: 10px;
        }
        .modal-footer { display: flex; justify-content: flex-end; gap: 8px; margin-top: 16px; }
        button {
            background: #0e639c; border: none; color: white;
            padding: 4px 14px; cursor: pointer; font-size: 0.85rem;
        }
        button:hover { background: #1177bb; }
    `;

    connectedCallback() {
        super.connectedCallback();
        void this.reload();
    }

    private async reload() {
        this.info = await loadOutputInfo();
    }

    private async selectOutput(id: string) {
        this.error = null;
        try {
            await CoreBridge.sendCommand('select_output', { id });
        } catch (err) {
            this.error = err instanceof Error ? err.message : String(err);
        }
        // The new output declares different parameters, so the whole schema is
        // re-read rather than the values patched.
        await this.reload();
        this.dispatchEvent(new CustomEvent('output-changed', { bubbles: true, composed: true }));
    }

    private async setParameter(name: string, value: string | number | boolean) {
        this.error = null;
        try {
            await CoreBridge.sendCommand('set_output_parameter', { name, value });
        } catch (err) {
            // The plugin's own words: "port 'X' not found" says more than
            // anything this dialog could invent.
            this.error = err instanceof Error ? err.message : String(err);
        }
        // Re-read either way. A set can change another parameter's choices, and
        // on failure the stored value is not what was just typed.
        await this.reload();
    }

    private close() {
        this.dispatchEvent(new CustomEvent('close', { bubbles: true, composed: true }));
    }

    private renderParameter(p: OutputParameter) {
        const control = () => {
            switch (p.type) {
                case 'enum':
                    return html`
                        <select @change=${(e: Event) =>
                            this.setParameter(p.name, (e.target as HTMLSelectElement).value)}>
                            ${p.value === null
                                ? html`<option value="" selected>— none —</option>` : ''}
                            ${(p.choices ?? []).map(c => html`
                                <option value=${c.value} ?selected=${c.value === p.value}>
                                    ${c.label}
                                </option>`)}
                        </select>`;
                case 'int':
                    return html`
                        <input type="number" .value=${String(p.value ?? 0)}
                               min=${p.min ?? 0} max=${p.max ?? 0} step=${p.step ?? 1}
                               @change=${(e: Event) => this.setParameter(
                                   p.name, parseInt((e.target as HTMLInputElement).value))}>
                        ${p.unit ? html`<span class="unit">${p.unit}</span>` : ''}`;
                case 'bool':
                    return html`
                        <input type="checkbox" .checked=${p.value === true}
                               @change=${(e: Event) => this.setParameter(
                                   p.name, (e.target as HTMLInputElement).checked)}>`;
                case 'file':
                    // Typed for now. A real picker needs a bridge call to the
                    // native dialog, which arrives with the first plugin that
                    // has a file to pick.
                    return html`
                        <input type="text" .value=${String(p.value ?? '')}
                               placeholder=${p.filter ?? ''}
                               @change=${(e: Event) => this.setParameter(
                                   p.name, (e.target as HTMLInputElement).value)}>`;
                default:
                    return html`
                        <input type="text" .value=${String(p.value ?? '')}
                               @change=${(e: Event) => this.setParameter(
                                   p.name, (e.target as HTMLInputElement).value)}>`;
            }
        };

        return html`
            <div class="row">
                <label>${p.label}</label>
                <div class="control">${control()}</div>
            </div>`;
    }

    render() {
        return html`
            <div class="modal-overlay" @click=${() => this.close()}>
                <div class="modal-content" @click=${(e: Event) => e.stopPropagation()}>
                    <h3>Output Settings</h3>
                    ${this.info && this.info.available.length > 1
                        ? html`
                            <div class="row">
                                <label>Output</label>
                                <div class="control">
                                    <select @change=${(e: Event) =>
                                        this.selectOutput((e.target as HTMLSelectElement).value)}>
                                        ${this.info.available.map(o => html`
                                            <option value=${o.id} ?selected=${o.id === this.info!.id}>
                                                ${o.name}
                                            </option>`)}
                                    </select>
                                </div>
                            </div>`
                        : html`<div class="plugin-name">${this.info?.name ?? 'Loading…'}</div>`}

                    ${this.error ? html`<div class="error">${this.error}</div>` : ''}

                    ${this.info && this.info.parameters.length === 0
                        ? html`<div class="empty">This output has nothing to configure.</div>`
                        : (this.info?.parameters ?? []).map(p => this.renderParameter(p))}

                    <div class="modal-footer">
                        <button @click=${() => this.close()}>Close</button>
                    </div>
                </div>
            </div>
        `;
    }
}
