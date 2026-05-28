import AudioToolbox
import AVFoundation
import Foundation
import SentlogCore

/// Phase 4g-ii helper: an Opus encoder backed by AVAudioConverter
/// (which uses AudioToolbox's `kAudioFormatOpus` codec under the
/// hood — built into iOS 12+). Ready to plug into
/// `Sentlog.setAudioEncoder(...)`:
///
///     try OpusAudioEncoder.install()
///
/// That single call registers the encoder closure with the SDK; from
/// then on every Full+AudioClip / EvidenceClip envelope carries Opus
/// bytes instead of the placeholder block.
///
/// Encodes 16 kHz mono float PCM (the SDK's capture format) to Opus
/// at 32 kbps — a good default for voice; ~120 KiB worst case for a
/// 30 s clip. The encoder is stateless across calls; the SDK creates
/// one per evaluate, so we re-init AVAudioConverter on each
/// invocation (cost is negligible vs the encode itself).
///
/// On failure (allocator failure, codec unavailable, unexpected
/// input length) the closure returns non-zero; the SDK's
/// dispatch_envelopes path then falls back to the placeholder
/// audio_clip block with the documented note.
public enum OpusAudioEncoder {

    /// Install the Opus encoder against the SDK. Idempotent — replaces
    /// any previously-registered encoder. Throws SentlogError on
    /// underlying SDK error (no live handle, etc.).
    public static func install() throws {
        // The closure captures no state; safe to run on the SDK's
        // I/O thread.
        let cb: sentlog_audio_encoder_cb_t = { pcm, n, sr, out, outLen, _ in
            guard let pcm, let out, let outLen else { return -1 }
            guard sr == 16000, n > 0 else { return -2 }
            let pcmBuf = UnsafeBufferPointer(start: pcm, count: n)
            let encoded: Data
            do {
                encoded = try encode16kMonoFloatToOpus(samples: pcmBuf)
            } catch {
                return -3
            }
            let capacity = outLen.pointee
            if encoded.isEmpty || encoded.count > capacity { return -4 }
            encoded.withUnsafeBytes { raw in
                if let src = raw.baseAddress {
                    out.update(from: src.assumingMemoryBound(to: UInt8.self),
                               count: encoded.count)
                }
            }
            outLen.pointee = encoded.count
            return 0
        }
        try Sentlog.setAudioEncoder(cb, codecName: "opus")
    }

    /// Clear any previously-registered audio encoder. After this,
    /// AudioClip-bearing projections ship the placeholder block.
    public static func uninstall() throws {
        try Sentlog.setAudioEncoder(nil)
    }

    // ------- internals -------

    /// Encode the given 16 kHz mono float PCM to Opus bytes using
    /// AVAudioConverter. Returns the encoded payload as a single
    /// Data — Opus's internal framing is a sequence of self-contained
    /// packets, concatenable without an extra container.
    private static func encode16kMonoFloatToOpus(
        samples: UnsafeBufferPointer<Float>
    ) throws -> Data {
        guard let inFmt = AVAudioFormat(
                commonFormat: .pcmFormatFloat32,
                sampleRate: 16000, channels: 1, interleaved: false) else {
            throw NSError(domain: "OpusAudioEncoder", code: -1)
        }
        // Opus output format. AVAudioFormat builds a basic AudioStreamBasicDescription
        // for kAudioFormatOpus; bitrate / packet duration are set on the
        // converter below.
        var outDesc = AudioStreamBasicDescription(
            mSampleRate: 16000, mFormatID: kAudioFormatOpus,
            mFormatFlags: 0, mBytesPerPacket: 0, mFramesPerPacket: 320,
            mBytesPerFrame: 0, mChannelsPerFrame: 1,
            mBitsPerChannel: 0, mReserved: 0)
        guard let outFmt = AVAudioFormat(streamDescription: &outDesc) else {
            throw NSError(domain: "OpusAudioEncoder", code: -2)
        }
        guard let converter = AVAudioConverter(from: inFmt, to: outFmt) else {
            throw NSError(domain: "OpusAudioEncoder", code: -3)
        }
        converter.bitRate = 32_000

        // Wrap the input float buffer as AVAudioPCMBuffer. Copy is
        // unavoidable since AVAudioPCMBuffer requires its own backing.
        guard let inBuf = AVAudioPCMBuffer(
                pcmFormat: inFmt,
                frameCapacity: AVAudioFrameCount(samples.count)) else {
            throw NSError(domain: "OpusAudioEncoder", code: -4)
        }
        inBuf.frameLength = AVAudioFrameCount(samples.count)
        if let dst = inBuf.floatChannelData?[0] {
            for i in 0..<samples.count { dst[i] = samples[i] }
        }

        var inputConsumed = false
        let inputBlock: AVAudioConverterInputBlock = { _, status in
            if inputConsumed {
                status.pointee = .endOfStream
                return nil
            }
            status.pointee = .haveData
            inputConsumed = true
            return inBuf
        }

        var encoded = Data()
        encoded.reserveCapacity(256 * 1024)
        // Opus output frame size: 20 ms at 16 kHz = 320 frames.
        // Keep pulling 1024-frame chunks until the converter signals end.
        let outBuf = AVAudioCompressedBuffer(
            format: outFmt, packetCapacity: 64, maximumPacketSize: 4096)
        while true {
            var err: NSError?
            let status = converter.convert(
                to: outBuf, error: &err, withInputFrom: inputBlock)
            if let err { throw err }
            let pktCount = Int(outBuf.packetCount)
            let byteCount = Int(outBuf.byteLength)
            if pktCount > 0 && byteCount > 0 {
                // outBuf.data is UnsafeMutableRawPointer; bind to
                // UInt8 explicitly for Data.append(_:count:).
                let typed = UnsafeRawPointer(outBuf.data)
                    .assumingMemoryBound(to: UInt8.self)
                encoded.append(typed, count: byteCount)
            }
            if status == .endOfStream || status == .error { break }
            if pktCount == 0 { break }
        }
        return encoded
    }
}

