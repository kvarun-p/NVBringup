#!/bin/zsh
# Fails if any symbol the kext imports resolves to a KPI bundle (com.apple.kpi.*) its OSBundleLibraries doesn't
# list: kmutil then leaves the kext out of the auxiliary collection without an error. (Family kexts, such as
# IOPCIFamily through IOAcceleratorFamily2, resolve through the listed families' own dependencies.) (NVMetalAccel with thread_call_cancel_wait,
# com.apple.kpi.private, 2026-10-12: no accelerator, no display pipe, HDMI without signal).
set -euo pipefail
kext=$1
listed=(${(f)"$(/usr/libexec/PlistBuddy -c 'Print :OSBundleLibraries' $kext/Contents/Info.plist | awk -F' = ' '/=/ {gsub(/^ +/, "", $1); print $1}')"})
used=(${(f)"$(kmutil libraries -p $kext --undef-symbols 2>&1 | grep -oE ': [a-z0-9.A-Z-]+ \(' | sed 's/^: //; s/ ($//' | sort -u)"})
bad=0
for b in $used; do
    [[ $b == com.apple.kpi.* ]] || continue
    (( ${listed[(Ie)$b]} )) || { print -u2 "$kext: imports from $b, which OSBundleLibraries doesn't list"; bad=1; }
done
(( bad )) && exit 1
print "$kext: every KPI import is from a listed library (${#used} bundles)"
