#!/bin/zsh
# Installs the GSP-RM boot daemon (run from the project: sudo tools/install_daemon.sh).
# Copies nvgsp, the boot script and the r570 firmware (GSP-RM, bootloader, booters) to
# /Library/Application Support/NVBringup (the system volume, available early at boot),
# and the LaunchDaemon plist. Takes effect at the next boot; nothing is started now.
set -eu
[[ $EUID -eq 0 ]] || { echo "run with sudo"; exit 1; }
cd "$(dirname "$0")/.."
DIR="/Library/Application Support/NVBringup"
FW=firmware/nvidia
# GSP-RM and its bootloader are shared per architecture (Turing: tu102/gsp, Ampere GA10x:
# ga102/gsp, Ada: ad102/gsp); the booters are signed per group (TU102/TU104/TU106: tu102,
# TU116/TU117: tu116) or per chip (GA10x and AD10x: the chip's own directory). Install
# whichever are present, and require the set this machine's GPU needs (NVChipset, if the
# kext is loaded). The table mirrors nv_chips in src/nv_hal.cpp.
chip=$(ioreg -r -c NVBringup -d 1 | awk -F' = ' '/"NVChipset"/{print $2; exit}')
arch=tu102
case "$chip" in
    354|356|358) need=tu102 ;;                  # 0x162 0x164 0x166
    359|360)     need=tu116 ;;                  # 0x167 0x168
    370) arch=ga102; need=ga102 ;;              # 0x172
    371) arch=ga102; need=ga103 ;;              # 0x173
    372) arch=ga102; need=ga104 ;;              # 0x174
    374) arch=ga102; need=ga106 ;;              # 0x176
    375) arch=ga102; need=ga107 ;;              # 0x177
    402) arch=ad102; need=ad102 ;;              # 0x192
    403) arch=ad102; need=ad103 ;;              # 0x193
    404) arch=ad102; need=ad104 ;;              # 0x194
    406) arch=ad102; need=ad106 ;;              # 0x196
    407) arch=ad102; need=ad107 ;;              # 0x197
    *)   need= ;;
esac
req=(build/nvgsp $FW/$arch/gsp/gsp-570.144.bin $FW/$arch/gsp/bootloader-570.144.bin)
[[ -n $need ]] && req+=($FW/$need/gsp/booter_load-570.144.bin $FW/$need/gsp/booter_unload-570.144.bin)
for f in $req; do
    [[ -f "$f" ]] || { echo "missing $f (run make first; firmware from linux-firmware, see README)"; exit 1; }
done
mkdir -p "$DIR/firmware" /Library/Logs/NVBringup
cp build/nvgsp "$DIR/nvgsp"
cp tools/daemon/gsp_boot.sh "$DIR/gsp_boot.sh"
for a in tu102 ga102 ad102; do
    [[ -f $FW/$a/gsp/gsp-570.144.bin ]] || continue
    mkdir -p "$DIR/firmware/$a/gsp"
    cp $FW/$a/gsp/gsp-570.144.bin $FW/$a/gsp/bootloader-570.144.bin "$DIR/firmware/$a/gsp/"
done
for g in tu102 tu116 ga102 ga103 ga104 ga106 ga107 ad102 ad103 ad104 ad106 ad107; do
    for f in booter_load booter_unload; do
        [[ -f $FW/$g/gsp/$f-570.144.bin ]] || continue
        mkdir -p "$DIR/firmware/$g/gsp"
        cp $FW/$g/gsp/$f-570.144.bin "$DIR/firmware/$g/gsp/"
    done
done
cp tools/daemon/com.nvbringup.gsp.plist /Library/LaunchDaemons/
chown -R root:wheel "$DIR" /Library/LaunchDaemons/com.nvbringup.gsp.plist
chmod 755 "$DIR" "$DIR/nvgsp" "$DIR/gsp_boot.sh"
chmod 644 /Library/LaunchDaemons/com.nvbringup.gsp.plist
echo "installed; GSP-RM boots automatically on the next USB boot with nvgsp=1."
echo "logs: /Library/Logs/NVBringup/boot-*.txt   uninstall: sudo tools/uninstall_daemon.sh"
