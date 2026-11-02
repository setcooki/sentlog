import Foundation
import React
import Sentlog

/// React Native bridge over `Sentlog` (the Swift singleton in
/// ios/Sources/Sentlog). Every method matches the SentlogBridge
/// interface in rn/src/types.ts. The wire format for typed values
/// (config, result, capabilities, etc.) is JSON strings — the JS
/// shim does JSON.parse on receipt.
///
/// Async work hops off the bridge queue via Task so the JS bridge
/// thread is never blocked. Events route through RCTEventEmitter.
///
/// Methods are exported via the Obj-C bridge in SentlogModule.m
/// (RCT_EXTERN_MODULE + RCT_EXTERN_METHOD).
@objc(SentlogModule)
public class SentlogModule: RCTEventEmitter {

    private let jsonEncoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = []
        return e
    }()
    private let jsonDecoder = JSONDecoder()

    // Drain reply coordination — see drainQueueOnce / drainQueueReply.
    private struct DrainReply { let keep: Bool }
    private let drainLock = NSLock()
    private var drainReplies: [String: [Int: DrainReply]] = [:]

    public override init() {
        super.init()
    }

    public override class func requiresMainQueueSetup() -> Bool { false }

    public override func supportedEvents() -> [String]! {
        return ["SentlogEnvelope", "SentlogDrainEnvelope", "SentlogTelemetry"]
    }

    // MARK: lifecycle

    @objc public func version(_ resolve: @escaping RCTPromiseResolveBlock,
                              rejecter reject: @escaping RCTPromiseRejectBlock) {
        resolve(Sentlog.version)
    }

    @objc public func initialize(_ configJson: String,
                                 resolver resolve: @escaping RCTPromiseResolveBlock,
                                 rejecter reject: @escaping RCTPromiseRejectBlock) {
        Task {
            do {
                guard let data = configJson.data(using: .utf8) else {
                    throw NSError(domain: "Sentlog", code: -1,
                                  userInfo: [NSLocalizedDescriptionKey: "config not utf8"])
                }
                let cfg = try jsonDecoder.decode(SentlogConfig.self, from: data)
                try Sentlog.initialize(config: cfg) { [weak self] event in
                    guard let self, let data = try? self.jsonEncoder.encode(event) else { return }
                    self.sendEvent(withName: "SentlogTelemetry",
                                   body: String(decoding: data, as: UTF8.self))
                }
                resolve(nil)
            } catch {
                reject("initialize", error.localizedDescription, error)
            }
        }
    }

    @objc public func startCapture(_ resolve: @escaping RCTPromiseResolveBlock,
                                   rejecter reject: @escaping RCTPromiseRejectBlock) {
        do { try Sentlog.startCapture(); resolve(nil) }
        catch { reject("startCapture", error.localizedDescription, error) }
    }

    @objc public func evaluate(_ resolve: @escaping RCTPromiseResolveBlock,
                               rejecter reject: @escaping RCTPromiseRejectBlock) {
        Task {
            do {
                let r = try await Sentlog.evaluate()
                let data = try jsonEncoder.encode(r)
                resolve(String(decoding: data, as: UTF8.self))
            } catch {
                reject("evaluate", error.localizedDescription, error)
            }
        }
    }

    @objc public func cancel(_ resolve: @escaping RCTPromiseResolveBlock,
                             rejecter reject: @escaping RCTPromiseRejectBlock) {
        Sentlog.cancel()
        resolve(nil)
    }

    @objc public func destroy(_ resolve: @escaping RCTPromiseResolveBlock,
                              rejecter reject: @escaping RCTPromiseRejectBlock) {
        // setEnvelopeDispatcher(nil) throws when no handle is live;
        // destroy is documented as idempotent (SentlogBridge.destroy)
        // so swallow the dispatcher-clear failure if Sentlog is
        // already destroyed.
        try? Sentlog.setEnvelopeDispatcher(nil)
        Sentlog.destroy()
        resolve(nil)
    }

    // MARK: getters (JSON strings; JS side parses)

    @objc public func getPermissions(_ resolve: @escaping RCTPromiseResolveBlock,
                                     rejecter reject: @escaping RCTPromiseRejectBlock) {
        do {
            let p = try Sentlog.getPermissions()
            resolve(String(decoding: try jsonEncoder.encode(p), as: UTF8.self))
        } catch { reject("getPermissions", error.localizedDescription, error) }
    }

    @objc public func getCapabilities(_ resolve: @escaping RCTPromiseResolveBlock,
                                      rejecter reject: @escaping RCTPromiseRejectBlock) {
        do {
            let c = try Sentlog.getCapabilities()
            resolve(String(decoding: try jsonEncoder.encode(c), as: UTF8.self))
        } catch { reject("getCapabilities", error.localizedDescription, error) }
    }

    @objc public func modelVersions(_ resolve: @escaping RCTPromiseResolveBlock,
                                    rejecter reject: @escaping RCTPromiseRejectBlock) {
        do {
            let m = try Sentlog.modelVersions()
            resolve(String(decoding: try jsonEncoder.encode(m), as: UTF8.self))
        } catch { reject("modelVersions", error.localizedDescription, error) }
    }

    @objc public func checkForUpdates(_ manifestJson: String,
                                      resolver resolve: @escaping RCTPromiseResolveBlock,
                                      rejecter reject: @escaping RCTPromiseRejectBlock) {
        Task {
            do {
                let u = try await Sentlog.checkForUpdates(manifestJson: manifestJson)
                resolve(String(decoding: try jsonEncoder.encode(u), as: UTF8.self))
            } catch {
                reject("checkForUpdates", error.localizedDescription, error)
            }
        }
    }

    // MARK: signing + dispatcher

    @objc public func pushFaceFrame(_ timestampNs: NSNumber,
                                    valence: NSNumber,
                                    arousal: NSNumber,
                                    dominantFauCsv: String?,
                                    resolver resolve: @escaping RCTPromiseResolveBlock,
                                    rejecter reject: @escaping RCTPromiseRejectBlock) {
        do {
            try Sentlog.pushFaceFrame(
                timestampNs:    timestampNs.int64Value,
                valence:        valence.floatValue,
                arousal:        arousal.floatValue,
                dominantFauCsv: dominantFauCsv)
            resolve(nil)
        } catch {
            reject("pushFaceFrame", error.localizedDescription, error)
        }
    }

    @objc public func setDeviceAttestation(_ tokenBase64: String?,
                                           attestationType: String?,
                                           resolver resolve: @escaping RCTPromiseResolveBlock,
                                           rejecter reject: @escaping RCTPromiseRejectBlock) {
        let bytes: Data?
        if let token = tokenBase64 {
            guard let decoded = Data(base64Encoded: token) else {
                reject("setDeviceAttestation", "tokenBase64 not valid base64", nil)
                return
            }
            bytes = decoded
        } else {
            bytes = nil
        }
        do {
            try Sentlog.setDeviceAttestation(
                bytes, attestationType: attestationType)
            resolve(nil)
        } catch {
            reject("setDeviceAttestation", error.localizedDescription, error)
        }
    }

    @objc public func setSigningKey(_ secretKeyBase64: String,
                                    verificationMethod: String,
                                    resolver resolve: @escaping RCTPromiseResolveBlock,
                                    rejecter reject: @escaping RCTPromiseRejectBlock) {
        guard let bytes = Data(base64Encoded: secretKeyBase64) else {
            reject("setSigningKey", "secretKey not valid base64", nil)
            return
        }
        do {
            try Sentlog.setSigningKey(bytes, verificationMethod: verificationMethod)
            resolve(nil)
        } catch {
            reject("setSigningKey", error.localizedDescription, error)
        }
    }

    @objc public func setEnvelopeDispatcherActive(_ active: Bool,
                                                  resolver resolve: @escaping RCTPromiseResolveBlock,
                                                  rejecter reject: @escaping RCTPromiseRejectBlock) {
        do {
            if active {
                try Sentlog.setEnvelopeDispatcher { [weak self] name, kind, sj, ej in
                    self?.sendEvent(withName: "SentlogEnvelope", body: [
                        "forwarder_name": name,
                        "sink_kind":      kind,
                        "sink_json":      sj,
                        "envelope_json":  ej,
                    ])
                }
            } else {
                try Sentlog.setEnvelopeDispatcher(nil)
            }
            resolve(nil)
        } catch {
            reject("setEnvelopeDispatcherActive", error.localizedDescription, error)
        }
    }

    // MARK: drainQueue — event-bus protocol mirroring the Android side

    @objc public func drainQueueOnce(_ path: String,
                                     drainRequestId: String,
                                     resolver resolve: @escaping RCTPromiseResolveBlock,
                                     rejecter reject: @escaping RCTPromiseRejectBlock) {
        Task.detached { [weak self] in
            guard let self else { resolve(0); return }
            self.drainLock.lock()
            self.drainReplies[drainRequestId] = [:]
            self.drainLock.unlock()
            var lineIdx = 0
            let delivered = Sentlog.drainQueue(path: path) { envelopeJson in
                let idx = lineIdx
                lineIdx += 1
                self.sendEvent(withName: "SentlogDrainEnvelope", body: [
                    "drainRequestId": drainRequestId,
                    "line":           idx,
                    "envelope_json":  envelopeJson,
                ])
                // Spin-wait for the JS reply. Bounded so a hung JS
                // handler can't deadlock; on timeout we keep (safer
                // than dropping). drainQueue is rare (app-foreground
                // sync), so polling is cheap.
                let deadline = Date().addingTimeInterval(30.0)
                while Date() < deadline {
                    self.drainLock.lock()
                    let reply = self.drainReplies[drainRequestId]?[idx]
                    if reply != nil {
                        self.drainReplies[drainRequestId]?.removeValue(forKey: idx)
                        self.drainLock.unlock()
                        return reply!.keep == false  // C ABI: true=delete
                    }
                    self.drainLock.unlock()
                    Thread.sleep(forTimeInterval: 0.005)
                }
                return false
            }
            self.drainLock.lock()
            self.drainReplies.removeValue(forKey: drainRequestId)
            self.drainLock.unlock()
            resolve(delivered)
        }
    }

    @objc public func drainQueueReply(_ drainRequestId: String,
                                      line: NSNumber,
                                      keep: Bool,
                                      resolver resolve: @escaping RCTPromiseResolveBlock,
                                      rejecter reject: @escaping RCTPromiseRejectBlock) {
        drainLock.lock()
        drainReplies[drainRequestId]?[line.intValue] = DrainReply(keep: keep)
        drainLock.unlock()
        resolve(nil)
    }

    // RN ≥0.65 wants these no-ops on event-emitting modules.
    @objc public override func addListener(_ eventName: String) {
        super.addListener(eventName)
    }
    @objc public override func removeListeners(_ count: Double) {
        super.removeListeners(count)
    }
}
