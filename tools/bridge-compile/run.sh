#!/usr/bin/env bash
# Usage: tools/bridge-compile/run.sh [gradle-command]
# Needs `npm ci` in rn/ and capacitor/, and the SDK's debug AAR built with
# `gradle -p android :sentlog:assembleDebug`.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
# A one-artifact Maven repo, so the bridges' me.cooki.sentlog:sentlog:0.0.0
# resolves as it would for an integrator.
dest="$here/libs/me/cooki/sentlog/sentlog/0.0.0"
mkdir -p "$dest"
cp "$here/../../android/sentlog/build/outputs/aar/sentlog-debug.aar" \
   "$dest/sentlog-0.0.0.aar"
cat > "$dest/sentlog-0.0.0.pom" <<'POM'
<project>
  <modelVersion>4.0.0</modelVersion>
  <groupId>me.cooki.sentlog</groupId>
  <artifactId>sentlog</artifactId>
  <version>0.0.0</version>
  <packaging>aar</packaging>
</project>
POM
"${1:-gradle}" -p "$here" --no-daemon \
    :sentlog-rn:compileDebugKotlin :sentlog-capacitor:compileDebugKotlin
