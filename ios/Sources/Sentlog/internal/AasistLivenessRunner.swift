import Foundation
import OnnxRuntimeBindings

/// AASIST-L liveness inference. Utterance-level: takes the full
/// captured audio and emits a `LivenessResult` the binding pushes via
/// `sentlog_push_liveness_result`.
///
/// AASIST-L doesn't distinguish replay-vs-synthetic attack types; the
/// binary spoof probability is reported in both `replayRisk` and
/// `syntheticRisk`. If/when a separate attack-type classifier ships,
/// the runner splits the probability appropriately.
///
/// Mirrors the Android `AasistLivenessRunner.kt`. Audio length is
/// padded or truncated to ``targetSamples`` before inference; the
/// ONNX graph supports dynamic length so the truncation is purely
/// for inference-cost predictability.
internal final class AasistLivenessRunner {

    static let sampleRateHz: Int = 16_000
    static let targetSeconds: Int = 4
    static let targetSamples: Int = sampleRateHz * targetSeconds

    private let env: ORTEnv
    private let session: ORTSession

    init(modelPath: String) throws {
        env = try ORTEnv(loggingLevel: ORTLoggingLevel.warning)
        let opts = try ORTSessionOptions()
        try opts.setIntraOpNumThreads(1)
        session = try ORTSession(env: env,
                                 modelPath: modelPath,
                                 sessionOptions: opts)
    }

    /// Run AASIST-L on `audio`. Pads with zeros or truncates (keeping
    /// the most recent samples — the last 4 s of an utterance carries
    /// the most evidence) to ``targetSamples`` before inference.
    func infer(_ audio: [Float]) throws -> LivenessResult {
        let prepared = prepareAudio(audio)

        let inputData = NSMutableData(
            length: prepared.count * MemoryLayout<Float>.size
        )!
        prepared.withUnsafeBufferPointer { srcPtr in
            inputData.replaceBytes(
                in: NSRange(location: 0, length: inputData.length),
                withBytes: srcPtr.baseAddress!
            )
        }

        let shape: [NSNumber] = [1, NSNumber(value: prepared.count)]
        let inputTensor = try ORTValue(tensorData: inputData,
                                       elementType: .float,
                                       shape: shape)
        let outputs = try session.run(withInputs: ["audio": inputTensor],
                                      outputNames: ["embedding", "logits"],
                                      runOptions: nil)

        guard let logitsValue = outputs["logits"] else {
            throw NSError(domain: "me.cooki.sentlog", code: -3,
                          userInfo: [NSLocalizedDescriptionKey:
                              "AASIST-L output 'logits' missing"])
        }
        let logitsData = try logitsValue.tensorData() as Data
        // logits: float32 [1, 2] — index 0 = genuine, 1 = spoof.
        let logits = logitsData.withUnsafeBytes { ptr -> (Float, Float) in
            let base = ptr.baseAddress!.bindMemory(to: Float.self, capacity: 2)
            return (base[0], base[1])
        }
        let (genuineLogit, spoofLogit) = logits
        let max = Swift.max(genuineLogit, spoofLogit)
        let e0 = expf(genuineLogit - max)
        let e1 = expf(spoofLogit - max)
        let sum = e0 + e1
        let genuine = e0 / sum
        let spoof   = e1 / sum
        return LivenessResult(score: genuine,
                              replayRisk: spoof,
                              syntheticRisk: spoof)
    }

    private func prepareAudio(_ input: [Float]) -> [Float] {
        if input.count == Self.targetSamples { return input }
        if input.count > Self.targetSamples {
            return Array(input.suffix(Self.targetSamples))
        }
        var padded = [Float](repeating: 0, count: Self.targetSamples)
        for i in 0..<input.count { padded[i] = input[i] }
        return padded
    }
}

/// Mirrors `Liveness` on the C++ side and what
/// `sentlog_push_liveness_result` accepts.
internal struct LivenessResult {
    let score:         Float
    let replayRisk:    Float
    let syntheticRisk: Float
}
