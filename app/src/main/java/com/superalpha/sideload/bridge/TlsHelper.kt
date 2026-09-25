package com.superalpha.sideload.bridge

import android.util.Base64
import android.util.Log
import java.io.ByteArrayInputStream
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyStore
import java.security.SecureRandom
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.security.spec.PKCS8EncodedKeySpec
import javax.net.ssl.*

/**
 * TlsHelper — TLS handshake qua mux connection (Android SSLEngine).
 * Được gọi từ C JNI: TlsHelper.handshake(certPem, keyPem, connPtr) → Boolean
 * Sau handshake, C dùng nativeTlsSend/nativeTlsRecv để giao tiếp.
 */
object TlsHelper {
    private const val TAG = "TlsHelper"

    @JvmStatic
    fun handshake(certPem: ByteArray, keyPem: ByteArray, connPtr: Long): Boolean = try {
        val certFactory = CertificateFactory.getInstance("X.509")
        val cert = certFactory.generateCertificate(ByteArrayInputStream(certPem)) as X509Certificate
        val keyDer = pemKeyToDer(String(keyPem))
        val privateKey = KeyFactory.getInstance("RSA").generatePrivate(PKCS8EncodedKeySpec(keyDer))
        val ks = KeyStore.getInstance("PKCS12").apply {
            load(null, null); setKeyEntry("host", privateKey, null, arrayOf(cert))
        }
        val kmf = KeyManagerFactory.getInstance("X509").apply { init(ks, null) }
        val trustAll = arrayOf<TrustManager>(object : X509TrustManager {
            override fun getAcceptedIssuers() = arrayOf<X509Certificate>()
            override fun checkClientTrusted(c: Array<X509Certificate>, a: String) {}
            override fun checkServerTrusted(c: Array<X509Certificate>, a: String) {}
        })
        val engine = SSLContext.getInstance("TLS").also { it.init(kmf.keyManagers, trustAll, SecureRandom()) }
            .createSSLEngine().apply {
                useClientMode = true
                /* BUGFIX v11: TLS 1.0 và 1.1 bị Android 10+ vô hiệu hóa theo mặc định.
                 * lockdownd của Apple yêu cầu TLS 1.2 — chỉ bật đúng phiên bản này. */
                enabledProtocols = arrayOf("TLSv1.2")
            }
        val ok = doHandshake(engine, connPtr)
        if (ok) Log.i(TAG, "✅ TLS handshake thành công conn=$connPtr")
        else Log.e(TAG, "❌ TLS handshake thất bại conn=$connPtr")
        ok
    } catch (e: Exception) { Log.e(TAG, "TLS exception: ${e.message}", e); false }

    private fun doHandshake(engine: SSLEngine, connPtr: Long): Boolean {
        engine.beginHandshake()
        val appBuf = ByteBuffer.allocate(engine.session.applicationBufferSize)
        val netBuf = ByteBuffer.allocate(engine.session.packetBufferSize)
        val empty  = ByteBuffer.allocate(0)
        var status = engine.handshakeStatus
        while (status != SSLEngineResult.HandshakeStatus.FINISHED &&
               status != SSLEngineResult.HandshakeStatus.NOT_HANDSHAKING) {
            when (status) {
                SSLEngineResult.HandshakeStatus.NEED_WRAP -> {
                    netBuf.clear(); engine.wrap(empty, netBuf); netBuf.flip()
                    val bytes = ByteArray(netBuf.remaining()); netBuf.get(bytes)
                    if (!nativeTlsSend(connPtr, bytes)) return false
                }
                SSLEngineResult.HandshakeStatus.NEED_UNWRAP -> {
                    val data = nativeTlsRecv(connPtr, engine.session.packetBufferSize) ?: return false
                    appBuf.clear(); engine.unwrap(ByteBuffer.wrap(data), appBuf)
                }
                SSLEngineResult.HandshakeStatus.NEED_TASK -> engine.delegatedTask?.run()
                else -> break
            }
            status = engine.handshakeStatus
        }
        return status == SSLEngineResult.HandshakeStatus.FINISHED ||
               status == SSLEngineResult.HandshakeStatus.NOT_HANDSHAKING
    }

    private fun pemKeyToDer(pem: String): ByteArray =
        Base64.decode(pem
            .replace("-----BEGIN RSA PRIVATE KEY-----", "")
            .replace("-----END RSA PRIVATE KEY-----", "")
            .replace("-----BEGIN PRIVATE KEY-----", "")
            .replace("-----END PRIVATE KEY-----", "")
            .replace("\n", "").replace("\r", "").trim(), Base64.DEFAULT)

    @JvmStatic private external fun nativeTlsSend(connPtr: Long, data: ByteArray): Boolean
    @JvmStatic private external fun nativeTlsRecv(connPtr: Long, maxLen: Int): ByteArray?
}
