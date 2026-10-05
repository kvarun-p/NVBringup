#!/usr/bin/env python3
"""Decode the GA100+ device topology (PTOP) table and report the PMC reset bits.

nouveau resets SEC2 with a PMC toggle (device-enable register 0x600, bit = the `reset`
field below) in addition to the ENGINE reset; the GSP is reset without one. The bit is
not a constant: it comes from this table, which only the GPU can tell you. This tool
decodes it the way nouveau's ga100_top_parse does, for GA10x and AD10x.

Two sources:
  live      Linux, root, any driver state: maps BAR0 through sysfs and reads 0x224fc,
            0x22800.. and 0x600. Do not run it while a driver is mid-reset.
              tools/ptop_dump.py                      (first NVIDIA GPU found)
              tools/ptop_dump.py 0000:01:00.0
  file      a register dump, one "address value" pair of hex numbers per line (the
            registers 0x224fc, 0x22800 up and, if you have it, 0x600):
              tools/ptop_dump.py --regs dump.txt

Prints every table entry, and for SEC2 (type 0x0d) and GSP (type 0x14) the reset bit and
the mask 1 << reset.
"""
import glob
import mmap
import os
import struct
import sys

TYPES = {0x00: "GR", 0x0d: "SEC2", 0x0e: "NVENC", 0x10: "NVDEC", 0x12: "IOCTRL",
         0x13: "CE", 0x14: "GSP", 0x15: "NVJPG", 0x16: "OFA", 0x17: "FLA"}
SIZE_REG, TABLE, PMC_DEVICE_ENABLE = 0x224fc, 0x22800, 0x600


def parse(rd):
    """nouveau ga100_top_parse: returns a list of device dicts."""
    size = rd(SIZE_REG) >> 20
    devs, info, n = [], None, 0
    for i in range(size):
        if info is None:
            info, typ, inst, n = {"reset": None, "addr": 0, "fault": 0}, ~0, 0, 0
        data = rd(TABLE + 4 * i)
        if not data and n == 0:
            continue
        if n == 0:
            typ, inst = (data >> 24) & 0x3f, (data >> 16) & 0xf
            info["fault"] = data & 0x7f
        elif n == 1:
            info["addr"], info["reset"] = data & 0x00fff000, data & 0x1f
        n += 1
        if data & 0x80000000:
            continue
        n = 0
        info.update(type=typ, inst=inst, name=TYPES.get(typ, "?"))
        devs.append(info)
        info = None
    return size, devs


def bar0_reader(bdf):
    if bdf is None:
        for d in sorted(glob.glob("/sys/bus/pci/devices/*")):
            try:
                if open(d + "/vendor").read().strip() == "0x10de" and \
                   open(d + "/class").read().strip().startswith("0x03"):
                    bdf = os.path.basename(d)
                    break
            except OSError:
                pass
        if bdf is None:
            sys.exit("no NVIDIA display device under /sys/bus/pci/devices")
    path = f"/sys/bus/pci/devices/{bdf}/resource0"
    fd = os.open(path, os.O_RDONLY)
    mm = mmap.mmap(fd, 0x240000, mmap.MAP_SHARED, mmap.PROT_READ)
    print(f"reading BAR0 of {bdf} (BOOT_0 0x{struct.unpack_from('<I', mm, 0)[0]:08x})")
    return lambda a: struct.unpack_from("<I", mm, a)[0]


def file_reader(path):
    regs = {}
    for line in open(path):
        line = line.split("#")[0].split()
        if len(line) >= 2:
            regs[int(line[0], 16)] = int(line[1], 16)
    if SIZE_REG not in regs:
        sys.exit(f"{path}: no entry for 0x{SIZE_REG:x}")
    return lambda a: regs.get(a, 0)


def main(argv):
    if len(argv) >= 2 and argv[0] == "--regs":
        rd = file_reader(argv[1])
    elif len(argv) <= 1 and not (argv and argv[0].startswith("-")):
        rd = bar0_reader(argv[0] if argv else None)
    else:
        sys.exit(__doc__)
    size, devs = parse(rd)
    print(f"topology: {size} words, {len(devs)} devices")
    for d in devs:
        rst = "none" if d["reset"] is None else f"{d['reset']:2d}"
        print(f"  type 0x{d['type']:02x} {d['name']:6s} inst {d['inst']}  addr 0x{d['addr']:06x}  fault {d['fault']:3d}  reset bit {rst}")
    status = 0
    for name, want in (("SEC2", 0x0d), ("GSP", 0x14)):
        hit = [d for d in devs if d["type"] == want and d["inst"] == 0 and d["reset"] is not None]
        if hit:
            print(f"{name}: PMC reset bit {hit[0]['reset']} -> mask 0x{1 << hit[0]['reset']:x} in register 0x{PMC_DEVICE_ENABLE:x}")
        else:
            print(f"{name}: no reset bit in the table")
            status = 1
    en = rd(PMC_DEVICE_ENABLE)
    print(f"register 0x{PMC_DEVICE_ENABLE:x} reads 0x{en:08x}")
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
