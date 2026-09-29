#!/bin/zsh
# Removes the GSP-RM boot daemon (sudo tools/uninstall_daemon.sh). Keeps the logs.
set -u
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
launchctl bootout system/com.nvbringup.gsp 2>/dev/null
rm -f /Library/LaunchDaemons/com.nvbringup.gsp.plist
rm -rf "/Library/Application Support/NVBringup"
echo "removed (logs kept in /Library/Logs/NVBringup)"
