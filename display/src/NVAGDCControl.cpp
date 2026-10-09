// NVAGDCControl: the AppleGraphicsDeviceControl (AGDC) provider for NVFramebuffer. IOPresentment builds a display pipe for a
// framebuffer only if an AGDC lists that framebuffer's registry ID in its GPU-capability answer (selector 0x980). Intel's
// framebuffers are listed by IntelFBClientControl; nothing lists NVFramebuffer, so IOPresentmentCreateForRegistryID finds no
// association and CoreDisplay reports "Unable to find display pipe". See display/README.md.
//
// This class is attached to NVBringup's NVDisplayNub, as a sibling of NVFramebuffer, by the personality in Info.plist. That
// personality matches only while the nub's NVDisplayPipeArmed is set, and probe() checks it again. The kernel's AGDC does the
// rest: its user client (IOServiceOpen type 2, authorized as WindowServer), its start() queries, and its selector dispatch.
// This class answers the vendor hook for selector 1 (vendor info) and selector 0x980 (GPU capability), and returns
// kIOReturnUnsupported for every other selector.
//
// Byte layouts, from the reads of IOPresentment's CreateAssociationMap (selector 1: +0x24 platform u16, +0x28 kind u32;
// selector 0x980: +0x30 count u32, +0x3c u64 framebuffer registry IDs) and AGDC's start (vendor class at +0x28 must be 1
// or 2):
//   selector 1 (0x2c bytes out): +0x00 u32 vendor ID 0x10de (IOPresentment logs it; the association doesn't test it),
//     +0x24 u16 platform 0, +0x28 u32 kind 2 (kind 1 makes IOPresentment validate the platform ID; 2 skips that check).
//   selector 0x980 (0xdc bytes out): +0x30 u32 count of framebuffers, +0x3c u64 framebuffer registry ID [count], up to 20.
//     The fields not listed are zero. The association path (CreateAssociationMap, CreateForRegistryID) reads only the
//     fields above; the rest of the blob has not been traced.

#include "AppleGraphicsDeviceControl_decl.h"
#include <IOKit/IOLib.h>
#include <IOKit/IORegistryEntry.h>
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
    static constexpr UInt32 kListSlots = 20;

    static bool gateArmed(IOService *nub);
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

// NVBringup's gate: NVDisplayPipeArmed on the nub (set when NVBringup arms the display pipe, see NVGpu.cpp).
bool NVAGDCControl::gateArmed(IOService *nub)
{
    OSBoolean *b = nub ? OSDynamicCast(OSBoolean, nub->getProperty("NVDisplayPipeArmed")) : nullptr;
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

// The framebuffer registry IDs under the provider (NVDisplayNub): NVFramebuffer, a live one. Read at call time, since
// IOPresentment asks each time it builds an association and NVFramebuffer may attach after this object starts.
UInt32 NVAGDCControl::listFramebuffers(uint8_t *out, UInt32 slots) const
{
    UInt32 n = 0;
    IOService *nub = getProvider();
    if (!nub)
        return 0;
    auto it = nub->getChildIterator(gIOServicePlane);
    if (!it)
        return 0;
    while (IORegistryEntry *e = (IORegistryEntry *)it->getNextObject()) {
        IOService *s = OSDynamicCast(IOService, e);
        if (!s || s->isInactive() || strcmp(s->getMetaClass()->getClassName(), "NVFramebuffer") != 0)
            continue;
        if (n >= slots)
            break;
        put64(out, 8 * n, s->getRegistryEntryID());
        n++;
    }
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
    if (n)
        memcpy(&fb, b + kListOffset, 8);
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
