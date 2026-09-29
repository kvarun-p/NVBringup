#!/usr/bin/env python3
"""Save the VBIOS that NVBringup.kext read to a file.

The kext publishes the ROM as the NVVBIOS registry property; this pulls it out
via `ioreg` so it can be analyzed with build/vbios_tool or other VBIOS tools.

usage: tools/dump_vbios.py [output.rom]   (default: vbios.rom)
"""
import plistlib
import subprocess
import sys

out = sys.argv[1] if len(sys.argv) > 1 else "vbios.rom"
raw = subprocess.run(["ioreg", "-a", "-r", "-c", "NVBringup", "-d", "1"],
                     capture_output=True, check=True).stdout
if not raw.strip():
    sys.exit("NVBringup is not loaded (no NVBringup object in the IORegistry)")

props = plistlib.loads(raw)[0]
rom = props.get("NVVBIOS")
if not rom:
    sys.exit("NVBringup is loaded but has no NVVBIOS property; check its log")

with open(out, "wb") as f:
    f.write(rom)
print(f"wrote {len(rom)} bytes from {props.get('NVVBIOSSource', '?')} to {out}")
