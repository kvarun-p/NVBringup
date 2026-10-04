#!/bin/zsh
# Installs NVMetal.bundle into /System/Library/Extensions, where Metal.framework looks for the
# accelerator's MetalPluginName, the way OpenCore Legacy Patcher root-patches: mount the system
# volume read-write, change it, and bless a new (unsealed) snapshot to boot. Run with sudo.
#
#   sudo nvmetal_root_install.sh install [path/to/NVMetal.bundle]   copy it in, bless; then reboot
#   sudo nvmetal_root_install.sh remove                              take it out again, bless; then reboot
#   sudo nvmetal_root_install.sh revert                              boot Apple's last sealed snapshot
#                                                                     (drops every root change); then reboot
# Needs csr-active-config 0x803 (unrestricted filesystem + unauthenticated root) and FileVault off.
# Quick off switch without a reboot: sudo touch /Library/Preferences/io.github.kvarun-p.nvmetal.disabled
set -euo pipefail

DEFAULT_BUNDLE=/Volumes/NVBuild/build-nvk-main/src/nouveau/air/NVMetal.bundle
MNT=/private/var/nvmetal-livemount
DEST=System/Library/Extensions/NVMetal.bundle

die() { print -u2 "error: $*"; exit 1; }
[[ $EUID -eq 0 ]] || die "run with sudo"
cmd=${1:-}

if [[ $cmd == revert ]]; then
    bless --mount / --bootefi --last-sealed-snapshot
    print "Blessed the last sealed snapshot. Reboot to finish."
    exit 0
fi
[[ $cmd == install || $cmd == remove ]] || die "usage: $0 install [bundle] | remove | revert"

# ---- checks ---------------------------------------------------------------------------------------
csrutil status | grep -q "Filesystem Protections: disabled" ||
    die "SIP filesystem protection is on (csr-active-config needs 0x2; 0x803 with the kext bit)"
csrutil authenticated-root status | grep -qi "disabled" ||
    die "authenticated root is on (csr-active-config needs 0x800)"
fdesetup status | grep -q "FileVault is Off" || die "turn FileVault off first"

if [[ $cmd == install ]]; then
    BUNDLE=${2:-$DEFAULT_BUNDLE}
    [[ -x $BUNDLE/Contents/MacOS/NVMetal ]] || die "no bundle at $BUNDLE"
    [[ $(plutil -extract NSPrincipalClass raw "$BUNDLE/Contents/Info.plist") == NVMetalDevice ]] ||
        die "$BUNDLE isn't NVMetal.bundle"
fi

# The booted system is a snapshot of the System volume: /dev/diskNsMsK -> /dev/diskNsM.
snap=$(mount | awk '$3 == "/" { print $1; exit }')
[[ $snap =~ '^(/dev/disk[0-9]+s[0-9]+)s[0-9]+$' ]] || die "root ($snap) isn't a snapshot of a system volume"
vol=$match[1]
diskutil info "$vol" | grep -q "APFS Volume Group" || die "$vol doesn't look like an APFS system volume"
print "root snapshot $snap, system volume $vol"

# ---- mount, change, bless -------------------------------------------------------------------------
mkdir -p $MNT
if ! mount | grep -q " on $MNT "; then
    mount -o nobrowse -t apfs "$vol" $MNT
fi
trap 'umount $MNT 2>/dev/null || true' EXIT
[[ -d $MNT/System/Library/Extensions ]] || die "$vol mounted at $MNT has no System/Library/Extensions"

if [[ $cmd == install ]]; then
    rm -rf "$MNT/$DEST"
    ditto --norsrc --noextattr "$BUNDLE" "$MNT/$DEST"
    chown -R root:wheel "$MNT/$DEST"
    chmod -R 755 "$MNT/$DEST"
    print "copied $BUNDLE"
else
    [[ -e $MNT/$DEST ]] || die "NVMetal.bundle isn't installed"
    rm -rf "$MNT/$DEST"
    print "removed $DEST"
fi

bless --folder $MNT/System/Library/CoreServices --bootefi --create-snapshot
print "Blessed a new snapshot with the change. Reboot to boot it."
print "If anything goes wrong: sudo $0 revert (from Recovery: bless --mount /Volumes/<system> --bootefi --last-sealed-snapshot)"
