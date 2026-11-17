// The last-unmap notice for CPU mappings of VRAM, between NVPagerHook.kext (pagerhook/, a Lilu plugin) and
// NVBringup. With it a held BAR1 slice (docs/bar1-cpu-mappings.md) is released when the last duplicate of its
// mapping is gone, instead of when the driver reloads.
//
// What XNU does (checked in xnu-10002.1.13 to xnu-12377.121.6, Sonoma 14.0 to Tahoe 26):
// - IOMemoryDescriptor::withPhysicalAddress + createMappingInTask puts a device pager and one VM object behind
//   the mapping. The object is "named" and shared, so every duplicate (mach_vm_remap, fork with
//   VM_INHERIT_SHARE, a memory entry made from the mapping) references that same object.
// - Its reference count is 1 for the name, 1 for the descriptor's memory entry, and 1 per mapping, duplicate or
//   memory entry.
// - vm_object_deallocate calls memory_object_last_unmap(pager) when the count is about to drop from 2 to 1,
//   that is when only the name would remain. For this pager that is device_pager_last_unmap.
// - A mapping's page-table entries are removed and the TLB flushed (pmap_remove) before its reference is
//   dropped, so at that call no CPU can reach the pages through any mapping of the object.
// So once NVBringup has released its own mapping and descriptor (memFree), device_pager_last_unmap runs exactly
// when the last duplicate goes: at once if there is none, else when the process holding it unmaps it, exits, or
// drops its memory entry. The kernel reports that to nobody; NVPagerHook routes the function and passes it on.
//
// NVPagerHook finds NVBringup (a publish notification) and hands over its ops:
//   gpu->callPlatformFunction(sym(NV_PAGER_HOOK_FN), false, (void *)&ops, nullptr, nullptr, nullptr)
// NVBringup then calls ops->set(fn, ctx), and ops->set(nullptr, nullptr) before it goes away.
//
// Without NVPagerHook (or Lilu, or the symbol) nothing changes: held slices stay held until the driver reloads.
#pragma once

#include <stdint.h>

#define NV_PAGER_HOOK_FN        "NVPagerHook"
#define NV_PAGER_HOOK_VERSION   1

// Called for every device pager in the system, before the kernel's own device_pager_last_unmap, on whatever
// thread dropped the reference (a process unmapping or exiting, or NVBringup's own memFree with gspLock_ held).
// It must not block, take a mutex or touch `pager`: compare the pointer, take a spinlock, schedule work.
typedef void (*nv_pager_last_unmap_fn)(void *ctx, void *pager);

struct nv_pager_hook_ops {
    uint32_t version;               // NV_PAGER_HOOK_VERSION
    uint32_t size;                  // sizeof(nv_pager_hook_ops)
    // One listener. fn nullptr: stop calling; returns once no call is in flight (it may sleep).
    void (*set)(nv_pager_last_unmap_fn fn, void *ctx);
    // device_pager_last_unmap is routed (it isn't until Lilu's patcher has run, or ever if the symbol is missing).
    bool (*routed)(void);
};
