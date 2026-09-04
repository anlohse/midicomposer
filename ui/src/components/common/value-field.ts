import { LitElement, html, css } from 'lit';
import { customElement, property, state } from 'lit/decorators.js';

// Commits are debounced so holding an arrow key produces a handful of core
// commands (and undo entries) instead of one per repeat.
const COMMIT_DELAY_MS = 220;

/**
 * A numeric value that reads as plain text until you touch it: click to focus,
 * ArrowUp/ArrowDown to step, Shift+Arrow (or PageUp/PageDown) for a coarse
 * step, or just type a number. Enter commits immediately, Escape reverts.
 *
 * Emits `value-change` with `{ value }` only when the value actually changed.
 * The parent owns the commit — this component never talks to the core.
 */
@customElement('mc-value-field')
export class ValueField extends LitElement {
    @property({ type: Number }) value = 0;
    @property({ type: Number }) min = 0;
    @property({ type: Number }) max = 127;
    @property({ type: Number }) step = 1;
    @property({ type: Number }) coarse = 10;
    @property({ type: Boolean }) disabled = false;
    /** Accessible name — the panel's column header is not enough on its own. */
    @property() label = '';
    /** Optional muted suffix, e.g. the note name for a pitch. Live-updates while stepping. */
    @property({ attribute: false }) formatter?: (value: number) => string;

    // Local value while the user is editing. Kept separate from `value` so an
    // incoming document refresh mid-edit cannot yank the field out from under
    // the caret.
    @state() private pending: number | null = null;
    private focused = false;
    private commitTimer?: number;

    static styles = css`
        :host { display: flex; align-items: center; gap: 4px; min-width: 0; }
        input {
            flex: 1;
            min-width: 0;
            background: transparent;
            border: 1px solid transparent;
            border-radius: 2px;
            color: inherit;
            font: inherit;
            padding: 1px 3px;
            /* Hide the spinners: stepping is done with the keyboard. */
            -moz-appearance: textfield;
            appearance: textfield;
        }
        input::-webkit-inner-spin-button,
        input::-webkit-outer-spin-button { -webkit-appearance: none; margin: 0; }
        input:hover:not(:disabled) { border-color: #4a4a4a; background: #2a2a2a; }
        input:focus {
            outline: none;
            border-color: #007acc;
            background: #252526;
            color: #fff;
        }
        input:disabled { color: #666; }
        .suffix {
            color: #7a7a7a;
            font-size: 0.9em;
            white-space: nowrap;
            pointer-events: none;
        }
    `;

    disconnectedCallback() {
        super.disconnectedCallback();
        // Don't leave an edit unsaved if the row is re-rendered away mid-debounce.
        this.flush();
        if (this.commitTimer) clearTimeout(this.commitTimer);
    }

    willUpdate(changed: Map<string, unknown>) {
        // Adopt the authoritative value only while the user isn't editing.
        if (changed.has('value') && !this.focused) this.pending = null;
    }

    private get current(): number {
        return this.pending ?? this.value;
    }

    private clamp(v: number): number {
        return Math.max(this.min, Math.min(this.max, Math.round(v)));
    }

    private setPending(v: number, immediate = false) {
        const next = this.clamp(v);
        this.pending = next;
        if (this.commitTimer) clearTimeout(this.commitTimer);
        if (immediate) this.commit();
        else this.commitTimer = window.setTimeout(() => this.commit(), COMMIT_DELAY_MS);
    }

    private commit() {
        if (this.commitTimer) { clearTimeout(this.commitTimer); this.commitTimer = undefined; }
        if (this.pending === null || this.pending === this.value) return;
        this.dispatchEvent(new CustomEvent<{ value: number }>('value-change', {
            detail: { value: this.pending }, bubbles: true, composed: true,
        }));
    }

    /** Commit any debounced edit right now. */
    flush() {
        if (this.commitTimer) this.commit();
    }

    private onKeyDown(e: KeyboardEvent) {
        const dir = e.key === 'ArrowUp' ? 1 : e.key === 'ArrowDown' ? -1 : 0;
        if (dir !== 0) {
            // Take the arrows over from the native spinner so Shift can mean
            // "coarse step" — browsers don't do that for number inputs.
            e.preventDefault();
            this.setPending(this.current + (e.shiftKey ? this.coarse : this.step) * dir);
            return;
        }
        if (e.key === 'PageUp' || e.key === 'PageDown') {
            e.preventDefault();
            this.setPending(this.current + this.coarse * (e.key === 'PageUp' ? 1 : -1));
            return;
        }
        if (e.key === 'Enter') {
            e.preventDefault();
            this.setPending(this.current, true);
            return;
        }
        if (e.key === 'Escape') {
            e.preventDefault();
            if (this.commitTimer) { clearTimeout(this.commitTimer); this.commitTimer = undefined; }
            this.pending = null;
            return;
        }
    }

    /**
     * The wheel steps the value, but only once the field has been clicked into.
     *
     * Unfocused it stays a number in a table, and the wheel has to keep
     * scrolling the list — a row that quietly ate the gesture and edited a note
     * on the way past would be worse than useless. Focused, the step is the same
     * one the arrows use, which for a tick or a duration is the snap resolution,
     * so rolling the wheel walks the grid the score view snaps to.
     */
    private onWheel(e: WheelEvent) {
        if (!this.focused || this.disabled) return;
        const delta = e.deltaY || e.deltaX;
        if (!delta) return;
        e.preventDefault();
        this.setPending(this.current + (e.shiftKey ? this.coarse : this.step) * (delta < 0 ? 1 : -1));
    }

    private onInput(e: Event) {
        const raw = (e.target as HTMLInputElement).value;
        if (raw === '') return;               // mid-typing, wait for a number
        const parsed = Number(raw);
        if (!Number.isFinite(parsed)) return;
        this.setPending(parsed);
    }

    render() {
        const v = this.current;
        return html`
            <input type="number"
                   aria-label=${this.label}
                   .value=${String(v)}
                   min=${this.min} max=${this.max} step=${this.step}
                   ?disabled=${this.disabled}
                   @keydown=${this.onKeyDown}
                   @wheel=${this.onWheel}
                   @input=${this.onInput}
                   @focus=${() => { this.focused = true; }}
                   @blur=${() => { this.focused = false; this.flush(); this.pending = null; }}>
            ${this.formatter ? html`<span class="suffix">${this.formatter(v)}</span>` : ''}
        `;
    }
}
