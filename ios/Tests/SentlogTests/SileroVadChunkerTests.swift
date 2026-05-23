import XCTest
@testable import Sentlog

/// Tests for SileroVadStream.Chunker — the variable-input →
/// fixed-Silero-frame re-chunker that sits between AVAudioEngine's
/// variable-size tap buffers and Silero's required 512-sample chunks.
///
/// Mirrors the Android `SileroVadChunkerTest`. Does NOT exercise the
/// ONNX inference itself; that's verified on-device.
final class SileroVadChunkerTests: XCTestCase {

    func testChunkerEmitsOneFullChunkAfterExactly512Samples() {
        var emitted: [(UInt64, Int)] = []
        let chunker = SileroVadStream.Chunker { start, samples in
            emitted.append((start, samples.count))
        }
        let buf = [Float](repeating: 0, count: 512)
        buf.withUnsafeBufferPointer { ptr in
            chunker.feed(ptr.baseAddress!, count: 512)
        }
        XCTAssertEqual(emitted.count, 1)
        XCTAssertEqual(emitted[0].0, 0)
        XCTAssertEqual(emitted[0].1, 512)
    }

    func testChunkerHoldsPartialInputAndEmitsOnCompletion() {
        var emitted: [(UInt64, Int)] = []
        let chunker = SileroVadStream.Chunker { start, samples in
            emitted.append((start, samples.count))
        }
        let first = [Float](repeating: 0, count: 300)
        let second = [Float](repeating: 0, count: 212)
        first.withUnsafeBufferPointer  { chunker.feed($0.baseAddress!, count: 300) }
        XCTAssertEqual(emitted.count, 0)
        second.withUnsafeBufferPointer { chunker.feed($0.baseAddress!, count: 212) }
        XCTAssertEqual(emitted.count, 1)
        XCTAssertEqual(emitted[0].0, 0)
    }

    func testChunkerEmitsMultipleChunksFromLongInput() {
        var emitted: [(UInt64, Int)] = []
        let chunker = SileroVadStream.Chunker { start, samples in
            emitted.append((start, samples.count))
        }
        // 16000 samples (1 s @ 16 kHz) → 31 × 512 = 15872 with 128 residual.
        let buf = [Float](repeating: 0, count: 16000)
        buf.withUnsafeBufferPointer { chunker.feed($0.baseAddress!, count: 16000) }
        XCTAssertEqual(emitted.count, 31)
        for (i, e) in emitted.enumerated() {
            XCTAssertEqual(e.0, UInt64(i * 512), "chunk \(i) start sample")
            XCTAssertEqual(e.1, 512)
        }
    }

    func testChunkerTracksCumulativeOffsetsAcrossManySmallFeeds() {
        var emitted: [UInt64] = []
        let chunker = SileroVadStream.Chunker { start, _ in
            emitted.append(start)
        }
        // 100 chunks of 320 samples each = 32000 samples → 62 full
        // 512-sample chunks.
        let frame = [Float](repeating: 0, count: 320)
        for _ in 0..<100 {
            frame.withUnsafeBufferPointer { chunker.feed($0.baseAddress!, count: 320) }
        }
        XCTAssertEqual(emitted.count, 62)
        XCTAssertEqual(emitted.first, 0)
        XCTAssertEqual(emitted.last,  UInt64(62 * 512 - 512))
    }

    func testResetClearsPartialBufferAndResetsSampleCounter() {
        var emitted: [(UInt64, Int)] = []
        let chunker = SileroVadStream.Chunker { start, samples in
            emitted.append((start, samples.count))
        }
        let partial = [Float](repeating: 0, count: 300)
        partial.withUnsafeBufferPointer { chunker.feed($0.baseAddress!, count: 300) }
        chunker.reset()
        let full = [Float](repeating: 0, count: 512)
        full.withUnsafeBufferPointer { chunker.feed($0.baseAddress!, count: 512) }
        XCTAssertEqual(emitted.count, 1)
        XCTAssertEqual(emitted[0].0, 0)
    }

    func testEachEmittedChunkIsACopySafeToRetainPastCallback() {
        var emittedChunks: [[Float]] = []
        let chunker = SileroVadStream.Chunker { _, samples in
            emittedChunks.append(samples)
        }
        var buf = [Float](repeating: 0, count: 512)
        for i in 0..<512 { buf[i] = Float(i) }
        buf.withUnsafeBufferPointer { chunker.feed($0.baseAddress!, count: 512) }

        // Mutate the source after the callback completed — the
        // emitted chunk must NOT change.
        for i in 0..<512 { buf[i] = 99.0 }

        XCTAssertTrue(emittedChunks[0].contains { $0 != 99.0 },
                      "emitted chunk was not copied")
    }
}
