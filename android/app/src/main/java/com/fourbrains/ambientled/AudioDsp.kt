package com.fourbrains.ambientled

import kotlin.math.*

/**
 * Pure-Kotlin audio analysis — no Android classes, so it is unit-testable on a plain JVM.
 * Feed it 1102-sample PCM hops at 44.1 kHz; it returns the 24-byte WLED frame plus a
 * visualiser snapshot. Line-for-line port of the browser engine, including an exact
 * emulation of Web Audio's AnalyserNode so every tuned threshold carries over.
 */
class AudioDsp {
    data class Viz(val bands: IntArray, val beat: Boolean, val bpm: Int, val hz: Int,
                   val conf: Float, val music: Boolean)
    class Result(val frame: ByteArray, val viz: Viz?, val beat: Boolean, val bpm: Int, val majorHz: Int,
                 val volS: Int, val music: Boolean, val bands: IntArray)

    @Volatile var sensitivity = 128
    @Volatile var gate = true

    // ───────── constants ─────────
    val SR = 44_100
    private val N = 2048
    val HOP = 1102                    // 25 ms → 40 Hz
    private val BINS = N / 2
    private val EDGES = intArrayOf(43, 86, 129, 216, 301, 430, 560, 818, 1120, 1421, 1895, 2412, 3015, 3704, 4479, 7106, 9259)
    private val TILT = floatArrayOf(1.00f, 1.03f, 1.08f, 1.14f, 1.20f, 1.28f, 1.38f, 1.50f,
                                    1.62f, 1.76f, 1.92f, 2.08f, 2.24f, 2.40f, 2.55f, 2.70f)

    // ───────── FFT scratch ─────────
    private val ring = FloatArray(N)
    private val win = FloatArray(N) { n ->
        (0.42 - 0.5 * cos(2 * PI * n / N) + 0.08 * cos(4 * PI * n / N)).toFloat()   // Blackman
    }
    private val re = FloatArray(N)
    private val im = FloatArray(N)
    private val smooth = FloatArray(BINS)
    private val freq = IntArray(BINS)        // the "getByteFrequencyData" equivalent
    private val cosT = FloatArray(N / 2) { cos(-2 * PI * it / N).toFloat() }
    private val sinT = FloatArray(N / 2) { sin(-2 * PI * it / N).toFloat() }
    private val rev = IntArray(N).also { r ->
        val bits = 11
        for (i in 0 until N) { var x = i; var y = 0; repeat(bits) { y = (y shl 1) or (x and 1); x = x shr 1 }; r[i] = y }
    }

    // ───────── DSP state (mirrors the JS engine 1:1) ─────────
    private val ranges = Array(16) { i ->
        val nyq = SR / 2.0
        val lo = max(1, floor(EDGES[i] / nyq * BINS).toInt())
        val hi = max(2, ceil(EDGES[i + 1] / nyq * BINS).toInt())
        intArrayOf(lo, hi)
    }
    private var gPeak = 60f
    private var gFloor = 6f
    private val raw = FloatArray(16)
    private val bFlr = FloatArray(16) { 4f }
    private val bands = IntArray(16)
    private val disp = FloatArray(16)
    private val prevE = FloatArray(16)
    private var volS = 0f
    private var majorHz = 60
    private var magn = 0
    private val fluxHist = ArrayDeque<Float>()
    private val hist = ArrayDeque<Float>()
    private val iois = ArrayDeque<Double>()
    private var lastOnset = 0.0
    private var lastBeat = 0.0
    private var nextBeat = 0.0
    private var bpm = 0
    private var beatConf = 0f
    private var musicConf = 0f
    private var musicOn = false
    private var flat = 1f
    private var frameNo = 0


    /** one 25 ms hop of mono PCM16 → one frame */
    fun push(pcm: ShortArray, now: Double): Result {
        System.arraycopy(ring, HOP, ring, 0, N - HOP)
        for (i in 0 until HOP) ring[N - HOP + i] = pcm[i] / 32768f
        analyse()
        return process(now)
    }

    fun reset() {
        ring.fill(0f); smooth.fill(0f)
        gPeak = 60f; gFloor = 6f; bFlr.fill(4f); disp.fill(0f); prevE.fill(0f)
        fluxHist.clear(); hist.clear(); iois.clear()
        lastOnset = 0.0; lastBeat = 0.0; nextBeat = 0.0
        bpm = 0; beatConf = 0f; musicConf = 0f; musicOn = false; flat = 1f; volS = 0f; frameNo = 0
    }

    // ═════════════════════════════ Web-Audio-equivalent analyser ═════════════════════════════

    private fun analyse() {
        for (i in 0 until N) { val j = rev[i]; re[j] = ring[i] * win[i]; im[j] = 0f }
        var size = 2
        while (size <= N) {
            val half = size / 2; val step = N / size
            var start = 0
            while (start < N) {
                var k = 0
                for (j in start until start + half) {
                    val wr = cosT[k]; val wi = sinT[k]
                    val tr = wr * re[j + half] - wi * im[j + half]
                    val ti = wr * im[j + half] + wi * re[j + half]
                    re[j + half] = re[j] - tr; im[j + half] = im[j] - ti
                    re[j] += tr; im[j] += ti
                    k += step
                }
                start += size
            }
            size = size shl 1
        }
        val inv = 1f / N
        for (k in 0 until BINS) {
            val mag = sqrt(re[k] * re[k] + im[k] * im[k]) * inv
            smooth[k] = 0.35f * smooth[k] + 0.65f * mag
            val db = 20f * log10(smooth[k] + 1e-12f)
            freq[k] = (((db + 100f) / 70f) * 255f).roundToInt().coerceIn(0, 255)
        }
    }

    // ═════════════════════════════ the engine (port of the JS) ═════════════════════════════

    private fun majorPeak(): Int {
        val nyq = SR / 2.0
        val lo = max(1, floor(40 / nyq * BINS).toInt())
        val hi = min(BINS - 2, ceil(11000 / nyq * BINS).toInt())
        var bi = lo; var bv = 0
        for (k in lo until hi) if (freq[k] > bv) { bv = freq[k]; bi = k }
        if (bv < 12) { magn = 0; return majorHz }
        val a = freq[bi - 1].toDouble(); val b = freq[bi].toDouble(); val c = freq[bi + 1].toDouble()
        val den = 2 * (a - 2 * b + c)
        val d = if (den != 0.0) (a - c) / den else 0.0
        magn = min(255, bv)
        return ((bi + d.coerceIn(-1.0, 1.0)) * nyq / BINS).coerceIn(40.0, 11000.0).roundToInt()
    }

    private fun spectralFlatness(): Float {
        var lg = 0.0; var ln = 0.0; var n = 0
        var k = 2
        while (k < 200 && k < BINS) { val v = freq[k] / 255.0 + 1e-4; lg += ln(v); ln += v; n++; k += 2 }
        if (n == 0 || ln <= 0) return 1f
        return min(1.0, exp(lg / n) / (ln / n)).toFloat()
    }

    private fun estimateTempo() {
        if (iois.size < 4) { beatConf *= 0.97f; return }
        val BIN = 25.0; val LO = 280.0; val HI = 1000.0
        val bins = HashMap<Int, Int>()
        for (v in iois) {
            var iv = v
            while (iv < LO) iv *= 2; while (iv > HI) iv /= 2
            if (iv < LO || iv > HI) continue
            val b = (iv / BIN).roundToInt(); bins[b] = (bins[b] ?: 0) + 1
        }
        var best = 0.0; var bn = 0
        for ((b, c) in bins) if (c > bn) { bn = c; best = b * BIN }
        if (best == 0.0) { beatConf *= 0.97f; return }
        var near = 0
        for (v in iois) { var iv = v; while (iv < LO) iv *= 2; while (iv > HI) iv /= 2; if (abs(iv - best) < 80) near++ }
        beatConf = beatConf * 0.65f + (near.toFloat() / iois.size) * 0.35f
        bpm = (60000 / best).roundToInt()
    }

    private fun process(now: Double): Result {
        // 1. band energies + flux
        var peakRaw = 0f; var tot = 0f; var flux = 0f
        for (i in 0 until 16) {
            val (a, b) = ranges[i].let { it[0] to it[1] }
            var acc = 0f; var n = 0
            var k = a
            while (k < b && k < BINS) { acc += freq[k] * freq[k].toFloat(); n++; k++ }
            val e = if (n > 0) sqrt(acc / n) else 0f
            flux += max(0f, e - prevE[i]); prevE[i] = e
            raw[i] = e; tot += e
            val t = e * TILT[i]; if (t > peakRaw) peakRaw = t
        }

        // 2. shared AGC + per-band floor measured after tilt
        gPeak = if (peakRaw > gPeak) peakRaw * 0.6f + gPeak * 0.4f else gPeak * 0.975f + peakRaw * 0.025f
        gFloor = if (tot / 16 < gFloor) gFloor * 0.9f + (tot / 16) * 0.1f else gFloor * 0.9995f + (tot / 16) * 0.0005f
        if (gPeak < gFloor + 25) gPeak = gFloor + 25

        val sens = sensitivity / 255f
        var flrSum = 0f
        for (i in 0 until 16) {
            val tv = raw[i] * TILT[i]
            bFlr[i] = if (tv < bFlr[i]) bFlr[i] * 0.85f + tv * 0.15f else bFlr[i] * 0.998f + tv * 0.002f
            flrSum += bFlr[i]
        }
        val meanFlr = flrSum / 16
        val span = max(10f, (gPeak - meanFlr) * (1.30f - sens * 0.95f))
        var mx = 0; var sumB = 0
        for (i in 0 until 16) {
            val tv = raw[i] * TILT[i]
            val base = if (gate) bFlr[i] * 1.06f + 2 else 0f
            var v = ((tv - base) / span).coerceIn(0f, 1f)
            v = v.toDouble().pow(1.35).toFloat()
            if (v < 0.05f) v = 0f
            disp[i] = disp[i] * 0.35f + v * 0.65f
            bands[i] = if (disp[i] < 0.03f) 0 else (disp[i] * 255).roundToInt()
            if (bands[i] > mx) mx = bands[i]
            sumB += bands[i]
        }

        // 3. volume from the normalised bands + squelch
        val meanB = sumB / 16f
        var rawVol = min(255f, mx * 0.55f + meanB * 1.25f)
        var active = 0; for (i in 0 until 16) if (bands[i] > 25) active++
        if (rawVol < 18 && active < 2) { rawVol = 0f; bands.fill(0); mx = 0 }
        val volR = rawVol.roundToInt()
        volS = volS * 0.55f + rawVol * 0.45f

        // 4. dominant frequency
        majorHz = majorPeak()

        // 5. beat
        val bass = (raw[0] * 1.1f + raw[1]) / 2.1f
        hist.addLast(bass); if (hist.size > 40) hist.removeFirst()
        val bAvg = hist.sum() / hist.size
        fluxHist.addLast(flux); if (fluxHist.size > 60) fluxHist.removeFirst()
        val fMu = fluxHist.sum() / fluxHist.size
        var fv = 0f; for (f in fluxHist) fv += (f - fMu) * (f - fMu)
        val fSd = sqrt(fv / fluxHist.size)

        val kB = 1.42f - sens * 0.26f; val kF = 2.2f - sens * 0.8f
        val bassBacked = bass > bAvg * 1.12f
        var onset = false
        if ((bass > bAvg * kB && bass > meanFlr + 10) || (bassBacked && flux > fMu + kF * fSd && flux > fMu * 1.7f)) {
            if (now - lastOnset > 230) {
                onset = true
                if (lastOnset > 0) { val d = now - lastOnset; if (d > 200 && d < 2000) { iois.addLast(d); if (iois.size > 20) iois.removeFirst() } }
                lastOnset = now; estimateTempo()
            }
        }
        var beat = false
        if (onset && now - lastBeat > 250) {
            beat = true; lastBeat = now
            if (beatConf > 0.4f && bpm > 0) nextBeat = now + 60000.0 / bpm
        } else if (beatConf > 0.55f && bpm > 0 && nextBeat > 0 && now >= nextBeat && now - lastBeat > 250) {
            beat = true; lastBeat = now; nextBeat = now + 60000.0 / bpm          // carry through breakdowns
        }

        // 6. advisory music flag
        flat = flat * 0.92f + spectralFlatness() * 0.08f
        val tonal = ((1 - flat) * 1.5f).coerceIn(0f, 1f)
        // σ/μ of flux explodes when μ≈0, so room hiss used to read as "music".
        // Dynamics only count once there is real sound (not squelched) and real flux.
        val dyn = if (rawVol > 0f && fMu > 0.5f) ((fSd / fMu) * 1.1f).coerceIn(0f, 1f) else 0f
        musicConf = musicConf * 0.88f + (0.45f * beatConf + 0.30f * tonal + 0.25f * dyn) * 0.12f
        if (rawVol == 0f) musicConf *= 0.9f                     // silence drains confidence fast
        if (!musicOn && musicConf > 0.24f) musicOn = true
        if (musicOn && musicConf < 0.12f) musicOn = false

        // 7. frame
        val f = ByteArray(24)
        f[0] = 0xA3.toByte()
        for (i in 0 until 16) f[1 + i] = bands[i].toByte()
        f[17] = min(255, volS.roundToInt()).toByte()
        f[18] = volR.toByte()
        f[19] = ((if (beat) 1 else 0) or (if (onset) 2 else 0) or (if (musicOn) 4 else 0)).toByte()
        f[20] = magn.toByte()
        f[21] = ((majorHz shr 8) and 0xFF).toByte()
        f[22] = (majorHz and 0xFF).toByte()
        f[23] = min(255, (bpm / 2.0).roundToInt()).toByte()
        // visualiser at 20 Hz (beats are always pushed so the dot never misses one)
        frameNo++
        val viz = if (beat || frameNo % 2 == 0)
            Viz(bands.copyOf(), beat, if (beatConf > 0.3f) bpm else 0, majorHz, min(1f, musicConf * 1.6f), musicOn)
        else null
        return Result(f, viz, beat, bpm, majorHz, min(255, volS.roundToInt()), musicOn, bands.copyOf())
    }
}
