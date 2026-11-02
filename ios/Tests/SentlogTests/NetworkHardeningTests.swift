import XCTest
@testable import Sentlog

final class StubProtocol: URLProtocol {
    nonisolated(unsafe) static var handler: ((URLProtocol, URLProtocolClient) -> Void)?
    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }
    override func startLoading() { Self.handler?(self, client!) }
    override func stopLoading() {}
}

final class NetworkHardeningTests: XCTestCase {

    private func fetch(maxBytes: Int) async -> BoundedFetch.Result {
        let config = URLSessionConfiguration.ephemeral
        config.protocolClasses = [StubProtocol.self]
        let req = URLRequest(url: URL(string: "https://e.example/m")!)
        return await withCheckedContinuation { cont in
            BoundedFetch.run(req, maxBytes: maxBytes, configuration: config) {
                cont.resume(returning: $0)
            }
        }
    }

    private func respond(status: Int, headers: [String: String] = [:], body: Data) {
        StubProtocol.handler = { proto, client in
            let response = HTTPURLResponse(url: proto.request.url!, statusCode: status,
                                           httpVersion: nil, headerFields: headers)!
            client.urlProtocol(proto, didReceive: response, cacheStoragePolicy: .notAllowed)
            client.urlProtocol(proto, didLoad: body)
            client.urlProtocolDidFinishLoading(proto)
        }
    }

    func testReturnsBodyWithinLimit() async {
        respond(status: 200, body: Data([1, 2, 3]))
        guard case .success(let data) = await fetch(maxBytes: 3) else {
            return XCTFail("expected success")
        }
        XCTAssertEqual(data, Data([1, 2, 3]))
    }

    func testCancelsOversizeBody() async {
        respond(status: 200, body: Data(repeating: 0, count: 4))
        guard case .oversize = await fetch(maxBytes: 3) else {
            return XCTFail("expected oversize")
        }
    }

    func testReportsHttpError() async {
        respond(status: 500, body: Data())
        guard case .httpStatus(500) = await fetch(maxBytes: 3) else {
            return XCTFail("expected HTTP 500")
        }
    }

    func testRefusesRedirect() async {
        StubProtocol.handler = { proto, client in
            let response = HTTPURLResponse(url: proto.request.url!, statusCode: 302,
                                           httpVersion: nil,
                                           headerFields: ["Location": "https://other.example/"])!
            client.urlProtocol(proto,
                               wasRedirectedTo: URLRequest(url: URL(string: "https://other.example/")!),
                               redirectResponse: response)
            client.urlProtocolDidFinishLoading(proto)
        }
        if case .success(let data) = await fetch(maxBytes: 3) {
            XCTAssertTrue(data.isEmpty, "a refused redirect must not deliver a body")
        }
    }

    // MARK: - TSA responses

    private let digest = Data(repeating: 0xAB, count: 32)

    private func response(status: UInt8, token: Data?) -> Data {
        var body = Data([0x30, 0x03, 0x02, 0x01, status])
        if let token { body.append(token) }
        return Data([0x30, UInt8(body.count)]) + body
    }

    func testAcceptsGrantedResponseWithDigest() {
        let token = Data([0x30, 0x22, 0x04, 0x20]) + digest
        XCTAssertTrue(RfcTsaProvider.isGrantedResponse(response(status: 0, token: token), digest: digest))
        XCTAssertTrue(RfcTsaProvider.isGrantedResponse(response(status: 1, token: token), digest: digest))
    }

    func testRejectsPkiRejectionServedWithOK() {
        let token = Data([0x30, 0x22, 0x04, 0x20]) + digest
        XCTAssertFalse(RfcTsaProvider.isGrantedResponse(response(status: 2, token: token), digest: digest))
    }

    func testRejectsMissingOrWrongToken() {
        XCTAssertFalse(RfcTsaProvider.isGrantedResponse(response(status: 0, token: nil), digest: digest))
        let other = Data([0x30, 0x22, 0x04, 0x20]) + Data(repeating: 0xCD, count: 32)
        XCTAssertFalse(RfcTsaProvider.isGrantedResponse(response(status: 0, token: other), digest: digest))
    }

    func testRejectsMalformedResponse() {
        XCTAssertFalse(RfcTsaProvider.isGrantedResponse(Data([0x30, 0x7F, 0x00]), digest: digest))
        XCTAssertFalse(RfcTsaProvider.isGrantedResponse(Data(), digest: digest))
    }
}
