import Foundation
#if os(iOS) || os(watchOS) || os(visionOS)
import CoreMotion
#endif

/// Accelerometer + gyroscope capture using `CMMotionManager`. Pairs
/// the latest gyroscope reading with each accelerometer update and
/// pushes the combined frame through `pushFrame`. The C++ pipeline
/// analyses the buffered series at evaluate time and emits
/// `presence.motion`.
///
/// Mirrors the Android `MotionCapture.kt`. CoreMotion delivers
/// accelerometer + gyroscope on separate callbacks at the rate we
/// request; pairing the latest gyro with each accel sample is fine
/// for the baseline analysis (which only reads accelerometer
/// magnitude). When the future tremor/coupled-motion FFT lands,
/// this may need time-aligned interpolation.
///
/// Best-effort: if CoreMotion isn't available on the platform
/// (macOS doesn't expose `CMMotionManager`), `start()` silently
/// no-ops and the pipeline emits no `presence.motion`.
internal final class MotionCapture {

    typealias PushFrame = (
        _ timestampNs: Int64,
        _ ax: Float, _ ay: Float, _ az: Float,
        _ gx: Float, _ gy: Float, _ gz: Float
    ) -> sentlog_status_t

    #if os(iOS) || os(watchOS) || os(visionOS)
    private let manager = CMMotionManager()
    private let queue = OperationQueue()
    private var lastGx: Double = 0.0
    private var lastGy: Double = 0.0
    private var lastGz: Double = 0.0
    private var isRunning = false
    #endif

    private let pushFrame: PushFrame

    init(pushFrame: @escaping PushFrame) {
        self.pushFrame = pushFrame
        #if os(iOS) || os(watchOS) || os(visionOS)
        queue.name = "me.cooki.sentlog.motion"
        queue.maxConcurrentOperationCount = 1
        manager.accelerometerUpdateInterval = 0.01  // 100 Hz
        manager.gyroUpdateInterval          = 0.01
        #endif
    }

    func start() {
        #if os(iOS) || os(watchOS) || os(visionOS)
        guard !isRunning else { return }
        guard manager.isAccelerometerAvailable else { return }
        isRunning = true

        if manager.isGyroAvailable {
            manager.startGyroUpdates(to: queue) { [weak self] data, _ in
                guard let self, let data else { return }
                self.lastGx = data.rotationRate.x
                self.lastGy = data.rotationRate.y
                self.lastGz = data.rotationRate.z
            }
        }

        manager.startAccelerometerUpdates(to: queue) { [weak self] data, _ in
            guard let self, self.isRunning, let data else { return }
            // CMAcceleration is in g's; convert to m/s² so the units
            // match the Android side (SensorManager reports m/s²).
            let gToMs2: Double = 9.80665
            let ax = data.acceleration.x * gToMs2
            let ay = data.acceleration.y * gToMs2
            let az = data.acceleration.z * gToMs2
            // CMSensorData.timestamp is seconds since boot (mach_absolute_time
            // base). Convert to ns to match the C ABI contract.
            let tsNs = Int64(data.timestamp * 1_000_000_000.0)
            _ = self.pushFrame(
                tsNs,
                Float(ax), Float(ay), Float(az),
                Float(self.lastGx), Float(self.lastGy), Float(self.lastGz)
            )
        }
        #endif
    }

    func stop() {
        #if os(iOS) || os(watchOS) || os(visionOS)
        guard isRunning else { return }
        isRunning = false
        if manager.isAccelerometerActive { manager.stopAccelerometerUpdates() }
        if manager.isGyroActive          { manager.stopGyroUpdates() }
        // Drain barrier — any in-flight CoreMotion callback that ran
        // before isRunning flipped will guard out of pushFrame; we
        // wait for the queue to quiesce so a callback can't still be
        // mid-push when the next destroy() frees the C handle.
        queue.waitUntilAllOperationsAreFinished()
        #endif
    }
}
