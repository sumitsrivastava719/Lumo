package com.fourbrains.ambientled

import android.annotation.SuppressLint
import android.content.Context
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.MediaRecorder
import android.os.Process
import android.os.SystemClock
import kotlin.math.max

/**
 * Microphone capture → [AudioDsp] → 24-byte WLED frame, 40 times a second.
 * Runs inside the foreground service, so music sync keeps going with the screen off.
 */
class AudioEngine(
    private val ctx: Context,
    private val onFrame: (ByteArray) -> Unit,
    private val onViz: (Viz) -> Unit
) {
    data class Viz(val bands: IntArray, val beat: Boolean, val bpm: Int, val hz: Int,
                   val conf: Float, val music: Boolean)

    private val dsp = AudioDsp()
    var sensitivity: Int get() = dsp.sensitivity; set(v) { dsp.sensitivity = v }
    var gate: Boolean get() = dsp.gate; set(v) { dsp.gate = v }
    @Volatile private var running = false
    private var worker: Thread? = null
    val isRunning get() = running

    @SuppressLint("MissingPermission")
    fun start(): Boolean {
        if (running) return true
        val sr = dsp.SR; val hop = dsp.HOP
        val minBuf = AudioRecord.getMinBufferSize(sr, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        if (minBuf <= 0) return false
        val bufBytes = max(minBuf, hop * 2 * 8)

        // UNPROCESSED = no AGC, no noise suppression, no echo cancel — what music needs.
        // VOICE_RECOGNITION is the next-least-processed source; MIC is the last resort.
        val am = ctx.getSystemService(Context.AUDIO_SERVICE) as AudioManager
        val unprocessed = am.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED) == "true"
        val sources = buildList {
            if (unprocessed) add(MediaRecorder.AudioSource.UNPROCESSED)
            add(MediaRecorder.AudioSource.VOICE_RECOGNITION)
            add(MediaRecorder.AudioSource.MIC)
        }
        var rec: AudioRecord? = null
        for (src in sources) {
            try {
                val r = AudioRecord(src, sr, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, bufBytes)
                if (r.state == AudioRecord.STATE_INITIALIZED) { rec = r; break } else r.release()
            } catch (_: Exception) {}
        }
        val record = rec ?: return false
        dsp.reset()
        running = true
        worker = Thread({ runLoop(record) }, "ambient-audio").apply { start() }
        return true
    }

    fun stop() {
        running = false
        worker?.join(600)
        worker = null
    }

    private fun runLoop(record: AudioRecord) {
        Process.setThreadPriority(Process.THREAD_PRIORITY_URGENT_AUDIO)
        val hop = dsp.HOP
        val pcm = ShortArray(hop)
        try {
            record.startRecording()
            while (running) {
                var got = 0
                while (got < hop && running) {
                    val n = record.read(pcm, got, hop - got)
                    if (n <= 0) { if (n < 0) running = false; break }
                    got += n
                }
                if (!running || got < hop) break
                val r = dsp.push(pcm, SystemClock.elapsedRealtime().toDouble())
                onFrame(r.frame)
                r.viz?.let { onViz(Viz(it.bands, it.beat, it.bpm, it.hz, it.conf, it.music)) }
            }
        } catch (_: Exception) {
        } finally {
            try { record.stop() } catch (_: Exception) {}
            record.release()
            running = false
        }
    }
}
