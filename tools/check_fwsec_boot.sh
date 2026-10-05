#!/bin/bash
# Does the FWSEC v3 (GA10x/AD10x) image in a VBIOS use IMEMVirtBase 0?
#
# The Ampere/Ada HAL boots FWSEC with BOOTVEC and the IMEM tag base both set to
# IMEMVirtBase (as NVIDIA's driver does). nouveau uses vector 0 and image offset 0.
# The two agree only if IMEMVirtBase is 0. This reads the descriptor out of a VBIOS
# and says which case you have. Runs anywhere vbios_tool builds; no GPU access needed.
#
# usage: tools/check_fwsec_boot.sh [vbios.rom]   (VBIOS_TOOL=path skips the make step)
#   no argument: dump the ROM the loaded kext read (tools/dump_vbios.py, macOS only)
set -e
cd "$(dirname "$0")/.."
ROM="$1"
if [ -z "$ROM" ]; then
    ROM="$(mktemp -t vbios.XXXXXX)"
    python3 tools/dump_vbios.py "$ROM"
fi
TOOL="${VBIOS_TOOL:-build/vbios_tool}"
[ -n "$VBIOS_TOOL" ] || make build/vbios_tool >/dev/null
OUT="$($TOOL "$ROM")"
echo "$OUT" | grep -E "^FWSEC|IMEM|boot vector|PKC|IMEMVirtBase"
LINE="$(echo "$OUT" | grep IMEMVirtBase || true)"
if [ -z "$LINE" ]; then
    echo "no FWSEC v3 descriptor in this ROM (Turing or earlier, or the parse failed): nothing to check"
    exit 2
fi
VIRT="$(echo "$LINE" | sed -E 's/.*IMEMVirtBase (0x[0-9a-f]+),.*/\1/')"
if [ $((VIRT)) -eq 0 ]; then
    echo "OK: IMEMVirtBase is 0, so the HAL's boot vector matches nouveau's"
else
    echo "DIFFERS: IMEMVirtBase is $VIRT; the HAL boots at $VIRT, nouveau at 0."
    echo "         Check which one the FWSEC image really expects before the first boot."
    exit 1
fi
