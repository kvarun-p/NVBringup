#!/bin/zsh
# Builds "GPU Monitor.app" (menu bar app + WidgetKit extension) with swiftc (no Xcode project)
# and installs it in ~/Applications. Ad-hoc signed. Staged on the internal disk: exFAT adds
# ._* files that break code signatures.
set -euo pipefail
cd "${0:A:h}"

# Prefer Xcode when installed: the Command Line Tools 16.2 here ship a duplicate SwiftBridging
# module map that breaks (or hangs) Swift builds.
if [[ -z "${DEVELOPER_DIR:-}" && -d /Applications/Xcode.app/Contents/Developer/Toolchains ]]; then
    export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
fi

SDK=$(xcrun --sdk macosx --show-sdk-path)
SWIFTC=(swiftc -swift-version 5 -O -target x86_64-apple-macos14.0 -sdk "$SDK" -parse-as-library)
STAGE=$(mktemp -d)/"GPU Monitor.app"
DEST=${DEST:-$HOME/Applications}
APPEX="$STAGE/Contents/PlugIns/GPUWidget.appex"

mkdir -p "$STAGE/Contents/MacOS" "$STAGE/Contents/Resources" "$APPEX/Contents/MacOS"
"${SWIFTC[@]}" Shared/*.swift App/*.swift -o "$STAGE/Contents/MacOS/GPUMonitor"
"${SWIFTC[@]}" Shared/*.swift Widget/*.swift -o "$APPEX/Contents/MacOS/GPUWidget"
cp Resources/App-Info.plist "$STAGE/Contents/Info.plist"
cp Resources/AppIcon.icns "$STAGE/Contents/Resources/"      # regenerate with Resources/build_icon.sh
cp Resources/Widget-Info.plist "$APPEX/Contents/Info.plist"
printf 'APPL????' > "$STAGE/Contents/PkgInfo"

codesign --force --sign - --entitlements Resources/Widget.entitlements "$APPEX"
codesign --force --sign - "$STAGE"
codesign --verify --deep --strict "$STAGE"

mkdir -p "$DEST"
pkill -x GPUMonitor 2>/dev/null || true
# macOS keys the login registration of this ad-hoc signed app by its code hash, so each build
# would add one more "GPU Monitor" login item. Drop the old ones; the new build registers itself
# once at launch when "Launch at login" is on (LoginItem.sync).
LOGIN=$(defaults read io.github.kvarun-p.nvbringup.monitor launchAtLogin 2>/dev/null || echo 0)
if [[ $LOGIN == 1 ]]; then
    osascript -e 'tell application "System Events" to delete (every login item whose name is "GPU Monitor")' \
        >/dev/null 2>&1 || echo "note: couldn't clean up old login items (System Events permission)"
fi
rm -rf "$DEST/GPU Monitor.app"
ditto "$STAGE" "$DEST/GPU Monitor.app"
rm -rf "${STAGE:h}"
# Register the widget extension with the system.
pluginkit -a "$DEST/GPU Monitor.app/Contents/PlugIns/GPUWidget.appex"
echo "Installed $DEST/GPU Monitor.app"
[[ $LOGIN == 1 ]] && open "$DEST/GPU Monitor.app"      # re-registers this build for login
