package com.fourbrains.ambientled

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Binder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.graphics.drawable.Icon

/**
 * Owns the Bluetooth link and the microphone so neither dies when the screen turns off,
 * the phone rotates, or you switch apps.
 *
 * Lifetime policy:
 *  - runs while the app is open
 *  - keeps running in the background while music sync is on (that's the point of it)
 *  - otherwise it holds the link for [GRACE_MS] after you leave the app — switching to a game or
 *    the camera and back is instant, no reconnect — then lets go. The board keeps playing on its own.
 */
class LedService : Service(), BleLink.Listener {

    interface Ui {
        fun onLink(s: BleLink.LinkState)
        fun onDevices(list: List<BleLink.Found>)
        fun onState(json: String)
        fun onViz(v: AudioEngine.Viz)
        fun onMic(on: Boolean, error: String?)
        fun onDevLine(line: String)
        fun onOta(phase: String, sent: Int, total: Int, message: String?)
    }

    inner class LocalBinder : Binder() { val service get() = this@LedService }

    companion object {
        const val CHANNEL = "ambient_link"
        const val NOTIF_ID = 42
        const val ACTION_STOP = "com.fourbrains.ambientled.STOP"
        const val ACTION_MIC_OFF = "com.fourbrains.ambientled.MIC_OFF"
        const val GRACE_MS = 10 * 60_000L
    }

    private val main = Handler(Looper.getMainLooper())
    lateinit var link: BleLink; private set
    lateinit var audio: AudioEngine; private set
    @Volatile var ui: Ui? = null
    private var lastLink: BleLink.LinkState? = null
    private var lastState: String? = null
    private var lastPresets: String? = null
    private var uiBound = false
    private val graceStop = Runnable {
        if (!uiBound && !audio.isRunning && !link.otaRunning()) stopSelfClean() else if (!uiBound) armGrace()
    }

    override fun onCreate() {
        super.onCreate()
        createChannel()
        link = BleLink(this, this)
        audio = AudioEngine(this,
            onFrame = { f -> link.sendAudio(f) },
            onViz = { v -> main.post { ui?.onViz(v) } })
        // if Android refuses the foreground slot we must not linger, or it kills the app
        if (!goForeground(withMic = false)) stopSelf()
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_STOP -> { stopMic(); link.disconnect(); stopSelfClean(); return START_NOT_STICKY }
            ACTION_MIC_OFF -> { stopMic(); if (!uiBound) armGrace() }
        }
        return START_NOT_STICKY
    }

    override fun onBind(intent: Intent?): IBinder { uiBound = true; main.removeCallbacks(graceStop); return LocalBinder() }

    override fun onUnbind(intent: Intent?): Boolean {
        uiBound = false
        ui = null
        if (!audio.isRunning) armGrace()
        return true
    }

    override fun onRebind(intent: Intent?) { uiBound = true; main.removeCallbacks(graceStop) }

    private fun armGrace() { main.removeCallbacks(graceStop); main.postDelayed(graceStop, GRACE_MS) }

    override fun onDestroy() {
        main.removeCallbacks(graceStop)
        audio.stop()
        link.shutdown()
        super.onDestroy()
    }

    // ═════════════════════════════ microphone ═════════════════════════════

    fun startMic(): Boolean {
        if (audio.isRunning) return true
        // the microphone FGS type has to be declared before recording on Android 14+
        if (!goForeground(withMic = true)) { ui?.onMic(false, "Android refused the microphone service"); return false }
        val ok = audio.start()
        if (!ok) { goForeground(withMic = false); ui?.onMic(false, "Microphone unavailable"); return false }
        ui?.onMic(true, null)
        updateNotification()
        return true
    }

    fun stopMic() {
        if (!audio.isRunning) return
        audio.stop()
        goForeground(withMic = false)
        main.post { ui?.onMic(false, null) }
        updateNotification()
        if (!uiBound) armGrace()
    }

    // ═════════════════════════════ BleLink.Listener → UI ═════════════════════════════

    override fun onLink(s: BleLink.LinkState) {
        if (s.phase != BleLink.Phase.READY) { lastState = null }    // never replay a stale picture after a drop
        lastLink = s
        main.post { ui?.onLink(s); updateNotification() }
    }
    override fun onDevices(list: List<BleLink.Found>) { main.post { ui?.onDevices(list) } }
    override fun onStateJson(json: String) {
        when {
            json.startsWith("{\"t\":\"pre\"") -> lastPresets = json
            json.startsWith("{\"t\":") -> {}                       // dev info etc.: not part of the picture
            else -> lastState = json
        }
        main.post { ui?.onState(json) }
    }
    override fun onDevLine(line: String) { main.post { ui?.onDevLine(line) } }
    override fun onOta(phase: String, sent: Int, total: Int, message: String?) {
        main.post { ui?.onOta(phase, sent, total, message); updateNotification() }
    }

    /** replays the latest known state to a freshly attached screen */
    fun replay(to: Ui) {
        to.onLink(lastLink ?: link.snapshot())
        lastState?.let { to.onState(it) }
        lastPresets?.let { to.onState(it) }
        if (link.isReady()) link.requestState()            // and ask the board, in case anything moved
        to.onMic(audio.isRunning, null)
    }

    // ═════════════════════════════ foreground plumbing ═════════════════════════════

    private fun goForeground(withMic: Boolean): Boolean {
        val type = if (Build.VERSION.SDK_INT >= 29) {
            var t = ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE
            if (withMic && Build.VERSION.SDK_INT >= 30) t = t or ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE
            t
        } else 0
        return try {
            if (Build.VERSION.SDK_INT >= 29) startForeground(NOTIF_ID, buildNotification(), type)
            else startForeground(NOTIF_ID, buildNotification())
            true
        } catch (e: Exception) { false }
    }

    private fun stopSelfClean() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
    }

    private fun createChannel() {
        val ch = NotificationChannel(CHANNEL, "Lumo link", NotificationManager.IMPORTANCE_LOW)
        ch.description = "Keeps the Bluetooth link and music sync running"
        ch.setShowBadge(false)
        getSystemService(NotificationManager::class.java).createNotificationChannel(ch)
    }

    private fun buildNotification(): Notification {
        val s = lastLink
        val status = when (s?.phase) {
            BleLink.Phase.READY -> "Connected · ${s.name ?: BleLink.DEFAULT_NAME}"
            BleLink.Phase.RECONNECTING -> "Reconnecting…"
            BleLink.Phase.CONNECTING, BleLink.Phase.DISCOVERING -> "Connecting…"
            BleLink.Phase.SCANNING -> "Searching for your lights…"
            BleLink.Phase.OFF -> "Bluetooth is off"
            BleLink.Phase.NO_LOCATION -> "Location is off"
            else -> "Not connected"
        }
        val text = when {
            ::link.isInitialized && link.otaRunning() -> "$status · updating firmware…"
            ::audio.isInitialized && audio.isRunning -> "$status · music sync on"
            else -> status
        }
        val flags = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
        val open = PendingIntent.getActivity(this, 0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP), flags)
        val stop = PendingIntent.getService(this, 1,
            Intent(this, LedService::class.java).setAction(ACTION_STOP), flags)
        val icon = Icon.createWithResource(this, R.drawable.ic_stat_ambient)
        val b = Notification.Builder(this, CHANNEL)
            .setSmallIcon(R.drawable.ic_stat_ambient)
            .setContentTitle("Lumo")
            .setContentText(text)
            .setContentIntent(open)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setColor(0xFFA08CFF.toInt())
            .setCategory(Notification.CATEGORY_SERVICE)
            .addAction(Notification.Action.Builder(icon, "Disconnect", stop).build())
        if (Build.VERSION.SDK_INT >= 31) b.setForegroundServiceBehavior(Notification.FOREGROUND_SERVICE_IMMEDIATE)
        if (::audio.isInitialized && audio.isRunning) {
            val micOff = PendingIntent.getService(this, 2,
                Intent(this, LedService::class.java).setAction(ACTION_MIC_OFF), flags)
            b.addAction(Notification.Action.Builder(icon, "Stop music sync", micOff).build())
        }
        return b.build()
    }

    private fun updateNotification() {
        try { getSystemService(NotificationManager::class.java).notify(NOTIF_ID, buildNotification()) } catch (_: Exception) {}
    }
}
