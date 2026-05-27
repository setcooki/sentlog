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
    private static var motion:    MotionCapture?
    private static var currentConfig: SentlogConfig?
    private static var telemetry: NativeBridge.TelemetryBox?
    /// Opaque pointer to the DispatcherBox passed to the C ABI as
    /// `userdata` via `Unmanaged.passRetained(box).toOpaque()`. The
    /// box's lifetime is the +1 retain that ptr "owns"; release via
    /// `Unmanaged<DispatcherBox>.fromOpaque(ptr).release()` to
    /// balance. Stored as opaque (not the box itself) so the
    /// retain/release stays symmetric.
    private static var dispatcherUserdata: UnsafeMutableRawPointer?
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
        handle = h
        currentConfig = config
    }

    /// Start audio capture. Throws if not initialised, if lifecycle
    /// is violated, or if microphone permission is denied.
    public static func startCapture() throws {
        stateLock.lock()
        defer { stateLock.unlock() }

        let h = try requireHandleLocked()
        let s = sentlog_start_capture(h)
        try NativeBridge.mustSucceed(s, "startCapture")

        // Push the current audio input route so the C++ pipeline can
        // populate presence.audio_source. Without this push the route
        // stays UNKNOWN → audio_source = 0.0 in the result.
        let route = AudioRouteDetector.detect()
        _ = sentlog_set_audio_route(h, route)

        // Spin up a per-capture Silero session if the bundled model is
        // available. Failures fall back to the C++ pipeline's energy
        // VAD; see ADR 0009.
        let silero = loadSileroIfAvailable()
        vadStream = silero

        let cap = AudioCapture(
            pushFrame: { samplesPtr, count in
                sentlog_push_audio_frame(h, samplesPtr, count)
            },
            vad: silero,
            pushVad: silero == nil ? nil : { startSample, numSamples, prob in
                sentlog_push_vad_frame(h,
                                       Int(startSample),
                                       numSamples,
                                       prob)
            }
        )
        try cap.start()
        audio = cap

        // Phase 3c: motion-coherence. Opt-in via config. CoreMotion
        // doesn't require a runtime permission for the IMU on iOS;
        // if the platform lacks accelerometer (macOS) MotionCapture
        // silently no-ops and the pipeline emits no presence.motion.
        if currentConfig?.useMotionSensors == true {
            let m = MotionCapture { tsNs, ax, ay, az, gx, gy, gz in
                sentlog_push_motion_frame(h, tsNs, ax, ay, az, gx, gy, gz)
            }
            m.start()
            motion = m
        }
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

    /// Run AASIST-L over the captured audio if the model is bundled.
    /// Safe to call always — if the model file isn't present, this
    /// is a no-op and the pipeline emits the placeholder liveness.
    private static func runAasistAndPushLiveness(handle: OpaquePointer) {
        guard let modelURL = Bundle.module.url(
            forResource: "aasist_l", withExtension: "onnx"
        ) else { return }

        let capacity = AasistLivenessRunner.targetSamples
        var buf = [Float](repeating: 0, count: capacity)
        var written: size_t = 0
        let snapshotStatus = buf.withUnsafeMutableBufferPointer { ptr -> sentlog_status_t in
            sentlog_snapshot_audio(handle, ptr.baseAddress, capacity, &written)
        }
        guard snapshotStatus == SENTLOG_OK, written > 0 else { return }
        let audioSlice = written == capacity ? buf : Array(buf.prefix(Int(written)))

        do {
            let runner = try AasistLivenessRunner(modelPath: modelURL.path)
            let result = try runner.infer(audioSlice)
            _ = sentlog_push_liveness_result(
                handle, result.score, result.replayRisk, result.syntheticRisk
            )
        } catch {
            // ORT failure shouldn't fail the whole evaluation — the
            // pipeline carries the placeholder liveness and
            // model_versions.liveness flags it.
        }
    }

    /// Run whisper.cpp over the captured audio if the model is
    /// bundled. Safe to call always — when the resource is missing
    /// this is a no-op and the C++ pipeline emits the placeholder
    /// transcript.
    private static func runWhisperAndPushTranscript(handle: OpaquePointer) {
        // Capacity matches the C++ ring buffer (~30 s @ 16 kHz).
        let capacity = whisperMaxSamples
        var buf = [Float](repeating: 0, count: capacity)
        var written: size_t = 0
        let snapshotStatus = buf.withUnsafeMutableBufferPointer { ptr -> sentlog_status_t in
            sentlog_snapshot_audio(handle, ptr.baseAddress, capacity, &written)
        }
        guard snapshotStatus == SENTLOG_OK, written > 0 else { return }
        let audioSlice = written == capacity ? buf : Array(buf.prefix(Int(written)))

        guard let runner = WhisperRunner.loadIfAvailable() else { return }
        guard let json = runner.transcribe(audioSlice) else { return }
        _ = json.withCString { ptr in
            sentlog_push_transcript(handle, ptr)
        }
    }

    private static let whisperMaxSamples: Int = 16_000 * 30

    /// Stop capture and run the evaluation pipeline. Returns when the
    /// pipeline completes.
    public static func evaluate() async throws -> SentlogResult {
        // The whole synchronous critical section runs under stateLock:
        // we need to keep the C handle `h` alive against a concurrent
        // destroy() while AASIST + whisper snapshot + push into it,
        // and while sentlog_evaluate runs. The C ABI guarantees the
        // result callback fires synchronously from inside
        // sentlog_evaluate (see c_api.cpp), so the continuation has
        // already resolved by the time we release the lock and start
        // the await.
        let resultJson: String = try await withCheckedThrowingContinuation { cont in
            stateLock.lock()
            defer { stateLock.unlock() }

            let h: OpaquePointer
            do {
                audio?.stop()
                audio = nil
                vadStream = nil
                motion?.stop()
                motion = nil
                h = try requireHandleLocked()
            } catch {
                cont.resume(throwing: error)
                return
            }

            // Run AASIST-L (utterance liveness) and whisper.cpp (ASR)
            // in sequence BEFORE the native evaluate so their pushed
            // results land in the pipeline. Snapshots are
            // non-destructive; the pipeline still drains the audio
            // inside sentlog_evaluate. Both are best-effort —
            // failure leaves the corresponding placeholder in place
            // and model_versions.* flags it.
            runAasistAndPushLiveness(handle: h)
            runWhisperAndPushTranscript(handle: h)

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
            // sentlog_evaluate always fires cb synchronously — even on
            // failure — per the C ABI contract (c_api.cpp). We don't
            // need to handle a "kickoff failed without cb firing"
            // branch; the continuation is always resolved by cb.
            _ = sentlog_evaluate(h, cb, userdata)
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
        motion?.stop()
        motion = nil
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
            // The Phase 1/2 stub fires cb synchronously per the C ABI;
            // no "kickoff failed without cb" branch needed.
            _ = sentlog_check_for_updates(h, cb, userdata)
        }
        guard let data = updateJson.data(using: .utf8) else {
            throw SentlogError(status: SENTLOG_ERR_INTERNAL,
                               context: "checkForUpdates-decode")
        }
        return try decoder.decode(UpdateResult.self, from: data)
    }

    /// Drain a local-queue forwarder file (NDJSON). For each
    /// envelope in the file, `drain` is invoked; return `true` to
    /// remove the envelope from the queue, `false` to keep it for a
    /// later drain. Returns the number of envelopes delivered.
    ///
    /// Safe to call without a live SDK handle — the file I/O is
    /// pure POSIX, no Sentlog state required. We pass the current
    /// handle when available (the C ABI tolerates null).
    public static func drainQueue(path: String,
                                  drain: (String) -> Bool) -> Int {
        // sentlog_drain_queue is synchronous — the closure cb fires
        // before the call returns and then never again. Use
        // withoutActuallyEscaping so we can hand the non-escaping
        // closure across the C ABI boundary without needing the
        // caller to mark it @escaping.
        return withoutActuallyEscaping(drain) { escapingDrain in
            final class DrainBox {
                let cb: (String) -> Bool
                init(_ c: @escaping (String) -> Bool) { cb = c }
            }
            let box = DrainBox(escapingDrain)
            let userdata = Unmanaged.passUnretained(box).toOpaque()
            let cb: sentlog_drain_cb_t = { jsonCStr, _len, ud in
                guard let ud, let jsonCStr else { return 0 }
                let b = Unmanaged<DrainBox>.fromOpaque(ud).takeUnretainedValue()
                return b.cb(String(cString: jsonCStr)) ? 1 : 0
            }
            stateLock.lock()
            let h = handle
            stateLock.unlock()
            return Int(sentlog_drain_queue(h, path, cb, userdata))
        }
    }

    /// Phase 4d: register the Ed25519 signing key the SDK uses to
    /// populate the W3C VC 2.0 envelope's `proof` block.
    /// `secretKey` is the 64-byte RFC 8032 seed||pubkey
    /// concatenation; the integrator obtains it from libsodium /
    /// CryptoKit / hardware-backed Secure Enclave (Phase 7 swaps
    /// in `ecdsa-jcs-2019` for hardware). Throws if the key is the
    /// wrong length.
    public static func setSigningKey(_ secretKey: Data,
                                     verificationMethod: String) throws {
        stateLock.lock()
        defer { stateLock.unlock() }
        let h = try requireHandleLocked()
        let status = secretKey.withUnsafeBytes { buf -> sentlog_status_t in
            let p = buf.baseAddress?.bindMemory(to: UInt8.self,
                                                capacity: secretKey.count)
            return sentlog_set_signing_key(h, p, secretKey.count,
                                           verificationMethod)
        }
        try NativeBridge.mustSucceed(status, "setSigningKey")
    }

    /// Phase 4d: register (or clear, with `nil`) the per-handle
    /// envelope dispatcher. Fires once per configured forwarder after
    /// evaluate, for sinks other than `local_queue` (the C++ core
    /// writes those to file). The closure runs on the SDK's I/O
    /// thread; long-running work (HTTP POSTs, etc.) should hand off
    /// to the integrator's own scheduler.
    ///
    /// Exception semantics: the closure type is non-throwing, so
    /// Swift errors can't escape. Obj-C exceptions thrown from
    /// bridged code (e.g. NSException via Foundation calls in the
    /// dispatcher body) would unwind through the C++ dispatch loop
    /// and crash the host process — wrap such calls in try/catch on
    /// the integrator's side. Unlike the Android binding's JNI
    /// trampoline (which clears any pending Java exception so the
    /// loop survives), there is no Swift-side equivalent to catch
    /// for non-throwing closures.
    public static func setEnvelopeDispatcher(
        _ handler: ((_ forwarderName: String,
                     _ sinkKind: String,
                     _ sinkJson: String,
                     _ envelopeJson: String) -> Void)?
    ) throws {
        stateLock.lock()
        defer { stateLock.unlock() }
        let h = try requireHandleLocked()

        guard let handler else {
            // Clear at the C ABI first so the trampoline stops
            // firing, then release our retained box via the
            // symmetric fromOpaque(...).release().
            let s = sentlog_set_envelope_dispatcher(h, nil, nil)
            try NativeBridge.mustSucceed(s, "setEnvelopeDispatcher.clear")
            if let prev = dispatcherUserdata {
                Unmanaged<NativeBridge.DispatcherBox>
                    .fromOpaque(prev).release()
                dispatcherUserdata = nil
            }
            return
        }

        let box = NativeBridge.DispatcherBox(handler)
        let userdata = Unmanaged.passRetained(box).toOpaque()
        let cb: sentlog_envelope_dispatch_cb_t = { fwName, sKind, sJson, eJson, ud in
            guard let ud else { return }
            let b = Unmanaged<NativeBridge.DispatcherBox>
                .fromOpaque(ud).takeUnretainedValue()
            b.onEnvelope(
                fwName.map { String(cString: $0) } ?? "",
                sKind.map  { String(cString: $0) } ?? "",
                sJson.map  { String(cString: $0) } ?? "",
                eJson.map  { String(cString: $0) } ?? ""
            )
        }
        let s = sentlog_set_envelope_dispatcher(h, cb, userdata)
        guard s == SENTLOG_OK else {
            // Roll back our retain — the C ABI didn't accept the
            // pointer, so we own the imbalance.
            Unmanaged<NativeBridge.DispatcherBox>
                .fromOpaque(userdata).release()
            throw SentlogError(status: s, context: "setEnvelopeDispatcher")
        }
        // Release the previous registration's retain (if any) only
        // after the new one is accepted by the C ABI — keeps the old
        // box alive until the swap point.
        if let prev = dispatcherUserdata {
            Unmanaged<NativeBridge.DispatcherBox>
                .fromOpaque(prev).release()
        }
        dispatcherUserdata = userdata
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
        motion?.stop()
        motion = nil
        if let h = handle {
            // Clear the envelope dispatcher at the C ABI before
            // destroy so the trampoline can't fire against a
            // freed handle. Then release our retained box.
            _ = sentlog_set_envelope_dispatcher(h, nil, nil)
            sentlog_destroy(h)
            handle = nil
        }
        if let prev = dispatcherUserdata {
            Unmanaged<NativeBridge.DispatcherBox>
                .fromOpaque(prev).release()
            dispatcherUserdata = nil
        }
        telemetry = nil
        currentConfig = nil
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
        // Hold stateLock across the C call so a concurrent
        // destroy() can't free the handle between our read and the
        // cFn invocation. JSON decode happens outside the lock —
        // it operates on a heap copy of the C string we own via
        // sentlog_string_free.
        stateLock.lock()
        let cstr: UnsafeMutablePointer<CChar>?
        do {
            let h = try requireHandleLocked()
            cstr = cFn(h)
        } catch {
            stateLock.unlock()
            throw error
        }
        stateLock.unlock()

        guard let cstr else {
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
