#!/bin/sh
# Installs nvvdisplay for this user: the binary in ~/Library/Application Support/NVBringup and a
# LaunchAgent that runs it at login (it exits at once when no display is lit). No sudo.
#   vdisplay/install.sh            install (build first: make -C display vdisplay) and start it
#   vdisplay/install.sh -u         stop and remove
set -e
LABEL=io.github.kvarun-p.nvvdisplay
DIR="$HOME/Library/Application Support/NVBringup"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
LOG="$HOME/Library/Logs/nvvdisplay.log"
launchctl bootout "gui/$(id -u)/$LABEL" 2>/dev/null || true
if [ "$1" = "-u" ]; then
    rm -f "$PLIST" "$DIR/nvvdisplay"
    echo "nvvdisplay removed"
    exit 0
fi
BIN="$(cd "$(dirname "$0")/.." && pwd)/build/nvvdisplay"
[ -x "$BIN" ] || { echo "build it first: make -C display vdisplay" >&2; exit 1; }
mkdir -p "$DIR" "$HOME/Library/LaunchAgents"
cp "$BIN" "$DIR/nvvdisplay"
cat > "$PLIST" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>Label</key>
	<string>$LABEL</string>
	<key>ProgramArguments</key>
	<array>
		<string>$DIR/nvvdisplay</string>
	</array>
	<key>RunAtLoad</key>
	<true/>
	<key>KeepAlive</key>
	<dict>
		<key>SuccessfulExit</key>
		<false/>
	</dict>
	<key>ThrottleInterval</key>
	<integer>10</integer>
	<key>ProcessType</key>
	<string>Interactive</string>
	<key>StandardOutPath</key>
	<string>$LOG</string>
	<key>StandardErrorPath</key>
	<string>$LOG</string>
</dict>
</plist>
PL
# launchd may still be removing the old instance: retry for a few seconds.
for i in 1 2 3 4 5; do
    launchctl bootstrap "gui/$(id -u)" "$PLIST" 2>/dev/null && break
    [ $i = 5 ] && { echo "launchctl bootstrap failed" >&2; exit 1; }
    sleep 1
done
echo "nvvdisplay installed and started; log: $LOG"
