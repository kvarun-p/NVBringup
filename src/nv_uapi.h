// Kernel <-> user interface of NVBringup's GPU client (Phase 5): IOServiceOpen type 1.
// Shared by the kext (src/NVGpu.cpp) and user space (tools/libnvmac). Shaped after Mesa's
// nvkmd layer: memory objects, user-managed GPU VA with bind/unbind, execution contexts,
// and 64-bit timeline syncs.
//
// Every call goes through IOConnectCallMethod with the selector below. Structs are fixed
// size and naturally aligned; scalars are uint64_t. Errors are IOReturn values:
//   kIOReturnBadArgument   invalid handle, range, count or flag
//   kIOReturnNoMemory      VRAM / BAR1 / system memory exhausted
//   kIOReturnNoDevice      GSP-RM is not running, or a context of this connection was lost
//   kIOReturnTimeout       SYNC_WAIT timed out
//   kIOReturnAborted       SYNC_WAIT interrupted by a signal
//   kIOReturnBusy          EXEC: the context's ring stayed full for 5 s
#pragma once

#include <stdint.h>

#define NVMAC_CLIENT_TYPE   1
// Control client (IOServiceOpen type 2): GPU power without opening the GPU, so asking doesn't wake
// it. Same privilege as a GPU client (root or the console user).
#define NVMAC_CONTROL_TYPE  2
enum nvmac_control_selector {
    NVMAC_POWER_STATUS = 0, // scalar out: state, mode, idle seconds, power-offs, power-ons, last power-on ms, connections
    NVMAC_POWER_SET    = 1, // scalar in: mode (NVMAC_POWER_MODE_*); then as NVMAC_POWER_STATUS
    NVMAC_POWER_INFO   = 2, // struct out: nvmac_info, without powering the GPU on: live while it runs,
                            // else the copy from its last boot (vram_used, bar1_used 0);
                            // kIOReturnNotReady before the first boot, kIOReturnNoDevice when
                            // switched off. Lets Vulkan list the GPU.
    NVMAC_PERF         = 3, // scalars in: none (query), or boost command (NVMAC_BOOST_*) and seconds;
                            // scalars out: current P-state (0 = P0, fastest; ~0 unknown), RM status
                            // of the boost (0 ok), boost level held now (NVMAC_BOOST_*), boosts sent.
                            // kIOReturnNoDevice while the GPU is off or GSP-RM isn't running.
    NVMAC_PERF_POLICY  = 4, // scalars in: none (query), or the NVMAC_PERF_POLICY_COUNT fields below in
                            // order; scalars out: those fields, then level held, busy EWMA (per mille),
                            // and counts of TO_MAX, 1LEVEL and CLEAR requests sent. Works with the GPU off.
};
// NV2080_CTRL_PERF_BOOST_FLAGS_CMD values. A boost lasts the given seconds (at most 3600) or until
// cleared; GSP-RM tracks it per RM client, and other limits (power, thermals) still apply.
#define NVMAC_BOOST_CLEAR           0
#define NVMAC_BOOST_1LEVEL          1
#define NVMAC_BOOST_TO_MAX          2
#define NVMAC_PERF_COUNT            4
// Automatic boost after EXEC (boot-arg nvboost=<policy>, default adaptive):
//   OFF       clocks are left to GSP-RM's own controller (~250 ms to raise the memory clock)
//   FIXED     TO_MAX for `seconds` after each EXEC, asked again every seconds/2 while work comes
//   ADAPTIVE  `burst` (a boost command, or CLEAR for none) on the first EXEC after idle; TO_MAX
//             once the busy fraction (sampled every 20 ms, ~80 ms EWMA) reaches `busy_pct`, or when
//             an EXEC finds earlier work still queued; CLEAR once the GPU has been idle `idle_ms`.
#define NVMAC_PERF_POLICY_OFF       0
#define NVMAC_PERF_POLICY_FIXED     1
#define NVMAC_PERF_POLICY_ADAPTIVE  2
#define NVMAC_PERF_POLICY_COUNT     5   // policy, seconds, burst, busy_pct, idle_ms
#define NVMAC_PERF_POLICY_OUT       10
#define NVMAC_POWER_STATE_ON        0
#define NVMAC_POWER_STATE_OFF       1
#define NVMAC_POWER_STATE_SWITCHING 2
#define NVMAC_POWER_STATE_FAILED    3
#define NVMAC_POWER_MODE_OFF        0   // off now and kept off (opens fail) until another mode
#define NVMAC_POWER_MODE_ON         1   // on now and kept on
#define NVMAC_POWER_MODE_AUTO       2   // off after the idle time without connections, on at open
#define NVMAC_POWER_STATUS_COUNT    7
#define NVMAC_ABI_VERSION   1

enum nvmac_selector {
    NVMAC_GET_INFO = 0,     // struct out: nvmac_info
    NVMAC_MEM_ALLOC,        // scalars in: size, align, flags, pte kind; out: handle, size
    NVMAC_MEM_FREE,         // scalar in: handle (unbinds it everywhere, drops the CPU mapping)
    NVMAC_MEM_MAP,          // scalar in: handle; out: address, size (one CPU mapping per mem)
    NVMAC_MEM_UNMAP,        // scalar in: handle
    NVMAC_VM_BIND,          // struct in: nvmac_bind_hdr + ops
    NVMAC_CTX_CREATE,       // scalar in: engines; out: ctx handle, handle of its seqno sync
                            //   (wait-only: SYNC_WAIT on it waits for EXEC seqnos)
    NVMAC_CTX_DESTROY,      // scalar in: ctx
    NVMAC_EXEC,             // struct in: nvmac_exec_hdr + waits + pushes + signals; scalar out: seqno
    NVMAC_SYNC_CREATE,      // scalar in: initial value; out: handle, byte offset in the sync page
    NVMAC_SYNC_DESTROY,     // scalar in: handle
    NVMAC_SYNC_WAIT,        // struct in: nvmac_wait_hdr + entries; scalar out: index of a signaled entry (any)
    NVMAC_SYNC_SIGNAL,      // scalars in: handle, value (CPU signal; values only move forward)
    NVMAC_MAP_SYNC_PAGE,    // scalar out: address, size (read-only; sync values at their offsets)
    NVMAC_GET_TIMESTAMP,    // scalar out: GPU time in ns (PTIMER)
    NVMAC_CTX_STATUS,       // scalar in: ctx; out: state (0 ok, 1 lost), exception type, completed seqno
    NVMAC_SELECTOR_COUNT
};

// NVMAC_MEM_ALLOC flags. Exactly one of VRAM / GART. GART memory is always CPU-mappable
// (coherent); VRAM | CAN_MAP is mapped through BAR1 (256 MiB aperture, write-combined).
enum {
    NVMAC_MEM_VRAM    = 1u << 0,
    NVMAC_MEM_GART    = 1u << 1,
    NVMAC_MEM_CAN_MAP = 1u << 2,
};

// NVMAC_CTX_CREATE engines (same bits as nvkmd_engines). COPY alone gets a copy-engine
// channel; anything with 3D or COMPUTE gets a graphics channel with 3D (subchannel 0),
// compute (1) and, if asked, copy (4) objects. VDEC alone (no other engine) gets an NVDEC
// channel with a video decoder object (class cls_vdec; only if cls_vdec isn't 0). User push
// buffers bind subchannels with SET_OBJECT themselves.
enum {
    NVMAC_ENGINE_COPY    = 1u << 0,
    NVMAC_ENGINE_3D      = 1u << 2,
    NVMAC_ENGINE_COMPUTE = 1u << 4,
    NVMAC_ENGINE_VDEC    = 1u << 6,
};

struct nvmac_info {
    uint32_t version;               // NVMAC_ABI_VERSION
    uint16_t device_id, chipset;    // PCI device id, NV_PMC_BOOT_0 chipset (0x167 = TU117)
    uint16_t pci_domain;
    uint8_t  pci_bus, pci_dev, pci_func, revision;
    uint8_t  sm;                    // shader model, 75 on Turing
    uint8_t  gpc_count;
    uint16_t tpc_count;
    uint8_t  mp_per_tpc, max_warps_per_mp;
    uint16_t cls_copy, cls_eng3d, cls_compute, cls_gpfifo;
    uint16_t cls_vdec;              // NVDEC class (0xc4b0 on Turing), 0 = no video decode
                                    // (was padding: older clients ignore it, ABI unchanged)
    uint64_t vram_size, vram_used;  // host-owned VRAM heap
    uint64_t bar1_size, bar1_used;  // CPU-mappable VRAM window
    uint64_t va_start, va_end;      // user GPU VA range [start, end)
    uint32_t bind_align;            // 4096
    uint32_t max_pushes, max_waits, max_signals, max_bind_ops, max_wait_entries;
    uint32_t sync_count;            // sync slots per connection
    uint32_t pad1;
    char     name[64];
};

// NVMAC_VM_BIND. BIND needs [va, va + range) unbound; UNBIND may cover any part of any
// bindings (they are split). va, range and mem_offset are 4 KiB aligned.
// VRAM is mapped with 64 KiB pages wherever va and the VRAM address (allocations of
// >= 64 KiB are 64 KiB aligned) are both 64 KiB aligned for a whole 64 KiB, so bind VRAM
// at 64 KiB-aligned VAs for speed. An UNBIND whose start or end falls inside such a page
// fails with kIOReturnBadArgument.
// op: bits 7:0 NVMAC_BIND / NVMAC_UNBIND; for BIND, NVMAC_BIND_KIND(k) sets the PTE kind of
// this binding (otherwise the memory's allocation kind is used).
enum { NVMAC_BIND = 0, NVMAC_UNBIND = 1 };
#define NVMAC_BIND_KIND(k)  ((1u << 16) | ((uint32_t)(k) << 8))
struct nvmac_bind_op {
    uint32_t op;
    uint32_t mem;                   // handle (BIND only)
    uint64_t va, range, mem_offset;
};
struct nvmac_bind_hdr {
    uint32_t count, pad;
    // struct nvmac_bind_op ops[count];
};

// NVMAC_EXEC: waits (GPU-side, before the pushes), pushes, signals (after them, and after
// all work of the context is idle). The context's own seqno is signaled last and returned.
struct nvmac_sync_point {
    uint32_t sync, pad;
    uint64_t value;
};
enum { NVMAC_PUSH_NO_PREFETCH = 1u << 0 };
struct nvmac_push {
    uint64_t va;                    // user VA, 4-byte aligned
    uint32_t size;                  // bytes, multiple of 4, < 8 MiB
    uint32_t flags;
};
struct nvmac_exec_hdr {
    uint32_t ctx, n_wait, n_push, n_signal;
    // struct nvmac_sync_point waits[n_wait];
    // struct nvmac_push pushes[n_push];
    // struct nvmac_sync_point signals[n_signal];
};

// NVMAC_SYNC_WAIT
enum { NVMAC_WAIT_ALL = 1u << 0 };
struct nvmac_wait_hdr {
    uint32_t count, flags;
    uint64_t timeout_ns;            // relative; 0 = poll once
    // struct nvmac_sync_point entries[count];
};

#define NVMAC_MAX_PUSHES      64
#define NVMAC_MAX_WAITS       16
#define NVMAC_MAX_SIGNALS     16
#define NVMAC_MAX_BIND_OPS    256
#define NVMAC_MAX_WAIT_ENTRIES 64
#define NVMAC_SYNC_COUNT      2048
