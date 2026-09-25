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
