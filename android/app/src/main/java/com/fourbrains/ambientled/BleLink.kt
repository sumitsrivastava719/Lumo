package com.fourbrains.ambientled

import android.Manifest
import android.annotation.SuppressLint
import android.bluetooth.*
import android.bluetooth.le.*
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.location.LocationManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import java.util.UUID

/**
 * Everything Bluetooth lives here, on ONE dedicated thread.
 *
 * Android BLE breaks in a handful of well-known ways. Each one has an explicit answer:
 *
 *  1. Only one GATT operation may be in flight.            → serialized op queue + 400 ms watchdog
 *  2. Status 133 / stale GATT objects after a failure.     → always close() and back off before retrying
 *  3. Callbacks arrive on random binder threads.           → everything is posted onto [ble] thread
 *  4. "Connected" but the board stopped answering.         → end-to-end heartbeat ('?' every 5 s,
 *                                                             link declared dead after 15 s of silence)
 *  5. Stale service cache after firmware changes.          → gatt.refresh() when the service is missing
 *  6. Bluetooth toggled off/on, phone walks out of range.  → adapter receiver + endless backoff reconnect;
 *                                                             after 3 direct tries hand over to the OS
 *                                                             (autoConnect) which waits for the board
 *  7. Slider drags flooding the link.                       → per-key coalescing: only the newest B/S/I/C…
 *                                                             value is ever queued
 *  8. Audio must never delay a tap.                         → text commands always jump ahead of audio;
 *                                                             audio is "latest frame wins", never queued
 *  9. Android 8–11 scans return NOTHING with Location off.  → explicit NO_LOCATION phase, never a silent empty list
 * 10. More than 5 scan starts in 30 s are silently ignored. → scan starts are rate-limited here
 * 11. Phones stuck at the 23-byte MTU (old BT 4.0/4.1).     → 20-byte compact audio frame, so music still works
 * 12. Every (re)connect resyncs from the board.            → '?' (state) + 'L' (presets) the moment the link is up
 * 13. Firmware update over the same link.                   → windowed OTA: ≤ 8 KB in flight, board acks every 4 KB,
 *                                                             '~S' rewinds after a dropped chunk, CRC-32 end to end
 */
@SuppressLint("MissingPermission")
class BleLink(private val ctx: Context, private val listener: Listener) {

    interface Listener {
        fun onLink(s: LinkState)
        fun onDevices(list: List<Found>)
        fun onStateJson(json: String)
        /** a "~…" line from the board: dev replies (auth, pin, test…) — OTA lines are handled here */
        fun onDevLine(line: String)
        /** firmware-update progress: phase = SENDING / VERIFYING / DONE / FAILED */
        fun onOta(phase: String, sent: Int, total: Int, message: String?)
    }

    enum class Phase { OFF, NO_PERMISSION, NO_LOCATION, IDLE, SCANNING, CONNECTING, DISCOVERING, READY, RECONNECTING }

    data class LinkState(
        val phase: Phase, val name: String?, val address: String?,
        val mtu: Int, val attempt: Int, val known: Boolean
    )

    data class Found(val name: String, val address: String, val rssi: Int)

    companion object {
        val SVC: UUID = UUID.fromString("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
        val RX: UUID = UUID.fromString("6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
        val TX: UUID = UUID.fromString("6E400003-B5A3-F393-E0A9-E50E24DCCA9E")
        val CCCD: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val PREFS = "ble"
        private const val KEY_ADDR = "addr"
        private const val KEY_NAME = "name"
        private const val SCAN_MS = 12_000L
        private const val CONNECT_TIMEOUT_MS = 12_000L
        private const val HEARTBEAT_MS = 5_000L
        private const val DEAD_AFTER_MS = 15_000L
        private const val OP_WATCHDOG_MS = 400L
        /** command letters that are "slider-like": a newer one replaces an older queued one */
        private val COALESCE = setOf('B', 'S', 'I', 'G', 'C', 'D', 'E', 'K', 'M', 'A', 'R', 'X', 'U', 'Y', 'T', 'Q', 'N')
        /** per-wall commands: coalesce on letter + wall digit, so wall 1 never replaces wall 2 */
        private val COALESCE2 = setOf('g', 'H')
        private const val OTA_WINDOW = 8 * 1024
        private const val OTA_STALL_MS = 15_000L
        /** commands with no argument that only need to be in the queue once */
        private val ONCE = setOf("?", "L")
        const val DEFAULT_NAME = "LUMO"
    }

    private val thread = HandlerThread("ble").apply { start() }
    private val h = Handler(thread.looper)
    private val prefs = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val manager = ctx.getSystemService(Context.BLUETOOTH_SERVICE) as BluetoothManager
    private val adapter: BluetoothAdapter? get() = manager.adapter

    // ── link state (only touched on [h]) ──
    private var gatt: BluetoothGatt? = null
    private var rxChar: BluetoothGattCharacteristic? = null
    private var phase = Phase.IDLE
    private var target: BluetoothDevice? = null
    private var targetName: String? = null
    private var mtu = 23
    private var attempt = 0
    private var userClosed = false
    private var lastRxAt = 0L
    private var useAutoConnect = false
    private var cccdSent = false
    private val scanStarts = ArrayDeque<Long>()

    // ── op queue ──
    private val textQ = ArrayDeque<ByteArray>()
    private var pendingAudio: ByteArray? = null
    private var busy = false
    private var busySince = 0L
    private val notifyBuf = StringBuilder()

    // ── firmware update (only touched on [h]) ──
    private class OtaJob(val data: ByteArray, val crc: Long) {
        var phase = "BEGIN"          // BEGIN → SEND → END → DONE / FAILED
        var off = 0                  // next byte to send
        var acked = 0                // bytes the board has written to flash
        var lastMoveAt = SystemClock.elapsedRealtime()
        var lastReportAt = 0L
    }
    private var ota: OtaJob? = null

    // ── scanning ──
    private val found = LinkedHashMap<String, Found>()
    private var scanning = false
    private var lastDevicesPush = 0L
    private var autoPickAt = 0L

    init {
        // posted, so it runs after every property below has been initialised
        h.post {
            val f = IntentFilter(BluetoothAdapter.ACTION_STATE_CHANGED)
            if (Build.VERSION.SDK_INT >= 33) ctx.registerReceiver(adapterReceiver, f, Context.RECEIVER_NOT_EXPORTED)
            else ctx.registerReceiver(adapterReceiver, f)
            evaluateStartup()
        }
    }

    // ═════════════════════════════ public API (thread-safe) ═════════════════════════════

    fun scan() = h.post { startScan(userInitiated = true) }
    fun connect(address: String) = h.post { connectTo(address, fromUser = true) }
    fun disconnect() = h.post { userClosed = true; teardown("user"); setPhase(Phase.IDLE) }
    fun forget() = h.post {
        userClosed = true; teardown("forget")
        prefs.edit().remove(KEY_ADDR).remove(KEY_NAME).apply()
        target = null; targetName = null
        setPhase(Phase.IDLE)
    }
    fun refreshPermissions() = h.post { evaluateStartup() }
    fun requestState() = h.post { enqueueText("?") }

    /** A text command, e.g. "B80" or "CFF8800". */
    fun sendText(cmd: String) = h.post { enqueueText(cmd) }

    /** A 24-byte audio frame. Latest frame wins; never delays a command. */
    fun sendAudio(frame: ByteArray) = h.post {
        if (phase != Phase.READY || ota != null) return@post
        pendingAudio = if (mtu - 3 >= frame.size) frame else compact(frame)
        pump()
    }

    /** [0xA3][16 bands][volS][volR][flags][mag][pkHi][pkLo][bpm] → [0xA4][16 bands][volS][flags][pk/32] */
    private fun compact(f: ByteArray): ByteArray {
        if (f.size < 24) return f
        val out = ByteArray(20)
        out[0] = 0xA4.toByte()
        System.arraycopy(f, 1, out, 1, 16)
        out[17] = f[17]
        out[18] = f[19]
        val hz = ((f[21].toInt() and 0xFF) shl 8) or (f[22].toInt() and 0xFF)
        out[19] = (hz / 32).coerceIn(0, 255).toByte()
        return out
    }

    fun snapshot(): LinkState = LinkState(
        phase, targetName ?: prefs.getString(KEY_NAME, null),
        target?.address ?: prefs.getString(KEY_ADDR, null), mtu, attempt,
        prefs.getString(KEY_ADDR, null) != null
    )

    /** Android 8–11 deliver no scan results at all while system Location is off */
    private fun locationBlocksScan(): Boolean {
        if (Build.VERSION.SDK_INT >= 31) return false
        return try {
            val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager
            if (Build.VERSION.SDK_INT >= 28) !lm.isLocationEnabled
            else !(lm.isProviderEnabled(LocationManager.GPS_PROVIDER) || lm.isProviderEnabled(LocationManager.NETWORK_PROVIDER))
        } catch (_: Exception) { false }
    }

    fun isReady() = phase == Phase.READY
    fun otaRunning() = ota != null

    /** start a firmware update with a validated ESP32-C6 app image */
    fun startOta(image: ByteArray) = h.post {
        if (phase != Phase.READY) { listener.onOta("FAILED", 0, image.size, "Not connected"); return@post }
        if (ota != null) { listener.onOta("FAILED", 0, image.size, "An update is already running"); return@post }
        val crc = java.util.zip.CRC32().apply { update(image) }.value
        val job = OtaJob(image, crc)
        ota = job
        pendingAudio = null
        try { gatt?.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH) } catch (_: Exception) {}
        enqueueText("~B${image.size},${java.lang.Long.toHexString(crc)}")
        listener.onOta("SENDING", 0, image.size, "Preparing the controller…")
        h.postDelayed(otaWatch, 2_000)
    }
    fun cancelOta() = h.post { ota?.let { otaFail("Cancelled"); enqueueText("~A") } }

    private fun otaFail(msg: String) {
        val j = ota ?: return
        ota = null
        h.removeCallbacks(otaWatch)
        listener.onOta("FAILED", j.acked, j.data.size, msg)
    }

    /** stall detector: no progress for OTA_STALL_MS → give up (the board aborts on its own too) */
    private val otaWatch = object : Runnable {
        override fun run() {
            val j = ota ?: return
            if (SystemClock.elapsedRealtime() - j.lastMoveAt > OTA_STALL_MS) {
                otaFail("The controller stopped responding"); enqueueText("~A"); return
            }
            h.postDelayed(this, 1_000)
        }
    }

    private fun onTildeLine(line: String) {
        val j = ota
        val now = SystemClock.elapsedRealtime()
        when {
            j != null && line == "~R" -> { j.phase = "SEND"; j.lastMoveAt = now; pump() }
            j != null && line.startsWith("~K") -> {
                j.acked = line.substring(2).toIntOrNull() ?: j.acked
                j.lastMoveAt = now
                if (now - j.lastReportAt > 150 || j.acked == j.data.size) {
                    j.lastReportAt = now
                    listener.onOta("SENDING", j.acked, j.data.size, null)
                }
                if (j.acked >= j.data.size && j.phase == "SEND") {
                    j.phase = "END"; enqueueText("~E")
                    listener.onOta("VERIFYING", j.acked, j.data.size, "Checking the new firmware…")
                }
                pump()
            }
            j != null && line.startsWith("~S") -> {           // board missed a chunk: resend from there
                val at = line.substring(2).toIntOrNull()
                if (at != null && at in 0..j.data.size) { j.off = at; j.lastMoveAt = now }
                pump()
            }
            j != null && line == "~OK" -> {
                ota = null; h.removeCallbacks(otaWatch)
                listener.onOta("DONE", j.data.size, j.data.size, "Installed — the controller is restarting")
            }
            j != null && line.startsWith("~ER") -> otaFail(line.substring(3).trim().ifEmpty { "Rejected by the controller" })
            j != null && j.phase == "BEGIN" && line == "~auth required" -> {
                otaFail("Developer PIN expired — unlock again"); listener.onDevLine(line)
            }
            else -> listener.onDevLine(line)
        }
    }

    fun shutdown() {
        h.post {
            userClosed = true
            stopScan()
            teardown("shutdown")
            try { ctx.unregisterReceiver(adapterReceiver) } catch (_: Exception) {}
            thread.quitSafely()
        }
    }

    // ═════════════════════════════ startup / permissions ═════════════════════════════

    private fun hasPermissions(): Boolean {
        fun ok(p: String) = ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED
        return if (Build.VERSION.SDK_INT >= 31)
            ok(Manifest.permission.BLUETOOTH_SCAN) && ok(Manifest.permission.BLUETOOTH_CONNECT)
        else ok(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private fun evaluateStartup() {
        if (!hasPermissions()) { setPhase(Phase.NO_PERMISSION); return }
        if (adapter?.isEnabled != true) { setPhase(Phase.OFF); return }
        if (gatt != null && phase == Phase.READY) { publish(); return }
        // a connect already in flight: don't tear it down just because the screen came back
        if (gatt != null && (phase == Phase.CONNECTING || phase == Phase.DISCOVERING || phase == Phase.RECONNECTING)) { publish(); return }
        if (phase == Phase.SCANNING && scanning) { publish(); return }
        val addr = prefs.getString(KEY_ADDR, null)
        if (addr != null) {
            userClosed = false
            connectTo(addr, fromUser = false)          // known board: straight connect, no scan
        } else {
            startScan(userInitiated = false)           // first run: find it
        }
    }

    private val adapterReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            if (i.action != BluetoothAdapter.ACTION_STATE_CHANGED) return
            when (i.getIntExtra(BluetoothAdapter.EXTRA_STATE, -1)) {
                BluetoothAdapter.STATE_OFF -> h.post {
                    stopScan(); teardown("bt off"); setPhase(Phase.OFF)
                }
                BluetoothAdapter.STATE_ON -> h.postDelayed({
                    userClosed = false; attempt = 0; evaluateStartup()
                }, 600)
            }
        }
    }

    // ═════════════════════════════ scanning ═════════════════════════════

    private val scanCb = object : ScanCallback() {
        override fun onScanResult(callbackType: Int, r: ScanResult) { h.post { onFound(r) } }
        override fun onBatchScanResults(results: MutableList<ScanResult>) { h.post { results.forEach { onFound(it) } } }
        override fun onScanFailed(errorCode: Int) { h.post { scanning = false; if (phase == Phase.SCANNING) setPhase(Phase.IDLE) } }
    }

    private fun startScan(userInitiated: Boolean) {
        if (!hasPermissions()) { setPhase(Phase.NO_PERMISSION); return }
        val a = adapter
        if (a?.isEnabled != true) { setPhase(Phase.OFF); return }
        if (phase == Phase.READY && !userInitiated) return
        if (locationBlocksScan()) { setPhase(Phase.NO_LOCATION); return }
        // Android silently ignores the 6th scan start inside 30 s — space them out instead
        val now0 = SystemClock.elapsedRealtime()
        while (scanStarts.isNotEmpty() && now0 - scanStarts.first() > 30_000) scanStarts.removeFirst()
        if (scanStarts.size >= 4) {
            val wait = 30_000 - (now0 - scanStarts.first()) + 200
            h.removeCallbacks(deferredScan); h.postDelayed(deferredScan, wait)
            if (phase != Phase.READY) setPhase(Phase.SCANNING)
            return
        }
        scanStarts.addLast(now0)
        stopScan()
        found.clear(); listener.onDevices(emptyList())
        val scanner = a.bluetoothLeScanner ?: run { setPhase(Phase.OFF); return }
        // No hardware filter on purpose: some phones drop 128-bit UUID filters when the name
        // sits in the scan response. We match in software on UUID OR name instead.
        val settings = ScanSettings.Builder()
            .setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
            .setReportDelay(0)
            .build()
        try {
            scanner.startScan(null, settings, scanCb)
            scanning = true
            autoPickAt = SystemClock.elapsedRealtime() + 2_500
            if (phase != Phase.READY) setPhase(Phase.SCANNING)
            h.postDelayed(scanTimeout, SCAN_MS)
        } catch (e: SecurityException) { setPhase(Phase.NO_PERMISSION) }
    }

    private val deferredScan = Runnable { if (phase == Phase.SCANNING || phase == Phase.IDLE) startScan(userInitiated = true) }

    private val scanTimeout = Runnable {
        stopScan()
        if (phase == Phase.SCANNING) {
            // nothing chosen: if exactly one board showed up, just take it
            if (found.size == 1) connectTo(found.values.first().address, fromUser = false)
            else setPhase(Phase.IDLE)
        }
    }

    private fun stopScan() {
        h.removeCallbacks(scanTimeout)
        if (!scanning) return
        scanning = false
        try { adapter?.bluetoothLeScanner?.stopScan(scanCb) } catch (_: Exception) {}
    }

    private fun onFound(r: ScanResult) {
        if (!scanning) return
        val rec = r.scanRecord
        val name = rec?.deviceName ?: try { r.device.name } catch (_: SecurityException) { null }
        val hasSvc = rec?.serviceUuids?.any { it.uuid == SVC } == true
        val looksRight = hasSvc || (name?.startsWith("Ambient", ignoreCase = true) == true) ||
                         (name?.startsWith("LUMO", ignoreCase = true) == true)
        if (!looksRight) return

        val addr = r.device.address
        found[addr] = Found(name ?: DEFAULT_NAME, addr, r.rssi)

        val known = prefs.getString(KEY_ADDR, null)
        if (known != null && known == addr && phase != Phase.READY) {   // our board is back
            stopScan(); connectTo(addr, fromUser = false); return
        }
        val now = SystemClock.elapsedRealtime()
        if (now - lastDevicesPush > 350) { lastDevicesPush = now; listener.onDevices(found.values.sortedByDescending { it.rssi }) }

        // first-run convenience: one board in range for 2.5 s → connect without a tap
        if (known == null && found.size == 1 && now > autoPickAt && phase == Phase.SCANNING) {
            stopScan(); connectTo(addr, fromUser = false)
        }
    }

    // ═════════════════════════════ connect / discover / ready ═════════════════════════════

    private fun connectTo(address: String, fromUser: Boolean) {
        if (!hasPermissions()) { setPhase(Phase.NO_PERMISSION); return }
        val a = adapter ?: return
        if (!a.isEnabled) { setPhase(Phase.OFF); return }
        if (!BluetoothAdapter.checkBluetoothAddress(address)) return
        stopScan()
        if (fromUser) { attempt = 0; useAutoConnect = false }
        userClosed = false
        teardown("reconnect")
        val dev = a.getRemoteDevice(address)
        target = dev
        targetName = found[address]?.name ?: prefs.getString(KEY_NAME, null) ?: DEFAULT_NAME
        setPhase(if (attempt > 0) Phase.RECONNECTING else Phase.CONNECTING)
        try {
            gatt = if (Build.VERSION.SDK_INT >= 26)
                dev.connectGatt(ctx, useAutoConnect, gattCb, BluetoothDevice.TRANSPORT_LE)
            else dev.connectGatt(ctx, useAutoConnect, gattCb)
        } catch (e: SecurityException) { setPhase(Phase.NO_PERMISSION); return }
        // autoConnect waits indefinitely by design; direct connects get a deadline
        if (!useAutoConnect) h.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
    }

    private val connectTimeout = Runnable {
        if (phase == Phase.CONNECTING || phase == Phase.RECONNECTING || phase == Phase.DISCOVERING) {
            teardown("connect timeout"); scheduleReconnect()
        }
    }

    private val gattCb = object : BluetoothGattCallback() {

        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            h.post {
                if (g != gatt) { try { g.close() } catch (_: Exception) {}; return@post }   // stale object
                if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
                    h.removeCallbacks(connectTimeout)
                    setPhase(Phase.DISCOVERING)
                    // several Samsung / Pixel builds drop discovery if it starts too early
                    h.postDelayed({ try { gatt?.discoverServices() } catch (_: SecurityException) {} }, 400)
                    h.postDelayed(connectTimeout, CONNECT_TIMEOUT_MS)
                } else {
                    // includes the infamous status 133
                    teardown("state $newState status $status")
                    if (!userClosed) scheduleReconnect() else setPhase(Phase.IDLE)
                }
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            h.post {
                if (g != gatt) return@post
                val svc = g.getService(SVC)
                val rx = svc?.getCharacteristic(RX)
                val tx = svc?.getCharacteristic(TX)
                if (status != BluetoothGatt.GATT_SUCCESS || rx == null || tx == null) {
                    refreshCache(g)                    // stale cache after a firmware change
                    teardown("no service ($status)"); scheduleReconnect(); return@post
                }
                rxChar = rx
                try {
                    if (!g.requestMtu(247)) enableNotifications(g, tx)
                    else h.postDelayed({ if (phase == Phase.DISCOVERING) enableNotifications(g, tx) }, 1_500)
                } catch (_: SecurityException) { setPhase(Phase.NO_PERMISSION) }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            h.post {
                if (g != gatt) return@post
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                try { g.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH) } catch (_: Exception) {}
                g.getService(SVC)?.getCharacteristic(TX)?.let { enableNotifications(g, it) }
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            h.post {
                if (g != gatt || d.uuid != CCCD) return@post
                if (status != BluetoothGatt.GATT_SUCCESS) { teardown("cccd $status"); scheduleReconnect(); return@post }
                onReady()
            }
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            h.post { if (g == gatt) { busy = false; pump() } }
        }

        @Deprecated("pre-33 path")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            if (Build.VERSION.SDK_INT >= 33) return
            @Suppress("DEPRECATION") val v = c.value ?: return
            val copy = v.copyOf()
            h.post { if (g == gatt) onNotify(copy) }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            val copy = value.copyOf()
            h.post { if (g == gatt) onNotify(copy) }
        }
    }

    private fun enableNotifications(g: BluetoothGatt, tx: BluetoothGattCharacteristic) {
        if (phase != Phase.DISCOVERING || cccdSent) return      // MTU callback + fallback timer can both land here
        cccdSent = true
        try {
            g.setCharacteristicNotification(tx, true)
            val d = tx.getDescriptor(CCCD) ?: run { onReady(); return }
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE)
            } else {
                @Suppress("DEPRECATION")
                d.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                @Suppress("DEPRECATION") g.writeDescriptor(d)
            }
        } catch (_: SecurityException) { setPhase(Phase.NO_PERMISSION) }
    }

    private fun onReady() {
        if (phase == Phase.READY) return
        h.removeCallbacks(connectTimeout)
        attempt = 0
        useAutoConnect = false
        lastRxAt = SystemClock.elapsedRealtime()
        busy = false
        target?.let { prefs.edit().putString(KEY_ADDR, it.address).putString(KEY_NAME, targetName).apply() }
        setPhase(Phase.READY)
        enqueueText("?")                                // fresh state from the board, every time
        enqueueText("L")                                // and its presets
        h.removeCallbacks(heartbeat)
        h.postDelayed(heartbeat, HEARTBEAT_MS)
    }

    /** end-to-end liveness: the board must answer, not just the radio */
    private val heartbeat = object : Runnable {
        override fun run() {
            if (phase != Phase.READY) return
            if (SystemClock.elapsedRealtime() - lastRxAt > DEAD_AFTER_MS) {
                teardown("heartbeat lost"); scheduleReconnect(); return
            }
            if (ota == null) enqueueText("?")             // the board is busy flashing; its acks keep us alive
            h.postDelayed(this, HEARTBEAT_MS)
        }
    }

    private fun onNotify(bytes: ByteArray) {
        lastRxAt = SystemClock.elapsedRealtime()
        for (b in bytes) {
            val ch = (b.toInt() and 0xFF).toChar()
            if (ch == '\n') {
                val line = notifyBuf.toString().trim()
                notifyBuf.setLength(0)
                if (line.startsWith("{") && line.endsWith("}")) listener.onStateJson(line)
                else if (line.startsWith("~")) onTildeLine(line)
            } else if (notifyBuf.length < 2048) notifyBuf.append(ch)
            else notifyBuf.setLength(0)                 // garbage guard
        }
    }

    // ═════════════════════════════ write queue ═════════════════════════════

    private fun enqueueText(cmd: String) {
        if (phase != Phase.READY) return               // offline: UI resyncs from the board on reconnect
        val bytes = cmd.toByteArray(Charsets.US_ASCII)
        if (bytes.isEmpty()) return
        val key = cmd[0]
        if (key in COALESCE) {
            val it = textQ.iterator()
            while (it.hasNext()) { val q = it.next(); if (q.isNotEmpty() && q[0].toInt().toChar() == key) it.remove() }
        } else if (key in COALESCE2 && cmd.length >= 2) {
            val it = textQ.iterator()
            while (it.hasNext()) {
                val q = it.next()
                if (q.size >= 2 && q[0].toInt().toChar() == key && q[1].toInt().toChar() == cmd[1]) it.remove()
            }
        }
        if (cmd in ONCE && textQ.any { it.size == cmd.length && String(it, Charsets.US_ASCII) == cmd }) return
        if (textQ.size >= 64) textQ.removeFirst()
        textQ.addLast(bytes)
        pump()
    }

    private val opWatchdog = Runnable { if (busy) { busy = false; pump() } }

    private fun pump() {
        if (phase != Phase.READY) return
        val now = SystemClock.elapsedRealtime()
        if (busy && now - busySince < OP_WATCHDOG_MS) return
        val g = gatt ?: return
        val c = rxChar ?: return

        val text = textQ.removeFirstOrNull()
        val payload: ByteArray
        val type: Int
        var otaLen = 0
        val j = ota
        if (text != null) {
            payload = text; type = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT      // acknowledged
        } else if (j != null && j.phase == "SEND" && j.off < j.data.size && j.off - j.acked < OTA_WINDOW) {
            // [0xB7][offset 3 bytes LE][payload] — sized to the negotiated MTU
            otaLen = minOf(mtu - 3 - 4, j.data.size - j.off).coerceAtLeast(1)
            payload = ByteArray(4 + otaLen)
            payload[0] = 0xB7.toByte()
            payload[1] = (j.off and 0xFF).toByte()
            payload[2] = ((j.off shr 8) and 0xFF).toByte()
            payload[3] = ((j.off shr 16) and 0xFF).toByte()
            System.arraycopy(j.data, j.off, payload, 4, otaLen)
            type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE
        } else {
            payload = pendingAudio ?: return
            pendingAudio = null
            type = BluetoothGattCharacteristic.WRITE_TYPE_NO_RESPONSE                 // fire-and-forget
        }
        val ok = try {
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(c, payload, type) == BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                run { c.writeType = type; c.value = payload; g.writeCharacteristic(c) }
            }
        } catch (_: SecurityException) { false }

        if (ok) {
            busy = true; busySince = now
            if (otaLen > 0 && j != null) j.off += otaLen
            h.removeCallbacks(opWatchdog); h.postDelayed(opWatchdog, OP_WATCHDOG_MS)
        } else {
            // controller busy: put a command back at the front and try again shortly
            if (text != null) textQ.addFirst(text)
            h.postDelayed({ pump() }, 30)
        }
    }

    // ═════════════════════════════ teardown / backoff ═════════════════════════════

    private fun teardown(why: String) {
        ota?.let { j ->
            if (j.phase == "END") {
                // everything was sent; the board may already be rebooting into the new image
                ota = null; h.removeCallbacks(otaWatch)
                listener.onOta("CHECK", j.data.size, j.data.size,
                    "Link dropped while the controller was installing — reconnect and check the firmware version")
            } else otaFail("Bluetooth link lost ($why)")
        }
        h.removeCallbacks(connectTimeout)
        h.removeCallbacks(heartbeat)
        h.removeCallbacks(opWatchdog)
        busy = false
        textQ.clear(); pendingAudio = null
        notifyBuf.setLength(0)
        rxChar = null
        cccdSent = false
        mtu = 23
        gatt?.let { g ->
            try { g.disconnect() } catch (_: Exception) {}
            try { g.close() } catch (_: Exception) {}
        }
        gatt = null
    }

    private fun scheduleReconnect() {
        if (userClosed) { setPhase(Phase.IDLE); return }
        val addr = target?.address ?: prefs.getString(KEY_ADDR, null)
        if (addr == null) { setPhase(Phase.IDLE); return }
        if (adapter?.isEnabled != true) { setPhase(Phase.OFF); return }
        attempt++
        // after three quick direct attempts, let the OS wait for the board (autoConnect)
        useAutoConnect = attempt > 3
        val delay = if (useAutoConnect) 1_000L
                    else minOf(8_000L, 500L shl (attempt - 1).coerceAtMost(4))
        setPhase(Phase.RECONNECTING)
        h.postDelayed({ if (!userClosed && phase != Phase.READY) connectTo(addr, fromUser = false) }, delay)
    }

    private fun refreshCache(g: BluetoothGatt) {
        try { g.javaClass.getMethod("refresh").invoke(g) } catch (_: Exception) {}
    }

    // ═════════════════════════════ publishing ═════════════════════════════

    private fun setPhase(p: Phase) { phase = p; publish() }
    private fun publish() { listener.onLink(snapshot()) }
}
