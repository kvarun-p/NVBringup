#!/bin/zsh
# Verifies each installation step of the README, in order, and says what to fix.
#
#   tools/verify_install.sh           read-only checks; doesn't wake a powered-off GPU
#   tools/verify_install.sh --full    also runs nvtest, vktest, a Metal test and a short llama-bench (powers the GPU on)
#
# Paths (environment or local.env in the repo root, all optional):
#   NVB_LLAMA_SERVER  llama-server        (default: PATH)
#   NVB_VULKAN_PREFIX where the Vulkan loader and headers are installed (default: searched)
#   NVB_BENCH_MODEL   GGUF for --full's llama-bench (default: the smallest in ~/models)
#   NVB_NVMETAL_BUNDLE the NVMetal.bundle build (default: next to the NVK library, else nvmetal_root_install.sh's)
# Exit code: 0 when nothing failed (warnings allowed), 1 otherwise. No root needed.

set -u
cd "${0:A:h}/.." || exit 1
[[ -f local.env ]] && { set -a; . ./local.env; set +a; }
FULL=0
[[ "${1:-}" == --full ]] && FULL=1
[[ "${1:-}" == -h || "${1:-}" == --help ]] && { sed -n 2,12p "$0"; exit 0; }

typeset -i NPASS=0 NWARN=0 NFAIL=0
if [[ -t 1 ]]; then G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' B=$'\e[1m' N=$'\e[0m'; else G= Y= R= B= N=; fi
pass() { print "  ${G}PASS${N} $1"; NPASS+=1; }
warn() { print "  ${Y}WARN${N} $1"; [[ -n "${2:-}" ]] && print "       → $2"; NWARN+=1; }
fail() { print "  ${R}FAIL${N} $1"; [[ -n "${2:-}" ]] && print "       → $2"; NFAIL+=1; }
step() { print "\n${B}$1${N}"; }
# A fix hint for a missing path: says so when it's on a volume that isn't mounted.
hint() { [[ $1 == /Volumes/* && ! -d /Volumes/${${1#/Volumes/}%%/*} ]] && print "its volume /Volumes/${${1#/Volumes/}%%/*} isn't mounted" || print "$2"; }

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
FW=firmware/nvidia
DAEMON_DIR="/Library/Application Support/NVBringup"
DAEMON_PLIST=/Library/LaunchDaemons/com.nvbringup.gsp.plist

# The kext's registry properties, read once (plist; large ones like NVLog included).
ioreg -a -r -c NVBringup -d 1 > "$TMP/ioreg.plist" 2>/dev/null
KEXT_UP=0; [[ -s "$TMP/ioreg.plist" ]] && plutil -extract 0 raw -o /dev/null "$TMP/ioreg.plist" 2>/dev/null && KEXT_UP=1
prop() { plutil -extract "0.$1" raw -o - "$TMP/ioreg.plist" 2>/dev/null; }

CHIPSET=$(prop NVChipset); CHIPSET=${CHIPSET:-0}
case $CHIPSET in
    354|356|358) GROUP=tu102 ;;   # TU102 TU104 TU106
    359|360)     GROUP=tu116 ;;   # TU117 TU116
    *)           GROUP= ;;
esac

# ---------------------------------------------------------------------------------------------
step "1. Firmware (firmware/nvidia, linux-firmware r570.144)"
typeset -A SIZE=(
    tu102/gsp/gsp-570.144.bin 28542040          tu102/gsp/bootloader-570.144.bin 4196
    tu102/gsp/gen_bootloader-570.144.bin 816
    tu102/gsp/booter_load-570.144.bin 59272     tu102/gsp/booter_unload-570.144.bin 39304
    tu116/gsp/booter_load-570.144.bin 59016     tu116/gsp/booter_unload-570.144.bin 39048
)
for f in tu102/gsp/gsp-570.144.bin tu102/gsp/bootloader-570.144.bin tu102/gsp/gen_bootloader-570.144.bin \
         {tu102,tu116}/gsp/booter_{load,unload}-570.144.bin; do
    g=${f%%/*}
    needed=1
    [[ $f == *booter* && -n "$GROUP" && $g != "$GROUP" ]] && needed=0   # the other chip group's booters
    if [[ ! -f $FW/$f ]]; then
        if (( needed )); then fail "$f missing" "download it (README, step 1)"
        else warn "$f missing (only for $([[ $g == tu102 ]] && print TU102/104/106 || print TU116/117), not this GPU)"; fi
    elif [[ $(stat -f %z $FW/$f) != ${SIZE[$f]} ]]; then
        fail "$f has $(stat -f %z $FW/$f) bytes, expected ${SIZE[$f]}" "download it again (release 570.144)"
    else
        pass "$f"
    fi
done
[[ -z "$GROUP" ]] && warn "chip unknown (kext not loaded): both booter groups were treated as needed"

# ---------------------------------------------------------------------------------------------
step "2. Build (make)"
if [[ -d build/NVBringup.kext ]] && codesign -v build/NVBringup.kext 2>/dev/null; then
    pass "build/NVBringup.kext (signed)"
else
    fail "build/NVBringup.kext missing or its signature is broken" "run make (exFAT: make deletes ._* files before signing)"
fi
for t in nvgsp nvtest vbios_tool; do
    [[ -x build/$t ]] && pass "build/$t" || fail "build/$t missing" "run make"
done
if [[ -x build/vbios_tool ]]; then
    build/vbios_tool --selftest >/dev/null 2>&1 && pass "parser self-test" || fail "parser self-test failed" "build/vbios_tool --selftest"
    if [[ -n "$GROUP" && -f $FW/tu102/gsp/gsp-570.144.bin ]]; then
        grp=tu11x; [[ $GROUP == tu102 ]] && grp=tu10x
        vram=$(( $(prop NVVramSize || echo 0) >> 20 ))
        if build/vbios_tool --gsp $FW ${vram:-4096} $grp > "$TMP/gsp.txt" 2>&1; then
            pass "firmware parses for this GPU ($grp, WPR2 layout for ${vram} MiB)"
        else
            fail "firmware doesn't parse for $grp: $(grep -m1 -iE 'error|fail|not found|missing|No such' $TMP/gsp.txt)"
        fi
    fi
fi

# ---------------------------------------------------------------------------------------------
step "3. Kext loaded by OpenCore"
if (( KEXT_UP )); then
    pass "NVBringup is loaded and attached to the GPU"
    loaded=$(kmutil showloaded --bundle-identifier io.github.kvarun-p.nvbringup 2>/dev/null | awk 'NR>1{for(i=1;i<=NF;i++) if($i ~ /^[0-9A-F-]{36}$/) print $i}')
    built=$(dwarfdump --uuid build/NVBringup.kext/Contents/MacOS/NVBringup 2>/dev/null | awk '{print $2}')
    if [[ -n "$loaded" && -n "$built" && "$loaded" != "$built" ]]; then
        warn "the loaded kext isn't build/NVBringup.kext (UUID $loaded vs $built)" "copy the build to EFI/OC/Kexts and reboot, if that's intended"
    fi
else
    fail "NVBringup is not loaded" "EFI/OC/Kexts + config.plist Kernel → Add (README, step 3); the GPU must be visible"
fi
BA=" $(sysctl -n kern.bootargs) "
for a in nvfwsec=1 nvgsp=1; do
    [[ "$BA" == *" $a "* ]] && pass "boot-arg $a" || fail "boot-arg $a missing" "add it to boot-args in config.plist (NVRAM → Add)"
done
[[ "$BA" == *" -wegnoegpu "* ]] && fail "boot-arg -wegnoegpu is set: WhateverGreen terminates the GPU device" "remove it"
if (( KEXT_UP )); then
    if [[ -n "$GROUP" ]]; then
        pass "Turing chip: $(prop NVChipName) ($(prop NVGpuName || echo name after GSP-RM boot)), $(( $(prop NVVramSize) >> 20 )) MiB VRAM"
    else
        fail "chip $(prop NVChipName) (0x$(printf %x $CHIPSET)) is not a supported Turing chip"
    fi
    frts=$(prop NVFrtsResult)
    case $frts in
        success) pass "FWSEC-FRTS succeeded at boot" ;;
        "not run") fail "FWSEC-FRTS not run" "boot-arg nvfwsec=1" ;;
        *) fail "FWSEC-FRTS: ${frts:-no result}" "if 'refused' for WPR2: shut down fully (not restart) and boot again; kext log: nvgsp status" ;;
    esac
    if ioreg -r -c IONDRVFramebuffer -d 1 2>/dev/null | grep -q 'PEGP\|pci10de'; then
        warn "macOS attached a generic framebuffer to the NVIDIA GPU (laptops: brightness control can break)" "class-code <00 00 FF 00> via DeviceProperties (README, step 3.5)"
    fi
fi

# ---------------------------------------------------------------------------------------------
step "4. Boot daemon (sudo tools/install_daemon.sh)"
[[ -f $DAEMON_PLIST ]] && pass "LaunchDaemon $DAEMON_PLIST" || fail "LaunchDaemon not installed" "sudo tools/install_daemon.sh"
if [[ -x "$DAEMON_DIR/nvgsp" && -f "$DAEMON_DIR/gsp_boot.sh" ]]; then
    pass "nvgsp and gsp_boot.sh in $DAEMON_DIR"
    cmp -s "$DAEMON_DIR/nvgsp" build/nvgsp || warn "the daemon's nvgsp differs from build/nvgsp" "sudo tools/install_daemon.sh to update it"
    dfw=(tu102/gsp/gsp-570.144.bin tu102/gsp/bootloader-570.144.bin)
    [[ -n "$GROUP" ]] && dfw+=($GROUP/gsp/booter_load-570.144.bin $GROUP/gsp/booter_unload-570.144.bin)
    for f in $dfw; do
        [[ -f "$DAEMON_DIR/firmware/$f" ]] || fail "daemon firmware $f missing" "sudo tools/install_daemon.sh"
    done
else
    fail "daemon files missing in $DAEMON_DIR" "sudo tools/install_daemon.sh"
fi
last=$(ls -t /Library/Logs/NVBringup/boot-*.txt 2>/dev/null | head -1)
if [[ -n "$last" ]]; then
    if grep -q '== boot result 0' "$last"; then pass "last daemon run booted GSP-RM (${last:t})"
    elif grep -q 'GSP-RM already running' "$last" || ! grep -q '== boot result' "$last"; then warn "last daemon log has no boot result (${last:t})" "it may still have been running at shutdown"
    else fail "last daemon run failed (${last:t})" "read the log; common: FRTS refused → shut down fully"; fi
else
    warn "no daemon logs in /Library/Logs/NVBringup yet" "they appear after the first boot with the daemon installed"
fi
if (( KEXT_UP )); then
    gsp=$(prop NVGspResult)
    pstate=$(prop NVPower.State); pmode=$(prop NVPower.Mode); cap=$(prop NVPower.Capable)
    case $gsp in
        running) pass "GSP-RM running" ;;
        "powered off (idle)"|"powered on without GSP-RM"*) pass "GSP-RM booted earlier; the GPU is powered off while idle ($pmode mode)" ;;
        *) fail "GSP-RM: ${gsp:-not booted}" "check the daemon log; build/nvgsp status" ;;
    esac
    [[ -n "$pstate" ]] && pass "power: $pstate, mode $pmode$([[ $cap == false ]] && print ', no ACPI power control (stays on)')"
fi

# ---------------------------------------------------------------------------------------------
step "5. NVK (Mesa) and the Vulkan loader"
icd= lib=
for m in ${(s.:.)${VK_DRIVER_FILES:-}} ~/.config/vulkan/icd.d/*.json(N) /usr/local/share/vulkan/icd.d/*.json(N) \
         /opt/local/share/vulkan/icd.d/*.json(N) /etc/vulkan/icd.d/*.json(N); do
    [[ -f $m ]] || continue
    l=$(plutil -extract ICD.library_path raw -o - "$m" 2>/dev/null)
    [[ $l == *nouveau* ]] && { icd=$m; lib=$l; break; }
done
if [[ -n "$icd" ]]; then
    pass "NVK manifest ${icd/#$HOME/~}"
    [[ -f $lib ]] && pass "NVK library ${lib/#$HOME/~}" || fail "NVK library $lib missing" "$(hint $lib 'build Mesa (README, step 5) or fix library_path')"
else
    fail "no NVK ICD manifest found" "create ~/.config/vulkan/icd.d/nouveau_icd.x86_64.json (README, step 5)"
fi
vkprefix=
for p in ${NVB_VULKAN_PREFIX:-} /usr/local /opt/local /opt/homebrew; do
    [[ -n $p && -f $p/lib/libvulkan.1.dylib ]] && { vkprefix=$p; break; }
done
[[ -n $vkprefix ]] && pass "Vulkan loader $vkprefix/lib/libvulkan.1.dylib" \
    || fail "Vulkan loader (libvulkan.1.dylib) not found" "$(hint ${NVB_VULKAN_PREFIX:-/usr/local} 'build Vulkan-Loader (README, step 5); set NVB_VULKAN_PREFIX')"
if (( FULL )); then
    if (( KEXT_UP )); then
        build/nvtest > "$TMP/nvtest.txt" 2>&1
        grep -q ' 0 failed' "$TMP/nvtest.txt" && pass "nvtest: $(tail -1 $TMP/nvtest.txt | sed 's/^nvtest: //')" \
            || fail "nvtest: $(tail -1 $TMP/nvtest.txt)" "build/nvtest for details"
    fi
    inc=; for p in ${vkprefix:-} /usr/local /opt/local /opt/homebrew; do [[ -f $p/include/vulkan/vulkan.h ]] && { inc=$p/include; break; }; done
    if [[ -n "$lib" && -f "$lib" && -n "$inc" ]]; then
        if clang -std=c11 -O2 -I"$inc" tools/vktest.c -o "$TMP/vktest" 2>"$TMP/vkcc.txt"; then
            "$TMP/vktest" "$lib" > "$TMP/vktest.txt" 2>&1
            if ! grep -q 'FAIL' "$TMP/vktest.txt" && grep -q 'ok' "$TMP/vktest.txt"; then
                pass "vktest: $(grep -c '^  ok:' $TMP/vktest.txt) checks (compute dispatch through NVK)"
            else
                fail "vktest: $(grep -m1 FAIL $TMP/vktest.txt)" "run it: vktest $lib"
            fi
        else
            warn "couldn't build vktest: $(head -1 $TMP/vkcc.txt)"
        fi
    else
        warn "vktest skipped (needs the NVK library and Vulkan headers)"
    fi
fi

# ---------------------------------------------------------------------------------------------
step "6. llama.cpp"
srv=${NVB_LLAMA_SERVER:-$(command -v llama-server 2>/dev/null)}
if [[ -n "$srv" && -x "$srv" ]]; then
    pass "llama-server ${srv/#$HOME/~}"
    # Lists devices through the Vulkan loader and NVK; this doesn't power the GPU on.
    env ${icd:+VK_DRIVER_FILES=$icd} "$srv" --list-devices > "$TMP/devices.txt" 2>&1
    if grep -qE 'Vulkan[0-9]+:.*NVIDIA' "$TMP/devices.txt"; then
        pass "llama.cpp sees $(grep -m1 -oE 'Vulkan[0-9]+: [^(]*' $TMP/devices.txt | sed 's/ *$//')"
    else
        fail "llama.cpp lists no NVIDIA Vulkan device" "GPU power mode 'off' hides it; check the NVK manifest; $srv --list-devices"
    fi
    if (( FULL )); then
        bench=${srv:h}/llama-bench
        model=${NVB_BENCH_MODEL:-$(ls -S ~/models/*/*.gguf(N) 2>/dev/null | tail -1)}
        if [[ -x $bench && -f "$model" ]]; then
            env ${icd:+VK_DRIVER_FILES=$icd} GGML_VK_VISIBLE_DEVICES=0 "$bench" -m "$model" -ngl 99 -p 64 -n 16 -r 1 \
                > "$TMP/bench.txt" 2>&1
            tg=$(awk -F'|' '/tg16/{gsub(/ /,"",$(NF-1)); print $(NF-1)}' $TMP/bench.txt)
            [[ -n "$tg" ]] && pass "llama-bench ${model:t}: generation $tg t/s" || fail "llama-bench failed" "see $bench -m $model"
        else
            warn "llama-bench skipped (needs llama-bench next to llama-server and a model in ~/models)"
        fi
    fi
else
    warn "llama-server not found (optional)" "$(hint ${srv:-none} 'build llama.cpp (README, step 6); set NVB_LLAMA_SERVER')"
fi

# ---------------------------------------------------------------------------------------------
step "7. Metal acceleration (optional, README step 7)"
ACCEL_ID=io.github.kvarun-p.nvmetalaccel
SYS_BUNDLE=/System/Library/Extensions/NVMetal.bundle
KILL_FILE=/Library/Preferences/io.github.kvarun-p.nvmetal.disabled
ALLOW_FILE=/Library/Preferences/io.github.kvarun-p.nvmetal.allow
# the build to compare with: NVB_NVMETAL_BUNDLE, else next to the NVK library, else the install script's default
nvb=${NVB_NVMETAL_BUNDLE:-}
if [[ -z $nvb ]]; then
    for c in ${lib:+${lib:h:h}/air/NVMetal.bundle} \
             $(sed -n 's/^DEFAULT_BUNDLE=//p' accel/tools/nvmetal_root_install.sh 2>/dev/null); do
        [[ -x $c/Contents/MacOS/NVMetal ]] && { nvb=$c; break; }
    done
fi
if [[ "$BA" != *" nvaccel=1 "* && ! -d $SYS_BUNDLE && ! -d /Library/Extensions/NVMetalAccel.kext ]]; then
    warn "not set up (optional): Metal apps don't see the GPU" "README, step 7"
else
    [[ "$BA" == *" nvaccel=1 "* ]] && pass "boot-arg nvaccel=1" || fail "boot-arg nvaccel=1 missing: NVMetalAccel stays off" "add it to boot-args"
    accel_uuid=$(kmutil showloaded --bundle-identifier $ACCEL_ID 2>/dev/null | awk 'NR>1{for(i=1;i<=NF;i++) if($i ~ /^[0-9A-F-]{36}$/) print $i}')
    if [[ -n "$accel_uuid" ]]; then
        pass "NVMetalAccel loaded"
        built=$(dwarfdump --uuid accel/build/NVMetalAccel.kext/Contents/MacOS/NVMetalAccel 2>/dev/null | awk '{print $2}')
        [[ -n "$built" && "$built" != "$accel_uuid" ]] &&
            warn "the loaded NVMetalAccel isn't accel/build/NVMetalAccel.kext" "copy it to /Library/Extensions, approve it, reboot"
    elif [[ -d /Library/Extensions/NVMetalAccel.kext ]]; then
        fail "NVMetalAccel is in /Library/Extensions but not loaded" "approve it in System Settings → Privacy & Security, then reboot (needs the kext-signing SIP bit)"
    else
        fail "NVMetalAccel not installed" "accel: make, copy build/NVMetalAccel.kext to /Library/Extensions (accel/README.md)"
    fi
    sip=$(csrutil status 2>/dev/null)
    [[ $sip == *"Filesystem Protections: disabled"* ]] && csrutil authenticated-root status 2>/dev/null | grep -qi disabled &&
        pass "SIP allows root changes (filesystem protections and authenticated root off)" ||
        warn "SIP doesn't allow changing the system volume" "csr-active-config 0x803 (needed only to install NVMetal.bundle)"
    if [[ -x $SYS_BUNDLE/Contents/MacOS/NVMetal ]]; then
        pass "NVMetal.bundle installed in /System/Library/Extensions"
        if [[ -n "$nvb" && -x $nvb/Contents/MacOS/NVMetal ]]; then
            if cmp -s $SYS_BUNDLE/Contents/MacOS/NVMetal $nvb/Contents/MacOS/NVMetal; then
                pass "it's the current build (${nvb/#$HOME/~})"
            else
                warn "the installed NVMetal.bundle differs from the build ${nvb/#$HOME/~}" \
                     "sudo accel/tools/nvmetal_root_install.sh install $nvb, then reboot"
            fi
        else
            warn "no NVMetal.bundle build to compare with" "$(hint ${nvb:-/none} 'set NVB_NVMETAL_BUNDLE')"
        fi
    else
        fail "NVMetal.bundle not installed" "sudo accel/tools/nvmetal_root_install.sh install <bundle>, then reboot"
    fi
    [[ -e $KILL_FILE ]] && warn "the kill switch is on: no process gets the GPU" "sudo rm $KILL_FILE"
    if [[ -f $ALLOW_FILE ]]; then
        apps=(${(f)"$(grep -v '^[[:space:]]*\(#\|$\)' $ALLOW_FILE)"})
        (( ${#apps} )) && pass "apps let in: ${(j:, :)apps}" || warn "$ALLOW_FILE lists no apps" "one executable name or path per line"
    else
        warn "no apps let in: only processes run with NVMETAL_ALLOW=1 get the GPU" "list executables in $ALLOW_FILE (sudo)"
    fi
    if (( FULL )); then
        if clang -fobjc-arc -framework Metal -framework Foundation tools/metaltest.m -o "$TMP/metaltest" 2>"$TMP/mtcc.txt"; then
            NVMETAL_ALLOW=1 "$TMP/metaltest" > "$TMP/metal.txt" 2>"$TMP/metal-err.txt"
            if ! grep -q FAIL "$TMP/metal.txt" && grep -q 'ok: render' "$TMP/metal.txt"; then
                pass "Metal: $(grep -c '^  ok:' $TMP/metal.txt) checks on $(grep -m1 -o 'device .*' $TMP/metal.txt | sed 's/^device //') (compute, render)"
            else
                fail "Metal: $(grep -m1 FAIL $TMP/metal.txt || tail -1 $TMP/metal-err.txt)" "NVMETAL_ALLOW=1 metaltest (tools/metaltest.m); log show --predicate 'eventMessage CONTAINS \"NVMetal\"' --last 5m"
            fi
            grep 'WARN:' "$TMP/metal.txt" | sed 's/^ *WARN: //' | while read -r w; do
                warn "Metal: $w" "install the current NVMetal.bundle (above)"
            done
            if grep -q 'host memory import' "$TMP/metal-err.txt"; then
                pass "host memory import (zero-copy IOSurfaces and large no-copy buffers)"
            elif grep -q 'through NVK' "$TMP/metal-err.txt"; then
                warn "no host memory import: the loaded kext predates NVMAC_MEM_IMPORT (copies instead)" "put the current build/NVBringup.kext on the EFI and reboot"
            fi
        else
            warn "couldn't build metaltest: $(head -1 $TMP/mtcc.txt)"
        fi
    fi
fi

# ---------------------------------------------------------------------------------------------
step "8. GPU Monitor"
if [[ -d ~/Applications/"GPU Monitor.app" ]]; then
    pass "~/Applications/GPU Monitor.app"
    pgrep -x GPUMonitor >/dev/null && pass "running" || warn "not running" "open ~/Applications/GPU\\ Monitor.app"
else
    warn "not installed (optional)" "monitor/build.sh"
fi

print "\n${B}Summary:${N} ${G}$NPASS passed${N}, ${Y}$NWARN warnings${N}, ${R}$NFAIL failed${N}$( (( FULL )) || print ' (quick check; --full also runs GPU tests)')"
(( NFAIL == 0 ))
