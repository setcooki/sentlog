import XCTest
import SentlogCore
@testable import Sentlog

/// Tests for the suspicion-rank ordering encoded in
/// AudioRouteDetector. Validates the route constants match the C
/// ABI enum and the priorities are correctly ordered.
///
/// Per-port-type mapping (AVAudioSession.Port → route) is exercised
/// on real devices via the Phase 1g benchmark gate — AVAudioSession
/// requires an iOS runtime.
final class AudioRouteDetectorTests: XCTestCase {

    func testSuspicionRankOrderingLoopbackIsMostSuspicious() {
        // Most-suspicious route wins when multiple inputs enumerate;
        // a farm rig with USB AND built-in mic should report USB,
        // not built-in.
        let loopback = AudioRouteDetector.suspicionRank(SENTLOG_AUDIO_ROUTE_LOOPBACK)
        let usb      = AudioRouteDetector.suspicionRank(SENTLOG_AUDIO_ROUTE_USB)
        let bt       = AudioRouteDetector.suspicionRank(SENTLOG_AUDIO_ROUTE_BLUETOOTH)
        let unknown  = AudioRouteDetector.suspicionRank(SENTLOG_AUDIO_ROUTE_UNKNOWN)
        let builtIn  = AudioRouteDetector.suspicionRank(SENTLOG_AUDIO_ROUTE_BUILT_IN)

        XCTAssertGreaterThan(loopback, usb)
        XCTAssertGreaterThan(usb, unknown)
        XCTAssertGreaterThan(bt, unknown)
        XCTAssertGreaterThan(unknown, builtIn)
        XCTAssertEqual(usb, bt, "USB and Bluetooth share suspicion rank")
    }

    func testDetectOnMacOSHostReturnsUnknown() {
        // On the macOS SwiftPM test host AVAudioSession doesn't
        // exist, so detect() returns UNKNOWN. The pipeline maps
        // UNKNOWN → audio_source = 0.0 — a "no useful information"
        // signal rather than a "trusted built-in mic" signal.
        #if !os(iOS) && !os(tvOS) && !os(visionOS)
        XCTAssertEqual(AudioRouteDetector.detect(), SENTLOG_AUDIO_ROUTE_UNKNOWN)
        #endif
    }
}
