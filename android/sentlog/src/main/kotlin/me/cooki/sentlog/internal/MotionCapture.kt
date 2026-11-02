package me.cooki.sentlog.internal

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.HandlerThread
import android.os.Process

/**
 * Accelerometer + gyroscope capture using [SensorManager]. Pairs the
 * latest gyroscope reading with each accelerometer event and pushes
 * the combined frame through [pushFrame]. The C++ pipeline analyses
 * the buffered series at evaluate time and emits `presence.motion`.
 *
 * Gyro/accel arrive as separate sensor events with their own
 * timestamps; the baseline pipeline analysis (Phase 3c1) only looks
 * at the accelerometer magnitude, so pairing the latest gyro is
 * fine. When the future tremor/coupled-motion FFT lands, this may
 * need time-aligned interpolation.
 *
 * Sensor callbacks run on a dedicated background thread so the
 * main / audio threads aren't perturbed by the ~100 Hz event rate.
 *
 * Best-effort: if accelerometer or gyroscope sensors are absent on
 * the device, [start] silently no-ops (degrades to no motion data,
 * pipeline emits no `presence.motion`).
 */
internal class MotionCapture(
    private val context: Context,
    private val pushFrame: (
        timestampNs: Long,
        ax: Float, ay: Float, az: Float,
        gx: Float, gy: Float, gz: Float,
    ) -> Int,
) {

    private val sensorManager: SensorManager? =
        context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    @Volatile private var isRunning = false
    private val pushLock = Any()

    // Latest gyro reading, updated by the gyro listener and read by
    // the accel listener. Single-thread access (both listeners run on
    // the dedicated handler thread).
    private var lastGx: Float = 0.0f
    private var lastGy: Float = 0.0f
    private var lastGz: Float = 0.0f

    private val accelListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            synchronized(pushLock) {
                if (!isRunning) return
                pushFrame(
                    event.timestamp,
                    event.values[0], event.values[1], event.values[2],
                    lastGx, lastGy, lastGz,
                )
            }
        }
        override fun onAccuracyChanged(s: Sensor?, accuracy: Int) {}
    }

    private val gyroListener = object : SensorEventListener {
        override fun onSensorChanged(event: SensorEvent) {
            lastGx = event.values[0]
            lastGy = event.values[1]
            lastGz = event.values[2]
        }
        override fun onAccuracyChanged(s: Sensor?, accuracy: Int) {}
    }

    fun start() {
        if (isRunning) return
        val sm = sensorManager ?: return
        val accel = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER) ?: return
        val gyro  = sm.getDefaultSensor(Sensor.TYPE_GYROSCOPE)        // optional

        val t = HandlerThread("sentlog-motion", Process.THREAD_PRIORITY_DEFAULT)
        t.start()
        thread = t
        handler = Handler(t.looper)

        // SENSOR_DELAY_GAME ~= 20 ms (50 Hz). Phones typically deliver
        // a bit faster. The pipeline doesn't care about exact rate.
        sm.registerListener(accelListener, accel,
            SensorManager.SENSOR_DELAY_GAME, handler)
        if (gyro != null) {
            sm.registerListener(gyroListener, gyro,
                SensorManager.SENSOR_DELAY_GAME, handler)
        }
        isRunning = true
    }

    fun stop() {
        if (!isRunning) return
        // Once this block ends no push is running or can start, so the
        // caller may free the handle. unregisterListener alone doesn't
        // wait for a callback already on the handler thread.
        synchronized(pushLock) { isRunning = false }
        sensorManager?.unregisterListener(accelListener)
        sensorManager?.unregisterListener(gyroListener)
        thread?.quitSafely()
        thread = null
        handler = null
    }
}
