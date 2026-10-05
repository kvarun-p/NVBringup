#!/bin/zsh
# Removes what tools/install.sh and the README's steps install on macOS. Run it as yourself from the
# repository; it asks for sudo where it needs it. Keeps the logs (/Library/Logs/NVBringup), the
# repository, your Mesa and llama.cpp builds, and the EFI: it lists the EFI changes to undo by hand,
# since a broken config.plist can stop the Mac from booting.
#
#   tools/uninstall.sh [-n|--dry-run] [component ...]
#
# Components (default: all of them, in this order):
#   bundle    NVMetal.bundle out of /System/Library/Extensions (nvmetal_root_install.sh remove; a new snapshot)
#   accel     NVMetalAccel.kext out of /Library/Extensions
#   prefs     the Metal allow list and kill switch in /Library/Preferences
#   daemon    the GSP-RM boot daemon (tools/uninstall_daemon.sh)
#   icd       your NVK Vulkan manifest (~/.config/vulkan/icd.d, the ones that point at nouveau)
#   monitor   GPU Monitor (~/Applications)

set -u
cd "${0:A:h}/.." || exit 1

DRY=0
typeset -a want
while (( $# )); do
    case $1 in
        -n|--dry-run) DRY=1 ;;
        -h|--help)    sed -n 2,15p "$0"; exit 0 ;;
        bundle|accel|prefs|daemon|icd|monitor) want+=$1 ;;
        *)            print -u2 "unknown argument $1 (see --help)"; exit 2 ;;
    esac
    shift
done
[[ $EUID -eq 0 ]] && { print -u2 "run as yourself, not with sudo: it asks for sudo where needed"; exit 1; }
(( ${#want} )) || want=(bundle accel prefs daemon icd monitor)
has() { (( ${want[(Ie)$1]} )); }

if [[ -t 1 ]]; then G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' B=$'\e[1m' N=$'\e[0m'; else G= Y= R= B= N=; fi
typeset -a REBOOT
typeset -i NFAIL=0
step() { print "\n${B}$1${N}"; }
ok()   { print "  ${G}$1${N}"; }
note() { print "  ${Y}$1${N}"; }
err()  { print "  ${R}$1${N}"; NFAIL+=1; }
run()  { print "  + ${(j: :)${(q-)@}}"; (( DRY )) || "$@"; }

SYS_BUNDLE=/System/Library/Extensions/NVMetal.bundle
ACCEL=/Library/Extensions/NVMetalAccel.kext
ACCEL_ID=io.github.kvarun-p.nvmetalaccel
PREFS=(/Library/Preferences/io.github.kvarun-p.nvmetal.{allow,disabled})
BA=" $(sysctl -n kern.bootargs 2>/dev/null) "

print "components: ${(j:, :)want}"
if (( DRY )); then
    print "dry run: nothing is changed"
elif has bundle || has accel || has prefs || has daemon; then
    sudo -v || exit 1
fi

# ---------------------------------------------------------------------------------------------
if has bundle; then
    step "NVMetal.bundle"
    if [[ ! -e $SYS_BUNDLE ]]; then
        ok "not installed"
    elif run sudo accel/tools/nvmetal_root_install.sh remove; then
        REBOOT+="NVMetal.bundle removal (a new system snapshot)"
    else
        err "nvmetal_root_install.sh remove failed (it needs csr-active-config 0x803 and FileVault off)"
        note "alternative: sudo accel/tools/nvmetal_root_install.sh revert (Apple's last sealed snapshot; drops every root change)"
    fi
fi

if has accel; then
    step "NVMetalAccel"
    if [[ ! -e $ACCEL ]]; then
        ok "not installed"
    else
        # unloading fails while Metal clients hold it open; it's then gone after the reboot
        if kmutil showloaded --bundle-identifier $ACCEL_ID 2>/dev/null | grep -q $ACCEL_ID; then
            run sudo kmutil unload -b $ACCEL_ID 2>/dev/null || note "still in use; it stops at the reboot"
        fi
        if run sudo rm -rf $ACCEL; then
            REBOOT+="NVMetalAccel removal (macOS rebuilds the auxiliary kext collection)"
        else
            err "couldn't delete $ACCEL"
        fi
    fi
fi

if has prefs; then
    step "Metal allow list and kill switch"
    found=(${^PREFS}(N))
    if (( ! ${#found} )); then
        ok "none"
    else
        run sudo rm -f $found || err "couldn't delete ${(j: :)found}"
    fi
fi

if has daemon; then
    step "Boot daemon"
    if [[ ! -e /Library/LaunchDaemons/com.nvbringup.gsp.plist && ! -d "/Library/Application Support/NVBringup" ]]; then
        ok "not installed"
    else
        run sudo tools/uninstall_daemon.sh || err "uninstall_daemon.sh failed"
    fi
fi

if has icd; then
    step "NVK Vulkan manifest"
    typeset -a icds=()
    for m in ~/.config/vulkan/icd.d/*.json(N); do
        [[ $(plutil -extract ICD.library_path raw -o - $m 2>/dev/null) == *nouveau* ]] && icds+=$m
    done
    if (( ! ${#icds} )); then
        ok "none in ~/.config/vulkan/icd.d"
    else
        run rm -f $icds || err "couldn't delete ${(j: :)icds}"
    fi
fi

if has monitor; then
    step "GPU Monitor"
    APP=~/Applications/"GPU Monitor.app"
    if [[ ! -d $APP ]]; then
        ok "not installed"
    else
        pgrep -x GPUMonitor >/dev/null && run pkill -x GPUMonitor
        run rm -rf $APP || err "couldn't delete $APP"
        note "if \"Launch at login\" was on: remove GPU Monitor in System Settings → General → Login Items"
        note "remove its widget from the desktop if you added one"
    fi
fi

# ---------------------------------------------------------------------------------------------
step "Summary"
(( NFAIL )) && print "  ${R}$NFAIL step(s) failed${N} (above)"
(( ${#REBOOT} )) && print "  reboot to finish: ${(j:; :)REBOOT}"
print "  by hand, in the EFI's config.plist (back the EFI up first):"
print "    - delete EFI/OC/Kexts/NVBringup.kext and its Kernel → Add entry"
a=(${(M)${=BA}:#nv*})
print "    - remove the nv* boot-args${a:+ (now: ${(j: :)a})}"
csrutil authenticated-root status 2>/dev/null | grep -qi disabled &&
    print "    - after the reboot: restore csr-active-config (0x803 was set for the Metal step; SIP is partly off now)"
print "  kept: /Library/Logs/NVBringup, firmware/nvidia, your Mesa and llama.cpp builds"
(( NFAIL == 0 ))
