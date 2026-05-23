import Foundation
import SentlogCore

/// Sentlog — universal, device-first evidence and presence SDK.
///
/// Used as a namespace; there is one SDK instance per app session.
///
/// ```
/// try Sentlog.initialize(config: SentlogConfig())
/// try Sentlog.startCapture()
/// // ...integrator captures user attention for 5-60 seconds...
/// let result = try await Sentlog.evaluate()
/// Sentlog.destroy()    // optional; the next initialize() also destroys
/// ```
///
/// Threading: `initialize`/`destroy`/`startCapture`/`cancel`/
/// `getPermissions`/`getCapabilities`/`modelVersions` are synchronous
/// and main-thread safe. `evaluate` and `checkForUpdates` are
/// `async throws`; the SDK marshals the C ABI callback into the
/// awaited continuation.
public enum Sentlog {

    // Mutable singleton state — protected by `stateLock` against
    // concurrent lifecycle calls. The C++ core itself serialises
    // lifecycle internally; this lock is for the Swift wrapper's
    // own bookkeeping.
    private static var handle:    OpaquePointer?
    private static var audio:     AudioCapture?
    private static var vadStream: SileroVadStream?
    private static var telemetry: NativeBridge.TelemetryBox?
    private static let stateLock = NSLock()

    private static let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = []
        return e
    }()
    private static let decoder = JSONDecoder()

    /// SDK version string.
    public static var version: String {
        guard let cstr = sentlog_version() else { return "" }
        return String(cString: cstr)
    }

    /// Initialise the SDK with `config`. If already initialised, the
    /// previous handle is destroyed first.
    ///
    /// - Throws ``SentlogError`` if the config fails validation in
    ///   the C++ core.
    public static func initialize(
        config: SentlogConfig,
        onTelemetry: ((TelemetryEvent) -> Void)? = nil
    ) throws {
        stateLock.lock()
        defer { stateLock.unlock() }

        destroyLocked()

        let data = try encoder.encode(config)
        let jsonStr = String(data: data, encoding: .utf8) ?? "{}"

        // Telemetry adapter: wrap the user callback in a TelemetryBox
        // so the C ABI's userdata pointer survives the call lifetime.
        var telemetryC: sentlog_telemetry_cb_t? = nil
        var telemetryUserdata: UnsafeMutableRawPointer? = nil
        if let onTelemetry {
            let box = NativeBridge.TelemetryBox { eventJson in
                guard let data = eventJson.data(using: .utf8) else { return }
                guard let event = try? decoder.decode(TelemetryEvent.self, from: data) else {
                    // Unknown event type — forward-compatibility: ignore.
                    return
                }
                onTelemetry(event)
            }
            telemetry = box
            telemetryUserdata = Unmanaged.passUnretained(box).toOpaque()
            telemetryC = { eventCStr, userdata in
                guard let userdata, let eventCStr else { return }
                let box = Unmanaged<NativeBridge.TelemetryBox>
                    .fromOpaque(userdata).takeUnretainedValue()
                box.onEvent(String(cString: eventCStr))
            }
        }

        var status: sentlog_status_t = SENTLOG_OK
        let h = jsonStr.withCString { cstr in
            sentlog_create(cstr, telemetryC, telemetryUserdata, &status)
        }
        guard let h else {
            telemetry = nil
            throw SentlogError(status: status, context: "initialize")
        }
        handle = OpaquePointer(h)
    }

    /// Start audio capture. Throws if not initialised, if lifecycle
    /// is violated, or if microphone permission is denied.
    public static func startCapture() throws {
        stateLock.lock()
        defer { stateLock.unlock() }

        let h = try requireHandleLocked()
        let s = sentlog_start_capture(OpaquePointer(h))
        try NativeBridge.mustSucceed(s, "startCapture")

        // Spin up a per-capture Silero session if the bundled model is
        // available. Failures fall back to the C++ pipeline's energy
        // VAD; see ADR 0009.
        let silero = loadSileroIfAvailable()
        vadStream = silero

        let cap = AudioCapture(
            pushFrame: { samplesPtr, count in
                sentlog_push_audio_frame(OpaquePointer(h), samplesPtr, count)
            },
            vad: silero,
            pushVad: silero == nil ? nil : { startSample, numSamples, prob in
                sentlog_push_vad_frame(OpaquePointer(h),
                                       Int(startSample),
                                       numSamples,
                                       prob)
            }
        )
        try cap.start()
        audio = cap
    }

    /// Try to load the bundled Silero VAD model. Returns nil if the
    /// resource isn't found (degrades to energy VAD) or if ORT init
    /// fails for any reason.
    private static func loadSileroIfAvailable() -> SileroVadStream? {
        guard let modelURL = Bundle.module.url(
            forResource: "silero_vad", withExtension: "onnx"
        ) else {
            return nil
        }
        do {
            return try SileroVadStream(modelPath: modelURL.path)
        } catch {
            return nil
        }
    }

    /// Stop capture and run the evaluation pipeline. Returns when the
    /// pipeline completes.
    public static func evaluate() async throws -> SentlogResult {
        let h = try { () throws -> OpaquePointer in
            stateLock.lock()
            defer { stateLock.unlock() }
            audio?.stop()
            audio = nil
            vadStream = nil
            return try requireHandleLocked()
        }()

        let resultJson: String = try await withCheckedThrowingContinuation { cont in
            let box = NativeBridge.ResultBox(cont)
            let userdata = Unmanaged.passRetained(box).toOpaque()
            let cb: sentlog_result_cb_t = { status, resultCStr, userdata in
                guard let userdata else { return }
                let box = Unmanaged<NativeBridge.ResultBox>
                    .fromOpaque(userdata).takeRetainedValue()
                guard status == SENTLOG_OK, let resultCStr else {
                    box.continuation?.resume(throwing:
                        SentlogError(status: status, context: "evaluate"))
                    return
                }
                box.continuation?.resume(returning: String(cString: resultCStr))
            }
            let kickoffStatus = sentlog_evaluate(h, cb, userdata)
            if kickoffStatus != SENTLOG_OK {
                // The callback won't fire — release the retained box and
                // surface the error directly.
                Unmanaged<NativeBridge.ResultBox>
                    .fromOpaque(userdata).release()
                cont.resume(throwing:
                    SentlogError(status: kickoffStatus, context: "evaluate"))
            }
        }

        guard let data = resultJson.data(using: .utf8) else {
            throw SentlogError(status: SENTLOG_ERR_INTERNAL, context: "evaluate-decode")
        }
        return try decoder.decode(SentlogResult.self, from: data)
    }

    /// Cancel any in-progress capture or evaluation.
    public static func cancel() {
        stateLock.lock()
        defer { stateLock.unlock() }
        audio?.stop()
        audio = nil
        vadStream = nil
        if let h = handle {
            sentlog_cancel(h)
        }
    }

    /// Current permission state. Never throws.
    public static func getPermissions() throws -> Permissions {
        try decode(Permissions.self, from: sentlog_get_permissions, label: "getPermissions")
    }

    /// Current capability detection. Never throws beyond
    /// initialisation requirements.
    public static func getCapabilities() throws -> Capabilities {
        try decode(Capabilities.self, from: sentlog_get_capabilities, label: "getCapabilities")
    }

    /// Versions of loaded models.
    public static func modelVersions() throws -> ModelVersions {
        try decode(ModelVersions.self, from: sentlog_model_versions, label: "modelVersions")
    }

    /// Check for OTA model updates. Integrator-triggered; never autonomous.
    public static func checkForUpdates() async throws -> UpdateResult {
        let h = try { () throws -> OpaquePointer in
            stateLock.lock()
            defer { stateLock.unlock() }
            return try requireHandleLocked()
        }()

        let updateJson: String = try await withCheckedThrowingContinuation { cont in
            let box = NativeBridge.UpdateBox(cont)
            let userdata = Unmanaged.passRetained(box).toOpaque()
            let cb: sentlog_update_cb_t = { status, updateCStr, userdata in
                guard let userdata else { return }
                let box = Unmanaged<NativeBridge.UpdateBox>
                    .fromOpaque(userdata).takeRetainedValue()
                guard status == SENTLOG_OK, let updateCStr else {
                    box.continuation?.resume(throwing:
                        SentlogError(status: status, context: "checkForUpdates"))
                    return
                }
                box.continuation?.resume(returning: String(cString: updateCStr))
            }
            let kickoffStatus = sentlog_check_for_updates(h, cb, userdata)
            if kickoffStatus != SENTLOG_OK {
                Unmanaged<NativeBridge.UpdateBox>
                    .fromOpaque(userdata).release()
                cont.resume(throwing:
                    SentlogError(status: kickoffStatus, context: "checkForUpdates"))
            }
        }
        guard let data = updateJson.data(using: .utf8) else {
            throw SentlogError(status: SENTLOG_ERR_INTERNAL,
                               context: "checkForUpdates-decode")
        }
        return try decoder.decode(UpdateResult.self, from: data)
    }

    /// Drain a local queue. Phase 4; Phase 2a returns 0.
    public static func drainQueue(path: String,
                                  drain: (String) -> Bool) -> Int {
        _ = path; _ = drain
        return 0
    }

    /// Release the SDK handle. Safe to call multiple times.
    public static func destroy() {
        stateLock.lock()
        defer { stateLock.unlock() }
        destroyLocked()
    }

    // MARK: helpers

    private static func destroyLocked() {
        audio?.stop()
        audio = nil
        vadStream = nil
        if let h = handle {
            sentlog_destroy(h)
            handle = nil
        }
        telemetry = nil
    }

    private static func requireHandleLocked() throws -> OpaquePointer {
        guard let h = handle else {
            throw SentlogError(status: SENTLOG_ERR_INVALID_STATE,
                               context: "uninitialised")
        }
        return h
    }

    private static func decode<T: Decodable>(
        _ type: T.Type,
        from cFn: (OpaquePointer?) -> UnsafeMutablePointer<CChar>?,
        label: String
    ) throws -> T {
        stateLock.lock()
        let h = try requireHandleLocked()
        stateLock.unlock()

        guard let cstr = cFn(h) else {
            throw SentlogError(status: SENTLOG_ERR_INTERNAL, context: "\(label)-null")
        }
        defer { sentlog_string_free(cstr) }
        let json = String(cString: cstr)
        guard let data = json.data(using: .utf8) else {
            throw SentlogError(status: SENTLOG_ERR_INTERNAL,
                               context: "\(label)-encoding")
        }
        return try decoder.decode(T.self, from: data)
    }
}
