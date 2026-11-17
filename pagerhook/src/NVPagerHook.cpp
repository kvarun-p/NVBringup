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

static const nv_pager_hook_ops kOps = { NV_PAGER_HOOK_VERSION, sizeof(nv_pager_hook_ops), hookSet, hookRouted };

static void patcherLoaded(void *, KernelPatcher &patcher)
{
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
