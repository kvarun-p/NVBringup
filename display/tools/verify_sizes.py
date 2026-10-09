#!/usr/bin/env python3
"""verify_sizes.py <clang -fdump-record-layouts output>: checks the sizes of the classes NVAGDCControl.cpp lays out
against this Mac's kernel collections (the classSize each class's MetaClass constructor registers, read by kclib):
  - IOService (the SDK's) is the kernel's IOService, so AppleGraphicsDeviceControl's data starts where the kernel's do;
  - AppleGraphicsDeviceControl (our declaration) is the kernel's 272 bytes;
  - NVAGDCControl adds no data, so it is 272 bytes too (its MetaClass registers sizeof(NVAGDCControl)).
Fail-closed: a mismatch fails the build, and NVAGDCControl's probe refuses to attach at run time for the same reason."""
import os, re, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'accel', 'tools'))
import kclib

# class -> the size the kernel registers (None: the kernel's MetaClass isn't in the collections)
KERNEL = {'IOService': kclib.class_size('IOService'),
          'AppleGraphicsDeviceControl': kclib.class_size('AppleGraphicsDeviceControl')}
AGDC_BYTES = 272     # the value the declaration's static_assert and NVAGDCControl::probe() use

text = open(sys.argv[1]).read()
sizes = {}
for block in text.split('*** Dumping AST Record Layout')[1:]:
    # a record's own header is its top-level line alone ("0 | class X"); nested and MetaClass records are other blocks
    head = re.search(r'^\s*0 \| class (\w+)\s*$', block, re.M)
    sized = re.findall(r'\[sizeof=(\d+)', block)     # the record's own size closes its block, after any nested ones
    if head and sized and head.group(1) not in sizes:
        sizes[head.group(1)] = int(sized[-1])

bad = 0
for name, want in [('IOService', KERNEL['IOService']),
                   ('AppleGraphicsDeviceControl', KERNEL['AppleGraphicsDeviceControl']),
                   ('NVAGDCControl', KERNEL['AppleGraphicsDeviceControl'])]:
    got = sizes.get(name)
    if got is None:
        print('%s: no layout in the dump' % name); bad += 1
    elif want is None:
        print('%s: not in the kernel collections' % name); bad += 1
    elif got != want:
        print('%s: %d bytes, the kernel has %d' % (name, got, want)); bad += 1
    else:
        print('%s: %d bytes, the kernel has %d: ok' % (name, got, want))
if KERNEL['AppleGraphicsDeviceControl'] != AGDC_BYTES:
    print('AppleGraphicsDeviceControl: the kernel has %d, the declaration assumes %d' %
          (KERNEL['AppleGraphicsDeviceControl'], AGDC_BYTES)); bad += 1
print('PASS' if not bad else 'FAIL (%d)' % bad)
sys.exit(1 if bad else 0)
