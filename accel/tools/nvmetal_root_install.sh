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

# WindowServer enforces library validation even with AMFI relaxed: an ad-hoc signed library loads, an unsigned one
# doesn't (MacPorts' libLLVM: "mapped file has no cdhash", 2026-10-10). So the bundle carries its own copies of every
# non-system library it needs, in Contents/Frameworks, renamed to @rpath/<name> and ad-hoc signed, and its rpath is
# only @loader_path/../Frameworks. The build tree and /opt/local are left as they are.
embed_libs() {   # $1 = staged bundle
    local fw=$1/Contents/Frameworks bin=$1/Contents/MacOS/NVMetal
    local -a todo rpaths
    local -A done
    local obj dep name src r f
    rpaths=(${(f)"$(otool -l $bin | awk '/LC_RPATH/ { getline; getline; print $2 }')"})
    mkdir -p $fw
    todo=($bin)
    while (( $#todo )); do
        obj=$todo[1]; todo=(${todo[2,-1]})
        for dep in ${(f)"$(otool -L $obj | tail -n +2 | awk '{ print $1 }')"}; do
            [[ $dep == /usr/lib/* || $dep == /System/* ]] && continue
            name=${dep:t}; src=""
            if [[ $dep == @rpath/* ]]; then
                for r in $rpaths; do
                    r=${r/@loader_path/${BUNDLE}/Contents/MacOS}
                    [[ -f $r/$name ]] && { src=$r/$name; break }
                done
            else
                src=$dep
            fi
            [[ $obj == $bin || $dep != @rpath/* ]] && install_name_tool -change $dep @rpath/$name $obj 2>/dev/null
            [[ -n ${done[$name]:-} ]] && continue
            [[ -f $src ]] || die "can't find $dep (needed by ${obj:t})"
            cp -L $src $fw/$name
            chmod 755 $fw/$name
            install_name_tool -id @rpath/$name $fw/$name 2>/dev/null
            install_name_tool -add_rpath @loader_path $fw/$name 2>/dev/null || true
            done[$name]=1
            todo+=($fw/$name)
            print "embedded $name ($src)"
        done
    done
    for r in $rpaths; do install_name_tool -delete_rpath $r $bin 2>/dev/null; done
    install_name_tool -add_rpath @loader_path/../Frameworks $bin 2>/dev/null
    for f in $fw/*.dylib; do codesign --force --sign - $f; done
    codesign --force --sign - $1
}

if [[ $cmd == install ]]; then
    rm -rf "$MNT/$DEST"
    stage=$(mktemp -d /private/var/tmp/nvmetal-stage.XXXXXX)
    ditto --norsrc --noextattr "$BUNDLE" "$stage/NVMetal.bundle"
    embed_libs "$stage/NVMetal.bundle"
    codesign --verify --deep --strict "$stage/NVMetal.bundle" || die "staged bundle doesn't verify"
    ditto --norsrc --noextattr "$stage/NVMetal.bundle" "$MNT/$DEST"
    rm -rf "$stage"
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
