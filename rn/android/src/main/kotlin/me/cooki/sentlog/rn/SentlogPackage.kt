package me.cooki.sentlog.rn

import com.facebook.react.ReactPackage
import com.facebook.react.bridge.NativeModule
import com.facebook.react.bridge.ReactApplicationContext
import com.facebook.react.uimanager.ViewManager

/**
 * React Native autolinking entry point. Consumer apps' generated
 * `PackageList` discovers this class via the npm package's
 * `package.json` `react-native.module-name` field (added in 6c)
 * and instantiates it at app startup.
 */
class SentlogPackage : ReactPackage {
    override fun createNativeModules(
        reactContext: ReactApplicationContext
    ): List<NativeModule> = listOf(SentlogModule(reactContext))

    override fun createViewManagers(
        @Suppress("UNUSED_PARAMETER") reactContext: ReactApplicationContext
    ): List<ViewManager<*, *>> = emptyList()
}
