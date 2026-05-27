// GENERATED — DO NOT EDIT. Synced from shared/types/sentlog.ts
// by tools/sync-types.sh. CI verifies these copies stay byte-equal
// with the canonical file; edit shared/types/sentlog.ts and re-run
// the script if you need to change a type.

// Sentlog public TypeScript surface — canonical.
//
// This file is the source of truth for the types both @sentlog/
// react-native and @sentlog/capacitor expose. Each package's
// src/types.ts is a byte-identical copy synced from here by
// tools/sync-types.sh; CI's `tools (types)` job verifies the
// copies haven't drifted.
//
// The types mirror the public API in
// docs/technical-specification.md §3 and the Kotlin/Swift bindings.
// JSON wire format is what's documented in §6.4 — these TS types
// describe the post-parse shape after the bridge has run
// JSON.parse on the C++ output.
//
// All fields use the snake_case JSON names from the wire shape. The
// per-bridge code is responsible for any camelCase remapping
// integrators see (matching JS convention).

// ----- status codes (mirrors sentlog_status_t in sentlog.h) ------

export const SENTLOG_OK                    = 0;
export const SENTLOG_ERR_INVALID_CONFIG    = 1;
export const SENTLOG_ERR_INVALID_STATE     = 2;
export const SENTLOG_ERR_PERMISSION_DENIED = 3;
export const SENTLOG_ERR_CAPTURE_FAILED    = 4;
export const SENTLOG_ERR_MODEL_LOAD        = 5;
export const SENTLOG_ERR_MANIFEST_VERIFY   = 6;
export const SENTLOG_ERR_CANCELLED         = 7;
export const SENTLOG_ERR_HARDWARE_SIGNING  = 8;
export const SENTLOG_ERR_TSA               = 9;
export const SENTLOG_ERR_INTERNAL          = 99;

export type SentlogStatus =
    | typeof SENTLOG_OK
    | typeof SENTLOG_ERR_INVALID_CONFIG
    | typeof SENTLOG_ERR_INVALID_STATE
    | typeof SENTLOG_ERR_PERMISSION_DENIED
    | typeof SENTLOG_ERR_CAPTURE_FAILED
    | typeof SENTLOG_ERR_MODEL_LOAD
    | typeof SENTLOG_ERR_MANIFEST_VERIFY
    | typeof SENTLOG_ERR_CANCELLED
    | typeof SENTLOG_ERR_HARDWARE_SIGNING
    | typeof SENTLOG_ERR_TSA
    | typeof SENTLOG_ERR_INTERNAL;

/** Typed error surfaced when a native call fails. The `context`
 *  string names the operation (`"initialize"`, `"startCapture"`,
 *  etc.) so integrators can branch on the failure point. */
export interface SentlogError {
    readonly status: SentlogStatus;
    readonly context: string;
    readonly message: string;
}

// ----- config (mirrors SentlogConfig in spec §3.3) ---------------

export type Projection =
    | "Full"
    | "Full+AudioClip"
    | "ScoresOnly"
    | "TranscriptOnly"
    | "EvidenceFingerprint"
    | "EvidenceClip"
    | "Custom";

export type SinkKind = "http" | "webhook" | "local_queue" | "callback";

/** Sink-side JSON shapes. Per `parse_forwarder` in
 *  shared/src/internal/config.cpp, http/webhook require `url`,
 *  webhook additionally requires `hmac_secret`, local_queue
 *  requires `path`, callback has no required fields. */
export interface SinkHttp     { kind: "http";        url: string; headers?: Record<string, string>; }
export interface SinkWebhook  { kind: "webhook";     url: string; hmac_secret: string; signature_header?: string; headers?: Record<string, string>; }
export interface SinkLocal    { kind: "local_queue"; path: string; }
export interface SinkCallback { kind: "callback"; }
export type Sink = SinkHttp | SinkWebhook | SinkLocal | SinkCallback;

export interface Forwarder {
    name: string;
    projection: Projection;
    /** Only required when projection === "Custom". Dotted paths
     *  into SentlogResult (e.g. "transcript.text"). */
    custom_fields?: string[];
    /** Float in [0, 1]. Default 1.0. */
    sample_rate?: number;
    sink: Sink;
}

/** Known scoring_weight keys. Unknown keys are rejected at init.
 *  Defaults documented in docs/scoring-weights.md. */
export type ScoringWeightKey =
    | "liveness.replay"
    | "liveness.synthetic"
    | "engagement.voice"
    | "engagement.face"
    | "presence.audio_source"
    | "presence.motion"
    | "presence.integrity";

export interface SentlogConfig {
    liveness_threshold?:   number;
    engagement_threshold?: number;
    direction_threshold?:  number;
    min_confidence?:       number;

    challenge_enabled?:    boolean;
    language_hint?:        string;
    min_duration_ms?:      number;
    max_duration_ms?:      number;
    target_phrase?:        string | null;

    use_motion_sensors?:   boolean;
    use_video_affect?:     boolean;
    use_device_integrity?: boolean;

    scoring_weights?:      Partial<Record<ScoringWeightKey, number>>;

    /** Required when forwarders is non-empty (the envelope's
     *  `issuer`). */
    issuer?:               string;
    signing_key_id?:       string | null;
    use_hardware_signing?: boolean;
    hardware_signing_required?: boolean;

    tsa_endpoint?:         string | null;
    tsa_timeout_ms?:       number;
    tsa_required?:         boolean;

    session_id?:                 string | null;
    companion_evidence_refs?:    string[];

    forwarders?:           Forwarder[];

    /** OTA manifest override — spec §1.20. Defaults to the published
     *  Sentlog manifest URL + pinned pubkeys. Overriding requires
     *  overriding BOTH together — mixed-source configuration is
     *  rejected at init. `manifest_pubkeys` is `[active, standby]`,
     *  each entry the base64-encoded raw 32-byte Ed25519 pubkey. */
    manifest_url?:         string | null;
    manifest_pubkeys?:     [string, string];
}

// ----- result (mirrors SentlogResult in spec §3) -----------------

export type EvidenceGrade = "DeviceUnattested" | "DeviceAttested" | "EnclaveSigned";

export type FailureReason =
    | "TooShort"
    | "TooNoisy"
    | "NoSpeechDetected"
    | "LanguageMismatch"
    | "Interrupted"
    | "InternalError";

export type AudioQuality = "quiet" | "moderate" | "noisy" | "very_noisy";

export type EngagementModelStatus = "placeholder" | "training" | "trained";

export interface WordTiming {
    word: string;
    start_ms: number;
    end_ms: number;
}

export interface Transcript {
    text: string;
    language: string;
    confidence: number;
    word_timings?: WordTiming[];
    /** Null until the pipeline computes it (Phase 2d). Range [0, 1]. */
    target_match: number | null;
}

export interface Liveness {
    score: number;
    replay_risk: number;
    synthetic_risk: number;
}

export interface Presence {
    summary: number;
    audio_source: number;
    motion?: number;
    integrity?: number;
}

export interface Engagement {
    score: number;
    voice_score: number;
    /** Populated only when video affect aggregated frames (Phase 5). */
    face_score?: number;
    model_status: EngagementModelStatus;
}

export interface Affect {
    valence: number;     // [-1, 1]
    arousal: number;     // [0, 1]
    dominant_fau?: string[];
}

export interface AudioFingerprint {
    algo: string;        // currently "sentlog-pcp-v1"
    bytes: string;       // base64-encoded
}

export interface ModelVersions {
    liveness: string;
    asr: string;
    prosody: string;
    vad: string;
    affect?: string;
    manifest: string;
}

export interface Diagnostics {
    failure_reason?:    FailureReason;
    snr_db?:            number;
    duration_ms:        number;
    audio_quality:      AudioQuality;
    detected_language?: string;
    language_mismatch?: boolean;
}

export interface SentlogResult {
    schema_version:  string;
    transcript:      Transcript;
    liveness:        Liveness;
    presence:        Presence;
    direction:       number;
    confidence:      number;
    thresholds_met:  boolean;
    engagement:      Engagement;
    affect:          Affect | null;
    fingerprint:     AudioFingerprint;
    model_versions:  ModelVersions;
    diagnostics:     Diagnostics;
    evidence_grade:  EvidenceGrade;
    /** Integrator-opaque passthrough; SDK never interprets. */
    extensions:      Record<string, unknown>;
}

// ----- permissions / capabilities / model versions getters -------

export type PermissionState = "granted" | "denied" | "prompt" | "not_requested";

export interface Permissions {
    microphone: PermissionState;
    motion:     PermissionState;
    camera:     PermissionState;
}

export type Runtime = "native" | "capacitor" | "react_native" | "web_fallback";

export interface Capabilities {
    audio_capture: boolean;
    motion_sensors: boolean;
    video_face_mesh: boolean;
    device_integrity_check: boolean;
    ota_updates: boolean;
    local_queue: boolean;
    runtime: Runtime;
}

// ----- OTA -------------------------------------------------------

export interface UpdateResult {
    checked_at_ms:    number;
    update_found:     boolean;
    previous_version: string;
    new_version?:     string;
    applied:          boolean;
    error?:           string;
}

// ----- telemetry (mirrors §3.2 union, exhaustive) ----------------

export type ForwarderOutcomeKind = "ok" | "retry" | "dropped";
export type PermissionKind       = "microphone" | "motion" | "camera";

export type TelemetryEvent =
    | { type: "PipelineTiming";            stage: string; duration_ms: number }
    | { type: "ScoreHistogram";            field: string; bucket: string }
    | { type: "ForwarderOutcome";          forwarder_name: string; outcome: ForwarderOutcomeKind; attempts: number }
    | { type: "ManifestVerifyFailed";      reason: string; attempted_version: string }
    | { type: "ModelLoadError";            model: string; reason: string }
    | { type: "PermissionDenied";          permission: PermissionKind }
    | { type: "CaptureAborted";            reason: string }
    | { type: "HardwareSigningUnavailable"; reason: string; fell_back_to: "software_ed25519" }
    | { type: "TsaRequestFailed";          tsa_url: string; reason: string };

// ----- forwarder dispatcher --------------------------------------

/** Phase 4d envelope dispatcher signature. Bindings forward
 *  envelopes to the integrator's handler exactly as the C++ core
 *  produces them. `envelope_json` is a JSON string ready to POST
 *  / persist; the integrator's HTTP client is what actually
 *  delivers http/webhook sinks. local_queue is written by the
 *  C++ core itself and never reaches this callback. */
export type EnvelopeDispatcher = (
    forwarder_name: string,
    sink_kind: SinkKind,
    sink_json: string,
    envelope_json: string,
) => void;

// ----- drain queue -----------------------------------------------

/** Phase 4i drain callback. Return `true` to delete the envelope
 *  from the queue, `false` to keep it for a later drain. */
export type DrainHandler = (envelope_json: string) => boolean;

// ----- the Sentlog public surface (shape both bridges expose) ---

/** Common shape both @sentlog/react-native and @sentlog/capacitor
 *  expose. The packages each ship their own implementation (RN
 *  uses NativeModules; Capacitor uses registerPlugin) but the
 *  signatures match this interface byte-for-byte. */
export interface SentlogBridge {
    /** Native SDK version reported by the underlying platform binding. */
    version(): Promise<string>;

    /** Initialize the SDK. Replaces an existing handle if one was open. */
    initialize(config: SentlogConfig): Promise<void>;

    /** Start audio capture. Throws on permission-denied or lifecycle violation. */
    startCapture(): Promise<void>;

    /** Stop capture and run the pipeline. */
    evaluate(): Promise<SentlogResult>;

    /** Cancel an in-progress capture or evaluation. Safe to call any time. */
    cancel(): Promise<void>;

    /** Release SDK resources. Idempotent. */
    destroy(): Promise<void>;

    /** Current permission state. */
    getPermissions(): Promise<Permissions>;

    /** Runtime capabilities. */
    getCapabilities(): Promise<Capabilities>;

    /** Versions of loaded models. */
    modelVersions(): Promise<ModelVersions>;

    /** Check for an OTA model update (Phase 7 stub returns no-update today). */
    /** Verify a fetched signed manifest envelope (spec §5.1) against
     *  the configured pubkeys. The HTTPS fetch is the integrator's
     *  responsibility today; a no-argument overload that performs the
     *  fetch internally lands in 7b-ii. */
    checkForUpdates(manifestJson: string): Promise<UpdateResult>;

    /** Drain a local-queue NDJSON file. */
    drainQueue(path: string, drain: DrainHandler): Promise<number>;

    /** Phase 4d: register the Ed25519 signing key. `secretKey` is a
     *  base64 string of the 64-byte RFC 8032 seed||pubkey. */
    setSigningKey(secretKeyBase64: string, verificationMethod: string): Promise<void>;

    /** Phase 4d: register (or clear, with `null`) the per-handle
     *  envelope dispatcher. */
    setEnvelopeDispatcher(handler: EnvelopeDispatcher | null): Promise<void>;
}
