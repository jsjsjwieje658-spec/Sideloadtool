package com.superalpha.sideload.bridge

import android.content.Context
import java.io.File

/** Tên các file thư viện phụ thuộc mà zsign (build từ Termux) cần lúc chạy —
 * xem ghi chú chi tiết ở [AppPaths.nativeDepsDir]. */
private val ZSIGN_DEP_FILES = listOf("libssl.so.3", "libcrypto.so.3", "libc++_shared.so")

/**
 * Exposes app-private filesystem locations to Python via Chaquopy's Java-interop
 * (`from com.superalpha.sideload.bridge import AppPaths`). The original Termux-based
 * tool used relative paths in the current working directory (config.json, cert.pem,
 * .sideload_state.json, ./zsign); on Android there is no meaningful "cwd", so every
 * ported Python module reads these paths instead.
 *
 * [init] MUST be called once (from MainActivity/SuperAlphaApp) before any Python code
 * that touches these paths runs, since a Kotlin `object` has no constructor arguments.
 */
object AppPaths {
    private lateinit var appContext: Context

    fun init(context: Context) {
        appContext = context.applicationContext
    }

    /** App-private persistent storage directory, e.g. /data/data/<pkg>/files */
    @JvmStatic
    fun filesDir(): String = appContext.filesDir.absolutePath

    /** Scratch directory for IPA extraction/repacking, wiped and recreated per run. */
    @JvmStatic
    fun workDir(): String {
        val dir = File(appContext.filesDir, "sideload_work")
        dir.mkdirs()
        return dir.absolutePath
    }

    /** Directory containing bundled native "libraries" — this is where Android installs
     * jniLibs/arm64-v8a/libzsign.so, and it is one of the few directories on Android
     * where the app is allowed to `exec()` a file it ships (App Bundle/Play policy
     * requires such files to be named lib*.so and to live here). */
    @JvmStatic
    fun nativeLibDir(): String = appContext.applicationInfo.nativeLibraryDir

    /** Full path to the bundled zsign binary. */
    @JvmStatic
    fun zsignPath(): String = File(nativeLibDir(), "libzsign.so").absolutePath

    /**
     * [FIX resultCode/CMD ERROR "library libssl.so.3 not found"] `libzsign.so`
     * trong jniLibs/ được build sẵn trong môi trường Termux — `readelf -d` cho
     * thấy nó cần `libssl.so.3`, `libcrypto.so.3`, `libc++_shared.so` với
     * RUNPATH trỏ thẳng vào `/data/data/com.termux/files/usr/lib` (chỉ tồn
     * tại nếu máy có cài Termux). Trên điện thoại thường (không Termux),
     * linker không tìm thấy 3 thư viện này nên báo "library ... not found"
     * và zsign thoát với "CANNOT LINK EXECUTABLE" — IPA không bao giờ ký
     * được, dù mọi bước trước đó (App ID, Provisioning Profile) đã đúng.
     *
     * Các file .so này KHÔNG thể để trong jniLibs/ như libzsign.so vì tên
     * của chúng (có hậu tố phiên bản ".so.3") không khớp mẫu `lib*.so` mà
     * trình cài đặt Android dùng để nhận diện & giải nén thư viện native vào
     * nativeLibraryDir — APK sẽ đóng gói chúng như asset thường (có thể bị
     * nén), không đảm bảo linker tìm thấy được. Giải pháp: đóng gói 3 file
     * này dưới assets/zsign_deps/ (tên file tuỳ ý, không bị ràng buộc bởi
     * quy ước lib*.so), rồi tự giải nén ra một thư mục ghi được trong
     * filesDir() lúc chạy — sideload_core.py sẽ set LD_LIBRARY_PATH trỏ vào
     * đây khi spawn tiến trình zsign để linker tìm thấy chúng thay vì tìm
     * (không thấy) RUNPATH của Termux.
     *
     * Chỉ giải nén lại 1 lần (đánh dấu bằng file ".extracted") — tránh copy
     * lại ~7MB mỗi lần sideload. Dùng `assets.open()` (InputStream, đọc được
     * cả khi APK nén asset) thay vì `openFd()` (chỉ đọc được asset LƯU
     * KHÔNG NÉN trong APK — mặc định AAPT có thể nén các phần mở rộng lạ
     * như ".3" nên không dùng openFd() ở đây để tránh FileNotFoundException
     * "probably compressed" tuỳ cấu hình build).
     */
    @JvmStatic
    fun nativeDepsDir(): String {
        val dir = File(appContext.filesDir, "zsign_deps")
        dir.mkdirs()
        val marker = File(dir, ".extracted")
        if (!marker.exists()) {
            for (name in ZSIGN_DEP_FILES) {
                appContext.assets.open("zsign_deps/$name").use { input ->
                    File(dir, name).outputStream().use { output -> input.copyTo(output) }
                }
                File(dir, name).setReadable(true, false)
            }
            marker.writeText("1")
        }
        return dir.absolutePath
    }
}
