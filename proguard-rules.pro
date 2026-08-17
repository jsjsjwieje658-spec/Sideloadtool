# Add project specific ProGuard rules here.
-keep class com.superalpha.sideload.bridge.** { *; }
-keep class com.superalpha.sideload.python.** { *; }
-keepclassmembers class * {
    @android.webkit.JavascriptInterface <methods>;
}
-keepattributes Signature
-keepattributes *Annotation*
-dontwarn org.bouncycastle.**
-keep class org.bouncycastle.** { *; }
