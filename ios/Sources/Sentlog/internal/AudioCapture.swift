import AVFoundation
import Foundation

/// Real-time audio capture using AVAudioEngine. Captures the device's
/// native input format, converts to 16 kHz mono Float32 via
/// AVAudioConverter, and forwards each frame to `pushFrame`.
///
/// AVAudioSession is configured for `.record` with mode `.measurement`
/// — that disables iOS's voice processing (noise suppression, AGC,
/// AEC) which would otherwise distort the audio in ways that break
/// liveness scoring.
///
/// The integrator is responsible for requesting microphone permission
/// before `start()` is called; we surface the platform error if denied.
internal final class AudioCapture {

    private let pushFrame: (UnsafePointer<Float>?, Int) -> sentlog_status_t

    private let engine = AVAudioEngine()
    private var converter: AVAudioConverter?
    private var targetFormat: AVAudioFormat?
    private var isRunning = false

    init(pushFrame: @escaping (UnsafePointer<Float>?, Int) -> sentlog_status_t) {
        self.pushFrame = pushFrame
    }

    func start() throws {
        guard !isRunning else { return }

        #if os(iOS) || os(tvOS) || os(visionOS)
        let session = AVAudioSession.sharedInstance()
        try session.setCategory(.record, mode: .measurement, options: [])
        try session.setPreferredSampleRate(Double(Self.sampleRateHz))
        try session.setActive(true, options: [])
        #endif

        let inputNode = engine.inputNode
        let inputFormat = inputNode.inputFormat(forBus: 0)
        guard inputFormat.sampleRate > 0 else {
            throw NSError(
                domain: "me.cooki.sentlog",
                code: -1,
                userInfo: [NSLocalizedDescriptionKey:
                    "AVAudioEngine input format is invalid — microphone permission denied?"])
        }

        let outputFormat = AVAudioFormat(
            commonFormat: .pcmFormatFloat32,
            sampleRate:   Double(Self.sampleRateHz),
            channels:     1,
            interleaved:  true)!
        targetFormat = outputFormat
        converter = AVAudioConverter(from: inputFormat, to: outputFormat)

        // Tap returns native-format buffers. We convert to 16 kHz mono
        // float32 inside the tap and push the converted samples.
        inputNode.installTap(
            onBus: 0,
            bufferSize: AVAudioFrameCount(Self.tapBufferFrames),
            format: inputFormat
        ) { [weak self] buffer, _ in
            self?.handleTap(buffer)
        }

        try engine.start()
        isRunning = true
    }

    func stop() {
        guard isRunning else { return }
        isRunning = false
        engine.inputNode.removeTap(onBus: 0)
        engine.stop()
        #if os(iOS) || os(tvOS) || os(visionOS)
        try? AVAudioSession.sharedInstance().setActive(false, options: [])
        #endif
    }

    private func handleTap(_ inBuffer: AVAudioPCMBuffer) {
        guard isRunning,
              let converter,
              let targetFormat else { return }

        // Convert to our target format. The output buffer needs to be
        // sized for the worst-case input → output ratio.
        let inFrames = Double(inBuffer.frameLength)
        let ratio = Double(Self.sampleRateHz) / inBuffer.format.sampleRate
        let outFrames = max(1, AVAudioFrameCount((inFrames * ratio).rounded(.up)))

        guard let outBuffer = AVAudioPCMBuffer(
            pcmFormat: targetFormat,
            frameCapacity: outFrames
        ) else { return }

        var error: NSError?
        var inputProvided = false
        let status = converter.convert(
            to: outBuffer,
            error: &error
        ) { _, outStatus in
            if inputProvided {
                outStatus.pointee = .noDataNow
                return nil
            }
            inputProvided = true
            outStatus.pointee = .haveData
            return inBuffer
        }
        if status == .error || error != nil { return }

        guard let chData = outBuffer.floatChannelData?[0] else { return }
        let count = Int(outBuffer.frameLength)
        if count > 0 {
            _ = pushFrame(chData, count)
        }
    }

    static let sampleRateHz: Int    = 16_000
    static let tapBufferFrames: Int = 2_048
}
