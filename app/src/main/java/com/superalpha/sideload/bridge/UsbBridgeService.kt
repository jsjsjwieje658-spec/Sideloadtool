package com.superalpha.sideload.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Intent
import android.os.Build
import android.os.IBinder
import com.superalpha.sideload.MainActivity

/**
 * UsbBridgeService — Giữ process sống trong khi sideload đang chạy.
 *
 * Học từ termux-usbmuxd/usbmuxd_proxy.c:
 * - Proxy process ignore SIGHUP/SIGPIPE/SIGTTIN/SIGTTOU để không bị kill
 *   khi terminal/shell đóng. Android tương đương: foreground service với
 *   START_STICKY — process không bị Android kill dù app ở background.
 * - Notification hiển thị trạng thái hiện tại (connect, pair, sideload)
 *   để người dùng biết đang ở bước nào (như log của termux-usbmuxd).
 *
 * Các thành phần giữ kết nối USB thực sự: [UsbTransport], [NativeBridge].
 * Service này chỉ giữ process alive và cập nhật notification.
 */
class UsbBridgeService : Service() {

    private val channelId = "usb_bridge_channel"

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val status = intent?.getStringExtra(EXTRA_STATUS) ?: "Đang kết nối..."
        startForeground(NOTIFICATION_ID, buildNotification(status))
        return START_STICKY
    }

    private fun buildNotification(statusText: String): Notification {
        ensureChannel()
        val openAppIntent = Intent(this, MainActivity::class.java)
        val pendingIntent = android.app.PendingIntent.getActivity(
            this, 0, openAppIntent,
            android.app.PendingIntent.FLAG_IMMUTABLE
        )
        val builder = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            Notification.Builder(this, channelId)
        } else {
            @Suppress("DEPRECATION")
            Notification.Builder(this)
        }
        return builder
            .setContentTitle("SUPER ALPHA Sideload")
            .setContentText(statusText)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(pendingIntent)
            .setOngoing(true)
            .build()
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val manager = getSystemService(NotificationManager::class.java)
            if (manager.getNotificationChannel(channelId) == null) {
                val channel = NotificationChannel(
                    channelId,
                    "USB Connection",
                    NotificationManager.IMPORTANCE_LOW
                ).apply {
                    description = "Giữ kết nối USB với iPhone trong khi cài đặt IPA"
                    setShowBadge(false)
                }
                manager.createNotificationChannel(channel)
            }
        }
    }

    companion object {
        private const val NOTIFICATION_ID = 42
        const val EXTRA_STATUS = "status"

        /** Cập nhật text notification khi bước xử lý thay đổi */
        fun updateStatus(context: android.content.Context, status: String) {
            val intent = Intent(context, UsbBridgeService::class.java)
                .putExtra(EXTRA_STATUS, status)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                context.startForegroundService(intent)
            } else {
                context.startService(intent)
            }
        }

        fun start(context: android.content.Context, status: String = "Đang kết nối với iPhone...") {
            updateStatus(context, status)
        }

        fun stop(context: android.content.Context) {
            context.stopService(Intent(context, UsbBridgeService::class.java))
        }
    }
}
