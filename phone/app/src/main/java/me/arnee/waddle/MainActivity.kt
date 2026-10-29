package me.arnee.waddle

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import org.json.JSONArray
import org.json.JSONObject

/** Samtaler: "Alle", the groups and the people, newest first. Syncs with the Pi when open. */
class MainActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private lateinit var list: LinearLayout
    private lateinit var status: TextView
    private var name = ""

    // While the app is open, a sync every five minutes. Nothing runs when it is closed.
    private val every5Min = object : Runnable {
        override fun run() {
            sync()
            main.postDelayed(this, 5 * 60 * 1000L)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val page = Ui.column(this)
        page.setBackgroundColor(Ui.PAPER)
        val top = Ui.row(this)
        top.addView(Ui.header(this, "Waddle"), LinearLayout.LayoutParams(0, -2, 1f))
        top.addView(Ui.button(this, "Synk", filled = false) { sync() })
        top.setPadding(0, 0, Ui.dp(this, 12), 0)
        page.addView(top)
        status = Ui.text(this, "", 13f, color = Ui.DIM)
        status.setPadding(Ui.dp(this, 16), 0, Ui.dp(this, 16), Ui.dp(this, 8))
        page.addView(status)
        list = Ui.column(this)
        page.addView(ScrollView(this).apply { addView(list) }, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(page)
    }

    override fun onResume() {
        super.onResume()
        val dir = filesDir.absolutePath
        Mesh.run {
            val state = JSONObject(Mesh.start(dir))
            main.post {
                if (!state.optBoolean("configured")) {
                    startActivity(Intent(this, PairActivity::class.java))
                    finish()
                } else {
                    name = state.optString("name")
                    refresh()
                    main.post(every5Min)
                }
            }
        }
    }

    override fun onPause() {
        super.onPause()
        main.removeCallbacks(every5Min)
    }

    private fun sync() {
        status.text = "Synker …"
        Mesh.run {
            val error = Server.sync()
            main.post {
                status.text = error ?: "$name · synket ${Ui.time(System.currentTimeMillis() / 1000)}"
                refresh()
            }
        }
    }

    private fun refresh() {
        Mesh.run {
            val conversations = JSONArray(Mesh.conversations())
            main.post { show(conversations) }
        }
    }

    private fun show(conversations: JSONArray) {
        list.removeAllViews()
        val items = (0 until conversations.length()).map { conversations.getJSONObject(it) }
        // "Alle" first, then the most recent; those with no messages by name.
        val sorted = items.filter { it.getInt("conv") == 0 } +
            items.filter { it.getInt("conv") != 0 }
                .sortedWith(compareByDescending<JSONObject> { it.optLong("time") }.thenBy { it.optString("title") })
        for (c in sorted) {
            list.addView(item(c))
            // "+ Ny gruppe" right after "Alle", as on the T5.
            if (c.getInt("conv") == 0) list.addView(newGroup())
        }
    }

    private fun newGroup(): View {
        val c = JSONObject().put("conv", -1).put("title", "+ Ny gruppe").put("last", "Skriv til noen av dere")
        return item(c)
    }

    private fun item(c: JSONObject): View {
        val box = Ui.row(this)
        box.setPadding(Ui.dp(this, 16), Ui.dp(this, 12), Ui.dp(this, 16), Ui.dp(this, 12))
        val texts = Ui.column(this)
        texts.addView(Ui.text(this, c.optString("title"), 17f, bold = true))
        val last = c.optString("last").ifEmpty { "Ingen meldinger ennå" }
        texts.addView(Ui.text(this, last, 14f, color = Ui.DIM).apply { maxLines = 1 })
        box.addView(texts, LinearLayout.LayoutParams(0, -2, 1f))
        val unread = c.optInt("unread")
        if (unread > 0) {
            box.addView(Ui.text(this, unread.toString(), 14f, bold = true, color = android.graphics.Color.WHITE).apply {
                background = Ui.rounded(Ui.INK, Ui.dp(this@MainActivity, 14).toFloat())
                setPadding(Ui.dp(this@MainActivity, 9), Ui.dp(this@MainActivity, 2), Ui.dp(this@MainActivity, 9), Ui.dp(this@MainActivity, 2))
            })
        }
        box.setOnClickListener {
            val conv = c.getInt("conv")
            if (conv < 0) {
                startActivity(Intent(this, NewGroupActivity::class.java))
            } else {
                startActivity(Intent(this, ChatActivity::class.java).putExtra("conv", conv))
            }
        }
        val wrap = Ui.column(this)
        wrap.addView(box)
        wrap.addView(View(this).apply { setBackgroundColor(Ui.LINE) }, LinearLayout.LayoutParams(-1, 1))
        return wrap
    }
}
