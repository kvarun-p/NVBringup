// NVFramebuffer: an IOFramebuffer for the display NVBringup lights (boot-arg nvdisp=2), so macOS
// sees the NVIDIA GPU's HDMI output as a screen. NVBringup does the modeset over GSP-RM and scans
// out one pitch-linear X8R8G8B8 surface in VRAM; this driver hands that surface to IOGraphics as the
// aperture (its physical BAR1 range) with the one mode it was lit at, and the sink's EDID over DDC.
// No acceleration: WindowServer draws into the aperture with the CPU.
//
// It lives in its own kext because IOGraphicsFamily is in the system kernel collection, which an
// OpenCore-injected kext (NVBringup) can't link against. The two share no symbols: NVBringup's
// NVDisplayFB property carries everything this needs.
//
// The vertical blanking interrupt WindowServer paces its frames by comes from a timer at the mode's
// refresh rate, not from the display engine.

#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <kern/thread_call.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) IOLog("NVFramebuffer: " fmt "\n", ##__VA_ARGS__)

class NVFramebuffer : public IOFramebuffer {
    OSDeclareDefaultStructors(NVFramebuffer)

    static constexpr IODisplayModeID kMode = 1;

    struct Interrupt {
        IOFBInterruptProc proc = nullptr;
        OSObject *target = nullptr;
        void *ref = nullptr;
        bool enabled = true;
    };

    IODeviceMemory *aperture_ = nullptr;
    uint64_t phys_ = 0, size_ = 0;
    uint32_t width_ = 0, height_ = 0, pitch_ = 0, pclkKhz_ = 0;
    uint32_t htotal_ = 0, vtotal_ = 0, hsyncStart_ = 0, hsyncWidth_ = 0, vsyncStart_ = 0, vsyncWidth_ = 0;
    uint32_t syncFlags_ = 0;            // bit 0: hsync negative, bit 1: vsync negative
    uint8_t edid_[128] = {};
    bool haveEdid_ = false;
    Interrupt vbl_, connect_;           // guarded by vblLock_
    thread_call_t vblCall_ = nullptr;   // allocated in start, freed in free; armed while vblActive_
    IOSimpleLock *vblLock_ = nullptr;   // guards vbl_, connect_, vblActive_, vblBusy_, vblNext_, vblLast_
    bool vblActive_ = false;            // the timer rearms only while this is set
    int vblBusy_ = 0;                   // vblFire calls past their entry check (stop waits for them to leave)
    uint64_t vblNext_ = 0;              // absolute deadline of the next vertical blank
    uint64_t vblLast_ = 0;              // deadline of the last vertical blank fired, for getVBLTime
    uint64_t vblCount_ = 0;             // vertical blanks fired by the timer (published as NVFBVblanks)
    uint64_t vblPublishAt_ = 0;         // absolute time after which NVFBVblanks is published again

    bool readDisplay(IOService *provider);
    IOFixed1616 refresh() const;
    uint64_t framePeriod() const;
    void publishTiming();
    void publishVblanks();
    static void vblFire(thread_call_param_t self, thread_call_param_t);

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    IOReturn enableController() override;
    bool isConsoleDevice() override;
    IODeviceMemory *getApertureRange(IOPixelAperture aperture) override;
    IODeviceMemory *getVRAMRange() override;
    void getVBLTime(AbsoluteTime *time, AbsoluteTime *delta) override;
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
    IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttribute(IOSelect attribute, uintptr_t value) override;
    IOReturn setGammaTable(UInt32 channelCount, UInt32 dataCount, UInt32 dataWidth, void *data) override;
    IOItemCount getConnectionCount() override;
    IOReturn getAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t *value) override;
    IOReturn setAttributeForConnection(IOIndex connect, IOSelect attribute, uintptr_t value) override;
    bool hasDDCConnect(IOIndex connect) override;
    IOReturn getDDCBlock(IOIndex connect, UInt32 blockNumber, IOSelect blockType, IOOptionBits options,
                         UInt8 *data, IOByteCount *length) override;
    IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref,
                                      void **interruptRef) override;
    IOReturn unregisterInterrupt(void *interruptRef) override;
    IOReturn setInterruptState(void *interruptRef, UInt32 state) override;
};

OSDefineMetaClassAndStructors(NVFramebuffer, IOFramebuffer)

// NVBringup's NVDisplayFB, on the NVDisplayNub it publishes while a display is lit: the scanout surface and
// the mode it was lit at.
bool NVFramebuffer::readDisplay(IOService *provider)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, provider->getProperty("NVDisplayFB"));
    if (!d)
        return false;
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
    return phys_ && width_ && height_ && pitch_ >= width_ * 4 && size_ >= (uint64_t)pitch_ * height_ &&
           htotal_ && vtotal_;
}

IOFixed1616 NVFramebuffer::refresh() const
{
    return (IOFixed1616)(((uint64_t)pclkKhz_ * 1000 << 16) / ((uint64_t)htotal_ * vtotal_));
}

uint64_t NVFramebuffer::framePeriod() const
{
    uint64_t ns = 16666667;
    if (pclkKhz_ && htotal_ && vtotal_) {
        const uint64_t q = (uint64_t)htotal_ * vtotal_ * 1000000 / pclkKhz_;
        if (q >= 4000000 && q <= 50000000)      // 20 to 250 Hz
            ns = q;
    }
    uint64_t abs = 0;
    nanoseconds_to_absolutetime(ns, &abs);
    return abs;
}

IOService *NVFramebuffer::probe(IOService *provider, SInt32 *score)
{
    if (!readDisplay(provider))
        return nullptr;
    return IOFramebuffer::probe(provider, score);
}

bool NVFramebuffer::start(IOService *provider)
{
    if (!aperture_ && !(aperture_ = IODeviceMemory::withRange(phys_, size_)))
        return false;
    if (!vblLock_ && !(vblLock_ = IOSimpleLockAlloc()))      // before the base start: it may register VBL
        return false;
    if (!vblCall_ && !(vblCall_ = thread_call_allocate_with_options(vblFire, this, THREAD_CALL_PRIORITY_HIGH,
                                                                    THREAD_CALL_OPTIONS_ONCE)))
        return false;
    if (!IOFramebuffer::start(provider)) {
        LOG("IOFramebuffer::start failed");
        return false;
    }
    // No registerPowerDriver: IOFramebuffer's power callbacks use its controller, which IOGraphics sets up
    // later than this; a power change from here panicked in IOFramebuffer::powerStateWillChangeTo on a NULL
    // controller (2026-10-09). The display's power follows NVBringup's GPU anyway.
    publishTiming();
    publishVblanks();
    const uint64_t now = mach_absolute_time();
    IOSimpleLockLock(vblLock_);
    vblNext_ = now + framePeriod();
    vblActive_ = true;
    IOSimpleLockUnlock(vblLock_);
    thread_call_enter_delayed(vblCall_, vblNext_);
    LOG("started on %s: %ux%u @ %u.%02u Hz, pitch %u, surface 0x%llx (%llu bytes), EDID %s", provider->getName(),
        width_, height_,
        refresh() >> 16, ((refresh() & 0xffff) * 100) >> 16, pitch_, phys_, size_, haveEdid_ ? "yes" : "no");
    return true;
}

void NVFramebuffer::stop(IOService *provider)
{
    // Stop rearming first, then cancel the pending call. A vblFire already past its entry check (vblBusy_)
    // may still be calling the handler; wait for it to leave before the object can go.
    IOSimpleLockLock(vblLock_);
    vblActive_ = false;
    IOSimpleLockUnlock(vblLock_);
    thread_call_cancel(vblCall_);
    for (;;) {
        IOSimpleLockLock(vblLock_);
        const bool busy = vblBusy_ != 0;
        IOSimpleLockUnlock(vblLock_);
        if (!busy)
            break;
        IOSleep(1);
    }
    IOFramebuffer::stop(provider);
}

void NVFramebuffer::free()
{
    if (vblCall_) {
        thread_call_free(vblCall_);     // not pending: stop cancelled it and waited out vblFire
        vblCall_ = nullptr;
    }
    if (vblLock_) {
        IOSimpleLockFree(vblLock_);
        vblLock_ = nullptr;
    }
    if (aperture_) {
        aperture_->release();
        aperture_ = nullptr;
    }
    IOFramebuffer::free();
}

// One vertical blank, at its deadline: call the family's VBL handler, then rearm the next deadline on the same
// grid (absolute times, so no drift). A late wakeup skips to a frame from now rather than firing in a burst.
void NVFramebuffer::vblFire(thread_call_param_t self, thread_call_param_t)
{
    NVFramebuffer *fb = (NVFramebuffer *)self;
    IOSimpleLockLock(fb->vblLock_);
    if (!fb->vblActive_) {
        IOSimpleLockUnlock(fb->vblLock_);
        return;
    }
    fb->vblBusy_++;
    const uint64_t deadline = fb->vblNext_;
    const IOFBInterruptProc proc = fb->vbl_.enabled ? fb->vbl_.proc : nullptr;
    OSObject *target = fb->vbl_.target;
    void *ref = fb->vbl_.ref;
    IOSimpleLockUnlock(fb->vblLock_);

    if (proc)
        proc(target, ref);
    const uint64_t period = fb->framePeriod(), now = mach_absolute_time();
    if (++fb->vblCount_ == 1)
        LOG("first vertical blank (timer, %s handler)", proc ? "with a" : "no");
    if (now >= fb->vblPublishAt_) {                     // about once a second: no registry churn per frame
        fb->publishVblanks();
        uint64_t second = 0;
        nanoseconds_to_absolutetime(1000000000ULL, &second);
        fb->vblPublishAt_ = now + second;
    }

    uint64_t next = deadline + period;
    if (next <= now)                                    // fell behind: skip to the next frame from now
        next = now + period;
    IOSimpleLockLock(fb->vblLock_);
    fb->vblLast_ = deadline;
    fb->vblNext_ = next;
    if (fb->vblActive_)
        thread_call_enter_delayed(fb->vblCall_, next);
    fb->vblBusy_--;
    IOSimpleLockUnlock(fb->vblLock_);                   // the last touch: stop may free fb once this returns
}

// The framebuffer's VBL time and frame period, from the timer's own deadlines.
void NVFramebuffer::getVBLTime(AbsoluteTime *time, AbsoluteTime *delta)
{
    uint64_t last = 0;
    if (vblLock_) {
        IOSimpleLockLock(vblLock_);
        last = vblLast_;
        IOSimpleLockUnlock(vblLock_);
    }
    if (time)
        *time = last;
    if (delta)
        *delta = framePeriod();
}

// The pixel count and clock the family's pipe reads (IOFBCurrentPixelCount, IOFBCurrentPixelClock, in its
// displayModeDidChange). IOGraphics doesn't publish them, so the framebuffer does: pixel count = htotal x
// vtotal, clock in Hz. From these the pipe's frame period is count x 1e9 / clock.
void NVFramebuffer::publishTiming()
{
    const UInt64 count = (UInt64)htotal_ * vtotal_, clock = (UInt64)pclkKhz_ * 1000;
    if (OSNumber *n = OSNumber::withNumber(count, 32)) {
        setProperty("IOFBCurrentPixelCount", (OSObject *)n);
        n->release();
    }
    if (OSNumber *n = OSNumber::withNumber(clock, 32)) {
        setProperty("IOFBCurrentPixelClock", (OSObject *)n);
        n->release();
    }
}

void NVFramebuffer::publishVblanks()
{
    if (OSNumber *n = OSNumber::withNumber(vblCount_, 64)) {
        setProperty("NVFBVblanks", (OSObject *)n);
        n->release();
    }
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
    if (mode != kMode || depth != 0)
        return kIOReturnUnsupported;
    publishTiming();                // the one mode, republished as the pipe reads it on each mode change
    return kIOReturnSuccess;
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

// WindowServer's active state: IOFramebuffer keeps it; this only answers when the base class doesn't.
IOReturn NVFramebuffer::getAttribute(IOSelect attribute, uintptr_t *value)
{
    IOReturn r = IOFramebuffer::getAttribute(attribute, value);
    if (r == kIOReturnUnsupported && attribute == kIOWindowServerActiveAttribute) {
        if (value)
            *value = 1;
        r = kIOReturnSuccess;
    }
    return r;
}

// IOFramebuffer runs power management and asks for each change through kIOPowerAttribute. The display's
// power follows NVBringup's GPU, so only IOGraphics' notifications are sent, as IOFramebuffer.h asks.
IOReturn NVFramebuffer::setAttribute(IOSelect attribute, uintptr_t value)
{
    if (attribute == kIOPowerAttribute) {
        handleEvent(value ? kIOFBNotifyDidPowerOn : kIOFBNotifyWillPowerOff);
        return kIOReturnSuccess;
    }
    IOReturn r = IOFramebuffer::setAttribute(attribute, value);
    return (r == kIOReturnUnsupported && attribute == kIOWindowServerActiveAttribute) ? kIOReturnSuccess : r;
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
    case kConnectionColorModesSupported:        // RGB, 8 bits per component: what the head sends
    case kConnectionColorMode:
        if (value)
            *value = 1;                 // RGB (kIODisplayColorModeRGB, under #if 0 in the SDK)
        return kIOReturnSuccess;
    case kConnectionColorDepthsSupported:
    case kConnectionControllerColorDepth:
    case kConnectionControllerDepthsSupported:
        if (value)
            *value = kIODisplayRGBColorComponentBits8;
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
    case kConnectionPower:
    case kConnectionColorMode:
    case kConnectionControllerColorDepth:
    case kConnectionFlags:
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

// VBL: called by vblFire at the mode's refresh rate. Connect: kept, but never called (no hot-plug yet).
IOReturn NVFramebuffer::registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref,
                                                 void **interruptRef)
{
    Interrupt *i = type == kIOFBVBLInterruptType ? &vbl_ : type == kIOFBConnectInterruptType ? &connect_ : nullptr;
    if (!i)
        return kIOReturnUnsupported;
    if (!vblLock_)
        return kIOReturnNotReady;
    IOSimpleLockLock(vblLock_);
    i->target = target;
    i->ref = ref;
    i->enabled = true;
    i->proc = proc;
    IOSimpleLockUnlock(vblLock_);
    if (interruptRef)
        *interruptRef = i;
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::unregisterInterrupt(void *interruptRef)
{
    if (interruptRef != &vbl_ && interruptRef != &connect_)
        return kIOReturnBadArgument;
    if (!vblLock_)
        return kIOReturnNotReady;
    Interrupt *i = (Interrupt *)interruptRef;
    IOSimpleLockLock(vblLock_);
    i->proc = nullptr;
    i->target = nullptr;
    i->ref = nullptr;
    i->enabled = false;
    IOSimpleLockUnlock(vblLock_);
    return kIOReturnSuccess;
}

IOReturn NVFramebuffer::setInterruptState(void *interruptRef, UInt32 state)
{
    if (interruptRef != &vbl_ && interruptRef != &connect_)
        return kIOReturnBadArgument;
    if (!vblLock_)
        return kIOReturnNotReady;
    IOSimpleLockLock(vblLock_);
    ((Interrupt *)interruptRef)->enabled = state != 0;
    IOSimpleLockUnlock(vblLock_);
    return kIOReturnSuccess;
}
