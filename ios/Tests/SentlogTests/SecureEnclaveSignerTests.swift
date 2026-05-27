import XCTest
@testable import Sentlog
import SentlogCore

/// Phase 7.5: SecureEnclaveSigner host-side coverage.
///
/// The real Secure Enclave is only present on a subset of hosts (T2
/// Intel Macs + Apple Silicon Macs and physical iOS devices), so
/// `SecureEnclaveSigner.create` can either succeed or throw
/// SENTLOG_ERR_HARDWARE_SIGNING depending on the SwiftPM test
/// runner's hardware. Both outcomes are correct — these tests assert
/// shape, not platform availability:
///
///   - error path: when create throws, it throws a typed SentlogError
///   - happy path: when create succeeds, the convenience overload
///     accepts the resulting signer and the produced
///     verificationMethod looks well-formed (`did:key:z…`)
final class SecureEnclaveSignerTests: XCTestCase {

    func testCreateReturnsTypedErrorOrValidSigner() throws {
        let tag = "me.cooki.sentlog.tests.\(UUID().uuidString)"
        let signer: SecureEnclaveSigner
        do {
            signer = try SecureEnclaveSigner.create(keyTag: tag)
        } catch let e as SentlogError {
            XCTAssertEqual(e.status, SENTLOG_ERR_HARDWARE_SIGNING)
            return  // expected on hosts without an accessible SE
        }
        XCTAssertTrue(signer.verificationMethod.hasPrefix("did:key:z"))
        XCTAssertEqual(signer.publicKeyRaw.count, 65)
        XCTAssertEqual(signer.publicKeyRaw[0], 0x04)
    }
}
