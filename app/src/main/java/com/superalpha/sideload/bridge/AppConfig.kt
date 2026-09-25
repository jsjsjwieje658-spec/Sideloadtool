package com.superalpha.sideload.bridge

import android.content.Context
import android.content.SharedPreferences
import androidx.core.content.edit

object AppConfig {
    private const val PREFS_NAME = "superalpha_config"
    private lateinit var prefs: SharedPreferences

    fun init(context: Context) {
        prefs = context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)
    }

    var appleId: String
        get() = prefs.getString("apple_id", "") ?: ""
        set(v) = prefs.edit { putString("apple_id", v) }

    /*
     * v51: LƯU MẬT KHẨU Apple ID theo yêu cầu người dùng ("sau khi apple id
     * và mật khẩu được lưu sẽ không cần phải nhập lại nhiều lần nữa").
     *
     * Bảo mật: SharedPreferences nằm trong storage RIÊNG TƯ của app
     * (/data/data/<pkg>/shared_prefs, chế độ MODE_PRIVATE), manifest khai
     * allowBackup=false nên KHÔNG bao giờ ra khỏi máy (không backup lên
     * cloud, không sang app khác). Chỉ có rủi ro khi máy đã root.
     */
    var applePassword: String
        get() = prefs.getString("apple_password", "") ?: ""
        set(v) = prefs.edit { putString("apple_password", v) }

    /** v51: đã có thông tin đăng nhập (Apple ID + mật khẩu) chưa? */
    fun hasAppleAccount(): Boolean = appleId.isNotBlank() && applePassword.isNotBlank()

    /**
     * v51: Đăng xuất — xoá Apple ID + mật khẩu (+ session token nếu có).
     * Lần mở app tiếp theo sẽ phải đăng nhập lại (màn Login).
     * Giữ nguyên lastUdid vì pair record với iPhone độc lập với tài khoản Apple.
     */
    fun clearAppleAccount() = prefs.edit {
        remove("apple_id")
        remove("apple_password")
        remove("session_token")
        remove("dsid")
    }

    var anisetteUrl: String
        get() = prefs.getString("anisette_url", "") ?: ""
        set(v) = prefs.edit { putString("anisette_url", v) }

    var lastUdid: String
        get() = prefs.getString("last_udid", "") ?: ""
        set(v) = prefs.edit { putString("last_udid", v) }

    var teamId: String
        get() = prefs.getString("team_id", "") ?: ""
        set(v) = prefs.edit { putString("team_id", v) }

    fun clearSession() = prefs.edit { remove("session_token"); remove("dsid") }

    val defaultAnisetteServers: List<AnisetteServer> = listOf(
        AnisetteServer("SideStore Official", "https://ani.sidestore.io"),
        AnisetteServer("Josi's Server", "https://anisette.josi.eu"),
        AnisetteServer("Local (Sideloadly)", "http://localhost:6969"),
    )
    data class AnisetteServer(val name: String, val url: String)
}
