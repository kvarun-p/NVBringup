#!/bin/zsh
# Installs NVMetal.bundle where Metal finds the accelerator's MetalPluginName. libsystem_sandbox's
# gpu_bundle_find_trusted searches /Library/GPUBundles first, then /System/Library/Extensions, and WindowServer's
# sandbox may read the first (verified 2026-10-15: WindowServer mapped every library from there). So by default the
# bundle goes to /Library/GPUBundles, a plain copy: no SIP change, no system volume, no snapshot. With --system it
# goes into /System/Library/Extensions instead, the way OpenCore Legacy Patcher root-patches: mount the system
# volume read-write, change it, and bless a new (unsealed) snapshot to boot. Run with sudo.
#
#   sudo nvmetal_root_install.sh install [--system] [path/to/NVMetal.bundle]   embed its libraries and copy it in
#   sudo nvmetal_root_install.sh restore [--system] path/to/NVMetal.bundle      put back a copy of an installed bundle
#                                                    (from tools/nvctl's backups) as it is, without embedding again
#   sudo nvmetal_root_install.sh remove [--system]                              take it out again
#   sudo nvmetal_root_install.sh revert                                         boot Apple's last sealed snapshot
#                                                    (drops every system-volume change, a --system bundle included)
# Then reboot (WindowServer loads the bundle at start). /Library/GPUBundles is SIP-protected (rootless.conf class
# KernelExtensionManagement), so the default needs filesystem protection off (csr-active-config 0x2; 0x003 with the
# kext bit, verified 2026-10-15 with authenticated root on). --system also needs authenticated root off (0x803) and
# FileVault off. If both copies exist, the one in /Library/GPUBundles is used.
# Keep /Library/GPUBundles root-owned and not group- or world-writable: the trust check in that lookup passes any
# path, so whoever can write there can put code into WindowServer.
# Quick off switch without a reboot: sudo touch /Library/Preferences/io.github.kvarun-p.nvmetal.disabled
set -euo pipefail

DEFAULT_BUNDLE=/Volumes/NVBuild/build-nvk-main/src/nouveau/air/NVMetal.bundle
MNT=/private/var/nvmetal-livemount

die() { print -u2 "error: $*"; exit 1; }
[[ $EUID -eq 0 ]] || die "run with sudo"
cmd=${1:-}
shift $(( $# ? 1 : 0 ))
SYSTEM=0
if [[ ${1:-} == --system ]]; then
    SYSTEM=1
    shift
fi
# The installed bundle's path, and ROOT, where it is written: the mounted system volume for --system, else /.
if (( SYSTEM )); then
    DEST=System/Library/Extensions/NVMetal.bundle
    ROOT=$MNT
else
    DEST=Library/GPUBundles/NVMetal.bundle
    ROOT=""
fi

if [[ $cmd == revert ]]; then
    bless --mount / --bootefi --last-sealed-snapshot
    print "Blessed the last sealed snapshot. Reboot to finish."
    exit 0
fi
[[ $cmd == install || $cmd == restore || $cmd == remove ]] ||
    die "usage: $0 install [--system] [bundle] | restore [--system] bundle | remove [--system] | revert"

# ---- checks ---------------------------------------------------------------------------------------
if (( SYSTEM )); then
    csrutil status | grep -q "Filesystem Protections: disabled" ||
        die "SIP filesystem protection is on (csr-active-config needs 0x2; 0x803 with the kext bit)"
    csrutil authenticated-root status | grep -qi "disabled" ||
        die "authenticated root is on (csr-active-config needs 0x800)"
    fdesetup status | grep -q "FileVault is Off" || die "turn FileVault off first"
fi

if [[ $cmd == install ]]; then
    BUNDLE=${1:-$DEFAULT_BUNDLE}
    [[ -x $BUNDLE/Contents/MacOS/NVMetal ]] || die "no bundle at $BUNDLE"
    [[ $(plutil -extract NSPrincipalClass raw "$BUNDLE/Contents/Info.plist") == NVMetalDevice ]] ||
        die "$BUNDLE isn't NVMetal.bundle"
    ICD=${NVMETAL_ICD_SRC:-${BUNDLE:h:h}/vulkan/libvulkan_nouveau.dylib}   # build tree: src/nouveau/{air,vulkan}
fi
if [[ $cmd == restore ]]; then
    BUNDLE=${1:-}
    [[ -n $BUNDLE && -x $BUNDLE/Contents/MacOS/NVMetal ]] || die "no bundle at ${BUNDLE:-(none given)}"
    # Only a copy of a bundle installed at this target: its libraries are embedded and NVMetalICD points into it.
    [[ -d $BUNDLE/Contents/Frameworks && $(plutil -extract NVMetalICD raw "$BUNDLE/Contents/Info.plist" 2>/dev/null) == /$DEST/* ]] ||
        die "$BUNDLE isn't a copy of an installed NVMetal.bundle (use install for a build)"
    codesign --verify --deep --strict "$BUNDLE" || die "$BUNDLE doesn't verify"
fi

if (( SYSTEM )); then
    # The booted system is a snapshot of the System volume: /dev/diskNsMsK -> /dev/diskNsM.
    snap=$(mount | awk '$3 == "/" { print $1; exit }')
    [[ $snap =~ '^(/dev/disk[0-9]+s[0-9]+)s[0-9]+$' ]] || die "root ($snap) isn't a snapshot of a system volume"
    vol=$match[1]
    diskutil info "$vol" | grep -q "APFS Volume Group" || die "$vol doesn't look like an APFS system volume"
    print "root snapshot $snap, system volume $vol"

    # ---- mount, change, bless (below) ---------------------------------------------------------------
    mkdir -p $MNT
    if ! mount | grep -q " on $MNT "; then
        mount -o nobrowse -t apfs "$vol" $MNT
    fi
    trap 'umount $MNT 2>/dev/null || true' EXIT
    [[ -d $MNT/System/Library/Extensions ]] || die "$vol mounted at $MNT has no System/Library/Extensions"
else
    csrutil status | grep -q "Filesystem Protections: disabled" ||
        die "SIP filesystem protection is on: /Library/GPUBundles is SIP-protected (csr-active-config needs 0x2; 0x003 with the kext bit)"
    # /Library/GPUBundles ships with macOS, root:wheel 755; made if missing, and refused if others can write it.
    [[ -d /Library/GPUBundles ]] || { mkdir -p /Library/GPUBundles; chown root:wheel /Library/GPUBundles; chmod 755 /Library/GPUBundles }
    [[ $(stat -f '%Su %Lp' /Library/GPUBundles) == "root 755" ]] ||
        die "/Library/GPUBundles must be owned by root with mode 755 (is $(stat -f '%Su %Lp' /Library/GPUBundles))"
fi

# WindowServer enforces library validation even with AMFI relaxed: an ad-hoc signed library loads, an unsigned one
# doesn't (MacPorts' libLLVM: "mapped file has no cdhash", 2026-10-10). So the bundle carries its own copies of every
# non-system library it needs, in Contents/Frameworks, renamed to @rpath/<name> and ad-hoc signed, and its rpath is
# only @loader_path/../Frameworks. The build tree and /opt/local are left as they are.
# Only the bundle's own Frameworks on a library's rpath: its build-time ones (/opt/local, the build tree) would be searched
# first and their unsigned copies refused.
own_rpath() {   # $1 = library in Contents/Frameworks
    local r
    for r in ${(f)"$(otool -l $1 | awk '/LC_RPATH/ { getline; getline; print $2 }')"}; do
        install_name_tool -delete_rpath $r $1 2>/dev/null || true
    done
    install_name_tool -add_rpath @loader_path $1 2>/dev/null || true
}

embed_libs() {   # $1 = staged bundle
    local fw=$1/Contents/Frameworks bin=$1/Contents/MacOS/NVMetal
    local -a todo rpaths
    local -A done
    local obj dep name src r f
    rpaths=(${(f)"$(otool -l $bin | awk '/LC_RPATH/ { getline; getline; print $2 }')"})
    mkdir -p $fw
    todo=($bin)
    # The NVK ICD isn't linked: NVMetal dlopens it at run time from the bundle's NVMetalICD key, else the build tree
    # (library validation refused that copy in WindowServer, 2026-10-11 20:09). Embed it like the rest and point the
    # key at the installed copy.
    [[ -f $ICD ]] || die "no NVK ICD at $ICD (set NVMETAL_ICD_SRC)"
    cp -L $ICD $fw/${ICD:t}
    chmod 755 $fw/${ICD:t}
    install_name_tool -id @rpath/${ICD:t} $fw/${ICD:t} 2>/dev/null
    own_rpath $fw/${ICD:t}
    done[${ICD:t}]=1
    todo+=($fw/${ICD:t})
    plutil -replace NVMetalICD -string "/$DEST/Contents/Frameworks/${ICD:t}" $1/Contents/Info.plist
    print "embedded ${ICD:t} ($ICD), NVMetalICD set"
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
            own_rpath $fw/$name
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
    rm -rf "$ROOT/$DEST"
    stage=$(mktemp -d /private/var/tmp/nvmetal-stage.XXXXXX)
    ditto --norsrc --noextattr "$BUNDLE" "$stage/NVMetal.bundle"
    embed_libs "$stage/NVMetal.bundle"
    codesign --verify --deep --strict "$stage/NVMetal.bundle" || die "staged bundle doesn't verify"
    ditto --norsrc --noextattr "$stage/NVMetal.bundle" "$ROOT/$DEST"
    rm -rf "$stage"
    chown -R root:wheel "$ROOT/$DEST"
    chmod -R 755 "$ROOT/$DEST"
    print "copied $BUNDLE"
elif [[ $cmd == restore ]]; then
    rm -rf "$ROOT/$DEST"
    ditto --norsrc --noextattr "$BUNDLE" "$ROOT/$DEST"
    chown -R root:wheel "$ROOT/$DEST"
    chmod -R 755 "$ROOT/$DEST"
    print "restored $BUNDLE"
else
    [[ -e $ROOT/$DEST ]] || die "NVMetal.bundle isn't installed"
    rm -rf "$ROOT/$DEST"
    print "removed $DEST"
fi

if (( SYSTEM )); then
    bless --folder $MNT/System/Library/CoreServices --bootefi --create-snapshot
    print "Blessed a new snapshot with the change. Reboot to boot it."
    print "If anything goes wrong: sudo $0 revert (from Recovery: bless --mount /Volumes/<system> --bootefi --last-sealed-snapshot)"
else
    print "Reboot (or log out and sudo killall -HUP WindowServer) so WindowServer loads it."
    [[ -e /System/Library/Extensions/NVMetal.bundle ]] && [[ $cmd != remove ]] &&
        print "Note: an older copy is still in /System/Library/Extensions; /Library/GPUBundles is used first. sudo $0 revert drops it."
fi
