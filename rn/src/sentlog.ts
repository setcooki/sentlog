// @sentlog/react-native — bridge surface.
//
// Wraps the native `Sentlog` NativeModule (Kotlin
// SentlogModule.kt + Swift SentlogModule.swift land in 6c/6d).
// Every method returns a Promise; native methods that take a
// closure (envelope dispatcher, drain handler, telemetry) route
// through the NativeEventEmitter event bus because RN's bridge
// cannot pass arbitrary function references across natively.
//
// Method names and shapes mirror the SentlogBridge interface in
// ./types.

import { NativeEventEmitter, NativeModules } from "react-native";

import type {
    Capabilities,
    DrainHandler,
    EnvelopeDispatcher,
    ModelVersions,
    Permissions,
    SentlogBridge,
    SentlogConfig,
    SentlogResult,
    TelemetryEvent,
    UpdateResult,
} from "./types";

interface NativeSentlogModule {
    version(): Promise<string>;
    initialize(configJson: string): Promise<void>;
    startCapture(): Promise<void>;
    evaluate(): Promise<string>;       // returns SentlogResult JSON string
    cancel(): Promise<void>;
    destroy(): Promise<void>;
    getPermissions(): Promise<string>;   // returns JSON
    getCapabilities(): Promise<string>;  // returns JSON
    modelVersions(): Promise<string>;    // returns JSON
    checkForUpdates(): Promise<string>;  // returns JSON
    /** Native ⇒ JS: register interest. The native side fires the
     *  "SentlogEnvelope" event per envelope; JS subscribes via
     *  NativeEventEmitter. Returns when the registration is in
     *  place (or cleared with active === false). */
    setEnvelopeDispatcherActive(active: boolean): Promise<void>;
    setSigningKey(secretKeyBase64: string,
                  verificationMethod: string): Promise<void>;
    drainQueueOnce(path: string, drainRequestId: string): Promise<number>;
    /** JS ⇒ Native: reply to a "SentlogDrainEnvelope" event so the
     *  native side can decide keep-vs-delete per envelope inside
     *  drainQueueOnce. */
    drainQueueReply(drainRequestId: string, line: number, keep: boolean): Promise<void>;
}

const Native = (NativeModules.Sentlog as NativeSentlogModule | undefined);

function requireNative(): NativeSentlogModule {
    if (Native == null) {
        throw new Error(
            "@sentlog/react-native: native module 'Sentlog' not found. " +
            "Did you link the package and rebuild the app?",
        );
    }
    return Native;
}

const emitter = Native != null ? new NativeEventEmitter(Native as object) : null;

let currentDispatcher: EnvelopeDispatcher | null = null;
let dispatcherSub: { remove: () => void } | null = null;

function ensureDispatcherSub(): void {
    if (dispatcherSub != null) return;
    if (emitter == null) return;
    dispatcherSub = emitter.addListener("SentlogEnvelope",
        (ev: {
            forwarder_name: string;
            sink_kind: string;
            sink_json: string;
            envelope_json: string;
        }) => {
            currentDispatcher?.(
                ev.forwarder_name,
                // SinkKind is a narrow string union; we trust the
                // native side to only emit the four documented
                // values. Cast for the type, fail loud if wrong.
                ev.sink_kind as Parameters<EnvelopeDispatcher>[1],
                ev.sink_json,
                ev.envelope_json,
            );
        });
}

function clearDispatcherSub(): void {
    dispatcherSub?.remove();
    dispatcherSub = null;
}

/** Drain reply protocol: the native side emits one
 *  "SentlogDrainEnvelope" event per envelope inside drainQueueOnce,
 *  carrying a per-call drainRequestId + a per-envelope line index.
 *  The JS side runs the integrator's drain handler, then calls
 *  drainQueueReply to feed the keep-vs-delete decision back. The
 *  native side waits for the reply before moving to the next
 *  envelope, so the protocol is fully sequential and matches the
 *  C ABI's synchronous drain semantics. */
async function drainViaEventBridge(
    n: NativeSentlogModule,
    path: string,
    handler: DrainHandler,
): Promise<number> {
    if (emitter == null) throw new Error("drainQueue requires a NativeEventEmitter");
    const drainRequestId = `drain-${Date.now()}-${Math.random().toString(36).slice(2)}`;

    // Subscribe BEFORE kicking off — native side starts emitting
    // synchronously from drainQueueOnce.
    const sub = emitter.addListener("SentlogDrainEnvelope",
        async (ev: { drainRequestId: string; line: number; envelope_json: string }) => {
            if (ev.drainRequestId !== drainRequestId) return;
            // `keep` carries literal keep-semantics — true means
            // leave the envelope in the queue. handler returns
            // true=delete per its public contract, so we invert
            // once here and ship the literal keep flag over the
            // wire. Native side inverts again when feeding the C
            // ABI cb (which expects 1=delete).
            let keep = true;
            try {
                keep = !handler(ev.envelope_json);
            } catch (err) {
                console.error("[sentlog] drain handler threw; keeping envelope", err);
                keep = true;
            }
            await n.drainQueueReply(drainRequestId, ev.line, keep);
        });

    try {
        return await n.drainQueueOnce(path, drainRequestId);
    } finally {
        sub.remove();
    }
}

export const Sentlog: SentlogBridge = {
    async version(): Promise<string> {
        return requireNative().version();
    },

    async initialize(config: SentlogConfig): Promise<void> {
        const n = requireNative();
        await n.initialize(JSON.stringify(config));
    },

    async startCapture(): Promise<void> {
        await requireNative().startCapture();
    },

    async evaluate(): Promise<SentlogResult> {
        const json = await requireNative().evaluate();
        return JSON.parse(json) as SentlogResult;
    },

    async cancel(): Promise<void> {
        await requireNative().cancel();
    },

    async destroy(): Promise<void> {
        await requireNative().destroy();
        clearDispatcherSub();
        currentDispatcher = null;
    },

    async getPermissions(): Promise<Permissions> {
        const json = await requireNative().getPermissions();
        return JSON.parse(json) as Permissions;
    },

    async getCapabilities(): Promise<Capabilities> {
        const json = await requireNative().getCapabilities();
        return JSON.parse(json) as Capabilities;
    },

    async modelVersions(): Promise<ModelVersions> {
        const json = await requireNative().modelVersions();
        return JSON.parse(json) as ModelVersions;
    },

    async checkForUpdates(): Promise<UpdateResult> {
        const json = await requireNative().checkForUpdates();
        return JSON.parse(json) as UpdateResult;
    },

    async drainQueue(path: string, drain: DrainHandler): Promise<number> {
        return drainViaEventBridge(requireNative(), path, drain);
    },

    async setSigningKey(secretKeyBase64: string,
                        verificationMethod: string): Promise<void> {
        await requireNative().setSigningKey(secretKeyBase64, verificationMethod);
    },

    async setEnvelopeDispatcher(handler: EnvelopeDispatcher | null): Promise<void> {
        currentDispatcher = handler;
        if (handler == null) {
            await requireNative().setEnvelopeDispatcherActive(false);
            clearDispatcherSub();
        } else {
            ensureDispatcherSub();
            await requireNative().setEnvelopeDispatcherActive(true);
        }
    },
};

/** Subscribe to native telemetry events (spec §3.2). Returns a
 *  remover. Telemetry is opt-in: the native side only emits when
 *  `initialize`'s config caused telemetry to be wired (a future
 *  config flag). For now this is a passthrough subscription. */
export function onTelemetry(handler: (ev: TelemetryEvent) => void): { remove: () => void } {
    if (emitter == null) return { remove: () => {} };
    return emitter.addListener("SentlogTelemetry", (raw: string) => {
        try {
            handler(JSON.parse(raw) as TelemetryEvent);
        } catch (err) {
            console.error("[sentlog] failed to parse telemetry event", err);
        }
    });
}
