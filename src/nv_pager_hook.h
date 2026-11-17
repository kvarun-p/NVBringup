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
// No mapping left is not the same as nobody using the pages: I/O can still be in flight on them. Direct I/O
// (F_NOCACHE) on a mapping of this contiguous device memory goes through cluster_write_contig /
// cluster_read_contig, and the disk DMAs straight to or from the BAR1 address. The I/O page list (UPL) it uses
// holds no reference on the VM object: vm_map_create_upl drops its own once vm_object_iopl_request has raised
// the object's activity_in_progress, and that count is all that stays raised until the UPL is committed.
// vm_object_deallocate waits only for mappings in progress before calling last_unmap, not for activity. So a
// process that frees or unmaps the memory during such an I/O gets the report with the DMA still running;
// releasing the slice then hands the DMA's BAR1 range to another allocation, or leaves it unmapped (a disk DMA
// to unmapped BAR1 can make the disk controller fail: an NVMe "controller fatal" panic on 2026-11-17).
// So the listener asks io(pager) before treating a report as final. There is no race with I/O just starting:
// vm_map_create_upl raises the activity count before it drops its temporary object reference, so an I/O
// being set up as the last mapping goes keeps the count above 2 until it is counted, and then the report comes
// from its own drop with the activity already raised. Once the report has come, no new I/O can start (that
// needs a mapping or a memory entry, i.e. a reference).
// While I/O is in flight the listener pins the pager (pin: a device pager reference, which keeps the object's
// destruction back) and the slice stays on the dummy page; when io() says the I/O is done, NVBringup unpins and
// releases the slice.
//
// NVPagerHook finds NVBringup (a publish notification) and hands over its ops:
//   gpu->callPlatformFunction(sym(NV_PAGER_HOOK_FN), false, (void *)&ops, nullptr, nullptr, nullptr)
// NVBringup then calls ops->set(fn, ctx), and ops->set(nullptr, nullptr) before it goes away.
//
// Without NVPagerHook (or Lilu, or the symbol) nothing changes: held slices stay held until the driver reloads.
#pragma once

#include <stdint.h>

#define NV_PAGER_HOOK_FN        "NVPagerHook"
#define NV_PAGER_HOOK_VERSION   2       // 2: io, pin, unpin (1 had no I/O check, so released under in-flight DMA)

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
    // I/O page lists in flight on the VM object behind a device pager (its activity_in_progress plus
    // paging_in_progress), or -1 if the pager or its object doesn't look the way NVPagerHook knows (not a device
    // pager, a different layout, symbols missing): treat that as never idle. Reads two counters, takes no lock
    // and doesn't block. The object must be alive: call it from inside the listener, while something references
    // the memory, or on a pinned pager.
    int32_t (*io)(void *pager);
    // A reference on the device pager (device_pager_reference / device_pager_deallocate). Held, it keeps the VM
    // object from being destroyed after its last mapping, so io() stays safe to call. pin doesn't block (call it
    // from the listener); unpin may destroy the object and block: call it from a thread, with no driver lock held.
    void (*pin)(void *pager);
    void (*unpin)(void *pager);
};
