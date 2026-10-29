package me.arnee.waddle

import android.app.Activity
import android.graphics.Color
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import org.json.JSONArray
import org.json.JSONObject

/** One conversation: the messages, and writing (sent at the next sync, right away). */
class ChatActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private lateinit var messages: LinearLayout
    private lateinit var scroll: ScrollView
    private lateinit var input: EditText
    private var conv = 0

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        conv = intent.getIntExtra("conv", 0)
        val page = Ui.column(this)
        page.setBackgroundColor(Ui.PAPER)
        val header = Ui.column(this)
        page.addView(header)
        messages = Ui.column(this)
        messages.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 8))
        scroll = ScrollView(this).apply { addView(messages) }
        page.addView(scroll, LinearLayout.LayoutParams(-1, 0, 1f))
        val bottom = Ui.row(this)
        bottom.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 12))
        input = EditText(this).apply { hint = "Skriv melding …" }
        bottom.addView(input, LinearLayout.LayoutParams(0, -2, 1f))
        bottom.addView(Ui.button(this, "Send") { send() })
        page.addView(bottom)
        setContentView(page)
        Mesh.run {
            val title = Mesh.title(conv)
            main.post {
                val bar = Ui.header(this, if (conv > 32) "$title  ›" else title) { finish() }
                // A group: its members and "Forlat gruppe".
                if (conv > 32) bar.setOnClickListener {
                    startActivity(android.content.Intent(this, GroupActivity::class.java).putExtra("conv", conv))
                }
                header.addView(bar)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        refresh()
    }

    private fun send() {
        val text = input.text.toString().trim()
        if (text.isEmpty()) return
        input.setText("")
        Mesh.run {
            val sent = Mesh.send(conv, text)
            main.post { refresh() }
            if (sent) Server.sync()
            main.post {
                if (!sent) input.setText(text)
                refresh()
            }
        }
    }

    private fun refresh() {
        Mesh.run {
            Mesh.markRead(conv)
            val chat = JSONArray(Mesh.chat(conv))
            val writable = Mesh.canWrite(conv)
            main.post { show(chat, writable) }
        }
    }

    private fun show(chat: JSONArray, writable: Boolean) {
        messages.removeAllViews()
        input.isEnabled = writable
        if (!writable) input.hint = "Venter på nøkkel fra T5-en"
        val group = conv == 0 || conv > 32
        for (i in 0 until chat.length()) messages.addView(bubble(chat.getJSONObject(i), group))
        scroll.post { scroll.fullScroll(ScrollView.FOCUS_DOWN) }
    }

    private fun bubble(m: JSONObject, group: Boolean): LinearLayout {
        val mine = m.optBoolean("mine")
        val box = Ui.column(this)
        box.gravity = if (mine) Gravity.END else Gravity.START
        val head = (if (!mine && group) m.optString("from") + "  " else "") + Ui.time(m.optLong("time"))
        box.addView(Ui.text(this, head, 12f, color = Ui.DIM))
        val text = Ui.text(this, m.optString("text"), 16f, color = if (mine) Color.WHITE else Ui.INK)
        text.background = Ui.rounded(if (mine) Ui.INK else Color.WHITE, Ui.dp(this, 14).toFloat(), if (mine) 0 else Ui.dp(this, 2))
        text.setPadding(Ui.dp(this, 12), Ui.dp(this, 8), Ui.dp(this, 12), Ui.dp(this, 8))
        box.addView(text)
        val note = if (mine) m.optString("state") else if (m.optBoolean("warning")) "(!) en melding kan mangle" else ""
        if (note.isNotEmpty()) box.addView(Ui.text(this, note, 12f, color = Ui.DIM))
        box.setPadding(0, Ui.dp(this, 6), 0, Ui.dp(this, 6))
        return box
    }
}
