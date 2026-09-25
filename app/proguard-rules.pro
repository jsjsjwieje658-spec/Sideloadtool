# ════════════════════════════════════════════════════════════════════════
# v50 — ProGuard/R8 rules cho bản RELEASE (minify bật lần đầu).
#
# Nguyên tắc: mọi class có thể bị gọi "ngoài tầm nhìn" của R8 đều phải giữ:
#   • Python (Chaquopy) gọi Kotlin theo tên class/method qua JNI:
#       AppPaths.filesDir()/nativeDepsDir()/zsignPath(),
#       DeviceNative.connectAndPair()/diagnostics()/listInstalledApps()/reset()
#       /sideloadIpa(), NativeLog.log(), UiPrompt.requestInput()
#       (callAttr → static/direct lookup — không qua reflection Java thường).
#   • JNI C gọi static method trên NativeBridge/UsbTransport/CertHelper/
#       TlsHelper theo tên (GetMethodID "onNativeLog" "(Ljava/lang/String;)V"…).
# Giữ TRỌN GÓI com.superalpha.sideload.** (app nhỏ, phần giữ lại không đáng
# kể so với AndroidX; an toàn tuyệt đối cho interop) + com.chaquo.python.**.
# ════════════════════════════════════════════════════════════════════════

-keep class com.superalpha.sideload.** { *; }
-keep class com.chaquo.python.** { *; }

# Giữ annotation/signature để Chaquopy + coroutines + Compose hoạt động.
-keepattributes Signature
-keepattributes *Annotation*
-keepattributes InnerClasses,EnclosingMethod
-keepattributes RuntimeVisibleAnnotations,AnnotationDefault

# BouncyCastle dùng reflection nội bộ (CertHelper.kt).
-keep class org.bouncycastle.** { *; }
-dontwarn org.bouncycastle.**

# OkHttp (listAnisetteServers) — template chuẩn của okhttp3.
-dontwarn okhttp3.**
-dontwarn okio.**
-dontwarn org.conscrypt.**
-dontwarn org.openjsse.**

# Chaquopy ships consumer rules, giữ explicit cho chắc ở CI.
-dontwarn com.chaquo.python.**

# Không shrink các lớp được .py gọi (double-cover cho -keep ở trên).
-keepclassmembers class com.superalpha.sideload.** {
    public <methods>;
    public static <methods>;
}
