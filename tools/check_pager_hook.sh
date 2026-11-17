#!/bin/bash
# Checks what NVPagerHook.kext (pagerhook/) needs and whether NVBringup is using it. Run on the Mac.
#   tools/check_pager_hook.sh            symbol, then status
#   tools/check_pager_hook.sh symbol     are _device_pager_last_unmap and the I/O check's symbols
#                                        (_device_pager_ops/_reference/_deallocate) in this kernel's symbol table,
#                                        and is it the build the VM object offsets were checked on? Lilu resolves
#                                        them from there; without them the hook can't work on this macOS build.
#                                        Needs no kext: run it before building anything.
#   tools/check_pager_hook.sh status     Lilu and NVPagerHook loaded? NVBringup's hook state and counters.
# Exit status: 0 usable / active, 1 not, 2 could not tell.
set -u
what=${1:-all}
rc=0

symbol() {
    local k=/System/Library/Kernels/kernel
    local ver; ver=$(uname -r)
    echo "kernel: $(uname -v | sed 's/.*\(xnu-[0-9.~]*\).*/\1/') (Darwin $ver)"
    if [ ! -r "$k" ]; then
        echo "symbol: $k not readable: can't tell"
        return 2
    fi
    local syms; syms=$(nm "$k" 2>/dev/null)
    local line
    line=$(grep -E ' _device_pager_last_unmap$' <<<"$syms")
    if [ -z "$line" ]; then
        echo "symbol: _device_pager_last_unmap is NOT in $k: NVPagerHook can't route it on this build"
        return 1
    fi
    echo "symbol: $line"
    # The I/O check (interface version 2) needs these too; without any of them NVPagerHook routes last_unmap
    # but NVBringup never releases a slice early.
    local s miss=0
    for s in _device_pager_ops _device_pager_reference _device_pager_deallocate; do
        line=$(grep -E " $s\$" <<<"$syms")
        if [ -n "$line" ]; then
            echo "symbol: $line"
        else
            echo "symbol: $s is NOT in $k: no I/O check, so no early release on this build"
            miss=1
        fi
    done
    [ $miss -eq 1 ] && return 1
    # The VM object offsets were only checked on Darwin 23.6 (NVPagerHook.cpp); elsewhere it needs -nvpgrlayout.
    case $ver in
        23.6.*|23.[7-9].*) ;;
        *) echo "layout: VM object offsets not checked on Darwin $ver: NVPagerHook turns the I/O check off" \
                "unless booted with -nvpgrlayout (then only the self-test guards it)"
           return 1 ;;
    esac
    echo "layout: Darwin $ver, the build the VM object offsets were checked on"
    return 0
}

status() {
    local r=0
    if kextstat 2>/dev/null | grep -q 'as.vit9696.Lilu'; then
        echo "Lilu: loaded ($(kextstat 2>/dev/null | awk '/as.vit9696.Lilu/ {print $7}' | tr -d '()'))"
    else
        echo "Lilu: not loaded (config.plist → Kernel → Add, before its plugins)"; r=1
    fi
    if kextstat 2>/dev/null | grep -q 'nvpagerhook'; then
        echo "NVPagerHook: loaded"
    else
        echo "NVPagerHook: not loaded (config.plist → Kernel → Add, after Lilu.kext)"; r=1
    fi
    case " $(sysctl -n kern.bootargs) " in
        *" -nvpgroff "*)   echo "boot-arg -nvpgroff: NVPagerHook is disabled"; r=1 ;;
    esac
    case " $(sysctl -n kern.bootargs) " in
        *" nvbar1hook=0 "*) echo "boot-arg nvbar1hook=0: NVBringup ignores NVPagerHook"; r=1 ;;
    esac
    local stats
    stats=$(ioreg -r -c NVBringup -d 1 2>/dev/null | grep '"NVStats"')
    if [ -z "$stats" ]; then
        echo "NVBringup: no NVStats (not loaded, or the GPU hasn't booted yet)"
        return 2
    fi
    get() { echo "$stats" | grep -oE "\"$1\"=[0-9]+" | cut -d= -f2; }
    local p2p; p2p=$(ioreg -r -c NVBringup -d 1 2>/dev/null | grep -oE '"NVP2P" = "[^"]*"' | cut -d'"' -f4)
    echo "P2P:                ${p2p:-no NVP2P property (built before the peer-to-peer check)}: release-dio needs it allowed"
    local refused; refused=$(get VramMapRefused)
    if [ -n "$refused" ]; then
        echo "VRAM maps refused:  $refused (no peer-to-peer: no process gets a CPU mapping of VRAM; boot-arg nvvrammap=1 allows them)"
    fi
    local revoked; revoked=$(get VramMapRevoked)
    if [ -n "$revoked" ] && [ "$revoked" != 0 ]; then
        echo "VRAM maps revoked:  $revoked (taken back when a disk without peer-to-peer support came)"
    fi
    local st; st=$(get Bar1Hook)
    case "${st:-}" in
        0) echo "hook: none (NVBringup never got NVPagerHook's ops)"; r=1 ;;
        1) echo "hook: attached; the self-test runs at the first CPU mapping of VRAM (run build/bar1_alias_test release-none)"; [ $r -eq 0 ] && r=2 ;;
        2) echo "hook: active" ;;
        3) echo "hook: FAILED its self-test or isn't routed: slices stay held (see: log show --last boot | grep 'BAR1 hook')"; r=1 ;;
        4) echo "hook: off (nvbar1hook=0)"; r=1 ;;
        *) echo "hook: this NVBringup has no Bar1Hook statistic (built before the hook)"; r=1 ;;
    esac
    local held rel n
    held=$(get Bar1Held); rel=$(get Bar1Released); n=$(get Bar1ReleasedSlices)
    echo "Bar1Held:           $(( ${held:-0} >> 10 )) KiB held or promised to live CPU mappings"
    echo "Bar1Released:       $(( ${rel:-0} >> 10 )) KiB in ${n:-0} slices given back early"
    local def unk; def=$(get Bar1IoDeferred); unk=$(get Bar1IoUnknown)
    if [ -n "$def" ]; then
        echo "Bar1IoDeferred:     ${def:-0} releases waited for I/O in flight on the pages"
    else
        echo "Bar1IoDeferred:     missing: this NVBringup has no I/O check (NVPagerHook version 1): don't run release-dio"; r=1
    fi
    if [ "${unk:-0}" != 0 ]; then
        echo "Bar1IoUnknown:      $unk reports couldn't be checked for I/O: those slices stay held (see: log show --last boot | grep 'BAR1 hook')"; r=1
    fi
    local early; early=$(get Bar1HookEarly)
    if [ "${early:-0}" != 0 ]; then
        echo "Bar1HookEarly:      $early: a pager reported before its memory was freed. The kernel doesn't count references the way nv_pager_hook.h says: report this, with the macOS build"; r=1
    fi
    return $r
}

case "$what" in
    symbol) symbol; rc=$? ;;
    status) status; rc=$? ;;
    all)    symbol; rc=$?; echo; status; s=$?; [ $s -gt $rc ] && [ $rc -ne 1 ] && rc=$s; [ $s -eq 1 ] && rc=1 ;;
    *)      echo "usage: $0 [symbol|status]" >&2; rc=2 ;;
esac
exit $rc
