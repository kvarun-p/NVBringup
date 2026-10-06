// NVFramebuffer: an IOFramebuffer for the display NVBringup lights (boot-arg nvdisp=2), so macOS
// sees the NVIDIA GPU's HDMI output as a screen. NVBringup does the modeset over GSP-RM and scans
// out one pitch-linear X8R8G8B8 surface in VRAM; this driver hands that surface to IOGraphics as the
// aperture (its physical BAR1 range) with the one mode it was lit at, and the sink's EDID over DDC.
// No acceleration: WindowServer draws into the aperture with the CPU.
//
// It lives in its own kext because IOGraphicsFamily is in the system kernel collection, which an
// OpenCore-injected kext (NVBringup) can't link against. The two share no symbols: NVBringup's
// NVDisplayFB property carries everything this needs.

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) IOLog("NVFramebuffer: " fmt "\n", ##__VA_ARGS__)

class NVFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(NVFramebuffer)

    static constexpr IODisplayModeID kMode = 1;

    IODeviceMemory *aperture_ = nullptr;
    uint64_t phys_ = 0, size_ = 0;
    uint32_t width_ = 0, height_ = 0, pitch_ = 0, pclkKhz_ = 0;
    uint32_t htotal_ = 0, vtotal_ = 0, hsyncStart_ = 0, hsyncWidth_ = 0, vsyncStart_ = 0, vsyncWidth_ = 0;
    uint32_t syncFlags_ = 0;            // bit 0: hsync negative, bit 1: vsync negative
    uint8_t edid_[128] = {};
    bool haveEdid_ = false;

    bool readDisplay(IOService *provider);
    IOFixed1616 refresh() const;

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void free() override;

    IOReturn enableController() override;
    bool isConsoleDevice() override;
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override;
    const char *getPixelFormats() override;
    IOItemCount getDisplayModeCount() override;
    IOReturn getDisplayModes(IODisplayModeID *allDisplayModes) override;
    IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override;
    UInt64 getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                 IOPixelInformation *info) override;
    IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override;
    IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override;
    IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data) override;
    IOItemCount getConnectionCount() override;
    IOReturn getAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t value) override;
    bool hasDDCConnect(IOIndex connect) override;
    IOReturn getDDCBlock(IOIndex connect, UInt32 blockNumber, IOSelect blockType, IOOptionBits options,
                         UInt8 *data, IOByteCount *length) override;
};

OSDefineMetaClassAndStructors(NVFramebuffer, IOFramebuffer)

// NVBringup's NVDisplayFB property: the scanout surface and the mode it was lit at.
// provider: IOResources (default), or with boot-arg nvfb=1 the NVIDIA PCI device NVBringup drives.
bool NVFramebuffer::readDisplay(IOService *provider)
{
    OSDictionary *m = IOService::serviceMatching("NVBringup");
    IOService *gpu = m ? IOService::copyMatchingService(m) : nullptr;
    if (m)
        m->release();
    if (!gpu)
        return false;
    uint32_t onPci = 0;
    PE_parse_boot_argn("nvfb", &onPci, sizeof(onPci));
    const bool wantPci = onPci != 0;
    const bool isPci = provider != getResourceService();
    if (wantPci != isPci || (isPci && gpu->getProvider() != provider)) {
        gpu->release();
        return false;
    }
    bool ok = false;
    if (OSDictionary *d = OSDynamicCast(OSDictionary, gpu->getProperty("NVDisplayFB"))) {
        auto num = [&](const char *k) -> uint64_t {
            OSNumber *n = OSDynamicCast(OSNumber, d->getObject(k));
            return n ? n->unsigned64BitValue() : 0;
        };
        phys_ = num("PhysAddr");
        size_ = num("Size");
        width_ = (uint32_t)num("Width");
        height_ = (uint32_t)num("Height");
        pitch_ = (uint32_t)num("Pitch");
        pclkKhz_ = (uint32_t)num("PixelClockKHz");
        htotal_ = (uint32_t)num("HTotal");
        vtotal_ = (uint32_t)num("VTotal");
        hsyncStart_ = (uint32_t)num("HSyncStart");
        hsyncWidth_ = (uint32_t)num("HSyncWidth");
        vsyncStart_ = (uint32_t)num("VSyncStart");
        vsyncWidth_ = (uint32_t)num("VSyncWidth");
        syncFlags_ = (uint32_t)num("Flags");
        if (OSData *e = OSDynamicCast(OSData, d->getObject("EDID")); e && e->getLength() >= sizeof(edid_)) {
            memcpy(edid_, e->getBytesNoCopy(), sizeof(edid_));
            haveEdid_ = true;
        }
        ok = phys_ && width_ && height_ && pitch_ >= width_ * 4 && size_ >= (uint64_t)pitch_ * height_ &&
             htotal_ && vtotal_;
    }
    gpu->release();
    return ok;
}

IOFixed1616 NVFramebuffer::refresh() const
{
    return (IOFixed1616)(((uint64_t)pclkKhz_ * 1000 << 16) / ((uint64_t)htotal_ * vtotal_));
}

IOService *NVFramebuffer::probe(IOService *provider, SInt32 *score)
{
    if (!readDisplay(provider)) {
        return nullptr;
    }
    return IOFramebuffer::probe(provider, score);
}

bool NVFramebuffer::start(IOService *provider)
{
    if (!aperture_ && !(aperture_ = IODeviceMemory::withRange(phys_, size_)))
        return false;
    if (!IOFramebuffer::start(provider)) {
        LOG("IOFramebuffer::start failed");
        return false;
    }
    LOG("started on %s: %ux%u @ %u.%02u Hz, pitch %u, surface 0x%llx (%llu bytes), EDID %s", provider->getName(),
        width_, height_,
        refresh() >> 16, ((refresh() & 0xffff) * 100) >> 16, pitch_, phys_, size_, haveEdid_ ? "yes" : "no");
    return true;
}

void NVFramebuffer::free()
{
    if (aperture_) {
        aperture_->release();
        aperture_ = nullptr;
    }
    IOFramebuffer::free();
}

IOReturn NVFramebuffer::enableController()
{
    return kIOReturnSuccess;        // NVBringup lit the display before publishing it
}

bool NVFramebuffer::isConsoleDevice()
{
    return false;                   // the boot console stays on the internal panel
}

IODeviceMemory *NVFramebuffer::getApertureRange(IOPixelAperture aperture)
{
    if (aperture != kIOFBSystemAperture || !aperture_)
        return nullptr;
    aperture_->retain();            // the caller releases it
    return aperture_;
}

IODeviceMemory *NVFramebuffer::getVRAMRange()
{
    return getApertureRange(kIOFBSystemAperture);
}

const char *NVFramebuffer::getPixelFormats()
{
    static const char formats[] = IO32BitDirectPixels "\0";
    return formats;
}

IOItemCount NVFramebuffer::getDisplayModeCount()
{
    return 1;
}

IOReturn NVFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes)
{
    if (!allDisplayModes)
        return kIOReturnBadArgument;
    allDisplayModes[0] = kMode;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info)
{
    if (mode != kMode || !info)
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->nominalWidth = width_;
    info->nominalHeight = height_;
    info->refreshRate = refresh();
    info->maxDepthIndex = 0;
    info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | kDisplayModeDefaultFlag;
    if (haveEdid_) {            // the EDID's image size: bytes 21 and 22 are in cm
        info->imageWidth = (UInt16)(edid_[21] * 10);
        info->imageHeight = (UInt16)(edid_[22] * 10);
    }
    return kIOReturnSuccess;
}

UInt64 NVFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    return 0;                       // obsolete
}

IOReturn NVFramebuffer::getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture,
                                            IOPixelInformation *info)
{
    if (mode != kMode || depth != 0 || aperture != kIOFBSystemAperture || !info)
        return kIOReturnBadArgument;
    bzero(info, sizeof(*info));
    info->bytesPerRow = pitch_;
    info->bitsPerPixel = 32;
    info->pixelType = kIORGBDirectPixels;
    info->componentCount = 3;
    info->bitsPerComponent = 8;
    info->componentMasks[0] = 0x00ff0000;
    info->componentMasks[1] = 0x0000ff00;
    info->componentMasks[2] = 0x000000ff;
    strlcpy(info->pixelFormat, IO32BitDirectPixels, sizeof(info->pixelFormat));
    info->activeWidth = width_;
    info->activeHeight = height_;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
    if (mode)
        *mode = kMode;
    if (depth)
        *depth = 0;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth)
{
    return (mode == kMode && depth == 0) ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn NVFramebuffer::getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth)
{
    return getCurrentDisplayMode(mode, depth);
}

IOReturn NVFramebuffer::getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info)
{
    if (mode != kMode || !info)
        return kIOReturnBadArgument;
    const uint32_t flags = info->flags;     // the caller says which detailed form it wants
    bzero(info, sizeof(*info));
    info->appleTimingID = kIOTimingIDInvalid;
    if (!(flags & kIODetailedTimingValid))
        return kIOReturnSuccess;
    info->flags = kIODetailedTimingValid;
    IODetailedTimingInformationV2 &t = info->detailedInfo.v2;
    t.pixelClock = t.minPixelClock = t.maxPixelClock = (UInt64)pclkKhz_ * 1000;
    t.horizontalActive = width_;
    t.horizontalBlanking = htotal_ - width_;
    t.horizontalSyncOffset = hsyncStart_ - width_;
    t.horizontalSyncPulseWidth = hsyncWidth_;
    t.verticalActive = height_;
    t.verticalBlanking = vtotal_ - height_;
    t.verticalSyncOffset = vsyncStart_ - height_;
    t.verticalSyncPulseWidth = vsyncWidth_;
    t.horizontalSyncConfig = (syncFlags_ & 1) ? 0 : kIOSyncPositivePolarity;
    t.verticalSyncConfig = (syncFlags_ & 2) ? 0 : kIOSyncPositivePolarity;
    t.numLinks = 1;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kIOPowerAttribute)
        return kIOReturnSuccess;    // the display's power follows NVBringup's GPU
    return IOFramebuffer::setAttribute(attribute, value);
}

IOReturn NVFramebuffer::setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data)
{
    return kIOReturnSuccess;        // the head's output LUT stays identity for now
}

IOItemCount NVFramebuffer::getConnectionCount()
{
    return 1;
}

IOReturn NVFramebuffer::getAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t *value)
{
    if (connect != 0)
        return kIOReturnBadArgument;
    switch (attribute) {
    case kConnectionEnable:
    case kConnectionCheckEnable:
        if (value)
            *value = 1;             // lit, and stays connected (no hot-plug yet)
        return kIOReturnSuccess;
    case kConnectionFlags:
        if (value)
            *value = 0;             // external, not built in
        return kIOReturnSuccess;
    case kConnectionSupportsHLDDCSense:
        return haveEdid_ ? kIOReturnSuccess : kIOReturnUnsupported;
    case kConnectionDisplayParameterCount:
        if (value)
            *value = 0;
        return kIOReturnSuccess;
    default:
        return IOFramebuffer::getAttributeForConnection(connect, attribute, value);
    }
}

IOReturn NVFramebuffer::setAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t value)
{
    if (connect != 0)
        return kIOReturnBadArgument;
    switch (attribute) {
    case kConnectionPostWake:
    case kConnectionEnable:
        return kIOReturnSuccess;
    default:
        return IOFramebuffer::setAttributeForConnection(connect, attribute, value);
    }
}

bool NVFramebuffer::hasDDCConnect(IOIndex connect)
{
    return connect == 0 && haveEdid_;
}

IOReturn NVFramebuffer::getDDCBlock(IOIndex connect, UInt32 blockNumber, IOSelect blockType,
                                    IOOptionBits options, UInt8 *data, IOByteCount *length)
{
    if (connect != 0 || !haveEdid_ || blockType != kIODDCBlockTypeEDID || !data || !length)
        return kIOReturnBadArgument;
    if (blockNumber != 1)           // only the base block: NVBringup reads 128 bytes
        return kIOReturnUnsupported;
    const IOByteCount n = *length < sizeof(edid_) ? *length : sizeof(edid_);
    memcpy(data, edid_, n);
    *length = n;
    return kIOReturnSuccess;
}
