import Foundation
import OnnxRuntimeBindings

/// Streaming Silero VAD (v5) wrapper. Owns an `ORTSession`, keeps the
/// LSTM hidden state across frames, and exposes a single `infer(frame:)`
/// call that returns the per-frame speech probability.
///
/// Mirrors the Android `SileroVadStream.kt`. Silero v5 requires
/// exactly ``frameSamples`` samples per inference at 16 kHz; callers
/// feed it fixed-size chunks via ``Chunker``.
///
/// Not part of the public API. Owned by ``Sentlog``.
///
/// Thread-safety: not safe to call `infer(frame:)` concurrently with
/// itself. Construct once per capture; ARC disposes when the last
/// strong reference drops.
internal final class SileroVadStream {

    static let sampleRateHz: Int    = 16_000
    static let frameSamples: Int    = 512          // ~32 ms @ 16 kHz
    static let stateHiddenSize: Int = 128
    static let stateSize: Int       = 2 * 128      // [2, 1, 128] flattened

    private let env: ORTEnv
    private let session: ORTSession

    // Silero v5 hidden state shape: [2, batch=1, 128]. We keep it as
    // an NSMutableData blob because ORTValue expects NSMutableData
    // anyway.
    private let stateData: NSMutableData

    // Sample rate is an int64 scalar input.
    private let sampleRateData: NSMutableData

    init(modelPath: String) throws {
        env = try ORTEnv(loggingLevel: ORTLoggingLevel.warning)
        let opts = try ORTSessionOptions()
        // Single-threaded — Silero is small and per-chunk inference
        // is fast enough that thread-pool overhead dominates.
        try opts.setIntraOpNumThreads(1)
        session = try ORTSession(env: env,
                                 modelPath: modelPath,
                                 sessionOptions: opts)

        stateData = NSMutableData(
            length: Self.stateSize * MemoryLayout<Float>.size
        )!  // ! is safe — length is finite and non-zero
        var sr: Int64 = Int64(Self.sampleRateHz)
        sampleRateData = NSMutableData(
            bytes: &sr, length: MemoryLayout<Int64>.size
        )
    }

    /// Run one frame through Silero. `frame` must be exactly
    /// ``frameSamples`` samples of mono 16 kHz Float audio.
    /// Returns the speech probability in [0,1].
    func infer(_ frame: [Float]) throws -> Float {
        precondition(frame.count == Self.frameSamples,
            "Silero v5 expects \(Self.frameSamples) samples per chunk, got \(frame.count)")

        // Pack the float frame into NSMutableData for ORTValue. We
        // copy because ORT may retain the data internally; cheaper
        // than risking a use-after-free on the caller's array.
        let inputData = NSMutableData(
            length: frame.count * MemoryLayout<Float>.size
        )!
        frame.withUnsafeBufferPointer { srcPtr in
            inputData.replaceBytes(
                in: NSRange(location: 0, length: inputData.length),
                withBytes: srcPtr.baseAddress!
            )
        }

        let inputShape: [NSNumber] = [1, NSNumber(value: Self.frameSamples)]
        let stateShape: [NSNumber] = [2, 1, NSNumber(value: Self.stateHiddenSize)]
        let srShape:    [NSNumber] = []

        let inputTensor = try ORTValue(tensorData: inputData,
                                       elementType: .float,
                                       shape: inputShape)
        let stateTensor = try ORTValue(tensorData: stateData,
                                       elementType: .float,
                                       shape: stateShape)
        let srTensor    = try ORTValue(tensorData: sampleRateData,
                                       elementType: .int64,
                                       shape: srShape)

        let inputs: [String: ORTValue] = [
            "input": inputTensor,
            "state": stateTensor,
            "sr":    srTensor,
        ]
        let outputs = try session.run(withInputs: inputs,
                                      outputNames: ["output", "stateN"],
                                      runOptions: nil)

        guard let outValue = outputs["output"],
              let stateNValue = outputs["stateN"] else {
            throw NSError(domain: "me.cooki.sentlog",
                          code: -2,
                          userInfo: [NSLocalizedDescriptionKey:
                              "Silero output missing expected tensors"])
        }

        // output: float32 [1, 1] — single speech probability.
        let outData = try outValue.tensorData()
        let prob: Float = outData.withUnsafeBytes { rawPtr -> Float in
            rawPtr.load(as: Float.self)
        }

        // stateN: float32 [2, 1, 128] — copy back into our state blob
        // for the next inference call. ORT returns a fresh
        // NSMutableData; we replace our existing state bytes.
        let newStateData = try stateNValue.tensorData()
        precondition(newStateData.length == stateData.length,
            "Silero stateN size mismatch")
        stateData.replaceBytes(
            in: NSRange(location: 0, length: stateData.length),
            withBytes: newStateData.bytes
        )

        return prob
    }

    /// Reset the LSTM state. Call between captures.
    func reset() {
        stateData.resetBytes(in: NSRange(location: 0, length: stateData.length))
    }
}

extension SileroVadStream {

    /// Accumulates variable-size audio reads into fixed-size Silero
    /// chunks. Calls back on every full chunk with (startSample, samples).
    /// Mirrors the Android `SileroVadStream.Chunker`.
    final class Chunker {
        private let onChunk: (UInt64, [Float]) -> Void
        private var buf: [Float]
        private var bufLen: Int
        private var samplesSeen: UInt64

        init(onChunk: @escaping (UInt64, [Float]) -> Void) {
            self.onChunk = onChunk
            self.buf = [Float](repeating: 0, count: SileroVadStream.frameSamples)
            self.bufLen = 0
            self.samplesSeen = 0
        }

        func reset() {
            bufLen = 0
            samplesSeen = 0
        }

        /// Feed `n` samples from `samples`. Emits zero or more full
        /// 512-sample chunks. Any residual stays buffered for the
        /// next feed.
        func feed(_ samples: UnsafePointer<Float>, count n: Int) {
            var srcIdx = 0
            while srcIdx < n {
                let take = min(SileroVadStream.frameSamples - bufLen, n - srcIdx)
                buf.withUnsafeMutableBufferPointer { dest in
                    let destPtr = dest.baseAddress!.advanced(by: bufLen)
                    destPtr.update(from: samples.advanced(by: srcIdx),
                                   count: take)
                }
                bufLen += take
                srcIdx += take
                if bufLen == SileroVadStream.frameSamples {
                    let start = samplesSeen
                    // Copy the chunk so the callee can retain it past
                    // the callback without the chunker overwriting it.
                    onChunk(start, buf)
                    samplesSeen += UInt64(SileroVadStream.frameSamples)
                    bufLen = 0
                }
            }
        }
    }
}
