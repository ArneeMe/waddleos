package me.arnee.waddle

import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.security.KeyStore
import java.security.cert.CertificateFactory
import java.security.cert.X509Certificate
import javax.net.ssl.HttpsURLConnection
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory

/** Talking to the Pi (protocol/PROTOCOL.md, "Server over Wi-Fi"). Call on the mesh thread. */
object Server {
    private const val FULL = 200  // the server's most packets per answer: ask again

    /** One sync, repeated while answers are full. Returns an error text, or null. */
    fun sync(): String? {
        val info = JSONObject(Mesh.server())
        val url = info.optString("url")
        if (url.isEmpty()) return "Ingen server i nettverket"
        try {
            val pinned = certificate(info.optString("cert"))
            for (round in 0 until 10) {
                val conn = URL("$url/api/packets").openConnection() as HttpsURLConnection
                conn.sslSocketFactory = pinnedContext(pinned).socketFactory
                // The Pi's own certificate, whatever name it has: the only one trusted.
                conn.setHostnameVerifier { _, session -> session.peerCertificates.firstOrNull() == pinned }
                conn.requestMethod = "POST"
                conn.doOutput = true
                conn.connectTimeout = 10000
                conn.readTimeout = 20000
                conn.setRequestProperty("Authorization", "Bearer " + info.optString("token"))
                conn.setRequestProperty("Content-Type", "application/octet-stream")
                conn.outputStream.use { it.write(Mesh.syncRequest()) }
                if (conn.responseCode != 200) return "Serveren svarte ${conn.responseCode}"
                val answer = conn.inputStream.use { readAll(it) }
                val date = conn.getHeaderFieldDate("Date", 0L) / 1000
                if (Mesh.syncAnswer(answer, date) < FULL) return null
            }
            return null
        } catch (e: Exception) {
            return "Fikk ikke kontakt med serveren (${e.javaClass.simpleName})"
        }
    }

    private fun certificate(pem: String): X509Certificate =
        CertificateFactory.getInstance("X.509").generateCertificate(pem.byteInputStream()) as X509Certificate

    private fun pinnedContext(cert: X509Certificate): SSLContext {
        val store = KeyStore.getInstance(KeyStore.getDefaultType())
        store.load(null, null)
        store.setCertificateEntry("pi", cert)
        val trust = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm())
        trust.init(store)
        return SSLContext.getInstance("TLS").apply { init(null, trust.trustManagers, null) }
    }

    fun readAll(input: java.io.InputStream): ByteArray {
        val out = ByteArrayOutputStream()
        input.copyTo(out)
        return out.toByteArray()
    }

    /** Plain HTTP to the T5 on the home Wi-Fi (pairing). Returns (status, body). */
    fun unit(address: String, path: String, json: String? = null): Pair<Int, String> {
        val conn = URL("http://$address$path").openConnection() as HttpURLConnection
        conn.connectTimeout = 8000
        conn.readTimeout = 15000
        if (json != null) {
            conn.requestMethod = "POST"
            conn.doOutput = true
            conn.setRequestProperty("Content-Type", "application/json")
            conn.outputStream.use { it.write(json.toByteArray()) }
        }
        val code = conn.responseCode
        val stream = if (code in 200..299) conn.inputStream else conn.errorStream
        val body = stream?.use { String(readAll(it)) } ?: ""
        return Pair(code, body)
    }
}
