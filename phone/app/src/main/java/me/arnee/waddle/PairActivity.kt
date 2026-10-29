package me.arnee.waddle

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import org.json.JSONObject
import java.net.URLEncoder

/**
 * Pairing with your T5 (protocol/PROTOCOL.md, "Devices"): the T5 on File
 * Transfer, both on the home Wi-Fi. The same six digits on both screens show
 * nothing sits between them; then the T5 hands over a sealed copy of its keys.
 */
class PairActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private lateinit var address: EditText
    private lateinit var status: TextView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val page = Ui.column(this)
        page.setBackgroundColor(Ui.PAPER)
        val pad = Ui.dp(this, 20)
        page.setPadding(pad, pad, pad, pad)
        page.addView(Ui.text(this, "Koble til T5-en din", 24f, bold = true))
        page.addView(Ui.text(this, "\nMobilen blir en ekstra enhet for deg: den leser og skriver det samme som T5-en, " +
            "gjennom serveren hjemme.\n\nPå T5-en: Filoverføring → Bli med i nettverk. Skriv inn adressen som står på skjermen.\n", 16f))
        address = EditText(this).apply {
            hint = "192.168.1.23"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_URI
        }
        page.addView(address)
        page.addView(Ui.button(this, "Koble til") { pair() }, LinearLayout.LayoutParams(-1, -2).apply { topMargin = pad })
        status = Ui.text(this, "", 15f, color = Ui.DIM)
        status.setPadding(0, pad, 0, 0)
        page.addView(status)
        setContentView(page)
    }

    private fun say(text: String) {
        main.post { status.text = text }
    }

    private fun pair() {
        val ip = address.text.toString().trim()
        if (ip.isEmpty()) return
        say("Kobler til …")
        Mesh.run {
            try {
                val commit = Mesh.pairBegin()
                val (code1, ident) = Server.unit(ip, "/api/mesh/identity?commit=" + URLEncoder.encode(commit, "UTF-8"))
                val unit = JSONObject(ident)
                if (code1 != 200 || !unit.has("nonce")) return@run say("T5-en svarte ikke som ventet (eldre programvare?)")
                val answer = JSONObject(Mesh.pairAnswer(unit.getString("publicKey"), unit.getString("nonce")))
                val reveal = JSONObject().put("tool", answer.getString("tool")).put("nonce", answer.getString("nonce"))
                val (code2, _) = Server.unit(ip, "/api/mesh/pair", reveal.toString())
                if (code2 != 200) return@run say("T5-en godtok ikke koblingen")
                val code = answer.getString("code")
                main.post { confirm(ip, code) }
            } catch (e: Exception) {
                say("Fant ikke T5-en på $ip (${e.javaClass.simpleName})")
            }
        }
    }

    private fun confirm(ip: String, code: String) {
        AlertDialog.Builder(this)
            .setTitle("Står det $code på T5-en?")
            .setMessage("Sjekk at sifrene på T5-skjermen er de samme.")
            .setPositiveButton("Ja") { _, _ -> takeKeys(ip) }
            .setNegativeButton("Nei") { _, _ -> say("Stoppet: sifrene var ikke like. Prøv igjen.") }
            .show()
    }

    private fun takeKeys(ip: String) {
        say("Henter nøklene …")
        Mesh.run {
            try {
                val (code, body) = Server.unit(ip, "/api/mesh/phone", Mesh.pairRequest())
                if (code != 200) return@run say("T5-en sa: $body")
                val error = Mesh.pairFinish(JSONObject(body).getString("box"))
                if (error.isNotEmpty()) return@run say(error)
                say("Koblet! Henter meldinger fra serveren …")
                Server.sync()
                main.post {
                    startActivity(Intent(this, MainActivity::class.java))
                    finish()
                }
            } catch (e: Exception) {
                say("Noe gikk galt (${e.javaClass.simpleName})")
            }
        }
    }
}
