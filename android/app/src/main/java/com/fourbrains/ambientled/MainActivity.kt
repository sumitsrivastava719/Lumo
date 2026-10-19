package com.fourbrains.ambientled

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.bluetooth.BluetoothAdapter
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.IBinder
import android.provider.Settings
import android.view.HapticFeedbackConstants
import android.view.View
import android.view.WindowInsets
import android.view.WindowInsetsController
import android.view.ViewGroup
import android.webkit.JavascriptInterface
import android.webkit.RenderProcessGoneDetail
import android.window.OnBackInvokedDispatcher
import android.webkit.WebResourceRequest
import android.webkit.WebResourceResponse
import android.webkit.WebView
import android.webkit.WebViewClient
import org.json.JSONArray
import org.json.JSONObject
import java.io.IOException

/**
 * Hosts the controller UI and bridges it to the native Bluetooth + audio service.
 *
 * The UI ships inside the APK and is served from https://appassets.androidplatform.net —
 * fully offline. Plain platform APIs only: no third-party libraries in the app at all.
 */
class MainActivity : Activity(), LedService.Ui {

    companion object {
        private const val REQ_BT = 11
        private const val REQ_MIC = 12
        private const val REQ_BT_ON = 13
        private const val REQ_OTA = 14
        private const val OTA_SLOT = 0x140000          // 1.25 MB app partition on the C6 default table
        private const val HOST = "appassets.androidplatform.net"
        private val BG = Color.parseColor("#07080C")
    }

    private lateinit var web: WebView
    private lateinit var root: android.widget.FrameLayout
    private var service: LedService? = null
    private var bound = false
    private var pageReady = false
    private var pendingMic = false
    private var backCb: Any? = null
    private var otaImage: ByteArray? = null

    // ═════════════════════════════ permissions ═════════════════════════════

    private val btPerms: Array<String> get() = buildList {
        if (Build.VERSION.SDK_INT >= 31) { add(Manifest.permission.BLUETOOTH_SCAN); add(Manifest.permission.BLUETOOTH_CONNECT) }
        else add(Manifest.permission.ACCESS_FINE_LOCATION)
        if (Build.VERSION.SDK_INT >= 33) add(Manifest.permission.POST_NOTIFICATIONS)
    }.toTypedArray()

    private fun granted(p: String) = checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED
    private fun hasBt() = btPerms.filter { it != Manifest.permission.POST_NOTIFICATIONS }.all(::granted)

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        super.onRequestPermissionsResult(code, perms, results)
        when (code) {
            REQ_BT -> if (hasBt()) startAndBind() else if (results.isNotEmpty()) { markAsked(); push("link", JSONObject().put("phase", "NO_PERMISSION").put("known", false)) }
            REQ_MIC -> if (granted(Manifest.permission.RECORD_AUDIO)) {
                service?.startMic() ?: run { pendingMic = true }
            } else push("mic", JSONObject().put("on", false).put("error", "Microphone permission denied"))
        }
    }

    @Deprecated("platform API")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION") super.onActivityResult(requestCode, resultCode, data)
        if (requestCode == REQ_BT_ON) service?.link?.refreshPermissions()
        if (requestCode == REQ_OTA) {
            val uri = data?.data
            if (resultCode != RESULT_OK || uri == null) { pushOtaFile(null, 0, "No file chosen"); return }
            Thread { loadOtaImage(uri) }.start()
        }
    }

    // ═════════════════════════════ lifecycle ═════════════════════════════

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        window.decorView.setBackgroundColor(BG)
        root = android.widget.FrameLayout(this).apply { setBackgroundColor(BG) }
        setContentView(root)
        // edge-to-edge everywhere it exists (Android 15+ forces it anyway); content is
        // kept clear of the status bar, nav bar and camera cutout by padding [root]
        if (Build.VERSION.SDK_INT >= 30) {
            window.setDecorFitsSystemWindows(false)
            window.insetsController?.setSystemBarsAppearance(0,
                WindowInsetsController.APPEARANCE_LIGHT_STATUS_BARS or WindowInsetsController.APPEARANCE_LIGHT_NAVIGATION_BARS)
        }
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val b = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                v.setPadding(b.left, b.top, b.right, b.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop,
                             insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        buildWebView()
        root.requestApplyInsets()

        // Android 13+ predictive back (and Android 16, where onBackPressed is no longer called)
        if (Build.VERSION.SDK_INT >= 33) backCb = Back33.register(this) { handleBack() }

        if (hasBt()) startAndBind() else requestPermissions(btPerms, REQ_BT)
    }

    @SuppressLint("SetJavaScriptEnabled")
    private fun buildWebView() {
        val bg = BG
        pageReady = false
        web = WebView(this)
        web.setBackgroundColor(bg)
        web.overScrollMode = View.OVER_SCROLL_NEVER
        web.isVerticalScrollBarEnabled = false
        // A WebView ignores its own padding (content still draws from 0,0, i.e. under the
        // status bar), so the system-bar insets are applied to a plain container around it.
        root.removeAllViews()
        root.addView(web, android.widget.FrameLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        web.settings.apply {
            javaScriptEnabled = true
            domStorageEnabled = true
            setSupportZoom(false)
            builtInZoomControls = false
            displayZoomControls = false
            allowFileAccess = false
            allowContentAccess = false
            textZoom = 100
        }
        web.webViewClient = object : WebViewClient() {
            /** serves the bundled UI; nothing is ever fetched from the network */
            override fun shouldInterceptRequest(v: WebView, r: WebResourceRequest): WebResourceResponse? {
                val u = r.url
                if (u.host != HOST) return WebResourceResponse("text/plain", "utf-8", 403, "Blocked", emptyMap(), null)
                val path = u.path ?: return null
                if (!path.startsWith("/assets/")) return null
                val name = path.removePrefix("/assets/")
                val mime = when {
                    name.endsWith(".html") -> "text/html"
                    name.endsWith(".js") -> "application/javascript"
                    name.endsWith(".css") -> "text/css"
                    name.endsWith(".svg") -> "image/svg+xml"
                    name.endsWith(".png") -> "image/png"
                    name.endsWith(".json") -> "application/json"
                    else -> "application/octet-stream"
                }
                return try { WebResourceResponse(mime, "utf-8", assets.open(name)) }
                catch (_: IOException) { WebResourceResponse("text/plain", "utf-8", 404, "Not found", emptyMap(), null) }
            }
            override fun shouldOverrideUrlLoading(v: WebView, r: WebResourceRequest): Boolean = r.url.host != HOST
            override fun onPageFinished(v: WebView, url: String) {
                pageReady = true
                service?.replay(this@MainActivity)
            }
            /** the WebView renderer can be killed under memory pressure — rebuild instead of crashing */
            override fun onRenderProcessGone(v: WebView, d: RenderProcessGoneDetail): Boolean {
                if (v !== web) return true
                pageReady = false
                try { v.destroy() } catch (_: Exception) {}
                if (!isFinishing && !isDestroyed) buildWebView()
                return true
            }
        }
        web.addJavascriptInterface(Bridge(), "Native")
        web.loadUrl("https://$HOST/assets/index.html")
    }

    override fun onStart() {
        super.onStart()
        if (hasBt() && !bound) startAndBind()
    }

    override fun onResume() {
        super.onResume()
        service?.link?.refreshPermissions()       // the user may have changed things in Settings
    }

    /** back first closes any open sheet / returns to Home in the page; only then leaves */
    @Deprecated("platform API")
    override fun onBackPressed() { handleBack() }

    private fun handleBack() {
        if (!pageReady) { moveTaskToBack(true); return }
        web.evaluateJavascript("(window.onBack&&window.onBack())||false") { r ->
            // leave like the launcher does: the task (and the Bluetooth link) stays alive
            if (r != "true") moveTaskToBack(true)
        }
    }

    override fun onDestroy() {
        if (Build.VERSION.SDK_INT >= 33) backCb?.let { Back33.unregister(this, it) }
        service?.ui = null
        if (bound) try { unbindService(conn) } catch (_: Exception) {}
        bound = false
        service = null
        web.destroy()
        super.onDestroy()
    }

    // ═════════════════════════════ service binding ═════════════════════════════

    private val conn = object : ServiceConnection {
        override fun onServiceConnected(n: ComponentName, b: IBinder) {
            val s = (b as LedService.LocalBinder).service
            service = s
            s.ui = this@MainActivity
            if (pageReady) s.replay(this@MainActivity)
            if (pendingMic) { pendingMic = false; s.startMic() }
        }
        override fun onServiceDisconnected(n: ComponentName) { service = null; bound = false }
    }

    private fun startAndBind() {
        if (bound) return
        val i = Intent(this, LedService::class.java)
        try { startForegroundService(i) } catch (_: Exception) {}
        bound = bindService(i, conn, Context.BIND_AUTO_CREATE)
    }

    // ═════════════════════════════ native → page ═════════════════════════════

    private fun push(type: String, payload: Any) {
        if (!pageReady) return
        val js = "window.onNative&&window.onNative('$type',$payload)"
        runOnUiThread { if (!isDestroyed) web.evaluateJavascript(js, null) }
    }

    override fun onLink(s: BleLink.LinkState) {
        push("link", JSONObject()
            .put("phase", s.phase.name).put("name", s.name ?: JSONObject.NULL)
            .put("addr", s.address ?: JSONObject.NULL).put("mtu", s.mtu)
            .put("attempt", s.attempt).put("known", s.known))
    }

    override fun onDevices(list: List<BleLink.Found>) {
        val a = JSONArray()
        list.forEach { a.put(JSONObject().put("name", it.name).put("addr", it.address).put("rssi", it.rssi)) }
        push("devices", a)
    }

    override fun onState(json: String) {
        // only ever forward well-formed JSON objects into the page
        try { JSONObject(json); push("state", json) } catch (_: Exception) {}
    }

    override fun onViz(v: AudioEngine.Viz) {
        push("audio", JSONObject().put("bands", JSONArray(v.bands.toList())).put("beat", v.beat)
            .put("bpm", v.bpm).put("hz", v.hz).put("conf", v.conf.toDouble()).put("music", v.music))
    }

    override fun onDevLine(line: String) { push("dev", JSONObject().put("line", line)) }

    override fun onOta(phase: String, sent: Int, total: Int, message: String?) {
        push("ota", JSONObject().put("phase", phase).put("sent", sent).put("total", total)
            .put("msg", message ?: JSONObject.NULL))
    }

    // ═════════════════════════════ firmware image ═════════════════════════════

    private fun displayName(uri: Uri): String = try {
        contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)?.use {
            if (it.moveToFirst()) it.getString(0) else null
        } ?: "firmware.bin"
    } catch (_: Exception) { "firmware.bin" }

    /** read + validate: must be an ESP32-C6 *application* image that fits the OTA slot */
    private fun loadOtaImage(uri: Uri) {
        val name = displayName(uri)
        otaImage = null
        if (name.endsWith(".ino", true) || name.endsWith(".cpp", true) || name.endsWith(".h", true)) {
            pushOtaFile(name, 0, "That's source code. The phone can't compile it — in Arduino IDE use " +
                "Sketch → Export Compiled Binary and pick the LumoLED.ino.bin file."); return
        }
        val bytes = try {
            contentResolver.openInputStream(uri)?.use { ins ->
                val out = java.io.ByteArrayOutputStream()
                val buf = ByteArray(16384); var total = 0
                while (true) {
                    val n = ins.read(buf); if (n < 0) break
                    total += n; if (total > 4_400_000) break
                    out.write(buf, 0, n)
                }
                out.toByteArray()
            }
        } catch (_: Exception) { null }
        val err = when {
            bytes == null -> "Couldn't read the file"
            bytes.size > OTA_SLOT -> if (bytes.size >= 0x300000) "That's a merged image (bootloader + app). Use the plain " +
                    "LumoLED.ino.bin, not .merged.bin" else "Too big for the controller (max 1.25 MB)"
            bytes.size < 64 * 1024 -> "Too small to be firmware"
            (bytes[0].toInt() and 0xFF) != 0xE9 -> "Not an ESP32 firmware image"
            ((bytes[12].toInt() and 0xFF) or ((bytes[13].toInt() and 0xFF) shl 8)) != 0x000D ->
                "Built for a different chip — select board XIAO_ESP32C6 and export again"
            else -> null
        }
        if (err == null) otaImage = bytes
        pushOtaFile(name, bytes?.size ?: 0, err)
    }

    private fun pushOtaFile(name: String?, size: Int, err: String?) {
        push("otaFile", JSONObject().put("name", name ?: JSONObject.NULL).put("size", size)
            .put("ok", err == null).put("error", err ?: JSONObject.NULL))
    }

    override fun onMic(on: Boolean, error: String?) {
        push("mic", JSONObject().put("on", on).put("error", error ?: JSONObject.NULL))
    }

    // ═════════════════════════════ page → native ═════════════════════════════

    inner class Bridge {
        @JavascriptInterface fun send(cmd: String) { if (cmd.length in 1..60) service?.link?.sendText(cmd) }
        @JavascriptInterface fun scan() {
            runOnUiThread { if (!hasBt()) requestPermissions(btPerms, REQ_BT) else service?.link?.scan() }
        }
        @JavascriptInterface fun connect(addr: String) { service?.link?.connect(addr) }
        @JavascriptInterface fun disconnect() { service?.link?.disconnect() }
        @JavascriptInterface fun forget() { service?.link?.forget() }

        @JavascriptInterface fun micStart() {
            runOnUiThread {
                if (granted(Manifest.permission.RECORD_AUDIO)) service?.startMic() ?: run { pendingMic = true }
                else requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO), REQ_MIC)
            }
        }
        @JavascriptInterface fun micStop() { runOnUiThread { service?.stopMic() } }

        @JavascriptInterface fun audioCfg(sens: Int, gate: Boolean) {
            service?.audio?.let { it.sensitivity = sens.coerceIn(1, 255); it.gate = gate }
        }

        @JavascriptInterface fun haptic(strong: Boolean) {
            runOnUiThread {
                web.performHapticFeedback(
                    if (strong) HapticFeedbackConstants.LONG_PRESS else HapticFeedbackConstants.KEYBOARD_TAP)
            }
        }

        @SuppressLint("MissingPermission")
        @JavascriptInterface fun enableBluetooth() {
            runOnUiThread {
                try {
                    @Suppress("DEPRECATION")
                    startActivityForResult(Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_BT_ON)
                } catch (_: Exception) { startActivity(Intent(Settings.ACTION_BLUETOOTH_SETTINGS)) }
            }
        }

        @JavascriptInterface fun grantPermissions() {
            runOnUiThread {
                val blocked = btPerms.any { !granted(it) && !shouldShowRequestPermissionRationale(it) }
                if (blocked && hasAskedBefore()) {
                    // Android won't show the dialog again — send them to the app's settings page
                    startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:$packageName")))
                } else requestPermissions(btPerms, REQ_BT)
            }
        }

        @JavascriptInterface fun sdk(): Int = Build.VERSION.SDK_INT

        @JavascriptInterface fun otaPick() {
            runOnUiThread {
                val i = Intent(Intent.ACTION_OPEN_DOCUMENT).addCategory(Intent.CATEGORY_OPENABLE).setType("*/*")
                try { @Suppress("DEPRECATION") startActivityForResult(i, REQ_OTA) }
                catch (_: Exception) { pushOtaFile(null, 0, "No file picker on this phone") }
            }
        }
        @JavascriptInterface fun otaStart() {
            val img = otaImage ?: run { onOta("FAILED", 0, 0, "Choose a firmware file first"); return }
            service?.link?.startOta(img) ?: onOta("FAILED", 0, img.size, "Not connected")
        }
        @JavascriptInterface fun otaCancel() { service?.link?.cancelOta() }

        @JavascriptInterface fun openLocation() {
            runOnUiThread {
                try { startActivity(Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)) } catch (_: Exception) {}
            }
        }

        @JavascriptInterface fun version(): String = try {
            packageManager.getPackageInfo(packageName, 0).versionName ?: "2"
        } catch (_: Exception) { "2" }
    }

    private fun hasAskedBefore() = getSharedPreferences("app", MODE_PRIVATE).getBoolean("asked", false)
    private fun markAsked() = getSharedPreferences("app", MODE_PRIVATE).edit().putBoolean("asked", true).apply()
}

/** Android 13+ back API, kept in its own class so Android 8–12 never try to load it */
@android.annotation.TargetApi(33)
private object Back33 {
    fun register(a: Activity, onBack: () -> Unit): Any {
        val cb = android.window.OnBackInvokedCallback { onBack() }
        a.onBackInvokedDispatcher.registerOnBackInvokedCallback(OnBackInvokedDispatcher.PRIORITY_DEFAULT, cb)
        return cb
    }
    fun unregister(a: Activity, cb: Any) {
        try { a.onBackInvokedDispatcher.unregisterOnBackInvokedCallback(cb as android.window.OnBackInvokedCallback) } catch (_: Exception) {}
    }
}
