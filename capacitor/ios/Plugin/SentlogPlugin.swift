import Capacitor
import Foundation
import Sentlog

/// Capacitor plugin over the underlying `Sentlog` Swift singleton.
/// Method names + IPC shape match the JS shim in
/// capacitor/src/sentlog.ts; typed fields cross as JSON strings.
///
/// Events ride Capacitor's `notifyListeners`:
///   - `SentlogEnvelope`        — per-forwarder envelope.
///   - `SentlogDrainEnvelope`   — per-envelope inside drainQueueOnce.
///   - `SentlogTelemetry`       — per native telemetry event.
@objc(SentlogPlugin)
public class SentlogPlugin: CAPPlugin {

    private let jsonEncoder: JSONEncoder = {
        let e = JSONEncoder()
        e.outputFormatting = []
        return e
    }()
    private let jsonDecoder = JSONDecoder()

    private struct DrainReply { let keep: Bool }
    private let drainLock = NSLock()
    private var drainReplies: [String: [Int: DrainReply]] = [:]

    @objc public func version(_ call: CAPPluginCall) {
        call.resolve(["value": Sentlog.version])
    }

    @objc public func initialize(_ call: CAPPluginCall) {
        guard let configJson = call.getString("configJson") else {
            call.reject("configJson required"); return
        }
        Task {
            do {
                guard let data = configJson.data(using: .utf8) else {
                    throw NSError(domain: "Sentlog", code: -1)
                }
                let cfg = try jsonDecoder.decode(SentlogConfig.self, from: data)
                try Sentlog.initialize(config: cfg)
                call.resolve()
            } catch {
                call.reject(error.localizedDescription, nil, error)
            }
        }
    }

    @objc public func startCapture(_ call: CAPPluginCall) {
        do { try Sentlog.startCapture(); call.resolve() }
        catch { call.reject(error.localizedDescription, nil, error) }
    }

    @objc public func evaluate(_ call: CAPPluginCall) {
        Task {
            do {
                let r = try await Sentlog.evaluate()
                let data = try jsonEncoder.encode(r)
                call.resolve(["resultJson": String(decoding: data, as: UTF8.self)])
            } catch {
                call.reject(error.localizedDescription, nil, error)
            }
        }
    }

    @objc public func cancel(_ call: CAPPluginCall) {
        Sentlog.cancel()
        call.resolve()
    }

    @objc public func destroy(_ call: CAPPluginCall) {
        // setEnvelopeDispatcher(nil) throws when no handle is live;
        // destroy is documented as idempotent (SentlogBridge.destroy)
        // so swallow the dispatcher-clear failure if Sentlog is
        // already destroyed.
        try? Sentlog.setEnvelopeDispatcher(nil)
        Sentlog.destroy()
        call.resolve()
    }

    @objc public func getPermissions(_ call: CAPPluginCall) {
        do {
            let p = try Sentlog.getPermissions()
            call.resolve(["json": String(decoding: try jsonEncoder.encode(p), as: UTF8.self)])
        } catch { call.reject(error.localizedDescription, nil, error) }
    }

    @objc public func getCapabilities(_ call: CAPPluginCall) {
        do {
            let c = try Sentlog.getCapabilities()
            call.resolve(["json": String(decoding: try jsonEncoder.encode(c), as: UTF8.self)])
        } catch { call.reject(error.localizedDescription, nil, error) }
    }

    @objc public func modelVersions(_ call: CAPPluginCall) {
        do {
            let m = try Sentlog.modelVersions()
            call.resolve(["json": String(decoding: try jsonEncoder.encode(m), as: UTF8.self)])
        } catch { call.reject(error.localizedDescription, nil, error) }
    }

    @objc public func checkForUpdates(_ call: CAPPluginCall) {
        Task {
            do {
                let u = try await Sentlog.checkForUpdates()
                call.resolve(["json": String(decoding: try jsonEncoder.encode(u), as: UTF8.self)])
            } catch {
                call.reject(error.localizedDescription, nil, error)
            }
        }
    }

    @objc public func setSigningKey(_ call: CAPPluginCall) {
        guard let sk = call.getString("secretKeyBase64"),
              let vm = call.getString("verificationMethod"),
              let bytes = Data(base64Encoded: sk) else {
            call.reject("secretKeyBase64 + verificationMethod required (base64)")
            return
        }
        do {
            try Sentlog.setSigningKey(bytes, verificationMethod: vm)
            call.resolve()
        } catch {
            call.reject(error.localizedDescription, nil, error)
        }
    }

    @objc public func setEnvelopeDispatcherActive(_ call: CAPPluginCall) {
        let active = call.getBool("active") ?? false
        do {
            if active {
                try Sentlog.setEnvelopeDispatcher { [weak self] name, kind, sj, ej in
                    self?.notifyListeners("SentlogEnvelope", data: [
                        "forwarder_name": name,
                        "sink_kind":      kind,
                        "sink_json":      sj,
                        "envelope_json":  ej,
                    ])
                }
            } else {
                try Sentlog.setEnvelopeDispatcher(nil)
            }
            call.resolve()
        } catch {
            call.reject(error.localizedDescription, nil, error)
        }
    }

    @objc public func drainQueueOnce(_ call: CAPPluginCall) {
        guard let path = call.getString("path"),
              let id   = call.getString("drainRequestId") else {
            call.reject("path + drainRequestId required"); return
        }
        Task.detached { [weak self] in
            guard let self else { call.resolve(["delivered": 0]); return }
            self.drainLock.lock()
            self.drainReplies[id] = [:]
            self.drainLock.unlock()
            var lineIdx = 0
            let delivered = Sentlog.drainQueue(path: path) { envelopeJson in
                let idx = lineIdx
                lineIdx += 1
                self.notifyListeners("SentlogDrainEnvelope", data: [
                    "drainRequestId": id,
                    "line":           idx,
                    "envelope_json":  envelopeJson,
                ])
                let deadline = Date().addingTimeInterval(30.0)
                while Date() < deadline {
                    self.drainLock.lock()
                    let reply = self.drainReplies[id]?[idx]
                    if reply != nil {
                        self.drainReplies[id]?.removeValue(forKey: idx)
                        self.drainLock.unlock()
                        return reply!.keep == false  // C ABI: true=delete
                    }
                    self.drainLock.unlock()
                    Thread.sleep(forTimeInterval: 0.005)
                }
                return false
            }
            self.drainLock.lock()
            self.drainReplies.removeValue(forKey: id)
            self.drainLock.unlock()
            call.resolve(["delivered": delivered])
        }
    }

    @objc public func drainQueueReply(_ call: CAPPluginCall) {
        guard let id   = call.getString("drainRequestId"),
              let line = call.getInt("line") else {
            call.reject("drainRequestId + line required"); return
        }
        let keep = call.getBool("keep") ?? true
        drainLock.lock()
        drainReplies[id]?[line] = DrainReply(keep: keep)
        drainLock.unlock()
        call.resolve()
    }
}
