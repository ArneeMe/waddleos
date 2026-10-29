package me.arnee.waddle

import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.View
import android.widget.Button
import android.widget.LinearLayout
import android.widget.TextView
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/** Small helpers for screens built in code (no layout files, no extra libraries). */
object Ui {
    const val INK = 0xFF1D1D1B.toInt()
    const val DIM = 0xFF6B675E.toInt()
    const val PAPER = 0xFFF4F1EA.toInt()
    const val LINE = 0xFFE3DED2.toInt()

    fun dp(context: Context, value: Int): Int = (value * context.resources.displayMetrics.density).toInt()

    fun text(context: Context, value: String, size: Float = 16f, bold: Boolean = false, color: Int = INK) =
        TextView(context).apply {
            text = value
            textSize = size
            setTextColor(color)
            if (bold) typeface = Typeface.DEFAULT_BOLD
        }

    fun column(context: Context) = LinearLayout(context).apply { orientation = LinearLayout.VERTICAL }

    fun row(context: Context) = LinearLayout(context).apply {
        orientation = LinearLayout.HORIZONTAL
        gravity = Gravity.CENTER_VERTICAL
    }

    fun rounded(fill: Int, radius: Float, stroke: Int = 0, strokeColor: Int = INK) = GradientDrawable().apply {
        setColor(fill)
        cornerRadius = radius
        if (stroke > 0) setStroke(stroke, strokeColor)
    }

    fun button(context: Context, label: String, filled: Boolean = true, onClick: () -> Unit) =
        Button(context).apply {
            text = label
            isAllCaps = false
            setTextColor(if (filled) Color.WHITE else INK)
            background = rounded(if (filled) INK else Color.WHITE, dp(context, 12).toFloat(), dp(context, 2))
            setOnClickListener { onClick() }
        }

    fun header(context: Context, title: String, back: (() -> Unit)? = null): View {
        val bar = row(context)
        bar.setPadding(dp(context, 16), dp(context, 14), dp(context, 16), dp(context, 14))
        if (back != null) {
            bar.addView(text(context, "←  ", 22f).apply { setOnClickListener { back() } })
        }
        bar.addView(text(context, title, 20f, bold = true))
        return bar
    }

    /** "14:32", "i går 20:15" or "27. okt", as on the T5. */
    fun time(unix: Long): String {
        if (unix <= 0) return ""
        val then = Date(unix * 1000)
        val day = SimpleDateFormat("yyyyMMdd", Locale.ROOT)
        val now = Date()
        val clock = SimpleDateFormat("HH:mm", Locale.ROOT).format(then)
        return when {
            day.format(then) == day.format(now) -> clock
            day.format(then) == day.format(Date(now.time - 86_400_000)) -> "i går $clock"
            else -> SimpleDateFormat("d. MMM", Locale("nb")).format(then)
        }
    }
}
