# Consumer ProGuard rules. Applied to apps that depend on the AAR.

# Keep all of Sentlog's public types — integrators serialise/deserialise
# instances of these and may use reflection (e.g. for JSON in their own
# pipelines).
-keep public class me.cooki.sentlog.** { *; }

# Keep the JNI bridge declarations — names matter for native lookup.
-keepclassmembers class me.cooki.sentlog.internal.NativeBridge {
    *;
}

# kotlinx.serialization companions / generated serialisers.
-keepattributes *Annotation*, InnerClasses
-dontnote kotlinx.serialization.AnnotationsKt
-keepclasseswithmembers class me.cooki.sentlog.** {
    kotlinx.serialization.KSerializer serializer(...);
}
