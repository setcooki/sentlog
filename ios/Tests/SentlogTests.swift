import XCTest
@testable import Sentlog

final class SentlogTests: XCTestCase {
    func testVersionStringPresent() {
        XCTAssertFalse(Sentlog.version.isEmpty)
    }
}
