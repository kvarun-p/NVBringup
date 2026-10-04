"""kclib: read IOKit class layouts out of this Mac's kernel collections (x86_64, macOS 14).
vtable(cls) -> [(slot, symbol-or-None, level, addr)], class_size(cls), disasm(addr, n)."""
import struct, subprocess, functools
KCS = ['/System/Library/KernelCollections/BootKernelExtensions.kc',      # chained-pointer cacheLevel 0
       '/System/Library/KernelCollections/SystemKernelExtensions.kc']    # cacheLevel 1
LLVM_MC = '/opt/local/libexec/llvm-20/bin/llvm-mc'
class KC:
    def __init__(self, path):
        d = self.d = open(path, 'rb').read()
        ncmds = struct.unpack_from('<I', d, 16)[0]
        off = 32; self.entries = []; self.segs = []
        for _ in range(ncmds):
            cmd, csz = struct.unpack_from('<II', d, off)
            if cmd == 0x80000035:                                   # LC_FILESET_ENTRY
                vm, fo, eid = struct.unpack_from('<QQI', d, off + 8)
                self.entries.append((d[off + eid:d.index(b'\0', off + eid)].decode(), vm, fo))
            elif cmd == 0x19:                                       # LC_SEGMENT_64
                vm, vs, fo, fs = struct.unpack_from('<QQQQ', d, off + 24)
                self.segs.append((d[off+8:off+24].rstrip(b'\0').decode(), vm, vs, fo, fs))
            off += csz
        # chained pointer targets are offsets from the collection's mach header (file offset 0)
        # chained-pointer targets are offsets from the collection's base: the boot KC (kernel) is based at
        # 0xffffff8000100000 (found empirically: OSObject::release, __cxa_pure_virtual), the system KC symbols are already relative (0)
        self.base = 0xffffff8000100000 if self.segs[0][1] >> 63 else 0
        self.byaddr = {}; self.byname = {}
        for name, vm, fo in self.entries:
            nc = struct.unpack_from('<I', d, fo + 16)[0]; o = fo + 32
            for _ in range(nc):
                cmd, csz = struct.unpack_from('<II', d, o)
                if cmd == 2:                                        # LC_SYMTAB
                    symoff, nsyms, stroff, _ = struct.unpack_from('<IIII', d, o + 8)
                    for i in range(nsyms):
                        nx, ty, sect, desc, val = struct.unpack_from('<IBBHQ', d, symoff + 16 * i)
                        if (ty & 0x0e) == 0x0e and val:
                            s = d[stroff + nx:d.index(b'\0', stroff + nx)].decode(errors='replace')
                            self.byname.setdefault(s, (val, name, bool(ty & 1)))
                            # prefer external names for an address
                            if val not in self.byaddr or (ty & 1 and not self.byaddr[val][2]): self.byaddr[val] = (s, name, bool(ty & 1))
                o += csz
    def off(self, addr):
        for s in self.segs:
            if s[1] <= addr < s[1] + s[4]: return s[3] + addr - s[1]
        raise KeyError(hex(addr))
    def bytes(self, addr, n): o = self.off(addr); return self.d[o:o + n]
@functools.lru_cache(None)
def kcs(): return [KC(p) for p in KCS]
def lookup(sym):
    for lvl, k in enumerate(kcs()):
        if sym in k.byname: v, e, ext = k.byname[sym]; return lvl, v, e, ext
    return None
def name_at(lvl, addr):
    r = kcs()[lvl].byaddr.get(addr); return r
def vtable_by_symbol(vt):
    r = lookup(vt)
    if not r: return None
    lvl, a, e, _ = r; k = kcs()[lvl]; o = k.off(a) + 16
    out = []; i = 0
    while True:
        v = struct.unpack_from('<Q', k.d, o + 8 * i)[0]
        if v == 0: break
        tl = (v >> 30) & 3; ta = kcs()[tl].base + (v & 0x3fffffff)
        if lvl == 0 and tl == 0: ta -= 0x100000        # the boot KC's own pointers count from 0xffffff8000000000
        n = name_at(tl, ta)
        out.append((i, n[0] if n else None, tl, ta, n[2] if n else False))
        i += 1
    return out
def vtable(cls): return vtable_by_symbol('__ZTV%d%s' % (len(cls), cls))
def disasm(lvl, addr, n=64):
    b = kcs()[lvl].bytes(addr, n)
    r = subprocess.run([LLVM_MC, '--disassemble', '-triple=x86_64-apple-macos', '--output-asm-variant=1'],
                       input=' '.join('0x%02x' % x for x in b), capture_output=True, text=True)
    return [l.strip() for l in r.stdout.splitlines() if l.strip() and not l.strip().startswith('.')]
def class_size(cls):
    """the size argument of OSMetaClass::OSMetaClass(name, super, size) in Class::MetaClass::MetaClass()"""
    r = lookup('__ZN%d%s9MetaClassC2Ev' % (len(cls), cls))
    if not r: return None
    import re
    for l in disasm(r[0], r[1], 48):
        m = re.match(r'mov\s+ecx, (0x[0-9a-f]+|\d+)', l)
        if m: return int(m.group(1), 0)
    return None
def demangle(names):
    out = subprocess.run(['c++filt'], input='\n'.join(n or '' for n in names), capture_output=True, text=True).stdout.split('\n')
    return out[:len(names)]
