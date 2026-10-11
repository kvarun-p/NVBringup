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
// This class answers the vendor hook for selector 1 (vendor info), 0x980 (GPU capability), 0x921 (link config: the
// display's timing) and 0x711 (display pipe capability: the plane and pipeline-scaler blocks), and returns
// kIOReturnUnsupported for every other selector. 0x921 and 0x711 feed IOPresentment's capability set, without which
// CoreDisplay never presents to the display (scratchpad/accel-displaypipe/s3/S3-A, S3-B).
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

    static constexpr UInt32 kCmdLinkConfig = 0x921;        // GetLinkConfig (IOPresentment: logical device caps)
    static constexpr UInt32 kCmdPipeCapability = 0x711;    // getDisplayPipeCapabilityExtended
    static constexpr UInt32 kLinkConfigBytes = 0xb0;
    static constexpr UInt32 kPipeCapBytes = 0x196c;
    static constexpr UInt32 kPipeMaskPlanes = 0x10;        // plane scaler: the planes and their pixel formats
    static constexpr UInt32 kPipeMaskScaler = 0x8;         // pipeline scaler
    static constexpr UInt32 kPipeMaskCSC = 0x1;
    static constexpr UInt32 kPipeMaskGamma = 0x2;
    static constexpr UInt32 kPipeMaskLinearize = 0x4;
    static constexpr UInt32 kAGDCFormatBGRA8 = 0x10000;    // AGDCPixelFormatTypeToIOPresentment: IOP bit 48 (BGRA)

    static bool gateArmed(IOService *accel);
    static void put16(uint8_t *b, UInt32 off, UInt16 v) { memcpy(b + off, &v, 2); }
    static void put32(uint8_t *b, UInt32 off, UInt32 v) { memcpy(b + off, &v, 4); }
    static void put64(uint8_t *b, UInt32 off, UInt64 v) { memcpy(b + off, &v, 8); }
    UInt32 listFramebuffers(uint8_t *out, UInt32 slots) const;
    IOReturn vendorInfo(unsigned long *out, unsigned long *outCount);
    IOReturn gpuCapability(unsigned long *out, unsigned long *outCount);
    OSDictionary *copyDisplayFB() const;
    IOReturn linkConfig(const unsigned long *in, unsigned long inCount, unsigned long *out, unsigned long *outCount);
    IOReturn pipeCapability(const unsigned long *in, unsigned long inCount, unsigned long *out,
                            unsigned long *outCount);

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

// The lit display's NVDisplayFB (NVBringup's mode record: Width, Height, PixelClockKHz, HTotal, VTotal, HSyncStart,
// HSyncWidth, VSyncStart, VSyncWidth, Flags), from the first NVFramebuffer's nub. Retained, or nullptr. Found by its
// own walk, not listFramebuffers: those pointers are unretained (fine for 0x980, converted in the same call).
OSDictionary *NVAGDCControl::copyDisplayFB() const
{
    IOService *accel = getProvider();
    IOService *pci = accel ? accel->getProvider() : nullptr;
    if (!pci)
        return nullptr;
    IORegistryIterator *it = IORegistryIterator::iterateOver(pci, gIOServicePlane, kIORegistryIterateRecursively);
    if (!it)
        return nullptr;
    OSDictionary *d = nullptr;
    while (IORegistryEntry *e = it->getNextObject()) {
        IOService *s = OSDynamicCast(IOService, e);
        if (!s || s->isInactive() || strcmp(s->getMetaClass()->getClassName(), "NVFramebuffer") != 0)
            continue;
        // Both retained while used: the nub (provider) and its NVDisplayFB, which NVBringup replaces on a replug.
        IORegistryEntry *nub = s->copyParentEntry(gIOServicePlane);
        OSObject *o = nub ? nub->copyProperty("NVDisplayFB") : nullptr;
        d = OSDynamicCast(OSDictionary, o);
        if (!d)
            OSSafeReleaseNULL(o);
        OSSafeReleaseNULL(nub);
        break;
    }
    it->release();
    return d;
}

static UInt32 dictU32(OSDictionary *d, const char *k)
{
    OSNumber *n = d ? OSDynamicCast(OSNumber, d->getObject(k)) : nullptr;
    return n ? n->unsigned32BitValue() : 0;
}

// 0x921, Intel's GetLinkConfig (S3-B §GetLinkConfig; IOPresentment's __GatherAGDCLogicalDeviceCapabilities, S3-A):
// 176 bytes in and out, in+0 the 0-based index into the 0x980 list. The reply carries the current detailed timing
// (IODetailedTimingInformationV2 laid at +4): IOPresentment fails the whole capability set (0x80e) without it, and
// CoreDisplay then has no capabilities for the display and never presents to it ("Attempting to get capabilities
// when none exist", 2026-10-11 21:20). +0x08 bit0 asks IOPresentment for link capabilities (0x925/0x988, tolerated if
// they fail): left clear. No pointer conversion in the base class for this selector.
IOReturn NVAGDCControl::linkConfig(const unsigned long *in, unsigned long inCount, unsigned long *out,
                                   unsigned long *outCount)
{
    if (!in || !out || !outCount || inCount != kLinkConfigBytes || *outCount < kLinkConfigBytes)
        return kIOReturnBadArgument;
    UInt32 index;
    memcpy(&index, in, 4);
    if (index != 0)
        return kIOReturnNotFound;
    OSDictionary *d = copyDisplayFB();
    const UInt32 w = dictU32(d, "Width"), h = dictU32(d, "Height"), ht = dictU32(d, "HTotal"), vt = dictU32(d, "VTotal");
    const UInt32 hss = dictU32(d, "HSyncStart"), hsw = dictU32(d, "HSyncWidth");
    const UInt32 vss = dictU32(d, "VSyncStart"), vsw = dictU32(d, "VSyncWidth");
    const UInt32 flags = dictU32(d, "Flags");                      // bit0 -hsync, bit1 -vsync (NVGpu.cpp)
    const UInt64 pclk = (UInt64)dictU32(d, "PixelClockKHz") * 1000;
    OSSafeReleaseNULL(d);
    if (!w || !h || ht <= w || vt <= h || !pclk || hss < w || vss < h)
        return kIOReturnNotReady;                                  // no lit mode yet

    uint8_t *b = (uint8_t *)out;
    memset(b, 0, kLinkConfigBytes);
    put32(b, 0x08, 0x4);                // bit2 as Intel always sets; bit0 (link capabilities) clear
    put64(b, 0x2c, pclk);               // pixel clock, min, max (Hz)
    put64(b, 0x34, pclk);
    put64(b, 0x3c, pclk);
    put32(b, 0x44, w);                  // HActive, HBlank, HSyncOffset, HSyncPulseWidth
    put32(b, 0x48, ht - w);
    put32(b, 0x4c, hss - w);
    put32(b, 0x50, hsw);
    put32(b, 0x54, h);                  // VActive, VBlank, VSyncOffset, VSyncPulseWidth
    put32(b, 0x58, vt - h);
    put32(b, 0x5c, vss - h);
    put32(b, 0x60, vsw);
    put32(b, 0x74, (flags & 1) ? 0 : 1);  // HSyncConfig: kIOSyncPositivePolarity
    put32(b, 0x7c, (flags & 2) ? 0 : 1);  // VSyncConfig
    put32(b, 0x84, 1);                  // numLinks
    put16(b, 0x8c, 1);                  // pixel encoding RGB 4:4:4
    put16(b, 0x8e, 2);                  // 8 bits per colour
    put16(b, 0x90, 2);                  // colorimetry sRGB
    put16(b, 0x92, 1);                  // dynamic range
    put64(b, 0xa4, 1);                  // {1-based port, 0}
    put32(b, 0xac, 1);
    *outCount = kLinkConfigBytes;
    return kIOReturnSuccess;
}

// 0x711, Intel's getDisplayPipeCapabilityExtended (S3-B §0x711): 0x196c bytes in and out (the base class checks
// both), in+0 index, in+4 the capability block (one bit), in+8 the entry limit (1..8). Reply: the header echoed,
// out+8 the entry count, entries from out+12. IOPresentment needs two blocks: the planes (0x10; no plane, no
// capabilities) and the pipeline scaler (0x8, at least one entry). Gamma (0x2), linearization (0x4) and CSC (0x1) are
// optional to IOPresentment, but without them CoreDisplay does those steps on the GPU; the others may fail.
IOReturn NVAGDCControl::pipeCapability(const unsigned long *in, unsigned long inCount, unsigned long *out,
                                       unsigned long *outCount)
{
    if (!in || !out || !outCount || inCount != kPipeCapBytes || *outCount < kPipeCapBytes)
        return kIOReturnBadArgument;
    UInt32 hdr[3];
    memcpy(hdr, in, sizeof(hdr));
    if (hdr[0] != 0)
        return kIOReturnNotFound;
    if (hdr[2] < 1 || hdr[2] > 8)
        return kIOReturnBadArgument;
    if (hdr[1] != kPipeMaskPlanes && hdr[1] != kPipeMaskScaler && hdr[1] != kPipeMaskCSC &&
        hdr[1] != kPipeMaskGamma && hdr[1] != kPipeMaskLinearize)
        return kIOReturnUnsupported;

    uint8_t *b = (uint8_t *)out;
    memset(b, 0, kPipeCapBytes);
    memcpy(b, hdr, sizeof(hdr));
    put32(b, 8, 1);                     // one entry
    // Size limits: Intel reads two min/max pairs from the framebuffer; the order inside a pair is not decoded, so both
    // are given the same wide range (64 .. 4096), which satisfies min != max either way.
    if (hdr[1] == kPipeMaskPlanes) {
        uint8_t *e = b + 12;            // plane 0 (812-byte entries)
        put32(e, 0x00, 0x000A01C4);     // Intel's plane flags
        put64(e, 0x04, 1);
        put64(e, 0x1c, 1);              // one pixel-format record
        put32(e, 0x24, 64);             // record 0: the two size pairs, the format bits, 2
        put32(e, 0x28, 64);
        put32(e, 0x2c, 4096);
        put32(e, 0x30, 4096);
        put32(e, 0x34, kAGDCFormatBGRA8);
        put32(e, 0x38, 2);
    } else if (hdr[1] == kPipeMaskGamma || hdr[1] == kPipeMaskLinearize) {
        // Intel's gamma and linearization entries (S4-B §5; fields per S4-A §6.3): flags, the table-enable mask,
        // NonAtomicApplicationPeriod (3 frames for gamma, 1 for linearization, in ms at the refresh rate: Intel divides
        // 3000.0 and 1000.0 by its FB+41416), the gamma type (256 = AGDC bit 8, IOP 0x100: EOTF in the pipe), then
        // MaximumValue 1.0 and MaximumSize 1024 entries. Without the gamma block CoreDisplay logs "plane with no gamma"
        // and runs EOTF on the GPU (the fallback path).
        const bool gamma = hdr[1] == kPipeMaskGamma;
        const float period = (gamma ? 3000.0f : 1000.0f) / 60.0f;
        put32(b, 12, gamma ? 0x00101001 : 0x00100000);
        put64(b, 16, gamma ? 3 : 1);
        memcpy(b + 24, &period, 4);
        put64(b, 28, gamma ? 256 : 8);
        const float one = 1.0f;
        memcpy(b + 48, &one, 4);
        put64(b, 52, 1024);
    } else if (hdr[1] == kPipeMaskCSC) {
        // Intel's CSC entry (S4-B §5): 5, a 3x3 (8 = its matrix kind), 0x30300, coefficient range -1.0 .. 1.0
        put32(b, 12, 5);
        put64(b, 24, 0x800000000ULL);
        put32(b, 44, 0x00030300);
        const float lo = -1.0f, hi = 1.0f;
        memcpy(b + 48, &lo, 4);
        memcpy(b + 52, &hi, 4);
    } else {
        put32(b, 12, 0xC5);             // Intel's pipeline scaler entry, values as Intel writes them
        put64(b, 40, 1);
        put32(b, 48, 64);
        put32(b, 52, 64);
        put32(b, 56, 4096);
        put32(b, 60, 4096);
        put32(b, 64, 0x301);
        put32(b, 68, 2);
        put64(b, 432, 1);
        put32(b, 440, 64);
        put32(b, 444, 64);
        put32(b, 448, 4096);
        put32(b, 452, 4096);
        put64(b, 456, 0x200000301ULL);
    }
    *outCount = kPipeCapBytes;
    return kIOReturnSuccess;
}

IOReturn NVAGDCControl::vendor_doDeviceAttribute(UInt32 cmd, unsigned long *in, unsigned long inCount,
                                                 unsigned long *out, unsigned long *outCount,
                                                 IOExternalMethodArguments *args)
{
    (void)args;
    IOReturn r;
    switch (cmd) {
    case kCmdVendorInfo:
        return vendorInfo(out, outCount);
    case kCmdGPUCapability:
        return gpuCapability(out, outCount);
    case kCmdLinkConfig:
        r = linkConfig(in, inCount, out, outCount);
        break;
    case kCmdPipeCapability:
        r = pipeCapability(in, inCount, out, outCount);
        break;
    default:
        return kIOReturnUnsupported;
    }
    // The first answers in the log: these build CoreDisplay's capabilities for the display
    static volatile SInt32 logged = 0;
    if (OSIncrementAtomic(&logged) < 12)
        LOG("selector %#x (in %lu bytes, header %#lx): %#x", cmd, inCount, in ? in[0] : 0UL, r);
    return r;
}
