#!/usr/bin/env python3
"""verify_vtables.py <clang -fdump-vtable-layouts output>: checks each vtable slot of our classes against
the family class's vtable in this Mac's kernel collections. A slot we override must override the same
method (by name); every other slot must be the very function the family's vtable holds."""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kclib

OURS = {'NVMetalAccelerator': ('IOGraphicsAccelerator2', 'AppleParavirtAccelerator'),
        'NVMetalEventMachine': ('IOAccelEventMachineFast2', 'AppleParavirtEventMachine'),
        'NVMetalTask': ('IOAccelTask', 'AppleParavirtTask'),
        'NVMetalDisplayMachine': ('IOAccelDisplayMachine', 'AppleParavirtDisplayMachine')}

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
        if not ok:
            bad += 1
            print('%s slot %d: ours %s | kernel %s' % (ours, i, rows[i], names[i]))
    print('%s vs %s: %d slots %s' % (ours, fam, n, 'checked' if not bad else ''))
print('PASS' if not bad else 'FAIL (%d)' % bad)
sys.exit(1 if bad else 0)
