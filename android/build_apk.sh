#!/bin/bash
# Builds the signed Lumo APK with the SDK tools directly (no Gradle, no network):
#   aapt2 → javac(R) → kotlinc → d8 → zipalign → apksigner
# Override any path with env vars: JAVA_HOME, ANDROID_SDK, KOTLIN_HOME.
set -euo pipefail
cd "$(dirname "$0")"
export JAVA_HOME="${JAVA_HOME:-/opt/homebrew/opt/openjdk@17}"
J="$JAVA_HOME/bin"
SDK="${ANDROID_SDK:-/opt/homebrew/share/android-commandlinetools}"
BT=$(ls -d "$SDK"/build-tools/* | sort -V | tail -1)
PLAT="$SDK/platforms/android-36/android.jar"
KOTLIN_HOME="${KOTLIN_HOME:-/opt/homebrew/opt/kotlin/libexec}"
STDLIB="$KOTLIN_HOME/lib/kotlin-stdlib.jar"
VERSION_CODE=4; VERSION_NAME=2.2

# signing details live in keystore.properties (git-ignored) — see keystore.properties.example
[ -f keystore.properties ] || { echo "✖ keystore.properties missing — copy keystore.properties.example"; exit 1; }
prop(){ grep -E "^$1=" keystore.properties | head -1 | cut -d= -f2-; }
KS=$(prop storeFile); KS_PASS=$(prop storePassword); KEY_ALIAS=$(prop keyAlias); KEY_PASS=$(prop keyPassword)
mkdir -p ../release
OUT=build_manual; rm -rf $OUT; mkdir -p $OUT/gen $OUT/classes $OUT/dex

# AGP takes the package from 'namespace'; aapt2 needs it in the manifest
sed 's|<manifest xmlns:android=|<manifest package="com.fourbrains.ambientled" xmlns:android=|' \
    app/src/main/AndroidManifest.xml > $OUT/AndroidManifest.xml

echo "▸ aapt2 compile + link"
"$BT/aapt2" compile --dir app/src/main/res -o $OUT/res.zip
"$BT/aapt2" link -I "$PLAT" --manifest $OUT/AndroidManifest.xml -A app/src/main/assets \
   --min-sdk-version 26 --target-sdk-version 36 --version-code $VERSION_CODE --version-name $VERSION_NAME \
   --java $OUT/gen -o $OUT/base.apk $OUT/res.zip

echo "▸ javac R"
"$J/javac" -nowarn --release 17 -cp "$PLAT" -d $OUT/classes $(find $OUT/gen -name '*.java')

echo "▸ kotlinc"
JAVA_HOME="$JAVA_HOME" "$KOTLIN_HOME/bin/kotlinc" -no-stdlib -no-reflect -jvm-target 17 -nowarn \
   -cp "$PLAT:$OUT/classes:$STDLIB" -d $OUT/classes app/src/main/java 2>&1 | grep -v '^warning' || true
test -f $OUT/classes/com/fourbrains/ambientled/MainActivity.class

echo "▸ d8"
"$J/java" -cp "$BT/lib/d8.jar" com.android.tools.r8.D8 --release --min-api 26 --lib "$PLAT" \
   --output $OUT/dex $(find $OUT/classes -name '*.class') "$STDLIB" 2>&1 \
   | grep -vE 'kotlin metadata|Kotlin metadata|malformed kotlin|^\s+at |Hv0|^Info:|^Warning:|^$' || true
test -f $OUT/dex/classes.dex

echo "▸ package + align + sign"
cp $OUT/base.apk $OUT/unsigned.apk
(cd $OUT/dex && zip -qj ../unsigned.apk classes*.dex)
"$BT/zipalign" -p -f 4 $OUT/unsigned.apk $OUT/aligned.apk
"$J/java" -jar "$BT/lib/apksigner.jar" sign --ks "$KS" --ks-pass "pass:$KS_PASS" \
   --key-pass "pass:$KEY_PASS" --ks-key-alias "$KEY_ALIAS" --v1-signing-enabled true --v2-signing-enabled true \
   --v3-signing-enabled true --out ../release/Lumo-$VERSION_NAME.apk $OUT/aligned.apk
"$J/java" -jar "$BT/lib/apksigner.jar" verify -v ../release/Lumo-$VERSION_NAME.apk | head -5
ls -la ../release/Lumo-$VERSION_NAME.apk
