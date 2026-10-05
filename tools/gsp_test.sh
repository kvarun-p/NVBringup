#!/bin/zsh
# One GSP-RM test run after a boot from the USB EFI (boot-args nvfwsec=1 nvgsp=1):
# checks FRTS, boots GSP-RM (unless the LaunchDaemon already did), waits, checks that the message poller keeps running,
# saves everything to runs/<date-time>/ (kept across reboots, unlike /tmp), then
# unloads GSP-RM (clears WPR2) unless NO_UNLOAD=1.
#
#   tools/gsp_test.sh [wait_seconds]      (default 90; asks for the sudo password once)

set -u
cd "$(dirname "$0")/.." || exit 1
WAIT=${1:-90}
RUN="runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$RUN"
exec > >(tee "$RUN/output.txt") 2>&1

echo "== $RUN"
echo "== boot-args: $(sysctl -n kern.bootargs)"

if ! build/nvgsp status | grep -q 'FRTS: SUCCESS'; then
    echo "FRTS did not succeed this boot (shut down fully and boot from the USB). Status:"
    build/nvgsp status
    exit 1
fi
echo "== FRTS ok"

if ioreg -r -c NVBringup -d 1 | grep -q '"NVGspResult" = "running"'; then
    echo "== GSP-RM already running (booted by the LaunchDaemon); not booting again"
    BOOT=already
    WAIT=0
else
    echo "== booting GSP-RM"
    sudo build/nvgsp boot
    BOOT=$?
fi

echo "== waiting ${WAIT}s for the poller"
sleep "$WAIT"

echo "== kext log after ${WAIT}s (last 40 lines)"
build/nvgsp status | tail -40
ioreg -r -c NVBringup -d 1 | grep -E '"NVGspResult"|"NVFrtsResult"'

echo "== saving GSP log buffers"
sudo build/nvgsp logs "$RUN"
sudo chown "$USER" "$RUN"/*.bin 2>/dev/null
build/nvgsp decode "$RUN" > "$RUN/decoded.txt"
ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o "$RUN/NVLog.txt" -

if [[ "${NO_UNLOAD:-0}" != 1 ]]; then
    echo "== unloading GSP-RM (set NO_UNLOAD=1 to keep it running)"
    sudo build/nvgsp unload
    UNLOAD=$?
    ioreg -r -c NVBringup -d 1 | grep -E '"NVGspResult"'
    ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o "$RUN/NVLog-after-unload.txt" -
else
    UNLOAD=skipped
fi

echo "== done (boot result $BOOT, unload result $UNLOAD). Files in $RUN:"
ls -la "$RUN"
echo "If the unload succeeded, WPR2 is cleared: a normal restart is enough before Windows or the next test."
echo "Otherwise: no sleep while GSP-RM runs, and shut down fully."
