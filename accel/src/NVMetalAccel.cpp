// NVMetalAccel: an IOAccelerator for the GPU NVBringup drives, so Metal.framework lists a device for it
// and loads the NVMetal driver bundle (MetalPluginName / MetalPluginClassName in Info.plist) into apps.
//
// The accelerator does no GPU work: the bundle submits through NVK and NVBringup's own user client. It
// only gives IOAcceleratorFamily2 what its start() needs (config, a stamp page, the GPU tasks, an event
// machine, a headless display machine), modelled on Apple's paravirt GPU driver
// (AppleParavirtAccelerator, read from this Mac's kernel collection), as Navi48-MacOS's Navi48Accel does.
//
// Off unless boot-arg nvaccel=1. The family's classes are declared by IOAccelFamily2_decl.h, generated
// from the kernel collections by tools/gen_decl.py: rebuild it after a macOS update.
#include "IOAccelFamily2_decl.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
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

class NVMetalAccelerator : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVMetalAccelerator)
public:
    IOBufferMemoryDescriptor *stamp_ = nullptr;

    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;

    IOMemoryDescriptor *getStampMemory(unsigned int *count) override;
    IOAccelEventMachine2 *newEventMachine() override;
    IOAccelTask *createUserGPUTask() override;
    IOAccelTask *createKernelGPUTask() override;
    void populateAccelConfig(IOAccelConfig *cfg) override;
    bool configureDevice(IOPCIDevice *dev) override;
    void teardownDevice(IOPCIDevice *dev) override;
    IOAccelDisplayMachine *newDisplayMachine() override;
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

class NVMetalDisplayMachine : public IOAccelDisplayMachine {
    OSDeclareDefaultStructors(NVMetalDisplayMachine)
public:
    bool displayModeWillChange() override { return true; }
    bool displayModeDidChange() override { return true; }
};

OSDefineMetaClassAndStructors(NVMetalAccelerator, IOGraphicsAccelerator2)
OSDefineMetaClassAndStructors(NVMetalEventMachine, IOAccelEventMachineFast2)
OSDefineMetaClassAndStructors(NVMetalTask, IOAccelTask)
OSDefineMetaClassAndStructors(NVMetalDisplayMachine, IOAccelDisplayMachine)

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

// ---- NVMetalAccelerator ---------------------------------------------------------------------------

IOService *NVMetalAccelerator::probe(IOService *provider, SInt32 *score)
{
    if (!enabled() || !layout_ok())
        return nullptr;
    return IOGraphicsAccelerator2::probe(provider, score);
}

bool NVMetalAccelerator::start(IOService *provider)
{
    if (!enabled())
        return false;
    if (!IOGraphicsAccelerator2::start(provider)) {
        LOG("IOGraphicsAccelerator2::start failed");
        return false;
    }
    registerService();      // as AppleParavirtAccelerator::start does
    LOG("started on %s", provider->getName());
    return true;
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

// The paravirt driver's configuration (its populateAccelConfig stores, same on macOS 14 and 26), except
// the display-pipe user client stays off: this accelerator drives no display.
void NVMetalAccelerator::populateAccelConfig(IOAccelConfig *config)
{
    uint8_t *c = (uint8_t *)config;
    *(const char **)(c + 0x00) = kName;
    *(uint32_t *)(c + 0x08) = 0x480000;
    *(uint64_t *)(c + 0x0c) = 7;
    *(uint64_t *)(c + 0x14) = 0x2000000000ull;
    *(uint64_t *)(c + 0x20) = 0x40000000ull;
    *(uint64_t *)(c + 0x28) = 0x100000000008ull;
    *(uint32_t *)(c + 0x30) = 0x40004000;
    c[0x47] = 0;
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
    return OSTypeAlloc(NVMetalDisplayMachine);
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
