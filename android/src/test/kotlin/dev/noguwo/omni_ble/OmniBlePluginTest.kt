package dev.noguwo.omni_ble

import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import org.mockito.Mockito
import kotlin.test.Test

/*
 * This demonstrates a simple unit test of the Kotlin portion of this plugin's implementation.
 *
 * Once you have built the plugin's example app, you can run these tests from the command
 * line by running `./gradlew testDebugUnitTest` in the `example/android/` directory, or
 * you can run them directly from IDEs that support JUnit such as Android Studio.
 */

internal class OmniBlePluginTest {
    @Test
    fun onMethodCall_getCapabilities_returnsExpectedValue() {
        val plugin = OmniBlePlugin()

        val call = MethodCall("getCapabilities", null)
        val mockResult: MethodChannel.Result = Mockito.mock(MethodChannel.Result::class.java)
        plugin.onMethodCall(call, mockResult)

        Mockito.verify(mockResult).success(
            mapOf(
                "platform" to "android",
                "platformVersion" to android.os.Build.VERSION.RELEASE,
                "availableFeatures" to emptyList<String>(),
                "metadata" to mapOf("adapterState" to "unavailable"),
            ),
        )
    }
}
