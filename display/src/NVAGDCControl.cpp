// NVAGDCControl: the AppleGraphicsDeviceControl (AGDC) provider for NVFramebuffer. IOPresentment builds a display pipe for a
// framebuffer only if an AGDC lists that framebuffer's registry ID in its GPU-capability answer (selector 0x980). Intel's
// framebuffers are listed by IntelFBClientControl; nothing lists NVFramebuffer, so IOPresentmentCreateForRegistryID finds no
// association and CoreDisplay reports "Unable to find display pipe". See display/README.md.
//
// This class is attached to NVMetalAccelerator in display-pipe mode (NVDisplayPipeMode, set only on an armed boot), by the
// personality in Info.plist; probe() checks the property again. It sits under the accelerator, not beside NVFramebuffer,
// because IOPresentment pairs an AGDC with an accelerator through their first display-class ancestor (else their own
// registry ID): with PEGP@0's class-code spoofed, the accelerator marks itself display-class (NVMetalAccel.cpp) and is
// that ancestor for this class, so both keys are the accelerator's registry ID. Beside NVFramebuffer, the keys differed
// and IOPresentment failed with "Interface creation error: 0x815". The kernel's AGDC does the
// rest: its user client (IOServiceOpen type 2, authorized as WindowServer), its start() queries, and its selector dispatch.
// This class answers the vendor hook for selector 1 (vendor info) and selector 0x980 (GPU capability), and returns
// kIOReturnUnsupported for every other selector.
//
// Byte layouts, from the reads of IOPresentment's CreateAssociationMap (selector 1: +0x24 platform u16, +0x28 kind u32;
// selector 0x980: +0x30 count u32, +0x3c u64 framebuffer registry IDs) and AGDC's start (vendor class at +0x28 must be 1
// or 2):
//   selector 1 (0x2c bytes out): +0x00 u32 vendor ID 0x10de (IOPresentment logs it; the association doesn't test it),
//     +0x24 u16 platform 0, +0x28 u32 kind 2 (kind 1 makes IOPresentment validate the platform ID; 2 skips that check).
//   selector 0x980 (0xdc bytes out): +0x30 u32 count of framebuffers, +0x3c framebuffer IOService pointer [count], up to
//     8. The vendor answers with kernel object pointers: AGDC's filtered_doDeviceAttribute (after the vendor hook, before
//     the copy out) replaces each nonzero pointer at +0x34 and +0x3c[min(count, 8)] with getRegistryEntryID(), so
//     userspace sees registry IDs. A registry ID here is dereferenced as an object (the 2026-10-10 12:45 panic). +0x34
//     (a device object, not traced) stays zero, which AGDC skips. The fields not listed are zero; the association path
//     (CreateAssociationMap, CreateForRegistryID) reads only the fields above.

#include "AppleGraphicsDeviceControl_decl.h"
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSDictionary.h>
#include <string.h>

#define LOG(fmt, ...) IOLog("NVAGDCControl: " fmt "\n", ##__VA_ARGS__)

class NVAGDCControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVAGDCControl)

    static constexpr UInt32 kCmdVendorInfo = 0x1;         // kAGDCVendorInfo
    static constexpr UInt32 kCmdGPUCapability = 0x980;     // kAGDCGPUCapability
    static constexpr UInt32 kVendorInfoBytes = 0x2c;
    static constexpr UInt32 kCapabilityBytes = 0xdc;
    static constexpr UInt32 kVendorNVIDIA = 0x10de;
    static constexpr UInt32 kKindDiscrete = 2;
    static constexpr UInt32 kCountOffset = 0x30;
    static constexpr UInt32 kListOffset = 0x3c;
    static constexpr UInt32 kListSlots = 8;            // AGDC converts at most 8 entries

    static bool gateArmed(IOService *accel);
    static void put32(uint8_t *b, UInt32 off, UInt32 v) { memcpy(b + off, &v, 4); }
    static void put64(uint8_t *b, UInt32 off, UInt64 v) { memcpy(b + off, &v, 8); }
    UInt32 listFramebuffers(uint8_t *out, UInt32 slots) const;
    IOReturn vendorInfo(unsigned long *out, unsigned long *outCount);
    IOReturn gpuCapability(unsigned long *out, unsigned long *outCount);

public:
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;   // the kernel's slot 267 stays visible past our slot 266
    IOService *probe(IOService *provider, SInt32 *score) override;
    IOReturn vendor_doDeviceAttribute(UInt32 cmd, unsigned long *in, unsigned long inCount, unsigned long *out,
                                      unsigned long *outCount, IOExternalMethodArguments *args) override;
};

OSDefineMetaClassAndStructors(NVAGDCControl, AppleGraphicsDeviceControl)

// The gate: NVDisplayPipeMode on the accelerator (its PCI personality, which matches only on an armed boot).
bool NVAGDCControl::gateArmed(IOService *accel)
{
    OSBoolean *b = accel ? OSDynamicCast(OSBoolean, accel->getProperty("NVDisplayPipeMode")) : nullptr;
    return b && b->isTrue();
}

// Refuses to attach unless the kernel's AppleGraphicsDeviceControl is the 272-byte class this layout was built for, and
// unless the gate is armed. The personality already matches on the gate; this is the same check made again.
IOService *NVAGDCControl::probe(IOService *provider, SInt32 *score)
{
    const OSMetaClass *base = getMetaClass()->getSuperClass();
    if (!base || base->getClassSize() != sizeof(AppleGraphicsDeviceControl)) {
        LOG("AppleGraphicsDeviceControl is not %lu bytes (kernel says %u): not attaching",
            (unsigned long)sizeof(AppleGraphicsDeviceControl), base ? base->getClassSize() : 0);
        return nullptr;
    }
    if (!gateArmed(provider))
        return nullptr;
    return IOService::probe(provider, score);
}

// The framebuffers of this GPU: NVFramebuffer services under the accelerator's PCI device (PEGP@0 > NVDisplayNub >
// NVFramebuffer), live ones, as object pointers for AGDC to convert (see the header). Read at call time, since
// IOPresentment asks each time it builds an association and NVFramebuffer may attach after this object starts. Not
// retained: AGDC converts them in the same call, and NVFramebuffer stays attached to the nub for the boot.
UInt32 NVAGDCControl::listFramebuffers(uint8_t *out, UInt32 slots) const
{
    UInt32 n = 0;
    IOService *accel = getProvider();
    IOService *pci = accel ? accel->getProvider() : nullptr;
    if (!pci)
        return 0;
    IORegistryIterator *it = IORegistryIterator::iterateOver(pci, gIOServicePlane, kIORegistryIterateRecursively);
    if (!it)
        return 0;
    while (IORegistryEntry *e = it->getNextObject()) {
        IOService *s = OSDynamicCast(IOService, e);
        if (!s || s->isInactive() || strcmp(s->getMetaClass()->getClassName(), "NVFramebuffer") != 0)
            continue;
        if (n >= slots)
            break;
        put64(out, 8 * n, (UInt64)(uintptr_t)s);
        n++;
    }
    it->release();
    return n;
}

IOReturn NVAGDCControl::vendorInfo(unsigned long *out, unsigned long *outCount)
{
    if (!out || !outCount || *outCount < kVendorInfoBytes)
        return kIOReturnBadArgument;
    uint8_t *b = (uint8_t *)out;
    memset(b, 0, kVendorInfoBytes);
    put32(b, 0x00, kVendorNVIDIA);
    put32(b, 0x28, kKindDiscrete);
    *outCount = kVendorInfoBytes;
    return kIOReturnSuccess;
}

IOReturn NVAGDCControl::gpuCapability(unsigned long *out, unsigned long *outCount)
{
    if (!out || !outCount || *outCount < kCapabilityBytes)
        return kIOReturnBadArgument;
    uint8_t *b = (uint8_t *)out;
    memset(b, 0, kCapabilityBytes);
    const UInt32 n = listFramebuffers(b + kListOffset, kListSlots);
    put32(b, kCountOffset, n);
    *outCount = kCapabilityBytes;
    // Log only when the answer changes: IOPresentment asks on every association (each frame, while CoreDisplay looks).
    static UInt32 lastCount = 0xffffffff;
    static UInt64 lastFB = 0;
    UInt64 fb = 0;
    if (n) {
        memcpy(&fb, b + kListOffset, 8);
        fb = ((IORegistryEntry *)(uintptr_t)fb)->getRegistryEntryID();
    }
    if (n != lastCount || fb != lastFB) {
        LOG("GPU capability: %u framebuffer(s), first regID %#llx", n, (unsigned long long)fb);
        lastCount = n;
        lastFB = fb;
    }
    return kIOReturnSuccess;
}

IOReturn NVAGDCControl::vendor_doDeviceAttribute(UInt32 cmd, unsigned long *in, unsigned long inCount,
                                                 unsigned long *out, unsigned long *outCount,
                                                 IOExternalMethodArguments *args)
{
    (void)in;
    (void)inCount;
    (void)args;
    switch (cmd) {
    case kCmdVendorInfo:
        return vendorInfo(out, outCount);
    case kCmdGPUCapability:
        return gpuCapability(out, outCount);
    default:
        return kIOReturnUnsupported;
    }
}
