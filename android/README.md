# LUMO for Android

A native Kotlin app with no third-party libraries. The UI is one offline HTML page
(`app/src/main/assets/index.html`) served from inside the APK. Bluetooth and the
microphone run in a foreground service.

```
app/src/main/
├── assets/index.html        the whole UI: dark theme, splash, room view, zones, layout editor
├── java/…/MainActivity.kt   WebView host, permissions, system bars, back gesture
├── java/…/LedService.kt     foreground service: keeps the link and music sync alive
├── java/…/BleLink.kt        BLE: scan, connect, reconnect, write queue, heartbeat
├── java/…/AudioEngine.kt    microphone capture
└── java/…/AudioDsp.kt       FFT → beat, BPM and 16 bands (pure Kotlin, unit-testable)
```

| | |
|---|---|
| Android | 8.0 to 16 (minSdk 26, targetSdk 36) |
| Package | `com.fourbrains.ambientled` (unchanged, so it updates over Ambient LED) |
| Internet | none, the app has no network permission |

## Signing

```bash
cp keystore.properties.example keystore.properties
```

Fill it in and put your `.jks` in `keystore/`. Both are git-ignored.
**Keep the same key forever:** Android only installs an update over an app signed
with the same key.

## Build

**Android Studio:** open this `android/` folder. It's a normal Gradle project
(AGP 8.13, Kotlin 2.2).

**No Gradle:** this script builds and signs with the SDK tools directly.

```bash
./build_apk.sh
```

It needs JDK 17, Kotlin, and Android SDK platform 36 + build-tools.
Override the paths with `JAVA_HOME`, `ANDROID_SDK` and `KOTLIN_HOME`. The APK lands in
`../release/Lumo-<version>.apk`.

## Audio engine test (desktop JVM)

`dsptest/DspTest.kt` runs `AudioDsp` against synthesised music, no phone needed.
