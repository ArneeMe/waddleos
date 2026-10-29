package me.arnee.waddle

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.LinearLayout
import org.json.JSONObject

/** A group: who is in it, and "Forlat gruppe" (asks once more). */
class GroupActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private var conv = 0
    private var armed = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        conv = intent.getIntExtra("conv", 0)
        Mesh.run {
            val title = Mesh.title(conv)
            val group = JSONObject(Mesh.group(conv).let { if (it == "null") "{}" else it })
            main.post { show(title, group) }
        }
    }

    private fun show(title: String, group: JSONObject) {
        val page = Ui.column(this)
        page.setBackgroundColor(Ui.PAPER)
        page.addView(Ui.header(this, title) { finish() })
        val body = Ui.column(this)
        body.setPadding(Ui.dp(this, 16), 0, Ui.dp(this, 16), 0)
        body.addView(Ui.text(this, "MEDLEMMER", 12f, bold = true, color = Ui.DIM))
        val names = group.optJSONArray("members")
        for (i in 0 until (names?.length() ?: 0)) {
            body.addView(Ui.text(this, names!!.getString(i), 17f, bold = true).apply {
                setPadding(0, Ui.dp(this@GroupActivity, 12), 0, Ui.dp(this@GroupActivity, 12))
            })
            body.addView(View(this).apply { setBackgroundColor(Ui.LINE) }, LinearLayout.LayoutParams(-1, 1))
        }
        if (group.optBoolean("left")) {
            body.addView(Ui.text(this, "\nDu har forlatt gruppen. Meldingene fra før kan du fortsatt lese.", 14f, color = Ui.DIM))
        } else {
            val leave = Ui.button(this, "Forlat gruppe", filled = false) {}
            leave.setOnClickListener {
                if (!armed) {
                    armed = true
                    leave.text = "Sikker? Trykk igjen"
                } else {
                    Mesh.run {
                        Mesh.leaveGroup(conv)
                        Server.sync()
                        main.post { finish() }
                    }
                }
            }
            body.addView(leave, LinearLayout.LayoutParams(-1, -2).apply { topMargin = Ui.dp(this@GroupActivity, 24) })
            body.addView(Ui.text(this, "\nDen som går ut, kan lese det som ble skrevet før, men ikke det som kommer etterpå.", 13f, color = Ui.DIM))
        }
        page.addView(body)
        setContentView(page)
    }
}
