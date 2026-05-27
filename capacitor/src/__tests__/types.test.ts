// Type-fixture test (Phase 6i). Compile-time only — exercising
// every exported symbol from @sentlog/capacitor so any breaking
// change to the canonical shared/types/sentlog.ts surfaces here
// before consumers feel it.

import {
    Sentlog,
    onTelemetry,
    SENTLOG_CAPACITOR_VERSION,
    SENTLOG_OK,
    SENTLOG_ERR_INVALID_CONFIG,
    SENTLOG_ERR_INVALID_STATE,
    SENTLOG_ERR_PERMISSION_DENIED,
    SENTLOG_ERR_CAPTURE_FAILED,
    SENTLOG_ERR_MODEL_LOAD,
    SENTLOG_ERR_MANIFEST_VERIFY,
    SENTLOG_ERR_CANCELLED,
    SENTLOG_ERR_HARDWARE_SIGNING,
    SENTLOG_ERR_TSA,
    SENTLOG_ERR_INTERNAL,
} from "../index";

import type {
    SentlogStatus,
    SentlogError,
    SentlogBridge,
    SentlogConfig,
    SentlogResult,
    Transcript,
    WordTiming,
    Liveness,
    Presence,
    Engagement,
    EngagementModelStatus,
    Affect,
    AudioFingerprint,
    ModelVersions,
    Diagnostics,
    FailureReason,
    AudioQuality,
    EvidenceGrade,
    Permissions,
    PermissionState,
    Capabilities,
    Runtime,
    UpdateResult,
    Projection,
    SinkKind,
    Sink,
    SinkHttp,
    SinkWebhook,
    SinkLocal,
    SinkCallback,
    Forwarder,
    ScoringWeightKey,
    TelemetryEvent,
    ForwarderOutcomeKind,
    PermissionKind,
    EnvelopeDispatcher,
    DrainHandler,
} from "../index";

const _statuses: SentlogStatus[] = [
    SENTLOG_OK, SENTLOG_ERR_INVALID_CONFIG, SENTLOG_ERR_INVALID_STATE,
    SENTLOG_ERR_PERMISSION_DENIED, SENTLOG_ERR_CAPTURE_FAILED,
    SENTLOG_ERR_MODEL_LOAD, SENTLOG_ERR_MANIFEST_VERIFY,
    SENTLOG_ERR_CANCELLED, SENTLOG_ERR_HARDWARE_SIGNING,
    SENTLOG_ERR_TSA, SENTLOG_ERR_INTERNAL,
];

const _err: SentlogError = { status: SENTLOG_OK, context: "x", message: "" };

const _projections: Projection[] = [
    "Full", "Full+AudioClip", "ScoresOnly", "TranscriptOnly",
    "EvidenceFingerprint", "EvidenceClip", "Custom",
];

const _sinks: Sink[] = [
    { kind: "http", url: "https://x" } satisfies SinkHttp,
    { kind: "webhook", url: "https://x", hmac_secret: "k" } satisfies SinkWebhook,
    { kind: "local_queue", path: "/tmp/q" } satisfies SinkLocal,
    { kind: "callback" } satisfies SinkCallback,
];

const _sinkKinds: SinkKind[] = ["http", "webhook", "local_queue", "callback"];

const _fw: Forwarder = {
    name: "x",
    projection: "Custom",
    custom_fields: ["transcript.text", "liveness.score"],
    sample_rate: 0.5,
    sink: { kind: "callback" },
};
const _swKey: ScoringWeightKey = "engagement.face";

const _cfgEmpty: SentlogConfig = {};
const _cfgFull: SentlogConfig = {
    liveness_threshold: 0.7,
    engagement_threshold: 0.5,
    direction_threshold: 0.6,
    min_confidence: 0.6,
    challenge_enabled: true,
    language_hint: "en",
    min_duration_ms: 5000,
    max_duration_ms: 60000,
    target_phrase: "x",
    use_motion_sensors: false,
    use_video_affect: false,
    use_device_integrity: false,
    scoring_weights: { "engagement.face": 0.8 },
    issuer: "did:example:x",
    signing_key_id: null,
    use_hardware_signing: false,
    hardware_signing_required: false,
    tsa_endpoint: null,
    tsa_timeout_ms: 5000,
    tsa_required: false,
    session_id: null,
    companion_evidence_refs: [],
    forwarders: [_fw],
};

declare const _r: SentlogResult;
const _v: string = _r.schema_version;
const _t: Transcript = _r.transcript;
const _wt: WordTiming = { word: "x", start_ms: 0, end_ms: 100 };
const _li: Liveness = _r.liveness;
const _pr: Presence = _r.presence;
const _eg: Engagement = _r.engagement;
const _es: EngagementModelStatus = _eg.model_status;
const _af: Affect | null = _r.affect;
const _fp: AudioFingerprint = _r.fingerprint;
const _mv: ModelVersions = _r.model_versions;
const _di: Diagnostics = _r.diagnostics;
const _fr: FailureReason | undefined = _di.failure_reason;
const _aq: AudioQuality = _di.audio_quality;
const _ev: EvidenceGrade = _r.evidence_grade;

declare const _perms: Permissions;
const _pmic: PermissionState = _perms.microphone;
declare const _caps: Capabilities;
const _rt: Runtime = _caps.runtime;
declare const _upd: UpdateResult;
const _uok: boolean = _upd.update_found;

const _telemEvents: TelemetryEvent[] = [
    { type: "PipelineTiming", stage: "ms", duration_ms: 1 },
    { type: "ScoreHistogram", field: "liveness", bucket: "high" },
    { type: "ForwarderOutcome", forwarder_name: "f", outcome: "ok" satisfies ForwarderOutcomeKind, attempts: 1 },
    { type: "ManifestVerifyFailed", reason: "x", attempted_version: "1" },
    { type: "ModelLoadError", model: "asr", reason: "missing" },
    { type: "PermissionDenied", permission: "microphone" satisfies PermissionKind },
    { type: "CaptureAborted", reason: "x" },
    { type: "HardwareSigningUnavailable", reason: "x", fell_back_to: "software_ed25519" },
    { type: "TsaRequestFailed", tsa_url: "https://x", reason: "x" },
];

const _dispatcher: EnvelopeDispatcher = (_n, _k, _sj, _ej) => { /* no-op */ };
const _drain: DrainHandler = (_envelope) => true;

const _bridge: SentlogBridge = Sentlog;
const _ver: string = SENTLOG_CAPACITOR_VERSION;

// onTelemetry returns a Promise<PluginListenerHandle>; just touch.
const _subPromise = onTelemetry((_e: TelemetryEvent) => { /* no-op */ });
void _subPromise;

export const _typesTestSink = {
    _statuses, _err, _projections, _sinks, _sinkKinds, _fw, _swKey,
    _cfgEmpty, _cfgFull, _v, _t, _wt, _li, _pr, _eg, _es, _af, _fp,
    _mv, _di, _fr, _aq, _ev, _perms, _pmic, _caps, _rt, _upd, _uok,
    _telemEvents, _dispatcher, _drain, _bridge, _ver, _subPromise,
};
