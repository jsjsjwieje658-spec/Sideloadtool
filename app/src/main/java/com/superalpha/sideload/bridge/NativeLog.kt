package com.superalpha.sideload.bridge

import android.util.Log
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.asSharedFlow

object NativeLog {
    private const val TAG = "NativeLog"
    private val _lines = MutableSharedFlow<String>(extraBufferCapacity = 500)
    val lines = _lines.asSharedFlow()

    /**
     * Emit một dòng log vào SharedFlow (hiện trong LogConsole UI) VÀ Android Logcat.
     *
     * @JvmStatic — BẮT BUỘC để Chaquopy Python gọi được từ sideload_core.py.
     * Không có @JvmStatic, Kotlin object method bị compile thành instance method
     * trên NativeLog.INSTANCE, không phải static method trên class — Chaquopy
     * tìm kiếm theo static method signature nên sẽ không tìm thấy và throw
     * NoSuchMethodError (bị _bridged_print bắt và bỏ qua silently).
     */
    @JvmStatic
    fun emit(line: String) {
        Log.i(TAG, line)
        _lines.tryEmit(line)
    }

    /** Alias đơn giản — cũng @JvmStatic để Python gọi được. */
    @JvmStatic
    fun log(message: String) = emit(message)

    /**
     * Hai tham số — gọi từ Python với tag rõ ràng, e.g.:
     *   NativeLog.log("[python]", "Bắt đầu ký IPA...")
     */
    @JvmStatic
    fun log(tag: String, message: String) = emit("[$tag] $message")
}
