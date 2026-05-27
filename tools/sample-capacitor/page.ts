// Sentlog — minimal Capacitor sample.
//
// Drop into your existing Capacitor page. Wire the four buttons
// (start, stopEval, reset, drain) to onStart/onStopEval/etc.
// from your framework (Ionic, Svelte, vanilla TS). See ./README.md
// for install + permission setup.

import {
    Sentlog, onTelemetry,
    type SentlogConfig, type SentlogResult, type TelemetryEvent,
} from "@sentlog/capacitor";

const DEFAULT_CONFIG: SentlogConfig = {
    issuer: "did:example:integrator",
    language_hint: "en",
    target_phrase: "please confirm your name",
    min_duration_ms: 2000,
};

let state: "idle" | "capturing" | "evaluating" | "done" = "idle";
let lastResult: SentlogResult | null = null;
let lastError:  string | null = null;
const events: TelemetryEvent[] = [];

// Wire once at page load.
let telemetryHandle: { remove: () => Promise<void> } | null = null;
export async function attachTelemetry(): Promise<void> {
    telemetryHandle = await onTelemetry((ev) => {
        events.push(ev);
        if (events.length > 10) events.shift();
        renderTelemetry?.(events);
    });
}
export async function detachTelemetry(): Promise<void> {
    await telemetryHandle?.remove();
    telemetryHandle = null;
}

export async function onStart(): Promise<void> {
    lastError = null;
    lastResult = null;
    try {
        await Sentlog.initialize(DEFAULT_CONFIG);
        await Sentlog.startCapture();
        state = "capturing";
    } catch (e) {
        // Common case: microphone permission denied. The integrator
        // is responsible for the runtime permission UX.
        lastError = String(e);
        state = "idle";
    }
    renderState?.(state, lastError);
}

export async function onStopEval(): Promise<void> {
    state = "evaluating";
    renderState?.(state, lastError);
    try {
        lastResult = await Sentlog.evaluate();
        state = "done";
    } catch (e) {
        lastError = String(e);
        state = "idle";
    }
    renderResult?.(lastResult, lastError);
    renderState?.(state, lastError);
}

export async function onReset(): Promise<void> {
    try { await Sentlog.destroy(); } catch { /* idempotent */ }
    lastResult = null;
    lastError = null;
    events.length = 0;
    state = "idle";
    renderState?.(state, lastError);
    renderResult?.(null, null);
    renderTelemetry?.(events);
}

/** Example drain: integrator passes the queue file path their
 *  forwarder is configured with, then decides keep-vs-delete per
 *  envelope. Here we delete everything we deliver — typical for
 *  "ship the envelope and move on" flows. */
export async function onDrain(path: string): Promise<number> {
    return await Sentlog.drainQueue(path, (envelopeJson) => {
        // Custom delivery here (POST to your backend, write to
        // disk, etc.). Return true to delete from queue.
        console.log("drained envelope (", envelopeJson.length, "bytes )");
        return true;
    });
}

// --- render hooks (your framework supplies these) ----------------

export let renderState: ((s: string, err: string | null) => void) | null = null;
export let renderResult: ((r: SentlogResult | null, err: string | null) => void) | null = null;
export let renderTelemetry: ((events: readonly TelemetryEvent[]) => void) | null = null;

export function bindRenderers(hooks: {
    state: typeof renderState;
    result: typeof renderResult;
    telemetry: typeof renderTelemetry;
}): void {
    renderState     = hooks.state;
    renderResult    = hooks.result;
    renderTelemetry = hooks.telemetry;
}
