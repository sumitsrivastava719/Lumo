import com.fourbrains.ambientled.AudioDsp
import kotlin.math.*
import java.util.Random

const val SR = 44100
const val HOP = 1102

/** synthesises PCM and pushes it through the shipping DSP, hop by hop */
fun run(seconds: Double, sens: Int, gen: (Double) -> Double): Map<String, Any> {
    val d = AudioDsp(); d.sensitivity = sens; d.reset()
    val hops = (seconds * SR / HOP).toInt()
    var t = 0; var beats = 0; var lastBpm = 0; var lastHz = 0; var lastVol = 0; var music = false
    var lastBands = IntArray(16); var maxVol = 0
    for (h in 0 until hops) {
        val pcm = ShortArray(HOP) { i ->
            val s = gen((t + i).toDouble() / SR)
            (s.coerceIn(-1.0, 1.0) * 32767).roundToInt().toShort()
        }
        t += HOP
        val now = t * 1000.0 / SR
        val r = d.push(pcm, now)
        if (r.beat) beats++
        lastBpm = r.bpm; lastHz = r.majorHz; lastVol = r.volS; music = r.music; lastBands = r.bands
        if (h > hops / 2) maxVol = max(maxVol, r.volS)
    }
    return mapOf("beats" to beats, "bpm" to lastBpm, "hz" to lastHz, "vol" to lastVol,
                 "maxVol" to maxVol, "music" to music, "bands" to lastBands.joinToString(","))
}

fun main() {
    val rnd = Random(7)
    val beatPeriod = 60.0 / 128.0
    // 128 BPM: decaying 55 Hz kick + 30 ms hat bursts on the off-beat + quiet 440 Hz lead
    val track: (Double) -> Double = { s ->
        val ph = s % beatPeriod
        val kick = 0.8 * exp(-ph / 0.07) * sin(2 * PI * 55 * s)
        val hp = s % (beatPeriod / 2)
        val hat = if (hp < 0.03) 0.18 * (rnd.nextDouble() * 2 - 1) else 0.0
        val lead = 0.12 * sin(2 * PI * 440 * s)
        kick + hat + lead
    }
    var ok = true
    fun check(name: String, cond: Boolean, info: String) {
        println((if (cond) "  PASS  " else "  FAIL  ") + name.padEnd(42) + info); ok = ok && cond
    }

    println("── 128 BPM track, 10 s (expect ~21 beats) ──")
    for (sens in listOf(40, 128, 230)) {
        val r = run(10.0, sens, track)
        val b = r["beats"] as Int
        check("sens $sens: beat count", b in 18..23, "beats=$b bpm=${r["bpm"]} vol=${r["maxVol"]} music=${r["music"]}")
        check("sens $sens: tempo locked near 128", (r["bpm"] as Int) in 120..136, "bpm=${r["bpm"]}")
    }

    println("── pure 440 Hz tone (major-peak accuracy) ──")
    val tone = run(3.0, 128) { s -> 0.3 * sin(2 * PI * 440 * s) }
    check("major peak within one bin of 440", abs((tone["hz"] as Int) - 440) <= 22, "hz=${tone["hz"]}")
    val tone2 = run(3.0, 128) { s -> 0.3 * sin(2 * PI * 1500 * s) }
    check("major peak within one bin of 1500", abs((tone2["hz"] as Int) - 1500) <= 22, "hz=${tone2["hz"]}")

    println("── near-silence (room hiss) ──")
    val quiet = run(15.0, 128) { _ -> 0.0006 * (rnd.nextDouble() * 2 - 1) }
    check("no beats in silence", quiet["beats"] == 0, "beats=${quiet["beats"]}")
    check("volume 0 in silence", quiet["vol"] == 0, "vol=${quiet["vol"]}")
    check("all 16 bands dark in silence", (quiet["bands"] as String).split(",").all { it == "0" }, "bands=${quiet["bands"]}")
    check("not flagged as music", quiet["music"] == false, "music=${quiet["music"]}")

    println("── steady fan-like broadband noise (should NOT strobe) ──")
    val fan = run(12.0, 128) { _ -> 0.05 * (rnd.nextDouble() * 2 - 1) }
    check("steady noise gives (almost) no beats", (fan["beats"] as Int) <= 2, "beats=${fan["beats"]}")

    println("── frame format ──")
    val d = AudioDsp(); d.reset()
    val f = d.push(ShortArray(HOP), 25.0).frame
    check("24 bytes, magic 0xA3", f.size == 24 && (f[0].toInt() and 0xFF) == 0xA3, "len=${f.size} magic=0x${"%02X".format(f[0])}")

    println(if (ok) "\nALL DSP TESTS PASSED" else "\nSOME DSP TESTS FAILED")
}
