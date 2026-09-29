#!/bin/zsh
# Regenerates Resources/AppIcon.icns from make_icon.swift (all macOS icon sizes, 16 to 1024 px).
set -euo pipefail
cd "${0:A:h}"
if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer/Toolchains ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
swiftc -swift-version 5 -O -parse-as-library -target x86_64-apple-macos14.0 make_icon.swift -o "$T/make_icon"
"$T/make_icon" "$T/icon_1024.png"
I="$T/AppIcon.iconset"
mkdir "$I"
for s in 16 32 128 256 512; do
    sips -z $s $s "$T/icon_1024.png" --out "$I/icon_${s}x${s}.png" >/dev/null
    sips -z $((s * 2)) $((s * 2)) "$T/icon_1024.png" --out "$I/icon_${s}x${s}@2x.png" >/dev/null
done
iconutil -c icns "$I" -o AppIcon.icns
echo "wrote Resources/AppIcon.icns"
