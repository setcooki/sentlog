package me.cooki.sentlog.internal

import android.content.Context
import android.media.AudioDeviceInfo
import android.media.AudioManager

/**
 * Reads the current audio capture route from [AudioManager] and maps
 * it to the C ABI's `sentlog_audio_route_t` enum.
 *
 * Pushed to native at startCapture so the C++ pipeline can populate
 * `presence.audio_source`. Without this push the route defaults to
 * UNKNOWN, which maps to audio_source = 0.0.
 *
 * Not part of the public API.
 */
internal object AudioRouteDetector {

    /**
     * Return the [NativeBridge] AUDIO_ROUTE_* code corresponding to the
     * device currently feeding our AudioRecord. Falls back to UNKNOWN
     * if the context is unavailable or there are no input devices.
     */
    fun detect(context: Context): Int {
        val am = context.getSystemService(Context.AUDIO_SERVICE) as? AudioManager
            ?: return NativeBridge.AUDIO_ROUTE_UNKNOWN

        val devices = am.getDevices(AudioManager.GET_DEVICES_INPUTS)
        if (devices.isEmpty()) return NativeBridge.AUDIO_ROUTE_UNKNOWN

        // Prefer the most specific match — built-in mic is the desired
        // default, anything else is a flag. If multiple inputs are
        // available the most-suspicious wins (so a USB rig with the
        // built-in mic also enumerated still reports USB).
        var saw: Int = NativeBridge.AUDIO_ROUTE_UNKNOWN
        for (dev in devices) {
            val mapped = mapDeviceType(dev.type)
            // Order of suspicion: LOOPBACK > USB / BT > UNKNOWN > BUILT_IN.
            // We compare ordinals so we keep the most-suspicious seen.
            if (suspicionRank(mapped) > suspicionRank(saw)) {
                saw = mapped
            }
        }
        return saw
    }

    private fun mapDeviceType(type: Int): Int = when (type) {
        AudioDeviceInfo.TYPE_BUILTIN_MIC          -> NativeBridge.AUDIO_ROUTE_BUILT_IN
        AudioDeviceInfo.TYPE_WIRED_HEADSET        -> NativeBridge.AUDIO_ROUTE_BUILT_IN
        AudioDeviceInfo.TYPE_USB_HEADSET,
        AudioDeviceInfo.TYPE_USB_DEVICE,
        AudioDeviceInfo.TYPE_USB_ACCESSORY        -> NativeBridge.AUDIO_ROUTE_USB
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO        -> NativeBridge.AUDIO_ROUTE_BLUETOOTH
        AudioDeviceInfo.TYPE_REMOTE_SUBMIX        -> NativeBridge.AUDIO_ROUTE_LOOPBACK
        AudioDeviceInfo.TYPE_TELEPHONY            -> NativeBridge.AUDIO_ROUTE_LOOPBACK
        else                                       -> NativeBridge.AUDIO_ROUTE_UNKNOWN
    }

    private fun suspicionRank(route: Int): Int = when (route) {
        NativeBridge.AUDIO_ROUTE_LOOPBACK  -> 4
        NativeBridge.AUDIO_ROUTE_USB       -> 3
        NativeBridge.AUDIO_ROUTE_BLUETOOTH -> 3
        NativeBridge.AUDIO_ROUTE_UNKNOWN   -> 1
        NativeBridge.AUDIO_ROUTE_BUILT_IN  -> 0  // most-trusted
        else -> 0
    }
}
