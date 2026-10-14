#!/bin/zsh
# Boots GSP-RM at startup (LaunchDaemon com.nvbringup.gsp, runs as root).
# Does nothing unless this boot has boot-arg nvgsp=1 and the NVBringup kext is loaded
# (USB EFI) and FWSEC-FRTS succeeded. GSP-RM is torn down again by the kext itself on
# sleep, power off and restart. The kext log after boot is kept in /Library/Logs/NVBringup.

DIR="/Library/Application Support/NVBringup"
LOGDIR="/Library/Logs/NVBringup"
NVGSP="$DIR/nvgsp"
mkdir -p "$LOGDIR"
LOG="$LOGDIR/boot-$(date +%Y%m%d-%H%M%S).txt"

# The boot-attempt guard (boot-arg nvguard=1; bootGuard in src/NVGpu.cpp): the kext counts every boot in NVRAM
# nv-boot-try, and too many unfinished ones in a row make a safe boot. A boot is finished, and the counter deleted,
# once WindowServer has run for a minute. Runs last, in the foreground: launchd ends the job's background processes.
boot_ok() {
    [[ " $(sysctl -n kern.bootargs) " == *" nvguard=1 "* ]] || return 0
    local pid= i
    for i in {1..300}; do
        pid=$(pgrep -x WindowServer | head -1)
        [[ -n $pid ]] && break
        sleep 2
    done
    [[ -n $pid ]] || { echo "== boot guard: no WindowServer after 10 min; nv-boot-try kept"; return 1; }
    sleep 60
    kill -0 $pid 2>/dev/null || { echo "== boot guard: WindowServer $pid didn't last a minute; nv-boot-try kept"; return 1; }
    if nvram nv-boot-try >/dev/null 2>&1; then
        nvram -d nv-boot-try && echo "== boot guard: healthy boot, nv-boot-try deleted"
    fi
}

case " $(sysctl -n kern.bootargs) " in
    *" nvgsp=1 "*) ;;
    *) boot_ok >> "$LOGDIR/boot-guard.txt" 2>&1; exit 0 ;;   # not a GSP test boot
esac

run() {
    echo "== $(date): boot-args: $(sysctl -n kern.bootargs)"

    # Wait for the kext and for its boot-time FRTS result (up to 60 s).
    for i in {1..60}; do
        st=$("$NVGSP" status 2>/dev/null) && [[ "$st" == *"FRTS:"* ]] && break
        sleep 1
    done
    if [[ "$st" != *"FRTS: SUCCESS"* ]]; then
        echo "== FRTS did not succeed (or the kext is not loaded); not booting GSP-RM"
        "$NVGSP" status 2>&1 | tail -20
        return 1
    fi
    if ioreg -r -c NVBringup -d 1 | grep -q '"NVGspResult" = "running"'; then
        echo "== GSP-RM already running"
        return 0
    fi

    echo "== booting GSP-RM"
    "$NVGSP" boot "$DIR/firmware"
    rc=$?
    sleep 5
    echo "== boot result $rc; kext log:"
    ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o - - 2>/dev/null
    ioreg -r -c NVBringup -d 1 | grep -E '"NV(GspResult|CopyEngineTest|ComputeTest|Bar1Test|VramTest|VaSpace)"'
    return $rc
}
run > "$LOG" 2>&1
rc=$?
boot_ok >> "$LOG" 2>&1

# Keep the newest 30 logs.
ls -1t "$LOGDIR"/boot-*.txt 2>/dev/null | tail -n +31 | while read -r f; do rm -f "$f"; done
exit $rc
