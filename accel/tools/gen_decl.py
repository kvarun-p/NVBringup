#!/usr/bin/env python3
"""Generates IOAccelFamily2_decl.h: C++ declarations of the IOAcceleratorFamily2 classes NVMetalAccel
subclasses, with every virtual in the slot order of the running macOS's kernel collections.

    gen_decl.py > ../src/IOAccelFamily2_decl.h

For each class C (parent P):
  - P's slots that C overrides are redeclared in C, so our subclasses' vtables point at C's versions.
    Signatures of kernel (IOService / OSObject) slots come from the SDK (iosvc.tsv / osobj.tsv, made with
    clang -fdump-vtable-layouts), so return types are exact.
  - C's own slots are declared from their demangled symbols. Return types aren't in the mangling: they
    come from RET below, else n_ret (uint64_t, which is what rax holds anyway). Only call or override a
    slot whose return type is in RET.
  - A pure virtual slot (___cxa_pure_virtual) takes its name from the paravirt driver's class for it
    (PARAVIRT below: its own override, or one inherited from a family subclass) and is declared = 0.
  - A slot whose implementation isn't exported can't be linked by name: it's listed in the
    NVA_FWD_DEFS_<C> macro, which defines it as a tail jump through C's own vtable slot.
  - Data: C is padded to its real size (the size its MetaClass constructor registers).
"""
import os, re, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import kclib

# (class, parent) in declaration order
CLASSES = [
    ('IOGraphicsAccelerator2',   'IOService'),
    ('IOAccelEventMachine2',     'OSObject'),
    ('IOAccelEventMachineFast2', 'IOAccelEventMachine2'),
    ('IOAccelTask',              'OSObject'),
    ('IOAccelDisplayMachine',    'IOService'),
]
PARAVIRT = {
    'IOGraphicsAccelerator2':   'AppleParavirtAccelerator',
    'IOAccelEventMachine2':     'AppleParavirtEventMachine',
    'IOAccelEventMachineFast2': 'AppleParavirtEventMachine',
    'IOAccelTask':              'AppleParavirtTask',
    'IOAccelDisplayMachine':    'AppleParavirtDisplayMachine',
}
# Return types of the slots we call or override (checked against the paravirt driver's disassembly).
RET = {
    'IOGraphicsAccelerator2::getStampMemory': 'IOMemoryDescriptor *',
    'IOGraphicsAccelerator2::newEventMachine': 'IOAccelEventMachine2 *',
    'IOGraphicsAccelerator2::createUserGPUTask': 'IOAccelTask *',
    'IOGraphicsAccelerator2::createKernelGPUTask': 'IOAccelTask *',
    'IOGraphicsAccelerator2::populateAccelConfig': 'void',
    'IOGraphicsAccelerator2::configureDevice': 'bool',
    'IOGraphicsAccelerator2::teardownDevice': 'void',
    'IOGraphicsAccelerator2::newDisplayMachine': 'IOAccelDisplayMachine *',
    'IOGraphicsAccelerator2::newGLContext': 'void *',
    'IOGraphicsAccelerator2::newCLContext': 'void *',
    'IOGraphicsAccelerator2::newSurface': 'void *',
    'IOGraphicsAccelerator2::new2DContext': 'void *',
    'IOGraphicsAccelerator2::newVideoContext': 'void *',
    'IOGraphicsAccelerator2::newSysMemory': 'void *',
    'IOGraphicsAccelerator2::newVidMemory': 'void *',
    'IOGraphicsAccelerator2::newResource': 'void *',
    'IOGraphicsAccelerator2::newMemoryMap': 'void *',
    'IOAccelEventMachine2::init': 'bool',
    'IOAccelEventMachine2::setStampBaseAddress': 'void',
    'IOAccelEventMachine2::enableStampInterrupt': 'void',
    'IOAccelEventMachine2::disableStampInterrupt': 'void',
    'IOAccelEventMachineFast2::init': 'bool',
    'IOAccelTask::init': 'bool',
    'IOAccelDisplayMachine::displayModeWillChange': 'bool',
    'IOAccelDisplayMachine::displayModeDidChange': 'bool',
}
KERNEL_TSV = {'IOService': 'iosvc.tsv', 'OSObject': 'osobj.tsv'}
BUILTIN = {'void', 'bool', 'char', 'short', 'int', 'long', 'unsigned', 'signed', 'float', 'double', 'const', 'volatile'}
KNOWN = {'IOService', 'OSObject', 'OSDictionary', 'OSSymbol', 'OSString', 'OSArray', 'OSData', 'OSNumber', 'OSSet',
         'OSIterator', 'OSMetaClass', 'IOMemoryDescriptor', 'IOBufferMemoryDescriptor', 'IOUserClient', 'IOWorkLoop',
         'IOCommandGate', 'IOEventSource', 'IOInterruptEventSource', 'IOTimerEventSource', 'IOPMPowerState',
         'IORegistryEntry', 'IORegistryPlane', 'IONotifier', 'IOPMrootDomain', 'IOMemoryMap', 'IOLock',
         'IORangeAllocator', 'IOReportChannelList', 'OSCollection', 'OSOrderedSet', 'OSAction', 'OSBoolean',
         'IOService_ptr'}
STRUCT = {'task'}                         # declared as struct in the kernel headers

def here(f): return os.path.join(os.path.dirname(os.path.abspath(__file__)), f)
def kernel_sigs(parent):
    sig = {}
    for l in open(here(KERNEL_TSV[parent])):
        i, s = l.rstrip('\n').split('\t'); sig[int(i)] = s
    return sig

types = set()
def note_types(args):
    for t in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', args):
        if t not in BUILTIN: types.add(t)

def split_dm(dm):
    """'C::name(args) const' -> (cls, name, args, const)"""
    m = re.match(r'^(.*?)::(~?[A-Za-z0-9_]+)\((.*)\)( const)?$', dm)
    if not m: raise ValueError('cannot parse ' + dm)
    return m.group(1), m.group(2), m.group(3), bool(m.group(4))

def kernel_decl(sig):
    """'bool IOService::start(IOService *)' -> ('bool', 'start', 'IOService *', const)"""
    m = re.match(r'^(.*?)\s*\b[A-Za-z0-9_]+::(~?[A-Za-z0-9_]+)\((.*)\)( const)?$', sig)
    return m.group(1).strip(), m.group(2), m.group(3), bool(m.group(4))

out, fwd = [], {}
decls = {}                                 # (cls, slot) -> (ret, name, args, const)
nslots = {}
pure = kclib.lookup('___cxa_pure_virtual')[1]
for cls, parent in CLASSES:
    vt = kclib.vtable(cls)
    pvt = kclib.vtable(parent)
    nslots[cls] = len(vt)
    dm = kclib.demangle([s for _, s, _, _, _ in vt])
    pv = kclib.vtable(PARAVIRT[cls]); pvdm = kclib.demangle([s for _, s, _, _, _ in pv])
    size = kclib.class_size(cls)
    body, fw = [], []
    for slot, sym, lvl, addr, ext in vt:
        if slot in (0, 1, 7):              # destructors, getMetaClass: OSDeclare*Structors
            continue
        inherited = slot < len(pvt)
        if inherited and pvt[slot][3] == addr:
            continue                       # not overridden here
        if inherited:                      # an override: the parent's signature
            if parent in KERNEL_TSV:
                ret, name, args, const = kernel_decl(kernel_sigs(parent)[slot])
            else:
                ret, name, args, const = decls[(parent, slot)]
        elif addr == pure:
            if '::' in pvdm[slot] and not pvdm[slot].startswith('_'):     # named by a descendant's override
                _, name, args, const = split_dm(pvdm[slot])
            else:
                name, args, const = '_vslot%d' % slot, '', False
        else:
            c, name, args, const = split_dm(dm[slot])
        if not inherited:
            ret = RET.get('%s::%s' % (cls, name), 'n_ret')
        if not (inherited and parent in KERNEL_TSV):
            note_types(args)               # kernel signatures use the SDK's own types
        decls[(cls, slot)] = (ret, name, args, const)
        spec = ' const' if const else ''
        tail = ' = 0' if addr == pure else (' override' if inherited else '')
        body.append('\t/* %3d */ virtual %s %s(%s)%s%s;' % (slot, ret, name, args, spec, tail))
        if addr != pure and not ext:
            fw.append((slot, ret, name, args, spec))
    fwd[cls] = fw
    out.append('class %s : public %s {' % (cls, parent))
    out.append('\tOSDeclareAbstractStructors(%s)' % cls)
    out.append('public:')
    out += body
    out.append('\tuint8_t _nva_data[0x%x - sizeof(%s)];   // the family\'s own fields' % (size, parent))
    out.append('};')
    out.append('static_assert(sizeof(%s) == 0x%x, "%s size");' % (cls, size, cls))
    out.append('#define NVA_%s_SLOTS %d' % (cls, len(vt)))
    out.append('')
    # forwarders: tail jumps through the family's own vtable (slot 0 sits 16 bytes into the symbol)
    lines = []
    for slot, ret, name, args, spec in fw:
        lines.append('\t__attribute__((naked)) %s %s::%s(%s)%s { __asm__("movq __ZTV%d%s@GOTPCREL(%%rip), %%rax\\n\\tjmpq *%d(%%rax)"); }'
                     % (ret, cls, name, ', '.join('%s a%d' % (t, i) for i, t in enumerate(a for a in args.split(', ') if a)) if args else '',
                        spec, len(cls), cls, 16 + 8 * slot))
    out.append('#define NVA_FWD_DEFS_%s \\' % cls)
    out.append(' \\\n'.join(lines) if lines else '\t/* none */')
    out.append('')

types -= KNOWN | set(c for c, _ in CLASSES)
hdr = ['// Generated by accel/tools/gen_decl.py from this Mac\'s kernel collections. Do not edit.',
       '// macOS %s' % os.popen('sw_vers -productVersion').read().strip() + ' (%s)' % os.popen('sw_vers -buildVersion').read().strip(),
       '#pragma once', '#include <IOKit/IOService.h>', '#include <IOKit/IORangeAllocator.h>',
       '#include <IOKit/IOMemoryDescriptor.h>', '', 'typedef uint64_t n_ret;   // a slot whose return type we don\'t use', '']
hdr += ['struct %s;' % t for t in sorted(types & STRUCT)]
hdr += ['class %s;' % t for t in sorted((types - STRUCT) | set(c for c, _ in CLASSES))]
print('\n'.join(hdr + [''] + out))
