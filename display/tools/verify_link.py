#!/usr/bin/env python3
"""verify_link.py <object>...: every C++ symbol the objects import must exist in this Mac's kernel collections (kclib reads
their symbol tables). `kmutil libraries --undef-symbols` lists only the imports that resolve, so it can't show a misspelt
one: a mangled name with the wrong parameter types (unsigned long vs unsigned long long) passes it and then fails to load.
C names (the kpi: _IOLog, _thread_call_*) aren't in the collections and are left to kmutil."""
import os, subprocess, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'accel', 'tools'))
import kclib

bad = total = 0
for obj in sys.argv[1:]:
    out = subprocess.run(['nm', '-arch', 'x86_64', '-u', obj], capture_output=True, text=True).stdout
    for line in out.split('\n'):
        sym = line.strip()
        if not sym.startswith('__ZN'):
            continue
        total += 1
        if kclib.lookup(sym) is None:
            print('%s: %s is not in the kernel collections' % (os.path.basename(obj), sym))
            bad += 1
print('%d C++ imports checked: %s' % (total, 'PASS' if not bad else 'FAIL (%d)' % bad))
sys.exit(1 if bad else 0)
