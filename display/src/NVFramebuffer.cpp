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
// Hot-plug (boot-arg nvdpipe=1): NVBringup keeps its nub up while the display engine runs, lit or not, and
// reports the display on it as Online and its mode as NVDisplayFB, changing both in place and sending
// kMsgState. Online is IOFramebuffer's kConnectionEnable (no display while it's off), and a change raises the
// connect interrupt (kIOFBConnectInterruptType), which makes IOGraphics sense the connection again. The
// aperture stays valid while offline.
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

#include "../../src/nv_pipe_copy.h"

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
    static constexpr uint32_t kEdidMax = 1024;   // NVBringup's kNvEdidMax: the base block and 7 extensions
    uint8_t edid_[kEdidMax] = {};
    uint32_t edidLen_ = 0;              // whole 128-byte blocks in edid_
    bool haveEdid_ = false;
    Interrupt vbl_, connect_;           // guarded by vblLock_
    thread_call_t vblCall_ = nullptr;   // allocated in start, freed in free; armed while vblActive_
    IOLock *applyLock_ = nullptr;       // orders applyNub (start, and the nub's messages); never held across IOGraphics
    IOSimpleLock *vblLock_ = nullptr;   // guards vbl_, connect_, vblActive_, vblBusy_, vblNext_, vblLast_
    bool vblActive_ = false;            // the timer rearms only while this is set
    int vblBusy_ = 0;                   // vblFire calls past their entry check (stop waits for them to leave)
    uint64_t vblNext_ = 0;              // absolute deadline of the next vertical blank
    uint64_t vblLast_ = 0;              // deadline of the last vertical blank fired, for getVBLTime
    uint64_t vblCount_ = 0;             // vertical blanks fired by the timer (published as NVFBVblanks)
    uint64_t vblPublishAt_ = 0;         // absolute time after which NVFBVblanks is published again
    IOService *nub_ = nullptr;          // NVBringup's NVDisplayNub, the provider: set in start, cleared in stop
    OSDictionary *seen_ = nullptr;      // the NVDisplayFB applied last (retained): an equal one is no change
    bool online_ = true;                // NVDisplayNub's Online, true when absent (a nub from before the pipe)
    bool changed_ = false;              // a change IOGraphics has not acknowledged (kConnectionChanged)
    // The hardware cursor, NVBringup's (NV_CURSOR_*_FN): offered while NVDisplayFB has CursorSize.
    IOService *gpu_ = nullptr;          // NVBringup, beside the nub under the PCI device (it outlives the nub)
    uint32_t cursorSize_ = 0;           // NVDisplayFB's CursorSize: the image's largest side, 0 no cursor
    uint32_t *cursorBuf_ = nullptr;     // convertCursorImage's output (kCursorBufPx pixels)
    static constexpr uint32_t kCursorBufPx = 256 * 256;

    static constexpr UInt32 kMsgState = 0x4e564442;     // NVBringup's kNVDisplayMsgState ('NVDB'), the nub's change

    bool readDisplay(OSDictionary *d);
    bool applyNub();
    void raiseConnect();
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
    IOReturn message(UInt32 type, IOService *provider, void *argument = 0) override;

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
    IOReturn setCursorImage(void *cursorImage) override;
    IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;
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

// NVBringup's NVDisplayFB: the scanout surface and the mode it was lit at (or the fallback, while unlit).
// Nothing is assigned unless the whole dictionary is usable, so a bad update leaves the last mode in place.
// No locks: a caller that runs with the framebuffer up holds vblLock_ (applyNub).
bool NVFramebuffer::readDisplay(OSDictionary *d)
{
    if (!d)
        return false;
    auto num = [&](const char *k) -> uint64_t {
        OSNumber *n = OSDynamicCast(OSNumber, d->getObject(k));
        return n ? n->unsigned64BitValue() : 0;
    };
    const uint64_t phys = num("PhysAddr"), size = num("Size");
    const uint32_t width = (uint32_t)num("Width"), height = (uint32_t)num("Height"), pitch = (uint32_t)num("Pitch");
    const uint32_t htotal = (uint32_t)num("HTotal"), vtotal = (uint32_t)num("VTotal");
    if (!phys || !width || !height || pitch < width * 4 || size < (uint64_t)pitch * height || !htotal || !vtotal)
        return false;
    phys_ = phys;
    size_ = size;
    width_ = width;
    height_ = height;
    pitch_ = pitch;
    pclkKhz_ = (uint32_t)num("PixelClockKHz");
    htotal_ = htotal;
    vtotal_ = vtotal;
    hsyncStart_ = (uint32_t)num("HSyncStart");
    hsyncWidth_ = (uint32_t)num("HSyncWidth");
    vsyncStart_ = (uint32_t)num("VSyncStart");
    vsyncWidth_ = (uint32_t)num("VSyncWidth");
    syncFlags_ = (uint32_t)num("Flags");
    cursorSize_ = (uint32_t)num("CursorSize") <= 256 ? (uint32_t)num("CursorSize") : 0;
    haveEdid_ = false;
    edidLen_ = 0;
    if (OSData *e = OSDynamicCast(OSData, d->getObject("EDID")); e && e->getLength() >= 128) {
        // The base block and the extensions it counts (byte 126), as far as NVBringup read them.
        uint32_t n = e->getLength() < kEdidMax ? e->getLength() : kEdidMax;
        n -= n % 128;
        memcpy(edid_, e->getBytesNoCopy(), n);
        const uint32_t want = 128u * (1u + edid_[126]);
        edidLen_ = want < n ? want : n;
        haveEdid_ = true;
    }
    return true;
}

// NVBringup's Online. A nub without it is one from before the display pipe, which exists only while lit.
static bool nubOnline(IOService *nub)
{
    OSBoolean *b = OSDynamicCast(OSBoolean, nub->getProperty("Online"));
    return b ? b->getValue() : true;
}

// Takes the nub's state (NVDisplayFB and Online) as it is now: at start, and on each message from NVBringup
// (outside its gspLock_). Returns true when the aperture, the mode or Online changed; the caller raises the connect
// interrupt then, outside applyLock_ (IOGraphics's handler may call back in, as kConnectionChanged).
bool NVFramebuffer::applyNub()
{
    if (!nub_ || !vblLock_ || !applyLock_)
        return false;
    IOLockLock(applyLock_);
    bool changed = false;
    // copyProperty: NVBringup replaces NVDisplayFB on a replug, and getProperty's unretained dictionary can be freed
    // between the read and the retain below.
    OSObject *o = nub_->copyProperty("NVDisplayFB");
    OSDictionary *d = OSDynamicCast(OSDictionary, o);
    const bool online = nubOnline(nub_);
    if (d && !(seen_ && online == online_ && d->isEqualTo(seen_))) {
        const uint64_t oldPhys = phys_, oldSize = size_;
        IOSimpleLockLock(vblLock_);
        const bool ok = readDisplay(d);
        if (ok)
            online_ = online;
        IOSimpleLockUnlock(vblLock_);
        if (!ok) {
            LOG("NVDisplayFB unusable: keeping the last mode");
        } else {
            changed = true;
            if (phys_ != oldPhys || size_ != oldSize) {  // a new surface: IOGraphics gets a new aperture
                IODeviceMemory *fresh = IODeviceMemory::withRange(phys_, size_), *old = nullptr;
                if (fresh) {
                    IOSimpleLockLock(vblLock_);
                    old = aperture_;
                    aperture_ = fresh;
                    IOSimpleLockUnlock(vblLock_);
                    OSSafeReleaseNULL(old);
                } else {
                    LOG("no aperture for the new surface 0x%llx: the old one stays", (unsigned long long)phys_);
                }
            }
            publishTiming();
            if (seen_)
                seen_->release();
            seen_ = d;
            d->retain();
            LOG("nub: %ux%u @ %u.%02u Hz, pitch %u, surface 0x%llx, online %s", width_, height_,
                refresh() >> 16, ((refresh() & 0xffff) * 100) >> 16, pitch_, (unsigned long long)phys_,
                online ? "yes" : "no");
        }
    }
    OSSafeReleaseNULL(o);
    IOLockUnlock(applyLock_);
    return changed;
}

// The connect interrupt (kIOFBConnectInterruptType): IOGraphics senses the connection again. kConnectionChanged stays
// set until IOGraphics acknowledges it. No lock is held while the handler runs.
void NVFramebuffer::raiseConnect()
{
    IOSimpleLockLock(vblLock_);
    changed_ = true;
    const IOFBInterruptProc proc = connect_.enabled ? connect_.proc : nullptr;
    OSObject *target = connect_.target;
    void *ref = connect_.ref;
    IOSimpleLockUnlock(vblLock_);
    if (proc)
        proc(target, ref);
}

// NVBringup's nub sends kMsgState to its clients after changing NVDisplayFB or Online (dispNubSync).
IOReturn NVFramebuffer::message(UInt32 type, IOService *provider, void *argument)
{
    if (type == kMsgState && provider == nub_) {
        if (applyNub())
            raiseConnect();
        return kIOReturnSuccess;
    }
    return IOFramebuffer::message(type, provider, argument);
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
    OSObject *o = provider->copyProperty("NVDisplayFB");
    const bool ok = readDisplay(OSDynamicCast(OSDictionary, o));
    OSSafeReleaseNULL(o);
    if (!ok)
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
    if (!applyLock_ && !(applyLock_ = IOLockAlloc()))
        return false;
    // NVBringup's state (Online, the mode) before the base start, which may probe the connection. A message from the
    // nub before this point is not lost: it applies the same NVDisplayFB.
    nub_ = provider;
    if (IOService *pci = provider->getProvider()) {
        if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
            while (OSObject *o = it->getNextObject())
                if (o->metaCast("NVBringup")) {
                    gpu_ = (IOService *)o;
                    break;
                }
            it->release();
        }
    }
    uint32_t sw = 0;                // boot-arg nvswcursor=1: no hardware cursor (WindowServer draws it)
    if (!cursorBuf_ && !(PE_parse_boot_argn("nvswcursor", &sw, sizeof(sw)) && sw))
        cursorBuf_ = (uint32_t *)IOMallocZero(kCursorBufPx * 4);
    applyNub();                     // no connect interrupt: IOGraphics hasn't registered for it yet
    if (!IOFramebuffer::start(provider)) {
        nub_ = nullptr;
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
    LOG("started on %s: %ux%u @ %u.%02u Hz, pitch %u, surface 0x%llx (%llu bytes), EDID %s, online %s",
        provider->getName(), width_, height_, refresh() >> 16, ((refresh() & 0xffff) * 100) >> 16, pitch_,
        (unsigned long long)phys_, (unsigned long long)size_, haveEdid_ ? "yes" : "no", online_ ? "yes" : "no");
    return true;
}

void NVFramebuffer::stop(IOService *provider)
{
    // Stop rearming first, then cancel the pending call. A vblFire already past its entry check (vblBusy_)
    // may still be calling the handler; wait for it to leave before the object can go.
    IOSimpleLockLock(vblLock_);
    vblActive_ = false;
    IOSimpleLockUnlock(vblLock_);
    nub_ = nullptr;                 // no more messages: NVBringup terminates the nub only from its dispNubSync
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
    if (applyLock_) {
        IOLockFree(applyLock_);
        applyLock_ = nullptr;
    }
    if (aperture_) {
        aperture_->release();
        aperture_ = nullptr;
    }
    OSSafeReleaseNULL(seen_);
    if (cursorBuf_) {
        IOFree(cursorBuf_, kCursorBufPx * 4);
        cursorBuf_ = nullptr;
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
    if (aperture != kIOFBSystemAperture || !vblLock_)
        return nullptr;
    IOSimpleLockLock(vblLock_);     // applyNub swaps it on a new surface
    IODeviceMemory *a = aperture_;
    if (a)
        a->retain();                // the caller releases it
    IOSimpleLockUnlock(vblLock_);
    return a;
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
    if (attribute == kIOHardwareCursorAttribute) {
        if (value)
            *value = gpu_ && cursorBuf_ && cursorSize_ ? 1 : 0;
        return kIOReturnSuccess;
    }
    IOReturn r = IOFramebuffer::getAttribute(attribute, value);
    if (r == kIOReturnUnsupported && attribute == kIOWindowServerActiveAttribute) {
        if (value)
            *value = 1;
        r = kIOReturnSuccess;
    }
    return r;
}

// ---- The hardware cursor --------------------------------------------------------------------------
//
// NVBringup's head 0 cursor: a 256x256 A8R8G8B8 image, straight alpha (what convertCursorImage writes for a 32-bit
// descriptor), positioned by its top-left corner. setCursorState must not block: NVBringup's move is a few
// register writes under a spin lock.

static const OSSymbol *nvSym(const OSSymbol *&s, const char *name)
{
    if (!s)
        s = OSSymbol::withCStringNoCopy(name);      // kept for the kext's lifetime
    return s;
}

IOReturn NVFramebuffer::setCursorImage(void *cursorImage)
{
    static const OSSymbol *fn;
    if (!gpu_ || !cursorBuf_ || !cursorSize_ || !nvSym(fn, NV_CURSOR_IMAGE_FN))
        return kIOReturnUnsupported;
    IOHardwareCursorDescriptor desc;
    bzero(&desc, sizeof(desc));
    desc.majorVersion = kHardwareCursorDescriptorMajorVersion;
    desc.minorVersion = kHardwareCursorDescriptorMinorVersion;
    desc.width = desc.height = cursorSize_;
    desc.bitDepth = 32;
    desc.supportedSpecialEncodings = kTransparentEncodedPixel;
    IOHardwareCursorInfo info;
    bzero(&info, sizeof(info));
    info.majorVersion = kHardwareCursorInfoMajorVersion;
    info.minorVersion = kHardwareCursorInfoMinorVersion;
    info.hardwareCursorData = (UInt8 *)cursorBuf_;
    bzero(cursorBuf_, kCursorBufPx * 4);
    if (!convertCursorImage(cursorImage, &desc, &info) || !info.cursorWidth || !info.cursorHeight ||
        info.cursorWidth > cursorSize_ || info.cursorHeight > cursorSize_)
        return kIOReturnUnsupported;
    IOReturn r = gpu_->callPlatformFunction(fn, false, cursorBuf_, (void *)(uintptr_t)info.cursorWidth,
                                            (void *)(uintptr_t)info.cursorHeight, nullptr);
    static uint32_t logged;
    if (logged++ < 4)
        LOG("cursor image %ux%u (hot spot %u,%u): %#x", (unsigned)info.cursorWidth, (unsigned)info.cursorHeight,
            (unsigned)info.cursorHotSpotX, (unsigned)info.cursorHotSpotY, r);
    return r == kIOReturnSuccess ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn NVFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
    static const OSSymbol *fn;
    if (!gpu_ || !cursorSize_ || !nvSym(fn, NV_CURSOR_MOVE_FN))
        return kIOReturnUnsupported;
    gpu_->callPlatformFunction(fn, false, (void *)(intptr_t)x, (void *)(intptr_t)y, (void *)(uintptr_t)visible,
                               nullptr);
    return kIOReturnSuccess;
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
    case kConnectionCheckEnable:
        // IOFramebuffer::updateOnline: the framebuffer is online exactly when this is non-zero (re-read at every
        // connect interrupt). Always 1 kept an unplugged HDMI display online, nameless ("Unknown Display").
        if (value)
            *value = online_ ? 1 : 0;
        return kIOReturnSuccess;
    case kConnectionEnable:
        if (value)
            *value = online_ ? 1 : 0;   // a display is lit (Online); off, IOGraphics sees no display
        return kIOReturnSuccess;
    case kConnectionChanged:
        if (value)
            *value = changed_ ? 1 : 0;  // a change since IOGraphics last acknowledged it
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
    case kConnectionChanged:
        if (value) {
            if (applyNub())         // IOGraphics asks for a fresh read of the nub: a change found here is raised,
                raiseConnect();     // and a repeat request finds nothing new, so it doesn't loop
        } else {
            IOSimpleLockLock(vblLock_);
            changed_ = false;       // acknowledged
            IOSimpleLockUnlock(vblLock_);
        }
        return kIOReturnSuccess;
    default:
        return IOFramebuffer::setAttributeForConnection(connect, attribute, value);
    }
}

bool NVFramebuffer::hasDDCConnect(IOIndex connect)
{
    return connect == 0 && haveEdid_ && online_;
}

IOReturn NVFramebuffer::getDDCBlock(IOIndex connect, UInt32 blockNumber, IOSelect blockType,
                                    IOOptionBits options, UInt8 *data, IOByteCount *length)
{
    if (connect != 0 || !haveEdid_ || blockType != kIODDCBlockTypeEDID || !data || !length)
        return kIOReturnBadArgument;
    // blockNumber counts from 1: the base block, then the extensions (the CTA-861 block of an HDMI sink).
    if (!blockNumber || (uint64_t)blockNumber * 128 > edidLen_)
        return kIOReturnUnsupported;
    const IOByteCount n = *length < 128 ? *length : 128;
    memcpy(data, edid_ + (blockNumber - 1) * 128, n);
    *length = n;
    return kIOReturnSuccess;
}

// VBL: called by vblFire at the mode's refresh rate. Connect: called by raiseConnect when Online or the mode changes.
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
