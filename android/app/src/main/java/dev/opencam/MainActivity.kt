package dev.opencam

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.LayerDrawable
import android.graphics.drawable.RippleDrawable
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.Gravity
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import org.json.JSONObject
import kotlin.math.abs
import kotlin.math.ln
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/** Camera-app style remote for [CamService]. Every control maps to one state key (PROTOCOL.md). */
class MainActivity : Activity(), SurfaceHolder.Callback {

    private companion object {
        val ACCENT = 0xFFFFC93C.toInt()
        val CHIP = 0x33FFFFFF
        val DIM = 0x99FFFFFF.toInt()
        val RED = 0xFFEF4444.toInt()
        val GREEN = 0xFF22C55E.toInt()
    }

    /** One slider-driven setting. Log scale suits zoom, ISO and shutter. */
    private class Spec(
        val key: String, val min: Double, val max: Double,
        log: Boolean = false, val kind: Int = 0, val label: (Double) -> String,
    ) {
        private val useLog = log && min > 0
        fun toP(v: Double): Int = if (max <= min) 0 else (1000 * (
            if (useLog) ln(v.coerceIn(min, max) / min) / ln(max / min) else (v.coerceIn(min, max) - min) / (max - min)
        )).roundToInt()
        fun fromP(p: Int): Double { val t = p / 1000.0; return if (useLog) min * (max / min).pow(t) else min + (max - min) * t }
        fun cast(v: Double): Any = when (kind) { 1 -> v.roundToInt(); 2 -> v.roundToLong(); else -> (v * 100).roundToInt() / 100.0 }
    }

    private val main = Handler(Looper.getMainLooper())
    private var svc: CamService? = null
    private var caps = JSONObject()
    private var st = JSONObject()
    private var started = false
    private var visible = false
    private var surfaceOk = false
    private var fixedSize = ""
    private var spec: Spec? = null
    private var dragging = false
    private lateinit var medium: Typeface

    private lateinit var surface: SurfaceView
    private lateinit var vf: FrameLayout
    private lateinit var hint: TextView
    private lateinit var torch: View
    private lateinit var afBtn: TextView
    private lateinit var stab: TextView
    private lateinit var rot: TextView
    private lateinit var evBtn: TextView
    private lateinit var mirror: View
    private lateinit var connCard: LinearLayout
    private lateinit var zoomRow: LinearLayout
    private lateinit var sliderBox: LinearLayout
    private lateinit var sliderLabel: TextView
    private lateinit var bar: SeekBar
    private lateinit var proRow: LinearLayout
    private lateinit var modeRow: LinearLayout
    private lateinit var connText: TextView
    private lateinit var liveBtn: TextView
    private lateinit var drawer: LinearLayout

    // ---------------------------------------------------------------- lifecycle

    override fun onCreate(b: Bundle?) {
        super.onCreate(b)
        medium = Typeface.create("sans-serif-medium", Typeface.NORMAL)
        buildUi()
        val need = buildList {
            add(Manifest.permission.CAMERA)
            if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
        }.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (need.isEmpty()) startCam() else requestPermissions(need.toTypedArray(), 1)
        handleUpdate(intent)
    }

    override fun onNewIntent(i: Intent) {
        super.onNewIntent(i)
        handleUpdate(i)
    }

    override fun onResume() {
        super.onResume()
        if (wantInstall) installUpdate() // back from the "install unknown apps" page
    }

    private var wantInstall = false

    private fun installUpdate() {
        wantInstall = !Updater.install(this)
    }

    /** Update notification tapped, or the system installer reporting back. */
    private fun handleUpdate(i: Intent?) {
        when (i?.action) {
            Updater.ACTION_INSTALL -> installUpdate()
            Updater.ACTION_STATUS -> when (i.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE)) {
                PackageInstaller.STATUS_PENDING_USER_ACTION ->
                    @Suppress("DEPRECATION") i.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)?.let { startActivity(it) }
                PackageInstaller.STATUS_SUCCESS -> {}
                else -> Toast.makeText(this, "Update failed: " + i.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE), Toast.LENGTH_LONG).show()
            }
        }
    }

    override fun onRequestPermissionsResult(rc: Int, perms: Array<out String>, res: IntArray) {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) startCam()
        else hint.text = "Camera permission is needed"
    }

    private fun startCam() {
        startForegroundService(Intent(this, CamService::class.java))
        started = true
        attach()
    }

    override fun onStart() {
        super.onStart()
        visible = true
        if (started) attach()
    }

    override fun onStop() {
        visible = false
        svc?.onUpdate = null
        super.onStop()
    }

    override fun onDestroy() {
        svc?.onQuit = null
        super.onDestroy()
    }

    /** The service is in-process; wait for it to come up, then hook in. */
    private fun attach() {
        if (!visible) return
        val s = CamService.instance ?: run { main.postDelayed({ attach() }, 50); return }
        svc = s
        s.onQuit = { finishAndRemoveTask() }
        s.onUpdate = { c, state -> caps = c; st = state; render() }
        if (surfaceOk) s.setPreview(surface.holder.surface)
    }

    override fun surfaceCreated(h: SurfaceHolder) {}

    override fun surfaceChanged(h: SurfaceHolder, format: Int, w: Int, hh: Int) {
        surfaceOk = true
        svc?.setPreview(h.surface)
    }

    override fun surfaceDestroyed(h: SurfaceHolder) {
        surfaceOk = false
        svc?.setPreview(null)
    }

    // ---------------------------------------------------------------- layout

    private fun dp(v: Number) = (v.toFloat() * resources.displayMetrics.density).roundToInt()

    private fun pill(color: Int) = GradientDrawable().apply { cornerRadius = dp(100).toFloat(); setColor(color) }

    private fun round(color: Int, r: Int) = GradientDrawable().apply { cornerRadius = dp(r).toFloat(); setColor(color) }

    /** Pressed feedback that never changes the view's bounds. */
    private fun View.ripple() { foreground = RippleDrawable(ColorStateList.valueOf(0x33FFFFFF), null, pill(Color.WHITE)) }

    private fun chip(text: String, size: Float = 13f, click: () -> Unit) = TextView(this).apply {
        this.text = text
        textSize = size
        typeface = medium
        gravity = Gravity.CENTER
        setTextColor(Color.WHITE)
        setPadding(dp(12), dp(7), dp(12), dp(7))
        minHeight = dp(36)
        setOnClickListener { click() }
        ripple()
    }

    private fun icon(text: String, size: Float = 17f, click: () -> Unit) = chip(text, size, click).apply {
        setPadding(dp(10), dp(10), dp(10), dp(10))
        minWidth = dp(48)
        minHeight = dp(48)
    }

    /** 48 dp touch target around a 22 dp vector icon. */
    private fun iconBtn(res: Int, desc: String, click: () -> Unit) = ImageView(this).apply {
        setImageResource(res)
        imageTintList = ColorStateList.valueOf(Color.WHITE)
        scaleType = ImageView.ScaleType.CENTER_INSIDE
        setPadding(dp(13), dp(13), dp(13), dp(13))
        minimumWidth = dp(48)
        minimumHeight = dp(48)
        contentDescription = desc
        setOnClickListener { click() }
        ripple()
    }

    private fun View.mark(on: Boolean, supported: Boolean = true) {
        val c = if (on) ACCENT else Color.WHITE
        if (this is TextView) setTextColor(c)
        if (this is ImageView) imageTintList = ColorStateList.valueOf(c)
        background = if (on) pill(CHIP) else null
        alpha = if (supported) 1f else 0.3f
    }

    private fun liveBg(on: Boolean): Drawable {
        val ring = GradientDrawable().apply { shape = GradientDrawable.OVAL; setStroke(dp(3), Color.WHITE) }
        val dot = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(if (on) RED else 0xFF444444.toInt()) }
        return LayerDrawable(arrayOf(ring, dot)).apply { setLayerInset(1, dp(7), dp(7), dp(7), dp(7)) }
    }

    private fun buildUi() {
        val root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }
        root.setOnApplyWindowInsetsListener { v, insets ->
            val i = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
            v.setPadding(i.left, i.top, i.right, i.bottom)
            WindowInsets.CONSUMED
        }
        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(col, MATCH_PARENT, MATCH_PARENT)

        // top bar: quick toggles + settings drawer
        val top = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL; setPadding(dp(8), dp(4), dp(8), dp(4)) }
        torch = iconBtn(R.drawable.ic_flash, "Torch") { toggle("torch", supported = caps.optBoolean("torch")) }
        afBtn = icon("AF", 14f) { toggle("af", def = true, supported = caps.optBoolean("af")) }
        stab = icon("EIS", 14f) { toggle("stab", supported = caps.optBoolean("stab")) }
        rot = icon("180°", 14f) { toggle("rot180") }
        for (v in listOf(torch, afBtn, stab, rot)) {
            val cell = FrameLayout(this)
            cell.addView(v, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.CENTER))
            top.addView(cell, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        }
        top.addView(iconBtn(R.drawable.ic_settings, "Settings") { showDrawer(drawer.visibility != View.VISIBLE) })
        col.addView(top, MATCH_PARENT, WRAP_CONTENT)

        // viewfinder with the zoom bar floating on it
        vf = FrameLayout(this)
        surface = SurfaceView(this)
        surface.holder.addCallback(this)
        vf.addView(surface, FrameLayout.LayoutParams(1, 1, Gravity.CENTER))
        hint = TextView(this).apply { setTextColor(0xCCFFFFFF.toInt()); textSize = 15f; gravity = Gravity.CENTER }
        vf.addView(hint, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.CENTER))
        val zbar = FrameLayout(this)
        evBtn = icon("±", 20f) { openSlider(evSpec()) }
        mirror = iconBtn(R.drawable.ic_mirror, "Mirror") { toggle("mirror") }
        zoomRow = LinearLayout(this).apply {
            gravity = Gravity.CENTER
            background = pill(0x66000000)
            setPadding(dp(4), dp(2), dp(4), dp(2))
        }
        zbar.addView(evBtn, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.START or Gravity.CENTER_VERTICAL))
        zbar.addView(zoomRow, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.CENTER))
        zbar.addView(mirror, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.END or Gravity.CENTER_VERTICAL))
        vf.addView(zbar, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.BOTTOM).apply {
            setMargins(dp(8), 0, dp(8), dp(12))
        })
        connCard = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = round(0xF21C1C1E.toInt(), 20)
            setPadding(dp(20), dp(18), dp(20), dp(14))
            isClickable = true
            visibility = View.GONE
        }
        vf.addView(connCard, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.CENTER).apply {
            setMargins(dp(16), dp(16), dp(16), dp(72))
        })
        vf.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> vf.post { fitSurface() } }
        val scaler = ScaleGestureDetector(this, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
            override fun onScale(d: ScaleGestureDetector): Boolean {
                val z = (st.optDouble("zoom", 1.0) * d.scaleFactor)
                    .coerceIn(caps.optDouble("zoomMin", 1.0), caps.optDouble("zoomMax", 1.0))
                st.put("zoom", z)
                svc?.set("zoom", z)
                renderZoom()
                return true
            }
        })
        vf.setOnTouchListener { _, e ->
            scaler.onTouchEvent(e)
            if (e.action == MotionEvent.ACTION_UP) showDrawer(false)
            true
        }
        col.addView(vf, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))

        // slider for the active setting
        sliderBox = LinearLayout(this).apply {
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(16), dp(6), dp(8), dp(2))
            visibility = View.GONE
        }
        sliderLabel = TextView(this).apply { setTextColor(ACCENT); textSize = 13f; typeface = medium; minWidth = dp(76) }
        bar = SeekBar(this).apply {
            max = 1000
            setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
                override fun onProgressChanged(sb: SeekBar, p: Int, fromUser: Boolean) {
                    val sp = spec ?: return
                    if (!fromUser) return
                    val v = sp.cast(sp.fromP(p))
                    st.put(sp.key, v)
                    sliderLabel.text = sp.label((v as Number).toDouble())
                    svc?.set(sp.key, v)
                }
                override fun onStartTrackingTouch(sb: SeekBar) { dragging = true }
                override fun onStopTrackingTouch(sb: SeekBar) { dragging = false }
            })
        }
        sliderBox.addView(sliderLabel)
        sliderBox.addView(bar, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        sliderBox.addView(iconBtn(R.drawable.ic_close, "Close") { spec = null; sliderBox.visibility = View.GONE; render() })
        col.addView(sliderBox, MATCH_PARENT, WRAP_CONTENT)

        proRow = LinearLayout(this).apply { gravity = Gravity.CENTER; visibility = View.GONE }
        col.addView(proRow, MATCH_PARENT, WRAP_CONTENT)

        modeRow = LinearLayout(this).apply { gravity = Gravity.CENTER; setPadding(0, dp(10), 0, dp(6)) }
        for ((k, label) in listOf("pro" to "Pro", "video" to "Video", "portrait" to "Portrait")) {
            modeRow.addView(chip(label, 15f) { setMode(k) }.apply { tag = k; setPadding(dp(16), dp(6), dp(16), dp(6)) })
        }
        col.addView(modeRow, MATCH_PARENT, WRAP_CONTENT)

        // bottom: connection · LIVE (pause/resume the PC feed) · flip
        val bottom = FrameLayout(this).apply { setPadding(dp(28), dp(14), dp(28), dp(24)) }
        connText = TextView(this).apply { textSize = 12f; typeface = medium; setLineSpacing(0f, 1.2f) }
        liveBtn = TextView(this).apply {
            gravity = Gravity.CENTER
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.WHITE)
            setOnClickListener { toggle("live", def = true) }
        }
        val flip = iconBtn(R.drawable.ic_flip, "Switch camera") {
            if (need((caps.optJSONArray("facings")?.length() ?: 0) > 1))
                svc?.set("facing", if (st.optString("facing") == "front") "back" else "front")
        }.apply { background = pill(0x22FFFFFF); setPadding(dp(16), dp(16), dp(16), dp(16)) }
        bottom.addView(connText, FrameLayout.LayoutParams(WRAP_CONTENT, WRAP_CONTENT, Gravity.START or Gravity.CENTER_VERTICAL))
        bottom.addView(liveBtn, FrameLayout.LayoutParams(dp(78), dp(78), Gravity.CENTER))
        bottom.addView(flip, FrameLayout.LayoutParams(dp(58), dp(58), Gravity.END or Gravity.CENTER_VERTICAL))
        col.addView(bottom, MATCH_PARENT, WRAP_CONTENT)

        drawer = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            background = GradientDrawable().apply {
                setColor(0xFF161618.toInt())
                cornerRadii = FloatArray(8) { i -> if (i >= 4) dp(24).toFloat() else 0f }
            }
            setPadding(dp(20), dp(12), dp(20), dp(16))
            visibility = View.GONE
            isClickable = true
        }
        root.addView(drawer, FrameLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT, Gravity.TOP))
        setContentView(root)
        render()
    }

    /** Keep the preview at the stream's aspect (portrait-rotated), letterboxed inside the viewfinder. */
    private fun fitSurface() {
        val (w, h) = resWH()
        val aspect = h.toDouble() / w
        val vw = vf.width
        val vh = vf.height
        if (vw == 0 || vh == 0) return
        var sw = vw
        var sh = (vw / aspect).roundToInt()
        if (sh > vh) { sh = vh; sw = (vh * aspect).roundToInt() }
        val lp = surface.layoutParams as FrameLayout.LayoutParams
        if (lp.width != sw || lp.height != sh) { lp.width = sw; lp.height = sh; surface.layoutParams = lp }
    }

    // ---------------------------------------------------------------- render

    private fun resWH(): Pair<Int, Int> {
        val p = st.optString("res", "1920x1080").split('x')
        return (p.getOrNull(0)?.toIntOrNull() ?: 1920) to (p.getOrNull(1)?.toIntOrNull() ?: 1080)
    }

    private fun render() {
        val s = st
        val c = caps
        val mode = s.optString("mode", "video")
        torch.mark(s.optBoolean("torch"), c.optBoolean("torch"))
        val af = s.optBoolean("af", true)
        afBtn.text = if (af) "AF" else "MF"
        afBtn.mark(!af, c.optBoolean("af"))
        stab.mark(s.optBoolean("stab"), c.optBoolean("stab"))
        rot.mark(s.optBoolean("rot180"))
        mirror.mark(s.optBoolean("mirror"))
        evBtn.mark(s.optInt("ev") != 0 || spec?.key == "ev", mode != "pro")
        renderZoom()
        for (i in 0 until modeRow.childCount) {
            val v = modeRow.getChildAt(i) as TextView
            v.setTextColor(if (v.tag == mode) ACCENT else Color.WHITE)
            v.alpha = if (v.tag == "portrait" && !c.optBoolean("portrait")) 0.3f else 1f
        }
        renderPro(mode == "pro")

        val live = s.optBoolean("live", true)
        liveBtn.background = liveBg(live)
        liveBtn.text = if (live) "LIVE" else "PAUSED"
        val conn = s.optString("conn")
        connText.text = (when (conn) { "usb" -> "● USB"; "wifi" -> "● Wi-Fi"; else -> "○ No PC" }) +
            "\n${resWH().second}p${s.optInt("fps", 30)}"
        connText.setTextColor(if (conn.isEmpty()) DIM else GREEN)
        hint.text = when {
            mode == "portrait" -> "Portrait blur is on\nPreview shows on the PC"
            s.length() == 0 -> "Starting camera…"
            else -> ""
        }

        // Preview buffer follows the stream size, capped at 1080p (the guaranteed preview + record combo).
        val (w, h) = resWH()
        val pw = min(w, 1920)
        val ph = pw * h / w
        if ("${pw}x$ph" != fixedSize) {
            fixedSize = "${pw}x$ph"
            surface.holder.setFixedSize(pw, ph)
            vf.post { fitSurface() }
        }
        if (drawer.visibility == View.VISIBLE) buildDrawer()
        spec?.let { if (!dragging) syncSlider(it) }
        renderConnCard(conn.isEmpty() && s.length() > 0)
    }

    private fun label(t: String, size: Float, color: Int, bold: Boolean = false) = TextView(this).apply {
        text = t
        textSize = size
        setTextColor(color)
        if (bold) typeface = medium
    }

    private fun caption(t: String) = label(t.uppercase(), 11f, DIM).apply { letterSpacing = 0.1f }

    /** While no PC is connected: this phone's pairing code, and a search for openCam PCs on the Wi-Fi. */
    private fun renderConnCard(show: Boolean) {
        connCard.visibility = if (show) View.VISIBLE else View.GONE
        if (!show) return
        val s = st
        val key = listOf(s.optString("code"), s.optString("pcs"), s.optBoolean("searching")).toString()
        if (connCard.tag == key) return
        connCard.tag = key
        connCard.removeAllViews()
        connCard.addView(label("Connect to PC", 18f, Color.WHITE, bold = true))
        connCard.addView(label("openCam on a PC on this Wi-Fi connects by itself. For USB, turn on USB debugging and plug in.",
            13f, DIM).apply { setPadding(0, dp(4), 0, dp(14)); setLineSpacing(0f, 1.15f) })

        connCard.addView(caption("Pairing code"))
        val code = s.optString("code")
        connCard.addView(label(if (code.isEmpty()) "Not on Wi-Fi" else code.chunked(3).joinToString(" "), if (code.isEmpty()) 15f else 34f, Color.WHITE).apply {
            typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
            letterSpacing = 0.08f
        })
        if (code.isNotEmpty()) connCard.addView(label("Type it into openCam on the PC  ·  IP ${s.optString("ip")}", 12f, DIM).apply {
            setPadding(0, 0, 0, dp(14))
        })

        val head = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        head.addView(caption("PCs on this network"), LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        val searching = s.optBoolean("searching")
        head.addView(chip(if (searching) "Searching…" else "Search", 13f) { svc?.searchPcs() }.apply {
            setTextColor(ACCENT)
            isEnabled = !searching && code.isNotEmpty()
            alpha = if (isEnabled) 1f else 0.5f
        })
        connCard.addView(head, MATCH_PARENT, WRAP_CONTENT)

        val pcs = s.optJSONArray("pcs")
        if (pcs == null || pcs.length() == 0) {
            connCard.addView(label(if (searching) "Looking for PCs…" else "Tap Search to list PCs running openCam.", 13f, DIM).apply {
                setPadding(0, dp(4), 0, dp(4))
            })
        }
        for (i in 0 until (pcs?.length() ?: 0)) {
            val pc = pcs!!.getJSONObject(i)
            val name = pc.optString("name")
            val row = LinearLayout(this).apply {
                gravity = Gravity.CENTER_VERTICAL
                background = round(0x14FFFFFF, 12)
                setPadding(dp(12), dp(10), dp(14), dp(10))
                setOnClickListener {
                    svc?.callPc(pc.optString("ip"))
                    Toast.makeText(this@MainActivity, "Asking $name to connect…", Toast.LENGTH_SHORT).show()
                }
                ripple()
            }
            row.addView(ImageView(this).apply { setImageResource(R.drawable.ic_computer); imageTintList = ColorStateList.valueOf(Color.WHITE) },
                dp(22), dp(22))
            row.addView(LinearLayout(this).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(12), 0, 0, 0)
                addView(label(name, 15f, Color.WHITE, bold = true))
                addView(label(pc.optString("ip"), 12f, DIM))
            }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            row.addView(label("Connect", 14f, ACCENT, bold = true))
            connCard.addView(row, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT).apply { topMargin = dp(8) })
        }
    }

    private fun fmtZoom(z: Double) = if (abs(z - z.roundToInt()) < 0.05) "${z.roundToInt()}" else String.format("%.1f", z)

    private fun renderZoom() {
        val zmin = caps.optDouble("zoomMin", 1.0)
        val zmax = caps.optDouble("zoomMax", 1.0)
        val z = st.optDouble("zoom", 1.0)
        val presets = buildList {
            if (zmin < 0.95) add((zmin * 10).roundToInt() / 10.0)
            add(1.0)
            for (p in listOf(2.0, 5.0, 10.0)) if (p <= zmax + 1e-6) add(p)
        }
        if (zoomRow.tag != presets) {
            zoomRow.removeAllViews()
            for (p in presets) zoomRow.addView(chip(fmtZoom(p), 12f) { onZoomChip(p) }.apply { tag = p; minWidth = dp(42) })
            zoomRow.tag = presets
        }
        // Like the stock camera: the preset at or below the live zoom is highlighted and shows the exact value.
        val active = presets.lastOrNull { it <= z + 1e-3 } ?: presets.first()
        for (i in 0 until zoomRow.childCount) {
            val v = zoomRow.getChildAt(i) as TextView
            val p = v.tag as Double
            v.text = if (p == active) fmtZoom(z) + "x" else fmtZoom(p)
            v.mark(p == active)
        }
    }

    private fun onZoomChip(p: Double) {
        if (abs(st.optDouble("zoom", 1.0) - p) < 0.01) openSlider(zoomSpec())
        else { st.put("zoom", p); svc?.set("zoom", p); renderZoom() }
    }

    private fun renderPro(show: Boolean) {
        proRow.visibility = if (show) View.VISIBLE else View.GONE
        if (!show) return
        val s = st
        val c = caps
        val manual = c.optBoolean("manual")
        val aps = c.optJSONArray("apertures")
        proRow.removeAllViews()
        proRow.addView(chip("ISO ${s.optInt("iso")}") { if (need(manual)) openSlider(isoSpec()) }
            .apply { mark(spec?.key == "iso", manual) })
        proRow.addView(chip(shutterLabel(s.optLong("exp"))) { if (need(manual)) openSlider(expSpec()) }
            .apply { mark(spec?.key == "exp", manual) })
        val canFocus = c.optDouble("focusMax") > 0
        proRow.addView(chip(if (s.optBoolean("af", true)) "AF" else focusLabel(s.optDouble("focus"))) {
            if (need(canFocus)) { svc?.set("af", false); openSlider(focusSpec()) }
        }.apply { mark(spec?.key == "focus", canFocus) })
        proRow.addView(chip("WB ${s.optString("wb", "auto")}") { cycle("wb", c.optJSONArray("wb")?.let { a -> List(a.length()) { a.getString(it) } }.orEmpty()) }
            .apply { mark(s.optString("wb", "auto") != "auto") })
        val apList = if (aps == null) emptyList() else List(aps.length()) { aps.getDouble(it) }
        proRow.addView(chip("f/${s.optDouble("aperture", apList.firstOrNull() ?: 0.0).let { if (it > 0) String.format("%.1f", it) else "–" }}") {
            if (need(apList.size > 1)) cycle("aperture", apList)
        }.apply { mark(false, apList.size > 1) })
    }

    private fun shutterLabel(ns: Long) = if (ns <= 0) "S –" else if (ns >= 500_000_000L) String.format("%.1fs", ns / 1e9) else "1/${(1e9 / ns).roundToInt()}"

    private fun focusLabel(d: Double) = if (d < 0.05) "∞" else "${(100 / d).roundToInt()} cm"

    // ---------------------------------------------------------------- slider

    private fun zoomSpec() = Spec("zoom", caps.optDouble("zoomMin", 1.0), caps.optDouble("zoomMax", 1.0), log = true) { String.format("%.1fx", it) }

    private fun evSpec(): Spec {
        val step = caps.optDouble("evStep", 1.0)
        return Spec("ev", caps.optInt("evMin").toDouble(), caps.optInt("evMax").toDouble(), kind = 1) { String.format("%+.1f EV", it * step) }
    }

    private fun isoSpec() = Spec("iso", caps.optDouble("isoMin", 100.0), caps.optDouble("isoMax", 3200.0), log = true, kind = 1) { "ISO ${it.roundToInt()}" }

    private fun expSpec() = Spec("exp", caps.optDouble("expMin", 1e5), min(caps.optDouble("expMax", 1e8), 1e9 / st.optInt("fps", 30)), log = true, kind = 2) { shutterLabel(it.toLong()) }

    private fun focusSpec() = Spec("focus", 0.0, caps.optDouble("focusMax", 10.0)) { focusLabel(it) }

    private fun openSlider(s: Spec) {
        if (s.key == "ev" && st.optString("mode") == "pro") return
        spec = s
        sliderBox.visibility = View.VISIBLE
        syncSlider(s)
        render()
    }

    private fun syncSlider(s: Spec) {
        val v = st.optDouble(s.key, s.min)
        sliderLabel.text = s.label(v)
        bar.progress = s.toP(v)
    }

    // ---------------------------------------------------------------- actions

    private fun need(supported: Boolean): Boolean {
        if (!supported) Toast.makeText(this, "Not supported on this camera", Toast.LENGTH_SHORT).show()
        return supported
    }

    private fun toggle(k: String, def: Boolean = false, supported: Boolean = true) {
        if (!need(supported)) return
        val v = !st.optBoolean(k, def)
        st.put(k, v)
        svc?.set(k, v)
        render()
    }

    private fun <T : Any> cycle(k: String, values: List<T>) {
        if (values.isEmpty()) return
        val cur = st.opt(k)
        val i = values.indexOfFirst { it.toString() == cur?.toString() }
        val next = values[(i + 1) % values.size]
        st.put(k, next)
        svc?.set(k, next)
        render()
    }

    private fun setMode(m: String) {
        if (m == "portrait" && !need(caps.optBoolean("portrait"))) return
        if (m != "pro" && spec?.key in setOf("iso", "exp", "focus")) { spec = null; sliderBox.visibility = View.GONE }
        st.put("mode", m)
        svc?.set("mode", m)
        render()
    }

    private fun showDrawer(show: Boolean) {
        drawer.visibility = if (show) View.VISIBLE else View.GONE
        if (show) buildDrawer()
    }

    private fun buildDrawer() {
        drawer.removeAllViews()
        fun header(t: String) = drawer.addView(TextView(this).apply {
            text = t.uppercase()
            textSize = 11f
            letterSpacing = 0.1f
            setTextColor(DIM)
            setPadding(dp(4), dp(14), 0, dp(4))
        })
        fun row(items: List<Pair<String, Boolean>>, supported: (Int) -> Boolean = { true }, click: (Int) -> Unit) {
            val r = LinearLayout(this)
            items.forEachIndexed { i, (label, on) -> r.addView(chip(label) { click(i) }.apply { mark(on, supported(i)) }) }
            drawer.addView(r)
        }
        val modes = caps.optJSONArray("modes")?.let { a -> List(a.length()) { a.getString(it) } }.orEmpty()
        val resList = modes.map { it.substringBefore('@') }.distinct()
        val res = st.optString("res")
        val fps = st.optInt("fps", 30)

        header("Resolution")
        row(resList.map { "${it.substringAfter('x')}p" to (it == res) }) { svc?.set("res", resList[it]) }
        header("Frame rate")
        val fpsList = listOf(30, 60)
        row(fpsList.map { "$it fps" to (it == fps) }, supported = { "$res@${fpsList[it]}" in modes }) {
            if (need("$res@${fpsList[it]}" in modes)) svc?.set("fps", fpsList[it])
        }
        header("Bitrate (Mbps)")
        val rates = listOf(0, 8, 16, 32, 50)
        row(rates.map { (if (it == 0) "Auto" else "$it") to (it == st.optInt("mbps")) }) { svc?.set("mbps", rates[it]) }
        header("PC output")
        row(listOf("Mirror" to st.optBoolean("mirror"), "Rotate 180°" to st.optBoolean("rot180"))) {
            toggle(if (it == 0) "mirror" else "rot180")
        }
        header("Updates")
        val upd = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL }
        val ready = st.optString("updReady")
        upd.addView(label(st.optString("upd").ifEmpty { "Checks GitHub every 6 hours" } + "\nVersion ${packageManager.getPackageInfo(packageName, 0).versionName}", 13f, DIM).apply {
            setPadding(dp(4), 0, dp(8), 0)
        }, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
        upd.addView(if (ready.isEmpty()) chip("Check now", 14f) { svc?.checkUpdate() }.apply { background = pill(CHIP) }
        else chip("Install $ready", 14f) { installUpdate() }.apply { background = pill(ACCENT); setTextColor(Color.BLACK) })
        drawer.addView(upd, MATCH_PARENT, WRAP_CONTENT)

        val actions = LinearLayout(this).apply { gravity = Gravity.CENTER_VERTICAL; setPadding(0, dp(18), 0, 0) }
        actions.addView(chip("Quit openCam", 14f) { svc?.quit() ?: finishAndRemoveTask() }.apply { setTextColor(RED) })
        actions.addView(View(this), LinearLayout.LayoutParams(0, 1, 1f))
        actions.addView(chip("Done", 14f) { showDrawer(false) }.apply { background = pill(CHIP) })
        drawer.addView(actions, MATCH_PARENT, WRAP_CONTENT)
    }
}
