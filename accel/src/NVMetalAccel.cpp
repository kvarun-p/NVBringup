// NVMetalAccel: an IOAccelerator for the GPU NVBringup drives. It has two personalities:
//
//  - IOResources (boot-arg nvaccel=1): so Metal.framework lists a device for the GPU and loads the NVMetal
//    driver bundle (MetalPluginName / MetalPluginClassName in Info.plist) into apps. Refused while the
//    display-pipe gate is armed.
//  - IOPCIDevice (S1, the display pipe): on the GPU's PCI device, only while NVBringup on that device has
//    NVDisplayPipeArmed = true. It owns an IOAccelDisplayPipe for NVBringup's NVFramebuffer, which copies
//    WindowServer's plane 0 into the scanout (CPU copy, see NVDisplayPipe). No Metal plugin is published here.
//
// The accelerator does no GPU work of its own. It only gives IOAcceleratorFamily2 what its start() needs
// (config, a stamp page, the GPU tasks, an event machine, a display machine), modelled on Apple's paravirt
// GPU driver (AppleParavirtAccelerator, read from this Mac's kernel collection), as Navi48-MacOS's Navi48Accel does.
//
// The family's classes are declared by IOAccelFamily2_decl.h, generated from the kernel collections
// by tools/gen_decl.py: rebuild it after a macOS update.
#include "IOAccelFamily2_decl.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOMapTypes.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) IOLog("NVMetalAccel: " fmt "\n", ##__VA_ARGS__)

static bool enabled()
{
    uint32_t v = 0;
    return PE_parse_boot_argn("nvaccel", &v, sizeof(v)) && v == 1;
}

// The GPU VA window of each IOAccelTask, as AppleParavirtTask::init sets it up (page 0 reserved).
static const uint64_t kTaskVaSize = 0x400000000ull;
static const char kName[] = "NVMetal";
static const char kGateKey[] = "NVDisplayPipeArmed";   // NVBringup's gate, on itself and on its NVDisplayNub
static const uint32_t kBGRA = 0x42475241;                // 'BGRA': the only plane format the pipe copies
static const uint32_t kSyncCompleteOffset = 3528;        // accelerator field: "transactions complete synchronously" (S0-4)

enum NVAccelMode : uint8_t {
    kModeOff,           // probe() refused: start() refuses too
    kModeResources,     // IOResources personality (nvaccel=1): Metal only
    kModeDisplayPipe,   // IOPCIDevice personality (gate armed): the display pipe, no Metal plugin (S1)
};

class NVMetalAccelerator;

// ---- declarations ---------------------------------------------------------------------------------

class NVMetalDisplayMachine : public IOAccelDisplayMachine {
    OSDeclareDefaultStructors(NVMetalDisplayMachine)
public:
    NVMetalAccelerator *accel_ = nullptr;   // the accelerator that made us; cleared when it stops
    bool displayModeWillChange() override { return true; }
    bool displayModeDidChange() override;   // keeps the accelerator enabled (S0-4 R4)
};

// The S1 display pipe: a plane-0 CPU copy into NVBringup's scanout. Completion is synchronous, so 278
// reports a transaction done once its copy has finished (S0-4 R1). Init is the family's own (not overridden).
class NVDisplayPipe : public IOAccelDisplayPipe {
    OSDeclareDefaultStructors(NVDisplayPipe)
public:
    n_ret validateTransaction(IOAccelDisplayPipeTransaction2 *txn) override;
    n_ret isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn) override;
    n_ret submitTransaction(IOAccelDisplayPipeTransaction2 *txn) override;
private:
    static const uint32_t kDoneRing = 16;   // more than the 4 transactions the family keeps in flight
    uint64_t done_[kDoneRing] = {};         // transaction id + 1 of recently copied transactions; 0 = empty
    uint32_t doneNext_ = 0;
    IOReturn copyPlane0(IOAccelDisplayPipeTransaction2 *txn);
    void markDone(uint32_t id);
    bool isDone(uint32_t id);
};

class NVMetalAccelerator : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVMetalAccelerator)
public:
    IOBufferMemoryDescriptor *stamp_ = nullptr;
    NVAccelMode mode_ = kModeOff;

    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;

    IOMemoryDescriptor *getStampMemory(unsigned int *count) override;
    IOAccelEventMachine2 *newEventMachine() override;
    IOAccelTask *createUserGPUTask() override;
    IOAccelTask *createKernelGPUTask() override;
    void populateAccelConfig(IOAccelConfig *cfg) override;
    bool configureDevice(IOPCIDevice *dev) override;
    void teardownDevice(IOPCIDevice *dev) override;
    IOAccelDisplayMachine *newDisplayMachine() override;
    IOAccelDisplayPipe *newDisplayPipe() override;
    // Contexts and memory objects the bundle never asks the family for.
    void *newGLContext() override { return nullptr; }
    void *newCLContext() override { return nullptr; }
    void *newSurface() override { return nullptr; }
    void *new2DContext() override { return nullptr; }
    void *newVideoContext() override { return nullptr; }
    void *newSysMemory() override { return nullptr; }
    void *newVidMemory() override { return nullptr; }
    void *newResource() override { return nullptr; }
    void *newMemoryMap() override { return nullptr; }

    volatile uint32_t *stampVA() { return stamp_ ? (volatile uint32_t *)stamp_->getBytesNoCopy() : nullptr; }

    // Display-pipe binding (kModeDisplayPipe). The NVFramebuffer is found by a publish notification, bound on
    // bindTimer_'s work loop (not inside the notification), and bound once (S1 does not rebind).
    IOService *pci_ = nullptr;                  // the provider: the GPU's IOPCIDevice (held by the family)
    IOAccelDisplayMachine *machine_ = nullptr;  // the display machine newDisplayMachine made (family-owned)
    IOWorkLoop *bindLoop_ = nullptr;
    IOTimerEventSource *bindTimer_ = nullptr;
    IOSimpleLock *bindLock_ = nullptr;
    IOService *pendingFB_ = nullptr;            // retained: found by fbPublished, waiting for bindTimer_
    IOService *boundFB_ = nullptr;              // retained: the framebuffer the pipe is bound to
    bool bindAttempted_ = false;
    IONotifier *fbPublishNote_ = nullptr;
    IONotifier *fbInterestNote_ = nullptr;

    static bool fbPublished(void *target, void *ref, IOService *fb, IONotifier *notifier);
    static IOReturn fbInterest(void *target, void *ref, UInt32 type, IOService *provider, void *arg, vm_size_t size);
    static void bindAction(OSObject *owner, IOTimerEventSource *sender);
    bool armBinding();
    void teardownBinding();
    void queueBind(IOService *fb);
    void bindPendingFB();
    void framebufferTerminated(IOService *fb);
};

class NVMetalEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVMetalEventMachine)
public:
    bool init(IOGraphicsAccelerator2 *accel, unsigned int count, int timeout) override;
    void enableStampInterrupt(int) override {}
    void disableStampInterrupt(int) override {}
    // No command streams go through the family, so there are no stamps or barriers to write.
    n_ret writeStamp(int, vendevtCommandRec *, unsigned int) override { return 0; }
    n_ret prepareBarrier(vendevtBarrierRec *) override { return 0; }
    n_ret completeBarrier(vendevtBarrierRec *) override { return 1; }
    n_ret writeBarrierElement(vendevtBarrierRec *, int, unsigned int) override { return 0; }
};

class NVMetalTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVMetalTask)
public:
    bool setup(IOGraphicsAccelerator2 *accel);
};

OSDefineMetaClassAndStructors(NVMetalDisplayMachine, IOAccelDisplayMachine)
OSDefineMetaClassAndStructors(NVDisplayPipe, IOAccelDisplayPipe)
OSDefineMetaClassAndStructors(NVMetalAccelerator, IOGraphicsAccelerator2)
OSDefineMetaClassAndStructors(NVMetalEventMachine, IOAccelEventMachineFast2)
OSDefineMetaClassAndStructors(NVMetalTask, IOAccelTask)

// The family's slots that aren't exported: tail jumps through its own vtables.
NVA_FWD_DEFS_IOGraphicsAccelerator2
NVA_FWD_DEFS_IOAccelEventMachine2
NVA_FWD_DEFS_IOAccelEventMachineFast2

// Refuses to run on a kernel whose family classes aren't the size the header was generated for.
static bool layout_ok()
{
    static const struct { const char *name; uint32_t size; } classes[] = {
        { "IOGraphicsAccelerator2", sizeof(IOGraphicsAccelerator2) },
        { "IOAccelEventMachine2", sizeof(IOAccelEventMachine2) },
        { "IOAccelEventMachineFast2", sizeof(IOAccelEventMachineFast2) },
        { "IOAccelTask", sizeof(IOAccelTask) },
        { "IOAccelDisplayMachine", sizeof(IOAccelDisplayMachine) },
        { "IOAccelDisplayPipe", sizeof(IOAccelDisplayPipe) },
        { "IOAccelDisplayPipeTransaction2", sizeof(IOAccelDisplayPipeTransaction2) },
        { "IOFramebuffer", sizeof(IOFramebuffer) },
    };
    for (const auto &c : classes) {
        const OSSymbol *s = OSSymbol::withCString(c.name);
        const OSMetaClass *m = s ? OSMetaClass::getMetaClassWithName(s) : nullptr;
        if (s)
            s->release();
        if (!m || m->getClassSize() != c.size) {
            LOG("%s is %u bytes on this kernel, built for %u: regenerate IOAccelFamily2_decl.h",
                c.name, m ? m->getClassSize() : 0, c.size);
            return false;
        }
    }
    return true;
}

// ---- NVBringup's gate ---------------------------------------------------------------------------------

// Is NVBringup's NVDisplayPipeArmed set? With `on`, only an NVBringup attached to that provider counts.
// NVBringup's probe score (30000) is above this kext's personalities, so NVBringup is registered before the
// PCI personality probes its device; a service not yet registered reads as not armed.
static bool gateArmed(IOService *on)
{
    OSDictionary *m = IOService::serviceMatching("NVBringup");
    if (!m)
        return false;
    OSIterator *it = IOService::getMatchingServices(m);     // consumes m
    if (!it)
        return false;
    bool armed = false;
    while (IOService *s = (IOService *)it->getNextObject()) {
        if (on && s->getProvider() != on)
            continue;
        OSBoolean *b = OSDynamicCast(OSBoolean, s->getProperty(kGateKey));
        if (b && b->isTrue())
            armed = true;
    }
    it->release();
    return armed;
}

// Only with NVBringup's GPU present (it's the GPU the NVMetal bundle drives, through NVK).
static bool have_gpu()
{
    OSDictionary *m = IOService::serviceMatching("NVBringup");
    IOService *s = m ? IOService::copyMatchingService(m) : nullptr;
    if (m)
        m->release();
    if (s)
        s->release();
    return s != nullptr;
}

// ---- counters (registry properties of the accelerator, NVPipe*) ----------------------------------------

static NVMetalAccelerator *gAccel = nullptr;     // the one accelerator of this kext (set in start, cleared in stop)
static uint64_t gSubmits, gCompletes, gErrors;
static uint32_t gLastError;

static void publishU(const char *key, uint64_t v)
{
    NVMetalAccelerator *a = gAccel;
    if (!a)
        return;
    OSNumber *n = OSNumber::withNumber(v, 64);
    if (n) {
        a->setProperty(key, n);
        n->release();
    }
}

static void publishBound(bool on)
{
    NVMetalAccelerator *a = gAccel;
    if (a)     // the kOSBoolean singletons are never freed: setProperty takes its own reference
        a->setProperty("NVPipeBound", (OSObject *)(on ? kOSBooleanTrue : kOSBooleanFalse));
}

static void noteError(IOReturn r)
{
    gErrors++;
    gLastError = (uint32_t)r;
    publishU("NVPipeErrors", gErrors);
    publishU("NVPipeLastError", (uint64_t)(uint32_t)r);
}

// IOAccelDisplayMachine's pipe count (S0-1 §3: found_framebuffer stores pipes at machine+136, count at +264).
static uint32_t pipeCount(IOAccelDisplayMachine *m)
{
    return *(volatile uint32_t *)((uint8_t *)m + 264);
}

// ---- NVMetalAccelerator ---------------------------------------------------------------------------

IOService *NVMetalAccelerator::probe(IOService *provider, SInt32 *score)
{
    if (!layout_ok())
        return nullptr;
    if (OSDynamicCast(IOPCIDevice, provider)) {
        // The display-pipe personality: only while NVBringup on this same device has its gate armed.
        if (!gateArmed(provider))
            return nullptr;
        mode_ = kModeDisplayPipe;
    } else {
        // The IOResources personality (nvaccel=1): it steps aside while the display-pipe gate is armed.
        if (!enabled() || gateArmed(nullptr) || !have_gpu())
            return nullptr;
        mode_ = kModeResources;
    }
    return IOGraphicsAccelerator2::probe(provider, score);
}

bool NVMetalAccelerator::start(IOService *provider)
{
    if (mode_ == kModeOff)
        return false;
    if (!IOGraphicsAccelerator2::start(provider)) {
        LOG("IOGraphicsAccelerator2::start failed");
        return false;
    }
    gAccel = this;
    if (mode_ == kModeDisplayPipe) {
        pci_ = provider;
        enableAccelerator();                // S0-4 R4: transactionEnd is refused while this bit is clear
        // accel+3528 (S0-4 R1): every transaction completes inside submitTransaction (a CPU copy), and no GPU
        // writes NVMetal's stamps. With the flag set, the base event machine passes stamp waits the GPU never
        // reaches (testEventUnlocked, slot 48) and transaction_interrupt_gated completes the live transaction.
        *(volatile uint32_t *)((uint8_t *)this + kSyncCompleteOffset) = 1;
        if (!armBinding()) {
            LOG("cannot arm the framebuffer binding");
            gAccel = nullptr;
            return false;
        }
    }
    registerService();      // as AppleParavirtAccelerator::start does
    LOG("started on %s (%s)", provider->getName(),
        mode_ == kModeDisplayPipe ? "display pipe" : "IOResources, Metal only");
    return true;
}

void NVMetalAccelerator::stop(IOService *provider)
{
    teardownBinding();
    if (gAccel == this)
        gAccel = nullptr;
    IOGraphicsAccelerator2::stop(provider);
}

bool NVMetalAccelerator::configureDevice(IOPCIDevice *)
{
    if (stamp_)
        return true;
    // One page the event machine keeps its stamps in.
    stamp_ = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous | kIOMemoryKernelUserShared, PAGE_SIZE,
        0xFFFFFFFFFull & ~(uint64_t)PAGE_MASK);
    if (!stamp_)
        return false;
    if (stamp_->prepare() != kIOReturnSuccess) {
        stamp_->release();
        stamp_ = nullptr;
        return false;
    }
    bzero(stamp_->getBytesNoCopy(), PAGE_SIZE);
    return true;
}

void NVMetalAccelerator::teardownDevice(IOPCIDevice *)
{
    if (stamp_) {
        stamp_->complete();
        stamp_->release();
        stamp_ = nullptr;
    }
}

IOMemoryDescriptor *NVMetalAccelerator::getStampMemory(unsigned int *count)
{
    if (count)
        *count = 0;
    return stamp_;          // not retained: the family retains it
}

// The paravirt driver's configuration (its populateAccelConfig stores, same on macOS 14 and 26).
void NVMetalAccelerator::populateAccelConfig(IOAccelConfig *config)
{
    uint8_t *c = (uint8_t *)config;
    *(const char **)(c + 0x00) = kName;
    *(uint32_t *)(c + 0x08) = 0x480000;
    *(uint64_t *)(c + 0x0c) = 7;
    *(uint64_t *)(c + 0x14) = 0x2000000000ull;
    *(uint64_t *)(c + 0x20) = 0x40000000ull;
    *(uint64_t *)(c + 0x28) = 0x100000000008ull;   // low dword: framebuffer bound (accel+3248), 8 (S0-1)
    *(uint32_t *)(c + 0x30) = 0x40004000;
    // accel+3279 (config+0x47): newUserClient builds the display-pipe user client (type 4) only when it is
    // nonzero (S0-2 / S0-4 §5.1). Set for the display-pipe personality, clear for Metal-only.
    c[0x47] = mode_ == kModeDisplayPipe ? 1 : 0;
    *(uint32_t *)(c + 0x5c) = 4;
    *(uint32_t *)(c + 0x64) = 2;
    *(uint64_t *)(c + 0x68) = 0x10000000aull;
    *(uint64_t *)(c + 0x88) = 0;
}

IOAccelEventMachine2 *NVMetalAccelerator::newEventMachine()
{
    return OSTypeAlloc(NVMetalEventMachine);   // the family calls init() next
}

static IOAccelTask *new_task(IOGraphicsAccelerator2 *accel)
{
    NVMetalTask *t = OSTypeAlloc(NVMetalTask);
    if (t && !t->setup(accel)) {
        t->release();
        t = nullptr;
    }
    return t;
}

IOAccelTask *NVMetalAccelerator::createUserGPUTask() { return new_task(this); }
IOAccelTask *NVMetalAccelerator::createKernelGPUTask() { return new_task(this); }

IOAccelDisplayMachine *NVMetalAccelerator::newDisplayMachine()
{
    NVMetalDisplayMachine *m = OSTypeAlloc(NVMetalDisplayMachine);
    if (m) {
        m->accel_ = this;
        machine_ = m;
    }
    return m;
}

// The family calls this from found_framebuffer; the pipe's base init runs next (it is not overridden).
IOAccelDisplayPipe *NVMetalAccelerator::newDisplayPipe()
{
    return OSTypeAlloc(NVDisplayPipe);
}

// ---- display-pipe binding -----------------------------------------------------------------------------

bool NVMetalAccelerator::armBinding()
{
    bindLock_ = IOSimpleLockAlloc();
    bindLoop_ = IOWorkLoop::workLoop();
    bindTimer_ = bindLoop_ ? IOTimerEventSource::timerEventSource(this, bindAction) : nullptr;
    if (!bindLock_ || !bindLoop_ || !bindTimer_ || bindLoop_->addEventSource(bindTimer_) != kIOReturnSuccess) {
        teardownBinding();
        return false;
    }
    OSDictionary *m = IOService::serviceMatching("NVFramebuffer");
    fbPublishNote_ = m ? addMatchingNotification(gIOPublishNotification, m, fbPublished, this, nullptr, 0) : nullptr;
    if (!fbPublishNote_) {
        teardownBinding();
        return false;
    }
    return true;
}

void NVMetalAccelerator::teardownBinding()
{
    if (fbPublishNote_) {
        fbPublishNote_->remove();
        fbPublishNote_ = nullptr;
    }
    if (fbInterestNote_) {
        fbInterestNote_->remove();
        fbInterestNote_ = nullptr;
    }
    if (bindTimer_) {
        bindTimer_->cancelTimeout();
        if (bindLoop_)
            bindLoop_->removeEventSource(bindTimer_);
        bindTimer_->release();
        bindTimer_ = nullptr;
    }
    if (bindLoop_) {
        bindLoop_->release();
        bindLoop_ = nullptr;
    }
    if (pendingFB_) {
        pendingFB_->release();
        pendingFB_ = nullptr;
    }
    if (boundFB_) {
        boundFB_->release();
        boundFB_ = nullptr;
        publishBound(false);
    }
    if (bindLock_) {
        IOSimpleLockFree(bindLock_);
        bindLock_ = nullptr;
    }
    if (machine_)
        static_cast<NVMetalDisplayMachine *>(machine_)->accel_ = nullptr;
    machine_ = nullptr;
}

// Publish notification (may run inside IORegistry's registerService): only records the framebuffer and
// schedules bindAction, which runs on bindLoop_.
bool NVMetalAccelerator::fbPublished(void *target, void *, IOService *fb, IONotifier *)
{
    NVMetalAccelerator *a = (NVMetalAccelerator *)target;
    // Framebuffer on our NVDisplayNub (provider) or directly on our PCI device, within two levels (S0-1 §1).
    IOService *p = fb->getProvider();
    bool under = false;
    for (int depth = 0; p && depth < 2 && !under; depth++, p = p->getProvider())
        under = p == a->pci_;
    if (under)
        a->queueBind(fb);
    return true;
}

void NVMetalAccelerator::queueBind(IOService *fb)
{
    IOSimpleLockLock(bindLock_);
    bool queue = !bindAttempted_ && !pendingFB_;
    if (queue) {
        bindAttempted_ = true;
        pendingFB_ = fb;
        fb->retain();
    }
    IOSimpleLockUnlock(bindLock_);
    if (queue)
        bindTimer_->setTimeoutMS(10);
}

void NVMetalAccelerator::bindAction(OSObject *owner, IOTimerEventSource *)
{
    NVMetalAccelerator *a = OSDynamicCast(NVMetalAccelerator, owner);
    if (a)
        a->bindPendingFB();
}

// On bindLoop_: hands the framebuffer to the display machine and checks that its pipe count grew.
void NVMetalAccelerator::bindPendingFB()
{
    IOSimpleLockLock(bindLock_);
    IOService *fb = pendingFB_;
    pendingFB_ = nullptr;
    IOSimpleLockUnlock(bindLock_);
    if (!fb)
        return;
    if (!machine_) {
        LOG("no display machine to bind %s", fb->getName());
        noteError(kIOReturnNotReady);
        fb->release();
        return;
    }
    uint32_t before = pipeCount(machine_);
    bool ok = machine_->found_framebuffer(static_cast<IOFramebuffer *>(fb));
    uint32_t after = pipeCount(machine_);
    if (!ok || after <= before) {
        LOG("found_framebuffer(%s) did not add a pipe (returned %d, count %u -> %u)",
            fb->getName(), ok, before, after);
        noteError(kIOReturnError);
        fb->release();
        return;
    }
    boundFB_ = fb;      // keeps the retain taken in queueBind
    fbInterestNote_ = fb->registerInterest(gIOGeneralInterest, fbInterest, this, nullptr);
    publishBound(true);
    LOG("bound display pipe %u to %s", after - 1, fb->getName());
}

IOReturn NVMetalAccelerator::fbInterest(void *target, void *, UInt32 type, IOService *provider, void *, vm_size_t)
{
    if (type == kIOMessageServiceIsTerminated && target)
        ((NVMetalAccelerator *)target)->framebufferTerminated(provider);
    return kIOReturnSuccess;
}

// S1 does not rebind: the pipe stays and transactions to it fail until the next boot.
void NVMetalAccelerator::framebufferTerminated(IOService *fb)
{
    LOG("NVFramebuffer %s terminated; the display pipe is not rebound (S1)", fb->getName());
    publishBound(false);
}

// ---- NVMetalDisplayMachine ----------------------------------------------------------------------------

bool NVMetalDisplayMachine::displayModeDidChange()
{
    if (accel_)
        accel_->enableAccelerator();
    return true;
}

// ---- NVMetalEventMachine --------------------------------------------------------------------------

bool NVMetalEventMachine::init(IOGraphicsAccelerator2 *accel, unsigned int count, int timeout)
{
    if (!IOAccelEventMachineFast2::init(accel, count, timeout))
        return false;
    NVMetalAccelerator *a = OSDynamicCast(NVMetalAccelerator, accel);
    volatile uint32_t *va = a ? a->stampVA() : nullptr;
    if (!va)
        return false;
    setStampBaseAddress(va);
    return true;
}

// ---- NVMetalTask ----------------------------------------------------------------------------------

// AppleParavirtTask::init: a range allocator over the VA window with page 0 taken, handed to
// IOAccelTask::init, which keeps its own reference.
bool NVMetalTask::setup(IOGraphicsAccelerator2 *accel)
{
    IORangeAllocator *ra = IORangeAllocator::withRange(kTaskVaSize - 1, PAGE_SIZE, 1024, 0);
    if (!ra)
        return false;
    if (!ra->allocateRange(0, PAGE_SIZE)) {
        ra->release();
        return false;
    }
    bool ok = IOAccelTask::init(accel, 1, &ra);
    if (ra)
        ra->release();
    return ok;
}

// ---- NVDisplayPipe --------------------------------------------------------------------------------

static inline uint32_t floorClamp(float v, uint32_t max)
{
    if (!(v > 0))
        return 0;
    if (v >= (float)max)
        return max;
    return (uint32_t)v;
}

static inline uint32_t ceilClamp(float v, uint32_t max)
{
    if (!(v > 0))
        return 0;
    if (v >= (float)max)
        return max;
    uint32_t i = (uint32_t)v;
    return (float)i < v ? i + 1 : i;
}

// Plane 0 of the transaction, if it is a 'BGRA' IOSurface. Otherwise the reason it is refused.
n_ret NVDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    IOSurface *s = txn->getPlaneIOSurface(0, 0);
    IOReturn r = !s ? kIOReturnBadArgument : (s->getPixelFormat() != kBGRA ? kIOReturnUnsupported : kIOReturnSuccess);
    if (r != kIOReturnSuccess)
        noteError(r);
    return (uint32_t)r;
}

// The copy is done here, so the transaction is complete when this returns. An error still completes the
// transaction (so WindowServer does not stall on the fifth submit) and is reported through txn+88.
n_ret NVDisplayPipe::submitTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    const uint32_t id = txn->getTransactionID();
    gSubmits++;
    IOReturn r = copyPlane0(txn);
    if (r == kIOReturnSuccess)
        gCompletes++;
    else
        noteError(r);
    markDone(id);
    publishU("NVPipeSubmits", gSubmits);
    publishU("NVPipeCompletes", gCompletes);
    signalTransactionInterrupt(nullptr);    // kicks completion: the next poll of 278 finds this one done
    return (uint32_t)r;
}

n_ret NVDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *txn)
{
    return isDone(txn->getTransactionID()) ? 1 : 0;
}

void NVDisplayPipe::markDone(uint32_t id)
{
    uint32_t slot = __atomic_fetch_add(&doneNext_, 1, __ATOMIC_RELAXED) % kDoneRing;
    __atomic_store_n(&done_[slot], (uint64_t)id + 1, __ATOMIC_RELEASE);
}

bool NVDisplayPipe::isDone(uint32_t id)
{
    for (uint32_t i = 0; i < kDoneRing; i++)
        if (__atomic_load_n(&done_[i], __ATOMIC_ACQUIRE) == (uint64_t)id + 1)
            return true;
    return false;
}

// Copies plane 0's dirty rect (or the whole surface when the rect is empty or invalid) from the transaction's
// IOSurface into the framebuffer's system aperture. The aperture is mapped write-combined per transaction;
// the source is prepared and mapped per transaction too. Both sides are clamped to the smaller of the two.
IOReturn NVDisplayPipe::copyPlane0(IOAccelDisplayPipeTransaction2 *txn)
{
    IOSurface *s = txn->getPlaneIOSurface(0, 0);
    IOFramebuffer *fb = getFramebuffer();
    if (!s)
        return kIOReturnBadArgument;
    if (!fb)
        return kIOReturnNotReady;
    if (s->getPixelFormat() != kBGRA)
        return kIOReturnUnsupported;

    IOMemoryDescriptor *src = s->getMemoryDescriptor();
    const uint32_t sw = s->getPlaneWidth(0), sh = s->getPlaneHeight(0), spitch = s->getPlaneBytesPerRow(0);
    if (!src || !sw || !sh || spitch < sw * 4 || (spitch & 3))
        return kIOReturnBadArgument;

    int mode = 0, depth = 0;
    IOPixelInformation info;
    bzero(&info, sizeof(info));
    if (fb->getCurrentDisplayMode(&mode, &depth) != kIOReturnSuccess ||
        fb->getPixelInformation(mode, depth, kIOFBSystemAperture, &info) != kIOReturnSuccess)
        return kIOReturnNotReady;
    const uint32_t fw = info.activeWidth, fh = info.activeHeight, fpitch = info.bytesPerRow;
    if (info.bitsPerPixel != 32 || !fw || !fh || fpitch < fw * 4 || (fpitch & 3))
        return kIOReturnUnsupported;
    IODeviceMemory *ap = fb->getApertureRange(kIOFBSystemAperture);
    if (!ap)
        return kIOReturnNotReady;

    const uint32_t w = sw < fw ? sw : fw, h = sh < fh ? sh : fh;
    uint32_t x0 = 0, y0 = 0, x1 = w, y1 = h;
    const IOAccelDisplayRect d = txn->getPlaneDirtyRect(0);
    if (d.w > 0 && d.h > 0) {               // false for NaN, so a bad rect falls back to the whole surface
        uint32_t dx0 = floorClamp(d.x, w), dy0 = floorClamp(d.y, h);
        uint32_t dx1 = ceilClamp(d.x + d.w, w), dy1 = ceilClamp(d.y + d.h, h);
        if (dx1 > dx0 && dy1 > dy0) {
            x0 = dx0; y0 = dy0; x1 = dx1; y1 = dy1;
        }
    }

    if (src->prepare() != kIOReturnSuccess) {
        ap->release();
        return kIOReturnNotReady;
    }
    IOReturn r = kIOReturnSuccess;
    IOMemoryMap *smap = src->map();
    IOMemoryMap *dmap = smap ? ap->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (!smap || !dmap || smap->getLength() < (uint64_t)spitch * sh || dmap->getLength() < (uint64_t)fpitch * fh) {
        r = kIOReturnNoMemory;
    } else {
        const uint8_t *s8 = (const uint8_t *)smap->getVirtualAddress();
        uint8_t *d8 = (uint8_t *)dmap->getVirtualAddress();
        const uint32_t cols = x1 - x0;
        for (uint32_t y = y0; y < y1; y++) {
            const uint32_t *srow = (const uint32_t *)(s8 + (size_t)y * spitch + (size_t)x0 * 4);
            uint32_t *drow = (uint32_t *)(d8 + (size_t)y * fpitch + (size_t)x0 * 4);
            for (uint32_t x = 0; x < cols; x++)
                drow[x] = srow[x];
        }
    }
    if (dmap)
        dmap->release();
    if (smap)
        smap->release();
    src->complete();
    ap->release();          // getApertureRange returns it retained: one leaked reference per frame overflows
    return r;
}

// ---- kext ------------------------------------------------------------------------------------------

// These classes' vtables are checked by `make verify` (tools/verify_vtables.py); nothing here runs on load.
