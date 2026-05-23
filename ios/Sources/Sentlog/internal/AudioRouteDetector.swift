import Foundation
#if os(iOS) || os(tvOS) || os(visionOS)
import AVFoundation
#endif
import SentlogCore

/// Reads the current audio capture route from AVAudioSession and
/// maps it to the C ABI's `sentlog_audio_route_t` enum.
///
/// Pushed to native at startCapture so the C++ pipeline can populate
/// `presence.audio_source`. Without this push the route defaults to
/// UNKNOWN, which maps to audio_source = 0.0.
///
/// On macOS (where AVAudioSession doesn't exist) this returns
/// UNKNOWN; tests that need a real route should override via the
/// `detect()` helper directly.
internal enum AudioRouteDetector {

    static func detect() -> sentlog_audio_route_t {
        #if os(iOS) || os(tvOS) || os(visionOS)
        let session = AVAudioSession.sharedInstance()
        // Inspect the current input route. The route may have multiple
        // inputs; we pick the most-suspicious one (so a USB-attached
        // farm rig with the built-in mic also enumerated still
        // reports USB).
        var saw: sentlog_audio_route_t = SENTLOG_AUDIO_ROUTE_UNKNOWN
        for input in session.currentRoute.inputs {
            let mapped = map(portType: input.portType)
            if suspicionRank(mapped) > suspicionRank(saw) {
                saw = mapped
            }
        }
        return saw
        #else
        return SENTLOG_AUDIO_ROUTE_UNKNOWN
        #endif
    }

    #if os(iOS) || os(tvOS) || os(visionOS)
    static func map(portType: AVAudioSession.Port) -> sentlog_audio_route_t {
        // Asymmetry vs Android: the Android detector maps
        // TYPE_REMOTE_SUBMIX and TYPE_TELEPHONY to LOOPBACK. iOS has
        // no equivalent AVAudioSession.Port — REMOTE_SUBMIX is an
        // Android-specific bus and AVAudioSession doesn't surface
        // telephony as an input port. So LOOPBACK is not currently
        // reachable from iOS detection; suspicionRank() still ranks
        // it because the enum is shared, and a future iOS API (or a
        // manual override via sentlog_set_audio_route) can still
        // surface it.
        switch portType {
        case .builtInMic, .headsetMic:                  return SENTLOG_AUDIO_ROUTE_BUILT_IN
        case .usbAudio:                                  return SENTLOG_AUDIO_ROUTE_USB
        case .bluetoothHFP, .bluetoothLE:                return SENTLOG_AUDIO_ROUTE_BLUETOOTH
        default:                                         return SENTLOG_AUDIO_ROUTE_UNKNOWN
        }
    }
    #endif

    static func suspicionRank(_ route: sentlog_audio_route_t) -> Int {
        switch route {
        case SENTLOG_AUDIO_ROUTE_LOOPBACK:  return 4
        case SENTLOG_AUDIO_ROUTE_USB:       return 3
        case SENTLOG_AUDIO_ROUTE_BLUETOOTH: return 3
        case SENTLOG_AUDIO_ROUTE_UNKNOWN:   return 1
        case SENTLOG_AUDIO_ROUTE_BUILT_IN:  return 0
        default:                            return 0
        }
    }
}
