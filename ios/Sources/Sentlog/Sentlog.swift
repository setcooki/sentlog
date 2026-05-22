// Phase 0 placeholder. The Swift public API (Sentlog.evaluate(),
// Sentlog.checkForUpdates(), etc.) lands in Phase 1 alongside the
// C++ shared core it bridges to via SentlogC.

import Foundation
@_implementationOnly import SentlogC

public enum Sentlog {
    public static var version: String {
        guard let cstr = sentlog_c_version() else { return "" }
        return String(cString: cstr)
    }
}
