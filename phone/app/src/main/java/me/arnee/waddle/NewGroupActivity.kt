package me.arnee.waddle

import android.app.Activity
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import org.json.JSONArray

/** Ny gruppe: who is in it (you always are) and a name; then its chat. */
class NewGroupActivity : Activity() {
    private val main = Handler(Looper.getMainLooper())
    private val chosen = mutableSetOf<Int>()
    private lateinit var make: Button
    private lateinit var name: EditText

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val page = Ui.column(this)
        page.setBackgroundColor(Ui.PAPER)
        page.addView(Ui.header(this, "Ny gruppe") { finish() })
        val list = Ui.column(this)
        list.setPadding(Ui.dp(this, 16), 0, Ui.dp(this, 16), 0)
        list.addView(Ui.text(this, "Hvem skal være med? Du er alltid med.", 14f, color = Ui.DIM))
        name = EditText(this).apply { hint = "Navn (valgfritt)" }
        page.addView(ScrollView(this).apply { addView(list) }, LinearLayout.LayoutParams(-1, 0, 1f))
        make = Ui.button(this, "Lag gruppen") { make() }
        val bottom = Ui.column(this)
        bottom.setPadding(Ui.dp(this, 16), Ui.dp(this, 8), Ui.dp(this, 16), Ui.dp(this, 16))
        bottom.addView(name)
        bottom.addView(make, LinearLayout.LayoutParams(-1, -2).apply { topMargin = Ui.dp(this@NewGroupActivity, 8) })
        page.addView(bottom)
        setContentView(page)
        update()
        Mesh.run {
            val members = JSONArray(Mesh.members())
            main.post {
                for (i in 0 until members.length()) {
                    val m = members.getJSONObject(i)
                    val n = m.getInt("number")
                    list.addView(CheckBox(this).apply {
                        text = m.getString("name")
                        textSize = 17f
                        setTextColor(Ui.INK)
                        setPadding(Ui.dp(this@NewGroupActivity, 8), Ui.dp(this@NewGroupActivity, 12), 0, Ui.dp(this@NewGroupActivity, 12))
                        setOnCheckedChangeListener { _, on ->
                            if (on) chosen.add(n) else chosen.remove(n)
                            update()
                        }
                    })
                    list.addView(View(this).apply { setBackgroundColor(Ui.LINE) }, LinearLayout.LayoutParams(-1, 1))
                }
            }
        }
    }

    private fun update() {
        make.isEnabled = chosen.isNotEmpty()
        make.text = if (chosen.isEmpty()) "Velg hvem som skal være med" else "Lag gruppen (${chosen.size} + deg)"
    }

    private fun make() {
        var bits = 0L
        for (n in chosen) bits = bits or (1L shl (n - 1))
        val title = name.text.toString().trim()
        Mesh.run {
            val conv = Mesh.createGroup(bits, title)
            if (conv >= 0) Server.sync()  // the invitations go out through the Pi
            main.post {
                if (conv < 0) {
                    make.text = "Kunne ikke lage gruppen (16 er maks)"
                } else {
                    startActivity(Intent(this, ChatActivity::class.java).putExtra("conv", conv))
                    finish()
                }
            }
        }
    }
}
