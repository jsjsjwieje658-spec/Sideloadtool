package com.superalpha.sideload.python

import com.chaquo.python.Python
import com.superalpha.sideload.bridge.AppConfig
import com.superalpha.sideload.bridge.NativeLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.json.JSONObject

/**
 * PythonBridge — cầu nối Kotlin ↔ Python (Chaquopy).
 *
 * v9: Sửa log Python đầy đủ:
 *   - Exception từ Chaquopy có message nhiều dòng (Python traceback).
 *     Phát từng dòng riêng để LogConsole hiển thị đầy đủ.
 *   - Thêm NativeLog.emit() cho từng bước quan trọng để người dùng thấy tiến độ.
 */
object PythonBridge {
    data class AnisetteServer(val name: String, val address: String)
    data class Outcome(val success: Boolean, val message: String)

    private val httpClient by lazy { OkHttpClient() }

    fun getSavedAppleId(): String = AppConfig.appleId
    fun saveAppleId(v: String) { AppConfig.appleId = v }
    fun getSavedAnisetteUrl(): String = AppConfig.anisetteUrl
    fun saveAnisetteUrl(v: String) { AppConfig.anisetteUrl = v }

    private fun pythonModule(name: String) = Python.getInstance().getModule(name)

    /**
     * Emit toàn bộ exception (có thể nhiều dòng traceback) ra NativeLog.
     * Chia theo \n để LogConsole không bị cụt dòng.
     */
    private fun emitException(prefix: String, e: Exception) {
        val full = buildString {
            append(e.javaClass.simpleName)
            val msg = e.message
            if (!msg.isNullOrBlank()) {
                append(": ")
                append(msg)
            }
        }
        full.lines().forEach { line ->
            if (line.isNotBlank()) NativeLog.emit("$prefix $line")
        }
    }

    /**
     * Thu hồi chứng chỉ Development bằng Apple ID.
     * Gọi sideload_core.do_revoke_certs() — hàm Python chỉ dùng HTTP, không cần USB.
     */
    suspend fun revokeCerts(
        appleId: String,
        password: String,
        anisetteUrl: String?,
        certSelector: String
    ): Outcome = withContext(Dispatchers.IO) {
        try {
            NativeLog.emit("[python] Đang đăng nhập & tra cứu chứng chỉ...")
            val core = pythonModule("sideload_core")
            AppConfig.appleId = appleId
            val effectiveAnisetteUrl = anisetteUrl?.takeIf { it.isNotBlank() } ?: ""
            val ok = core.callAttr(
                "do_revoke_certs",
                appleId,
                password,
                effectiveAnisetteUrl,
                certSelector
            ).toBoolean()
            Outcome(ok, if (ok) "Thu hồi chứng chỉ thành công." else "Thu hồi thất bại — xem nhật ký.")
        } catch (e: Exception) {
            emitException("[python] ❌ revokeCerts lỗi:", e)
            Outcome(false, e.message?.lines()?.firstOrNull { it.isNotBlank() } ?: e.toString())
        }
    }

    /**
     * Ký và cài đặt IPA bằng Apple ID.
     * Python xử lý: auth, cert, signing. Native C xử lý USB qua DeviceNative.
     */
    suspend fun sideload(
        ipaPath: String,
        appleId: String,
        password: String,
        twoFaCode: String?,
        anisetteUrl: String?
    ): Outcome = withContext(Dispatchers.IO) {
        try {
            NativeLog.emit("[python] Bắt đầu quá trình ký và cài đặt IPA...")
            val core = pythonModule("sideload_core")
            AppConfig.appleId = appleId
            val effectiveAnisetteUrl = anisetteUrl?.takeIf { it.isNotBlank() } ?: ""
            val ok = core.callAttr(
                "do_sideload",
                ipaPath,
                appleId,
                password,
                AppConfig.lastUdid,
                effectiveAnisetteUrl
            ).toBoolean()
            Outcome(ok, if (ok) "Cài đặt IPA thành công." else "Cài đặt thất bại — xem nhật ký.")
        } catch (e: Exception) {
            emitException("[python] ❌ sideload lỗi:", e)
            Outcome(false, e.message?.lines()?.firstOrNull { it.isNotBlank() } ?: e.toString())
        }
    }

    /**
     * Đăng ký UDID thiết bị iOS vào tài khoản Apple Developer — tách rời khỏi
     * luồng ký & cài IPA (do_register_device() trong sideload_core.py), chỉ
     * cần HTTP, không cần USB/IPA đã chọn.
     */
    suspend fun registerDevice(
        appleId: String,
        password: String,
        udid: String,
        deviceName: String,
        anisetteUrl: String?
    ): Outcome = withContext(Dispatchers.IO) {
        try {
            NativeLog.emit("[python] Đang đăng ký UDID thiết bị...")
            val core = pythonModule("sideload_core")
            AppConfig.appleId = appleId
            val effectiveAnisetteUrl = anisetteUrl?.takeIf { it.isNotBlank() } ?: ""
            val ok = core.callAttr(
                "do_register_device",
                appleId,
                password,
                udid,
                deviceName,
                effectiveAnisetteUrl
            ).toBoolean()
            Outcome(ok, if (ok) "Đăng ký UDID thành công." else "Đăng ký UDID thất bại — xem nhật ký.")
        } catch (e: Exception) {
            emitException("[python] ❌ registerDevice lỗi:", e)
            Outcome(false, e.message?.lines()?.firstOrNull { it.isNotBlank() } ?: e.toString())
        }
    }

    /**
     * Danh sách server Anisette công khai — Kotlin OkHttp, không cần Python.
     */
    suspend fun listAnisetteServers(): List<AnisetteServer> = withContext(Dispatchers.IO) {
        try {
            val request = Request.Builder()
                .url("https://servers.sidestore.io/servers.json")
                .build()
            httpClient.newCall(request).execute().use { response ->
                val body = response.body?.string()
                if (!response.isSuccessful || body.isNullOrBlank()) return@withContext defaultServers()
                val arr = JSONObject(body).optJSONArray("servers") ?: return@withContext defaultServers()
                val parsed = (0 until arr.length()).mapNotNull { i ->
                    val obj = arr.optJSONObject(i) ?: return@mapNotNull null
                    val address = obj.optString("address", "")
                    if (address.isBlank()) null else AnisetteServer(obj.optString("name", "?"), address)
                }
                parsed.ifEmpty { defaultServers() }
            }
        } catch (_: Exception) {
            defaultServers()
        }
    }

    private fun defaultServers(): List<AnisetteServer> =
        AppConfig.defaultAnisetteServers.map { AnisetteServer(it.name, it.url) }
}
