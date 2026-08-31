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

/** What this installation remembers between runs, as opposed to a project. */
export interface Preferences {
    selectedOutput: string;
    clapSearchPaths: string[];
}

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
    @state() private folders: string[] = [];

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
            max-width: 520px;
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
        .section {
            border-top: 1px solid #3c3c3c;
            margin-top: 14px; padding-top: 12px;
        }
        .section h4 { margin: 0 0 2px 0; font-size: 0.85rem; color: #ccc; font-weight: 600; }
        .hint { color: #888; font-size: 0.75rem; margin-bottom: 8px; }
        .folder {
            display: flex; align-items: center; gap: 8px;
            padding: 3px 0; font-size: 0.8rem;
        }
        .folder .path {
            flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
            direction: rtl; text-align: left;
        }
        button.link {
            background: none; border: none; color: #888; padding: 0 4px;
            cursor: pointer; font-size: 0.85rem;
        }
        button.link:hover { background: none; color: #f48771; }
        button.secondary { background: #3c3c3c; }
        button.inline { padding: 3px 8px; font-size: 0.8rem; white-space: nowrap; }
        button.secondary:hover { background: #4a4a4a; }
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
        try {
            const prefs = await CoreBridge.sendCommand<Preferences>('get_preferences');
            this.folders = prefs?.clapSearchPaths ?? [];
        } catch (err) {
            // The dialog still configures the output without them; only the
            // folder list is missing, so it says so there rather than here.
            console.error('Failed to read the preferences', err);
        }
    }

    private async setFolders(paths: string[]) {
        this.error = null;
        try {
            await CoreBridge.sendCommand('set_clap_search_paths', { paths });
        } catch (err) {
            this.error = err instanceof Error ? err.message : String(err);
        }
        // A scan can add outputs, so the whole schema is re-read: the output
        // list in this very dialog is what just changed.
        await this.reload();
    }

    private async addFolder() {
        const chosen = await CoreBridge.sendCommand<{ path?: string; cancelled?: boolean }>(
            'choose_folder');
        if (!chosen?.path) return;
        if (this.folders.includes(chosen.path)) return;
        await this.setFolders([...this.folders, chosen.path]);
    }

    private async removeFolder(path: string) {
        // Plugins already loaded from it stay until the next run. Unloading one
        // that a track is playing through would silence the project to tidy up
        // a list, which is not what removing a folder asked for.
        await this.setFolders(this.folders.filter(p => p !== path));
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

    private async browseFor(p: OutputParameter) {
        // The dialog is the core's, like every other path the core is given.
        // A path typed into the box works too, but a picker is how a file that
        // was just downloaded gets found.
        const chosen = await CoreBridge.sendCommand<{ path?: string; cancelled?: boolean }>(
            'choose_file', { filter: p.filter ?? '' });
        if (!chosen?.path) return;
        await this.setParameter(p.name, chosen.path);
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
                    // The box stays alongside the picker: a path can be pasted,
                    // and it is the only way to see the whole of a long one.
                    return html`
                        <input type="text" .value=${String(p.value ?? '')}
                               placeholder=${p.filter ?? ''}
                               @change=${(e: Event) => this.setParameter(
                                   p.name, (e.target as HTMLInputElement).value)}>
                        <button class="secondary inline"
                                @click=${() => this.browseFor(p)}>Browse…</button>`;
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

                    <div class="section">
                        <h4>Plugin folders</h4>
                        <div class="hint">
                            Searched for .clap plugins at startup, in addition to
                            the standard locations. A folder added here is found
                            straight away; one removed keeps its plugins until
                            the next run.
                        </div>
                        ${this.folders.map(path => html`
                            <div class="folder">
                                <span class="path" title=${path}>${path}</span>
                                <button class="link" title="Remove"
                                        @click=${() => this.removeFolder(path)}>&times;</button>
                            </div>`)}
                        <button class="secondary" @click=${() => this.addFolder()}>
                            Add folder…
                        </button>
                    </div>

                    <div class="modal-footer">
                        <button @click=${() => this.close()}>Close</button>
                    </div>
                </div>
            </div>
        `;
    }
}
