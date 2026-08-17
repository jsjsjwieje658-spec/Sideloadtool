package com.superalpha.sideload.bridge

import android.util.Base64
import android.util.Log
import org.bouncycastle.asn1.x500.X500Name
import org.bouncycastle.asn1.x509.BasicConstraints
import org.bouncycastle.asn1.x509.Extension
import org.bouncycastle.asn1.x509.KeyUsage
import org.bouncycastle.cert.jcajce.JcaX509CertificateConverter
import org.bouncycastle.cert.jcajce.JcaX509v3CertificateBuilder
import org.bouncycastle.jce.provider.BouncyCastleProvider
import org.bouncycastle.operator.jcajce.JcaContentSignerBuilder
import java.io.ByteArrayInputStream
import java.math.BigInteger
import java.security.*
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import java.security.spec.RSAPublicKeySpec
import java.security.spec.X509EncodedKeySpec
import java.util.Date

/**
 * CertHelper — Tạo RSA 2048-bit certificate chain cho Apple device pairing.
 * Được gọi từ C qua JNI: CertHelper.generateCertChain(devicePublicKeyDer)
 * Trả về: [rootCertPem, rootKeyPem, hostCertPem, hostKeyPem, deviceCertPem]
 */
object CertHelper {
    private const val TAG = "CertHelper"
    init { if (Security.getProvider("BC") == null) Security.addProvider(BouncyCastleProvider()) }

    @JvmStatic
    fun generateCertChain(devicePublicKeyDer: ByteArray): Array<String>? = try {
        val random = SecureRandom()
        val keyGen = KeyPairGenerator.getInstance("RSA", "BC").apply { initialize(2048, random) }
        val now = Date(); val tenYears = Date(now.time + 10L * 365 * 24 * 3600 * 1000)
        val rootKP = keyGen.generateKeyPair()
        val rootName = X500Name("CN=Root Certification Authority")
        val rootSigner = JcaContentSignerBuilder("SHA1withRSA").setProvider("BC").build(rootKP.private)
        val rootCert = JcaX509CertificateConverter().setProvider("BC").getCertificate(
            JcaX509v3CertificateBuilder(rootName, BigInteger.ONE, now, tenYears, rootName, rootKP.public)
                .addExtension(Extension.basicConstraints, true, BasicConstraints(true))
                .build(rootSigner))
        val hostKP = keyGen.generateKeyPair()
        val hostCert = JcaX509CertificateConverter().setProvider("BC").getCertificate(
            JcaX509v3CertificateBuilder(rootName, BigInteger.TWO, now, tenYears,
                X500Name("CN=iPhone OS Pair Record"), hostKP.public)
                .addExtension(Extension.basicConstraints, false, BasicConstraints(false))
                .addExtension(Extension.keyUsage, true, KeyUsage(KeyUsage.keyEncipherment or KeyUsage.digitalSignature))
                .build(rootSigner))
        /*
         * BUGFIX: iPhone gửi DevicePublicKey dưới dạng PKCS#1 DER (raw RSAPublicKey:
         * SEQUENCE { INTEGER modulus, INTEGER exponent }), KHÔNG phải X.509
         * SubjectPublicKeyInfo mà X509EncodedKeySpec yêu cầu.
         * X509EncodedKeySpec(pkcs1Bytes) → InvalidKeySpecException → exception
         * bị catch → generateCertChain trả null → pairing thất bại.
         *
         * Fix: dùng BouncyCastle RSAPublicKey.getInstance() để parse PKCS#1, sau đó
         * tạo PublicKey đúng cách qua RSAPublicKeySpec.
         */
        val devPub = pkcs1RsaToPublicKey(devicePublicKeyDer)
        val devCert = JcaX509CertificateConverter().setProvider("BC").getCertificate(
            JcaX509v3CertificateBuilder(rootName, BigInteger.valueOf(3), now, tenYears,
                X500Name("CN=Device"), devPub)
                .addExtension(Extension.basicConstraints, false, BasicConstraints(false))
                .build(rootSigner))
        Log.i(TAG, "✅ generateCertChain thành công")
        arrayOf(certPem(rootCert), keyPem(rootKP.private.encoded), certPem(hostCert),
                keyPem(hostKP.private.encoded), certPem(devCert))
    } catch (e: Exception) { Log.e(TAG, "generateCertChain lỗi: ${e.message}", e); null }

    /**
     * pkcs1RsaToPublicKey — Chuyển đổi PKCS#1 RSA public key DER (định dạng iPhone
     * gửi qua GetValue("DevicePublicKey")) sang java.security.PublicKey.
     *
     * PKCS#1 format:  SEQUENCE { INTEGER (modulus), INTEGER (exponent) }
     * X.509 format:   SEQUENCE { SEQUENCE { OID, NULL }, BIT STRING { PKCS#1 } }
     *
     * Java KeyFactory.getInstance("RSA").generatePublic(X509EncodedKeySpec) chỉ chấp
     * nhận X.509 — dùng BouncyCastle để parse PKCS#1 rồi tạo key qua RSAPublicKeySpec.
     */
    private fun pkcs1RsaToPublicKey(pkcs1Der: ByteArray): java.security.PublicKey {
        val rsaKey = org.bouncycastle.asn1.pkcs.RSAPublicKey.getInstance(pkcs1Der)
        return KeyFactory.getInstance("RSA", "BC")
            .generatePublic(RSAPublicKeySpec(rsaKey.modulus, rsaKey.publicExponent))
    }

    private fun certPem(c: X509Certificate) =
        "-----BEGIN CERTIFICATE-----\n" +
        Base64.encodeToString(c.encoded, Base64.DEFAULT) +
        "-----END CERTIFICATE-----\n"

    private fun keyPem(der: ByteArray) =
        "-----BEGIN RSA PRIVATE KEY-----\n" +
        Base64.encodeToString(der, Base64.DEFAULT) +
        "-----END RSA PRIVATE KEY-----\n"
}
