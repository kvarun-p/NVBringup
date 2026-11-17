// NVPagerHook: tells NVBringup when the last mapping of a device pager's memory is gone (a Lilu plugin).
//
// NVBringup holds the BAR1 slice of freed CPU-mapped VRAM because a process may have duplicated the mapping, and
// macOS doesn't tell a driver when the last duplicate goes. The kernel knows: it calls device_pager_last_unmap
// then (../../src/nv_pager_hook.h has the details and the XNU versions checked). This plugin routes that function
// and calls NVBringup's listener before the original runs. Everything else (which pagers are NVBringup's, what to
// release) is NVBringup's.
//
// Two IOService classes: Lilu's own (NVPagerHook, from plugin_start.cpp) and NVPagerHookLink below, which waits
// for NVBringup to be published and hands it the ops through callPlatformFunction. Neither kext links against
// the other, so either loads without the other, in any order.
//
// The hook runs on every last unmap of device memory in the system (framebuffers, other PCI drivers' BARs), on
// the thread that dropped the reference. It does nothing but call the listener, which doesn't block.
// It also gives NVBringup io/pin/unpin: whether I/O is still in flight on a pager's VM object (a last unmap
// doesn't mean the DMA is done), and a pager reference that keeps the object alive until it is.
// A routed kernel function can't be unrouted safely, so this kext doesn't unload (Lilu refuses plugin unloads).
// Disabled by -nvpgroff.

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/OSAtomic.h>

#include "nv_pager_hook.h"

static mach_vm_address_t orgLastUnmap;
static volatile bool gRouted;
static nv_pager_last_unmap_fn gFn;      // read and written with __atomic builtins
static void *gCtx;
static volatile SInt32 gInFlight;       // hooks between reading gFn and returning from it

// kern_return_t device_pager_last_unmap(memory_object_t mem_obj), osfmk/vm/device_vm.c. Same signature in every
// XNU checked; reached through the pager's ops table, so the function body always exists to route.
static kern_return_t wrapLastUnmap(void *memObj)
{
    OSIncrementAtomic(&gInFlight);
    nv_pager_last_unmap_fn fn = __atomic_load_n(&gFn, __ATOMIC_SEQ_CST);
    if (fn)
        fn(__atomic_load_n(&gCtx, __ATOMIC_SEQ_CST), memObj);
    OSDecrementAtomic(&gInFlight);
    return FunctionCast(wrapLastUnmap, orgLastUnmap)(memObj);
}

// nv_pager_hook_ops::set. Setting: the context first, so a hook never sees the function with a stale context.
// Clearing: the function first, then wait for hooks that had already read it.
static void hookSet(nv_pager_last_unmap_fn fn, void *ctx)
{
    if (fn) {
        __atomic_store_n(&gCtx, ctx, __ATOMIC_SEQ_CST);
        __atomic_store_n(&gFn, fn, __ATOMIC_SEQ_CST);
        return;
    }
    __atomic_store_n(&gFn, (nv_pager_last_unmap_fn)nullptr, __ATOMIC_SEQ_CST);
    while (gInFlight > 0)
        IOSleep(1);
    __atomic_store_n(&gCtx, (void *)nullptr, __ATOMIC_SEQ_CST);
}

static bool hookRouted()
{
    return gRouted;
}

// ---- In-flight I/O on a device pager's VM object (nv_pager_hook.h: io, pin, unpin) ----------
//
// Private layouts, x86_64, checked against xnu-10063.141.1 (macOS 14.8.9) source and its shipped kernel code:
// - struct memory_object (osfmk/mach/memory_object_types.h): mo_ikot and mo_ref (4 bytes each), then
//   mo_pager_ops at 0x8 (memory_object_deallocate: movq 0x8(%rdi)) and mo_control at 0x10
//   (device_pager_deallocate and device_pager_populate_object use 0x10). A memory_object_control_t is the
//   vm_object_t itself (memory_object_control_to_vm_object returns it).
// - struct vm_object (osfmk/vm/vm_object.h): pager at 0x50; paging_in_progress, 16 bits, at 0x74 and
//   activity_in_progress, 32 bits, at 0x78 (vm_object_iopl_request: incw 0x74(obj), incl 0x78(obj)).
// Every io() call checks the pager's ops table is device_pager_ops and that the object points back at the
// pager, else returns -1; NVBringup's self-test checks that an IOPL on its test page raises the count and
// committing it lowers it again. On a kernel where any of that doesn't hold, the early release stays off.
static const size_t kMoPagerOps = 0x8, kMoControl = 0x10;
static const size_t kVoPager = 0x50, kVoPagingInProgress = 0x74, kVoActivityInProgress = 0x78;

static mach_vm_address_t gDevicePagerOps;
static void (*gPagerReference)(void *);
static void (*gPagerDeallocate)(void *);

static bool kernelPtr(const void *p)
{
    return (uintptr_t)p >= 0xffffff8000000000ull;
}

static int32_t hookIo(void *pager)
{
    if (!gDevicePagerOps || !kernelPtr(pager))
        return -1;
    const uint8_t *mo = (const uint8_t *)pager;
    if (*(void *const *)(mo + kMoPagerOps) != (void *)gDevicePagerOps)
        return -1;                      // not a device pager
    const uint8_t *obj = *(const uint8_t *const *)(mo + kMoControl);
    if (!kernelPtr(obj) || *(void *const *)(obj + kVoPager) != pager)
        return -1;                      // no object yet, or not the layout above
    uint32_t act = __atomic_load_n((const uint32_t *)(obj + kVoActivityInProgress), __ATOMIC_ACQUIRE);
    uint16_t pip = __atomic_load_n((const uint16_t *)(obj + kVoPagingInProgress), __ATOMIC_ACQUIRE);
    uint64_t n = (uint64_t)act + pip;
    return n > 0x7fffffffull ? 0x7fffffff : (int32_t)n;
}

static void hookPin(void *pager)
{
    if (gPagerReference && kernelPtr(pager))
        gPagerReference(pager);
}

static void hookUnpin(void *pager)
{
    if (gPagerDeallocate && kernelPtr(pager))
        gPagerDeallocate(pager);
}

static const nv_pager_hook_ops kOps = { NV_PAGER_HOOK_VERSION, sizeof(nv_pager_hook_ops), hookSet, hookRouted,
                                        hookIo, hookPin, hookUnpin };

static void patcherLoaded(void *, KernelPatcher &patcher)
{
    // io/pin/unpin first: without them NVBringup's self-test fails and nothing is released early, so routing
    // last_unmap without them is harmless but useless. Each missing one is logged.
    gDevicePagerOps = patcher.solveSymbol(KernelPatcher::KernelID, "_device_pager_ops");
    mach_vm_address_t ref = patcher.solveSymbol(KernelPatcher::KernelID, "_device_pager_reference");
    mach_vm_address_t dealloc = patcher.solveSymbol(KernelPatcher::KernelID, "_device_pager_deallocate");
    patcher.clearError();
    if (!gDevicePagerOps || !ref || !dealloc) {
        SYSLOG("nvpgr", "device_pager_ops %d, device_pager_reference %d, device_pager_deallocate %d not found: no I/O "
               "check, so NVBringup won't release held slices early", !gDevicePagerOps, !ref, !dealloc);
        gDevicePagerOps = 0;
    } else {
        gPagerReference = (void (*)(void *))ref;
        gPagerDeallocate = (void (*)(void *))dealloc;
    }

    KernelPatcher::RouteRequest req("_device_pager_last_unmap", wrapLastUnmap, orgLastUnmap);
    if (!patcher.routeMultiple(KernelPatcher::KernelID, &req, 1)) {
        SYSLOG("nvpgr", "device_pager_last_unmap not routed (%d): NVBringup keeps held BAR1 slices until it reloads",
               patcher.getError());
        patcher.clearError();
        return;
    }
    gRouted = true;
    SYSLOG("nvpgr", "device_pager_last_unmap routed");
}

static void pluginStart()
{
    lilu.onPatcherLoadForce(patcherLoaded);
}

// ---- The link to NVBringup ------------------------------------------------------------------

class NVPagerHookLink : public IOService {
    OSDeclareDefaultStructors(NVPagerHookLink)

public:
    bool start(IOService *provider) override;

private:
    IONotifier *notifier_ = nullptr;
    static bool gpuPublished(void *target, void *ref, IOService *gpu, IONotifier *notifier);
};

OSDefineMetaClassAndStructors(NVPagerHookLink, IOService)

// Every NVBringup instance, now and later (it is published again after a reload).
bool NVPagerHookLink::start(IOService *provider)
{
    if (!IOService::start(provider))
        return false;
    OSDictionary *match = serviceMatching("NVBringup");
    if (match) {
        notifier_ = addMatchingNotification(gIOPublishNotification, match, &NVPagerHookLink::gpuPublished, this);
        match->release();
    }
    if (!notifier_)
        SYSLOG("nvpgr", "no publish notification for NVBringup: it won't get the hook");
    return true;                        // stays: the notifier lives as long as the kext
}

bool NVPagerHookLink::gpuPublished(void *, void *, IOService *gpu, IONotifier *)
{
    const OSSymbol *fn = OSSymbol::withCString(NV_PAGER_HOOK_FN);
    if (!fn)
        return true;
    IOReturn r = gpu->callPlatformFunction(fn, false, (void *)&kOps, nullptr, nullptr, nullptr);
    fn->release();
    // kIOReturnUnsupported: an NVBringup from before the hook. Anything else it logs itself.
    SYSLOG("nvpgr", "ops handed to NVBringup: 0x%x%s", r, gRouted ? "" : " (not routed yet)");
    return true;
}

static const char *kDisable[] {"-nvpgroff"};
static const char *kDebug[] {"-nvpgrdbg"};
static const char *kBeta[] {"-nvpgrbeta"};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    kDisable, arrsize(kDisable),
    kDebug, arrsize(kDebug),
    kBeta, arrsize(kBeta),
    KernelVersion::Sonoma,
    KernelVersion::Tahoe,
    pluginStart
};
