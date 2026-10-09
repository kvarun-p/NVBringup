#!/usr/bin/env python3
"""verify_vtables.py <clang -fdump-vtable-layouts output>: checks each vtable slot of our classes against
the family class's vtable in this Mac's kernel collections. A slot we override must override the same
method (by name); every other slot must be the very function the family's vtable holds."""
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'accel', 'tools'))
import kclib

# ours -> (family class in the kernel, a kernel class that implements the family's pure slots)
OURS = {'NVFramebuffer': ('IOFramebuffer', 'IONDRVFramebuffer'),
        'NVAGDCControl': ('AppleGraphicsDeviceControl', 'IntelFBClientControl')}

# typedefs our headers use for the kernel's types: a parameter list is compared after these are spelled out
TYPEDEFS = {'UInt32': 'unsigned int', 'IOOptionBits': 'unsigned int', 'SInt32': 'int', 'UInt64': 'unsigned long long',
            'task_t': 'task*'}

def canon(params):
    p = re.sub(r'\s+', ' ', params)
    for k, v in TYPEDEFS.items():
        p = re.sub(r'\b%s\b' % k, v, p)
    return re.sub(r'\s+', '', p)

def qualname(s):
    """'bool IOService::start(IOService *) const' -> ('IOService', 'start', 'IOService*')"""
    s = s.replace(' [complete]', '').replace(' [deleting]', '')
    m = re.search(r'([A-Za-z0-9_]+)::(~?[A-Za-z0-9_]+)\((.*)\)', s)
    return (m.group(1), m.group(2), re.sub(r'\s+', '', m.group(3))) if m else (None, s, '')

text = open(sys.argv[1]).read()
pure = kclib.lookup('___cxa_pure_virtual')[1]
bad = 0
for ours, (fam, pv) in OURS.items():
    seg = text[text.index("Vtable for '%s'" % ours):]
    seg = seg[:seg.index('\n\n')]
    rows = {}
    for l in seg.split('\n'):
        m = re.match(r'\s+(\d+) \| (.*)$', l)
        if m and int(m.group(1)) >= 2 and not m.group(2).startswith(('vcall_offset', 'vbase_offset')):
            rows[int(m.group(1)) - 2] = m.group(2)
    vt = kclib.vtable(fam)
    names = kclib.demangle([x[1] for x in vt])
    pvn = kclib.demangle([x[1] for x in kclib.vtable(pv)])
    n = len(vt)
    strict = fam == 'AppleGraphicsDeviceControl'     # the family whose parameter lists are checked (see below)
    pv_cls = pv                                      # the class whose overrides supply the family's pure slots
    if len(rows) != n:
        print('%s: %d slots, %s has %d' % (ours, len(rows), fam, n)); bad += 1
    for i in range(min(n, len(rows))):
        oc, om, oa = qualname(rows[i])
        fc, fm, fa = qualname(names[i] if vt[i][3] != pure else pvn[i])
        if oc == ours:
            ok = om == fm or (om.startswith('~') and fm.startswith('~'))
        else:
            ok = (oc, om) == (fc, fm) and vt[i][3] != pure
            # a forwarder (a slot the family doesn't export) tail-jumps through this very slot
            ok = ok or (om == fm and not vt[i][4] and vt[i][3] != pure)
        # AGDC's slots are also checked by parameter list: a parameter of the wrong type mangles to a symbol the kernel
        # doesn't export (unsigned long long for unsigned long), and nothing else here would notice.
        if strict and ok and not om.startswith('~') and fc in (fam, pv_cls):
            ok = canon(oa) == canon(fa)
        if not ok:
            bad += 1
            print('%s slot %d: ours %s | kernel %s' % (ours, i, rows[i], names[i]))
    print('%s vs %s: %d slots %s' % (ours, fam, n, 'checked' if not bad else ''))
print('PASS' if not bad else 'FAIL (%d)' % bad)
sys.exit(1 if bad else 0)
