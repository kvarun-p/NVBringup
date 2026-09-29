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
# GSP-RM and its bootloader serve every Turing chip; the booters are signed per group:
# TU102/TU104/TU106 use tu102/gsp, TU116/TU117 tu116/gsp. Install whichever are present,
# and require the set this machine's GPU needs (NVChipset, if the kext is loaded).
chip=$(ioreg -r -c NVBringup -d 1 | awk -F' = ' '/"NVChipset"/{print $2; exit}')
case "$chip" in
    354|356|358) need=tu102 ;;      # 0x162 0x164 0x166
    359|360)     need=tu116 ;;      # 0x167 0x168
    *)           need= ;;
esac
req=(build/nvgsp $FW/tu102/gsp/gsp-570.144.bin $FW/tu102/gsp/bootloader-570.144.bin)
[[ -n $need ]] && req+=($FW/$need/gsp/booter_load-570.144.bin $FW/$need/gsp/booter_unload-570.144.bin)
for f in $req; do
    [[ -f "$f" ]] || { echo "missing $f (run make first; firmware from linux-firmware, see README)"; exit 1; }
done
mkdir -p "$DIR/firmware/tu102/gsp" "$DIR/firmware/tu116/gsp" /Library/Logs/NVBringup
cp build/nvgsp "$DIR/nvgsp"
cp tools/daemon/gsp_boot.sh "$DIR/gsp_boot.sh"
cp $FW/tu102/gsp/gsp-570.144.bin $FW/tu102/gsp/bootloader-570.144.bin "$DIR/firmware/tu102/gsp/"
for g in tu102 tu116; do
    for f in booter_load booter_unload; do
        [[ -f $FW/$g/gsp/$f-570.144.bin ]] && cp $FW/$g/gsp/$f-570.144.bin "$DIR/firmware/$g/gsp/"
    done
done
cp tools/daemon/com.nvbringup.gsp.plist /Library/LaunchDaemons/
chown -R root:wheel "$DIR" /Library/LaunchDaemons/com.nvbringup.gsp.plist
chmod 755 "$DIR" "$DIR/nvgsp" "$DIR/gsp_boot.sh"
chmod 644 /Library/LaunchDaemons/com.nvbringup.gsp.plist
echo "installed; GSP-RM boots automatically on the next USB boot with nvgsp=1."
echo "logs: /Library/Logs/NVBringup/boot-*.txt   uninstall: sudo tools/uninstall_daemon.sh"
