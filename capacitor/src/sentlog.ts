// @sentlog/capacitor — bridge surface.
//
// Wraps the native `Sentlog` Capacitor plugin (Kotlin
// SentlogPlugin.kt + Swift SentlogPlugin.swift land in 6f/6g).
// Method names mirror the SentlogBridge interface in ./types so a
// codebase can swap between @sentlog/react-native and
// @sentlog/capacitor by changing imports only.
//
// Capacitor's IPC is strictly object-in / object-out. Closures that
// can't cross the bridge (envelope dispatcher, drain handler,
// telemetry) route through addListener events, same shape as the
// RN counterpart's NativeEventEmitter.

import { registerPlugin, type PluginListenerHandle } from "@capacitor/core";

import type {
    Capabilities,
    DrainHandler,
    EnvelopeDispatcher,
    ModelVersions,
    Permissions,
    SentlogBridge,
    SentlogConfig,
    SentlogResult,
    SinkKind,
    TelemetryEvent,
    UpdateResult,
} from "./types";

/** Shape the native plugin (Kotlin + Swift) exposes. Each method
 *  returns a Promise of an object — `{}` for void methods. Typed
 *  fields use JSON strings for cross-platform stability; the shim
 *  parses on receipt. */
interface NativeSentlogPlugin {
    version(): Promise<{ value: string }>;
    initialize(args: { configJson: string }): Promise<void>;
    startCapture(): Promise<void>;
    evaluate(): Promise<{ resultJson: string }>;
    cancel(): Promise<void>;
    destroy(): Promise<void>;
    getPermissions(): Promise<{ json: string }>;
    getCapabilities(): Promise<{ json: string }>;
    modelVersions(): Promise<{ json: string }>;
    checkForUpdates(args: { manifestJson: string }): Promise<{ json: string }>;
    setSigningKey(args: { secretKeyBase64: string;
                          verificationMethod: string }): Promise<void>;
    setEnvelopeDispatcherActive(args: { active: boolean }): Promise<void>;
    drainQueueOnce(args: { path: string;
                           drainRequestId: string }): Promise<{ delivered: number }>;
    drainQueueReply(args: { drainRequestId: string;
                            line: number;
                            keep: boolean }): Promise<void>;
    addListener(eventName: string,
                listenerFunc: (event: unknown) => void): Promise<PluginListenerHandle>;
    removeAllListeners(): Promise<void>;
}

const Native = registerPlugin<NativeSentlogPlugin>("Sentlog");

let currentDispatcher: EnvelopeDispatcher | null = null;
let dispatcherSub: PluginListenerHandle | null = null;

async function ensureDispatcherSub(): Promise<void> {
    if (dispatcherSub != null) return;
    dispatcherSub = await Native.addListener("SentlogEnvelope",
        (raw: unknown) => {
            const ev = raw as {
                forwarder_name: string;
                sink_kind: SinkKind;
                sink_json: string;
                envelope_json: string;
            };
            currentDispatcher?.(
                ev.forwarder_name,
                ev.sink_kind,
                ev.sink_json,
                ev.envelope_json,
            );
        });
}

async function clearDispatcherSub(): Promise<void> {
    if (dispatcherSub != null) {
        await dispatcherSub.remove();
        dispatcherSub = null;
    }
}

async function drainViaEventBridge(
    path: string,
    handler: DrainHandler,
): Promise<number> {
    const drainRequestId = `drain-${Date.now()}-${Math.random().toString(36).slice(2)}`;

    // Subscribe BEFORE kicking off — the native side starts emitting
    // events synchronously from drainQueueOnce.
    const sub = await Native.addListener("SentlogDrainEnvelope",
        async (raw: unknown) => {
            const ev = raw as { drainRequestId: string;
                                line: number;
                                envelope_json: string };
            if (ev.drainRequestId !== drainRequestId) return;
            // `keep` carries literal keep-semantics — true means
            // leave the envelope in the queue. handler returns
            // true=delete per its public contract, so we invert
            // once here and ship the literal keep flag over the
            // wire. Native inverts again when feeding the C ABI
            // cb (which expects 1=delete).
            let keep = true;
            try {
                keep = !handler(ev.envelope_json);
            } catch (err) {
                console.error("[sentlog] drain handler threw; keeping envelope", err);
                keep = true;
            }
            await Native.drainQueueReply({
                drainRequestId,
                line: ev.line,
                keep,
            });
        });
    try {
        const { delivered } = await Native.drainQueueOnce({ path, drainRequestId });
        return delivered;
    } finally {
        await sub.remove();
    }
}

export const Sentlog: SentlogBridge = {
    async version(): Promise<string> {
        return (await Native.version()).value;
    },

    async initialize(config: SentlogConfig): Promise<void> {
        await Native.initialize({ configJson: JSON.stringify(config) });
    },

    async startCapture(): Promise<void> {
        await Native.startCapture();
    },

    async evaluate(): Promise<SentlogResult> {
        const { resultJson } = await Native.evaluate();
        return JSON.parse(resultJson) as SentlogResult;
    },

    async cancel(): Promise<void> {
        await Native.cancel();
    },

    async destroy(): Promise<void> {
        await Native.destroy();
        await clearDispatcherSub();
        currentDispatcher = null;
    },

    async getPermissions(): Promise<Permissions> {
        const { json } = await Native.getPermissions();
        return JSON.parse(json) as Permissions;
    },

    async getCapabilities(): Promise<Capabilities> {
        const { json } = await Native.getCapabilities();
        return JSON.parse(json) as Capabilities;
    },

    async modelVersions(): Promise<ModelVersions> {
        const { json } = await Native.modelVersions();
        return JSON.parse(json) as ModelVersions;
    },

    async checkForUpdates(manifestJson: string): Promise<UpdateResult> {
        const { json } = await Native.checkForUpdates({ manifestJson });
        return JSON.parse(json) as UpdateResult;
    },

    async drainQueue(path: string, drain: DrainHandler): Promise<number> {
        return drainViaEventBridge(path, drain);
    },

    async setSigningKey(secretKeyBase64: string,
                        verificationMethod: string): Promise<void> {
        await Native.setSigningKey({ secretKeyBase64, verificationMethod });
    },

    async setEnvelopeDispatcher(handler: EnvelopeDispatcher | null): Promise<void> {
        currentDispatcher = handler;
        if (handler == null) {
            await Native.setEnvelopeDispatcherActive({ active: false });
            await clearDispatcherSub();
        } else {
            await ensureDispatcherSub();
            await Native.setEnvelopeDispatcherActive({ active: true });
        }
    },
};

/** Subscribe to native telemetry events (spec §3.2). Returns a
 *  PluginListenerHandle; call `.remove()` to unsubscribe.
 *
 *  Capacitor's notifyListeners requires an object payload (JSObject
 *  on Kotlin / [String: Any] on Swift), so the native plugin wraps
 *  the JSON-encoded event string in `{event: "<json>"}`. We unwrap
 *  here. */
export function onTelemetry(
    handler: (ev: TelemetryEvent) => void,
): Promise<PluginListenerHandle> {
    return Native.addListener("SentlogTelemetry", (raw: unknown) => {
        const wrap = raw as { event?: string };
        if (typeof wrap.event !== "string") return;
        try {
            handler(JSON.parse(wrap.event) as TelemetryEvent);
        } catch (err) {
            console.error("[sentlog] failed to parse telemetry event", err);
        }
    });
}
