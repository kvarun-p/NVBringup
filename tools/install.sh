#!/bin/zsh
# Installs (or updates) the macOS side of NVBringup: README steps 1-4, 7 and 8. Run it as yourself
# from the repository; it asks for sudo where it needs it. Mesa (step 5) and llama.cpp (step 6) are
# built from their own repositories.
#
#   tools/install.sh [options] [component ...]
#
# Components, in the order they run (default: every one that applies to this machine, see below):
#   firmware  download the missing r570.144 files into firmware/nvidia (step 1)
#   build     make (step 2)
#   efi       copy build/NVBringup.kext to EFI/OC/Kexts, after backing up the EFI (step 3).
#             Never edits config.plist: on a first install it prints the entries to add.
#   daemon    sudo tools/install_daemon.sh (step 4)
#   accel     make -C accel, NVMetalAccel.kext to /Library/Extensions (step 7)
#   bundle    NVMetal.bundle to /System/Library/Extensions (step 7; csr-active-config 0x803, FileVault off)
#   monitor   monitor/build.sh, GPU Monitor to ~/Applications (step 8)
# The default runs firmware, build, efi (when an EFI with OC/Kexts is found), daemon, accel and bundle
# (when the Metal step is set up: nvaccel=1 or either part installed) and monitor (when already installed).
# Parts that are already current are skipped.
#
# Options:
#   -n, --dry-run    print the commands, change nothing
#   --efi PATH       the EFI folder (default: the one mounted volume whose EFI/OC/Kexts has NVBringup.kext)
#   --bundle PATH    the NVMetal.bundle build (default: NVB_NVMETAL_BUNDLE, else next to the NVK library
#                    in the Vulkan manifest, else nvmetal_root_install.sh's default)
# Paths can also be set in local.env (NVB_NVMETAL_BUNDLE, NVB_EFI). Check the result with tools/verify_install.sh.

set -u
cd "${0:A:h}/.." || exit 1
[[ -f local.env ]] && { set -a; . ./local.env; set +a; }

DRY=0 EFI=${NVB_EFI:-} BUNDLE=${NVB_NVMETAL_BUNDLE:-}
typeset -a want
while (( $# )); do
    case $1 in
        -n|--dry-run) DRY=1 ;;
        --efi)        EFI=${2:?--efi needs a path}; shift ;;
        --bundle)     BUNDLE=${2:?--bundle needs a path}; shift ;;
        -h|--help)    sed -n 2,26p "$0"; exit 0 ;;
        firmware|build|efi|daemon|accel|bundle|monitor) want+=$1 ;;
        *)            print -u2 "unknown argument $1 (see --help)"; exit 2 ;;
    esac
    shift
done
[[ $EUID -eq 0 ]] && { print -u2 "run as yourself, not with sudo: it asks for sudo where needed"; exit 1; }

if [[ -t 1 ]]; then G=$'\e[32m' Y=$'\e[33m' R=$'\e[31m' B=$'\e[1m' N=$'\e[0m'; else G= Y= R= B= N=; fi
typeset -a REBOOT TODO
typeset -i NFAIL=0
step() { print "\n${B}$1${N}"; }
ok()   { print "  ${G}$1${N}"; }
note() { print "  ${Y}$1${N}"; }
err()  { print "  ${R}$1${N}"; NFAIL+=1; }
run()  { print "  + ${(j: :)${(q-)@}}"; (( DRY )) || "$@"; }

FW=firmware/nvidia
FW_BASE=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/nvidia
FW_FILES=(tu102/gsp/gsp tu102/gsp/bootloader tu102/gsp/gen_bootloader
          tu102/gsp/booter_load tu102/gsp/booter_unload tu116/gsp/booter_load tu116/gsp/booter_unload)
ACCEL=/Library/Extensions/NVMetalAccel.kext
SYS_BUNDLE=/System/Library/Extensions/NVMetal.bundle
BA=" $(sysctl -n kern.bootargs 2>/dev/null) "

# the EFI: --efi / NVB_EFI, else the one mounted volume that has EFI/OC/Kexts/NVBringup.kext
find_efi() {
    [[ -n $EFI ]] && return 0
    local -a c=(/Volumes/*/EFI/OC/Kexts/NVBringup.kext(N/))
    (( ${#c} == 1 )) && EFI=${c[1]:h:h:h}
}
# the NVMetal.bundle build
find_bundle() {
    [[ -n $BUNDLE ]] && return 0
    local m l c
    for m in ~/.config/vulkan/icd.d/*.json(N) /usr/local/share/vulkan/icd.d/*.json(N); do
        l=$(plutil -extract ICD.library_path raw -o - "$m" 2>/dev/null)
        [[ $l == *nouveau* ]] && { c=${l:h:h}/air/NVMetal.bundle; [[ -x $c/Contents/MacOS/NVMetal ]] && { BUNDLE=$c; return 0; }; }
    done
    c=$(sed -n 's/^DEFAULT_BUNDLE=//p' accel/tools/nvmetal_root_install.sh)
    [[ -x $c/Contents/MacOS/NVMetal ]] && BUNDLE=$c
}
# a kext or bundle at $2 that matches the build at $1: same Info.plist and executable UUID (a rebuild
# that only re-signs keeps the UUID, and reinstalling a kext asks for approval again)
uuid() { dwarfdump --uuid "$1" 2>/dev/null | awk '{print $2}'; }
same_bundle() {
    local exe=$(plutil -extract CFBundleExecutable raw -o - "$1/Contents/Info.plist" 2>/dev/null) u
    [[ -n $exe ]] && cmp -s "$1/Contents/Info.plist" "$2/Contents/Info.plist" || return 1
    u=$(uuid "$1/Contents/MacOS/$exe")
    [[ -n $u && $u == $(uuid "$2/Contents/MacOS/$exe") ]]
}
# the boot daemon matches this checkout (install_daemon.sh's files)
DAEMON_DIR="/Library/Application Support/NVBringup"
daemon_current() {
    cmp -s build/nvgsp "$DAEMON_DIR/nvgsp" && cmp -s tools/daemon/gsp_boot.sh "$DAEMON_DIR/gsp_boot.sh" &&
        cmp -s tools/daemon/com.nvbringup.gsp.plist /Library/LaunchDaemons/com.nvbringup.gsp.plist || return 1
    local f
    for f in $FW/{tu102,tu116}/gsp/*-570.144.bin(N); do
        [[ $f == *gen_bootloader* ]] && continue
        cmp -s $f "$DAEMON_DIR/firmware/${f#$FW/}" || return 1
    done
}

if (( ! ${#want} )); then
    want=(firmware build daemon)
    find_efi && [[ -d $EFI/OC/Kexts ]] && want+=efi
    [[ $BA == *" nvaccel=1 "* || -d $ACCEL || -d $SYS_BUNDLE ]] && want+=(accel bundle)
    [[ -d ~/Applications/"GPU Monitor.app" ]] && want+=monitor
fi
has() { (( ${want[(Ie)$1]} )); }
order=()
for c in firmware build efi daemon accel bundle monitor; do has $c && order+=$c; done
print "components: ${(j:, :)order}"
if (( DRY )); then
    print "dry run: nothing is changed"
elif has daemon || has accel || has bundle; then
    sudo -v || exit 1
fi

# ---------------------------------------------------------------------------------------------
if has firmware; then
    step "Firmware (step 1)"
    typeset -i got=0
    for f in $FW_FILES; do
        [[ -s $FW/$f-570.144.bin ]] && continue
        run mkdir -p $FW/${f:h}
        if run curl -fsSL -o $FW/$f-570.144.bin $FW_BASE/$f-570.144.bin; then got+=1
        else err "download of $f-570.144.bin failed"; (( DRY )) || rm -f $FW/$f-570.144.bin; fi
    done
    (( got )) && ok "downloaded $got file(s); tools/verify_install.sh checks their sizes" || ok "all present"
fi

if has build; then
    step "Build (step 2)"
    run make || err "make failed"
fi

if has efi; then
    step "Kext on the EFI (step 3)"
    find_efi
    if [[ -z $EFI ]]; then
        err "no EFI found: pass --efi /Volumes/<volume>/EFI (several mounted volumes, or none, have EFI/OC/Kexts/NVBringup.kext)"
    elif [[ ! -d $EFI/OC/Kexts ]]; then
        err "$EFI has no OC/Kexts"
    elif [[ ! -d build/NVBringup.kext ]] || ! codesign -v build/NVBringup.kext 2>/dev/null; then
        err "build/NVBringup.kext missing or unsigned: run make"
    elif [[ -d $EFI/OC/Kexts/NVBringup.kext ]] && same_bundle build/NVBringup.kext $EFI/OC/Kexts/NVBringup.kext; then
        ok "$EFI/OC/Kexts/NVBringup.kext is current"
    else
        bak=${EFI:h}/EFI-backup-$(date +%F)
        if [[ -e $bak ]]; then
            note "today's backup $bak exists; keeping it"
        else
            run cp -RX $EFI $bak && { (( DRY )) || diff -rq $EFI $bak >/dev/null; } ||
                { err "backup of the EFI failed; nothing changed"; bak=; }
        fi
        if [[ -n $bak ]]; then
            run rm -rf $EFI/OC/Kexts/NVBringup.kext
            run cp -RX build/NVBringup.kext $EFI/OC/Kexts/
            run find $EFI/OC/Kexts/NVBringup.kext -name '._*' -delete   # FAT: AppleDouble files break the signature
            if (( DRY )) || codesign -v $EFI/OC/Kexts/NVBringup.kext 2>/dev/null; then
                ok "copied; backup in $bak"
                REBOOT+="NVBringup.kext on the EFI"
            else
                err "the copied kext's signature doesn't verify; restore from $bak"
            fi
            others=(${EFI:h}/EFI-backup-*(N/))
            (( ${#others} > 1 )) && note "${#others} EFI backups on ${EFI:h}; delete old ones you don't need"
        fi
        if ! grep -q '<string>NVBringup.kext</string>' $EFI/OC/config.plist 2>/dev/null; then
            TODO+="config.plist → Kernel → Add: BundlePath NVBringup.kext, ExecutablePath Contents/MacOS/NVBringup, PlistPath Contents/Info.plist, Arch x86_64, MinKernel 23.0.0, Enabled true (README step 3)"
        fi
    fi
    for a in nvfwsec=1 nvgsp=1; do
        [[ $BA == *" $a "* ]] || TODO+="boot-arg $a in config.plist (NVRAM → Add → boot-args)"
    done
fi

if has daemon; then
    step "Boot daemon (step 4)"
    if daemon_current; then
        ok "the installed daemon is current"
    elif run sudo tools/install_daemon.sh; then
        REBOOT+="boot daemon (or now: sudo build/nvgsp boot)"
    else
        err "install_daemon.sh failed"
    fi
fi

if has accel; then
    step "NVMetalAccel (step 7)"
    if ! run make -C accel; then
        err "make -C accel failed"
    elif [[ -d $ACCEL ]] && same_bundle accel/build/NVMetalAccel.kext $ACCEL; then
        ok "$ACCEL is current"
    elif run sudo rm -rf $ACCEL && run sudo cp -R accel/build/NVMetalAccel.kext /Library/Extensions/ &&
         run sudo chown -R root:wheel $ACCEL; then
        # rebuilds the auxiliary kext collection; a new kext asks for approval in System Settings
        run sudo kmutil load -p $ACCEL || note "kmutil load didn't finish (normal when an older copy is loaded): reboot"
        REBOOT+="NVMetalAccel (approve it in System Settings → Privacy & Security if asked)"
    else
        err "copying NVMetalAccel.kext failed"
    fi
    [[ $BA == *" nvaccel=1 "* ]] || TODO+="boot-arg nvaccel=1 in config.plist (NVMetalAccel stays off without it)"
fi

if has bundle; then
    step "NVMetal.bundle (step 7)"
    find_bundle
    sip=$(csrutil status 2>/dev/null); ar=$(csrutil authenticated-root status 2>/dev/null)
    if [[ -z $BUNDLE || ! -x $BUNDLE/Contents/MacOS/NVMetal ]]; then
        err "no NVMetal.bundle build found: build Mesa (step 5) or pass --bundle PATH"
    elif [[ -d $SYS_BUNDLE ]] && same_bundle $BUNDLE $SYS_BUNDLE; then
        ok "$SYS_BUNDLE is current (${BUNDLE/#$HOME/~})"
    elif [[ $sip != *"Filesystem Protections: disabled"* || $ar != *[Dd]isabled* ]]; then
        err "SIP doesn't allow changing the system volume"
        TODO+="csr-active-config 03080000 (0x803) in config.plist, reboot, then: tools/install.sh bundle"
    elif ! fdesetup status | grep -q "FileVault is Off"; then
        err "FileVault is on: turn it off, then tools/install.sh bundle"
    elif run sudo accel/tools/nvmetal_root_install.sh install $BUNDLE; then
        REBOOT+="NVMetal.bundle (a new system snapshot)"
    else
        err "nvmetal_root_install.sh failed"
    fi
    [[ -f /Library/Preferences/io.github.kvarun-p.nvmetal.allow ]] ||
        TODO+="let apps use Metal on the GPU: echo <app> | sudo tee -a /Library/Preferences/io.github.kvarun-p.nvmetal.allow"
fi

if has monitor; then
    step "GPU Monitor (step 8)"
    run monitor/build.sh || err "monitor/build.sh failed"
fi

# ---------------------------------------------------------------------------------------------
step "Summary"
(( NFAIL )) && print "  ${R}$NFAIL step(s) failed${N} (above)"
for t in $TODO; do print "  to do: $t"; done
if (( ${#REBOOT} )); then
    print "  reboot to load: ${(j:; :)REBOOT}"
    print "  then check: tools/verify_install.sh"
elif (( ! NFAIL )); then
    print "  nothing new to load; check: tools/verify_install.sh"
fi
(( NFAIL == 0 ))
