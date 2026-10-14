// Phase 5: the user-space GPU interface (src/nv_uapi.h) on top of GSP-RM.
//
// Shared setup, done once after the RM objects exist: the engine list, a kernel copy-engine
// channel (VRAM scrubbing), the GR context buffers every graphics channel shares, GR unit
// counts, and our part of BAR1. Then per connection (NVGpuUserClient):
//   - a VA space of its own (split VAS as in Phase 4 step 4), with our page tables;
//   - memory: VRAM from the host heap (scrubbed by the copy engine before hand-out,
//     optionally CPU-visible through BAR1) or system memory (IOMMU-mapped, coherent);
//   - bindings of memory into the user part of the VA space;
//   - contexts: one GPFIFO channel each (unprivileged), with 3D/compute/copy objects;
//   - syncs: 64-bit timelines in a system-memory page that the GPU releases and acquires
//     with host semaphore methods and the process can read directly.
// EXEC writes the GPFIFO entries itself: user pushes, framed by a kernel push that does
// the waits before and the signals (plus the context's seqno) after.
//
// Everything runs under gspLock_ except SYNC_WAIT and GET_TIMESTAMP. Memory is freed right
// away: its PTEs are cleared and the TLB invalidated first, so no GPU work of this
// connection can reach it afterwards (in-flight work that still used it faults in its own
// context, which is the application's bug, as in Vulkan).

#include "nv_gsp_state.h"
#include "nv_regs.h"
#include "nv_uapi.h"
#include "nv_pipe_copy.h"
#include "nv_stack.h"
#include "NVGpuUserClient.hpp"

#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <kern/clock.h>
#include <libkern/OSAtomic.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

// Experiment (make NV_BAR1_REDIRECT=1 or 2): instead of holding the BAR1 slice of freed
// CPU-mapped memory, revoke its user mapping with IOMemoryMap::redirect (1: onto zeroed
// system memory, 2: onto no backing) and free the slice at once. Only a test of whether
// redirect reaches duplicates of the mapping (bar1_alias_test); 0 (holding) is the driver.
#ifndef NV_BAR1_REDIRECT
#define NV_BAR1_REDIRECT 0
#endif

// ---- Layout -------------------------------------------------------------------------
//
// Every connection's VA space:
//   0x0400_0000            sync page (system memory, NVMAC_SYNC_COUNT x 8 bytes)
//   0x0800_0000..          shared GR context buffers (privileged / read-only)
//   0x1000_0000 + i*16 MiB context slot i: ring, kernel pushes, GR main + patch buffers
//   0x1_0000_0000..        GSP-RM's reserved range (512 MiB)
//   0x1_2000_0000..        user range, up to the end of the VA space (1 TiB)
static const uint64_t KVA_SYNC      = 0x04000000;
static const uint64_t KVA_GRGLOBAL  = 0x08000000;
static const uint64_t KVA_GRGLOBAL_MAX = 0x08000000;    // span
static const uint64_t KVA_CTX       = 0x10000000;
static const uint64_t KVA_CTX_SLOT  = 0x01000000;
static const uint32_t kMaxCtx       = 64;
static const uint64_t USER_VA_START = 0x120000000ull;
static const uint64_t USER_VA_END   = 1ull << 40;       // GP entries and SEM_ADDR carry 40 bits

// Inside a context slot
static const uint64_t SLOT_RING  = 0;                   // 512 GP entries (4 KiB)
static const uint64_t SLOT_PUSH  = 0x10000;             // 64 kernel push slots of 1 KiB
static const uint64_t SLOT_MAIN  = 0x200000;            // up to 10 MiB
static const uint64_t SLOT_PATCH = 0xc00000;
static const uint32_t RING_ENTRIES = 512, PUSH_SLOTS = 64, PUSH_SLOT_BYTES = 1024;

static const uint32_t kFirstUserChid = 3;               // 0 GSP-RM, 1 utility CE, 2 boot compute test

struct Binding {
    uint64_t va, size, memOff;
    NVBringup::GpuMem *mem;
};

struct NVBringup::GpuMem {
    uint32_t handle = 0, flags = 0;
    uint64_t size = 0;
    uint64_t vram = 0;                  // VRAM address (NVMAC_MEM_VRAM)
    DmaBuf   sys;                       // system memory (NVMAC_MEM_GART)
    uint64_t bar1Va = 0;                // BAR1 offset of the CPU window, 0 if none
    uint64_t bar1Size = 0;              // its bar1Heap block: size, or more for a reused slice
    IOMemoryDescriptor *cpuMd = nullptr;
    IOMemoryMap *userMap = nullptr;
    IOMemoryDescriptor *userMd = nullptr;   // NVMAC_MEM_IMPORT: the caller's pages, prepared (wired)
    uint8_t  kind = 0;
    bool     borrowed = false;          // NVMAC_DISPLAY_MEM: the display's VRAM, not freed with the handle
    bool     cpuMapped = false;         // bar1Va may have a CPU duplicate (memMap'd, or a reused held
                                        // slice): memFree holds it, with an entry already promised
};

struct NVBringup::GpuCtx {
    GpuConn *conn = nullptr;
    uint32_t handle = 0, index = 0, engines = 0, chid = 0, hChannel = 0, token = 0, put = 0;
    uint32_t engineType = 0;
    uint64_t kva = 0;                   // slot base in the connection's VA space
    uint64_t inst = 0, userd = 0, mthd = 0, ring = 0, push = 0, main = 0, patch = 0;   // VRAM
    uint64_t submitted = 0;             // seqno of the last EXEC
    uint16_t putAfter[64] = {};         // ring position after EXEC seqno s, at [s % 64]
    uint32_t seqSync = 0;               // sync slot that holds the completed seqno
    volatile bool lost = false;
    uint32_t exceptType = 0;
};

struct NVBringup::GpuConn {
    NVBringup *drv = nullptr;
    task_t   task = nullptr;
    uint64_t serial = 0;                // unique for the driver's lifetime (owner of held BAR1 slices)
    uint32_t id = 0;                    // index in GspState::conns
    uint32_t hVas = 0;
    uint64_t pd3 = 0;
    U64Vec   tables;                    // our page tables of this VA space
    bool     noPool = false;            // allocating tables GSP-RM also writes (top levels)
    GpuMem **mems = nullptr;
    uint32_t memCap = 0;
    GpuCtx  *ctxs[kMaxCtx] = {};
    Binding *binds = nullptr;           // sorted by va, non-overlapping
    uint32_t nBinds = 0, bindCap = 0;
    DmaBuf   syncs;
    uint8_t  syncUsed[NVMAC_SYNC_COUNT] = {};   // 1 = user sync, 2 = context seqno
    IOMemoryMap *syncMap = nullptr;
    IOMemoryMap *dispMap = nullptr;     // NVMAC_DISPLAY_MAP
    uint64_t dispMapBar1 = 0;
    volatile bool dead = false;         // no GPU state: closed, failed, or GSP-RM gone
    volatile bool anyLost = false;      // a context of this connection was lost (device lost)
};

static inline volatile uint64_t *syncSlot(NVBringup::GpuConn *c, uint32_t slot)
{
    return (volatile uint64_t *)(c->syncs.va + 8 * (uint64_t)slot);
}

// PTE kinds a connection may use (Turing, dev_mmu.h): pitch/generic and the depth/stencil
// kinds, 0x00..0x06. The compressible ones (0x08..0x0e) become their uncompressed kind: we
// allocate no compression tags, and comptag line 0 would be shared by every connection
// (nouveau falls back the same way). Anything else is refused. -1: not allowed.
static int userKind(uint32_t kind)
{
    static const uint8_t uncompressed[7] = { 0x06, 0x06, 0x02, 0x01, 0x03, 0x04, 0x05 };  // 0x08..0x0e
    if (kind <= 0x06)
        return (int)kind;
    if (kind >= 0x08 && kind <= 0x0e)
        return uncompressed[kind - 0x08];
    return -1;
}

// ---- MMU callbacks for connection VA spaces and BAR1 ------------------------------------

uint64_t NVBringup::connMmuAlloc(void *ctx, uint32_t bytes)
{
    GpuConn *c = (GpuConn *)ctx;
    uint64_t a = bytes == 0x1000 && !c->noPool ? c->drv->ptPoolAlloc() : 0;
    if (!a)
        a = mmuAlloc(c->drv, bytes);
    if (a && !c->tables.push(a)) {
        c->drv->tableFree(a);
        return 0;
    }
    return a;
}

uint64_t NVBringup::connMmuRd64(void *ctx, uint64_t addr)
{
    return mmuRd64(((GpuConn *)ctx)->drv, addr);
}

void NVBringup::connMmuWr64(void *ctx, uint64_t addr, uint64_t value)
{
    mmuWr64(((GpuConn *)ctx)->drv, addr, value);
}

uint64_t NVBringup::bar1MmuAlloc(void *ctx, uint32_t bytes)
{
    NVBringup *me = (NVBringup *)ctx;
    uint64_t a = mmuAlloc(me, bytes);
    if (a && !me->gsp_->bar1Tables.push(a)) {
        nv_vram_free(me->gsp_->vram, a);
        return 0;
    }
    return a;
}

void NVBringup::bar1MmuLinked(void *ctx, uint64_t entry)
{
    NVBringup *me = (NVBringup *)ctx;
    me->gsp_->bar1Links.push(entry);    // on failure the link just stays until the GPU resets
}

static nv_mmu_ops connOps(NVBringup::GpuConn *c, uint64_t (*alloc)(void *, uint32_t),
                          uint64_t (*rd)(void *, uint64_t), void (*wr)(void *, uint64_t, uint64_t))
{
    nv_mmu_ops o = { c, alloc, rd, wr, nullptr, true };    // our own VA space: 64 KiB pages
    return o;
}

// ---- Shared setup ---------------------------------------------------------------------

// Engine list and the CE method buffer size (every channel needs one).
bool NVBringup::initEngines()
{
    GspState *g = gsp_;
    uint32_t st = 0;
    uint8_t eng[NV2080_GET_ENGINES_SIZE] = {};
    if (!gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GPU_GET_ENGINES_V2_, eng, sizeof(eng), &st))
        return false;
    uint32_t n = 0;
    memcpy(&n, eng + NV2080_GET_ENGINES_count, 4);
    uint32_t max = (uint32_t)((sizeof(eng) - NV2080_GET_ENGINES_list) / 4);
    if (n > max)
        n = max;
    char list[160];
    uint32_t pos = 0;
    list[0] = 0;
    g->nEngines = 0;
    g->ceEngine = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t t;
        memcpy(&t, eng + NV2080_GET_ENGINES_list + 4 * i, 4);
        if (g->nEngines < 32)
            g->engines[g->nEngines++] = t;
        if (pos < sizeof(list))
            pos += (uint32_t)snprintf(list + pos, sizeof(list) - pos, " 0x%x", t);
        if (!g->ceEngine && isCopyEngine(t))
            g->ceEngine = t;
    }
    LOG("GSP: engines (%u):%s; utility copy engine 0x%x", n, list, g->ceEngine);
    uint32_t mthdSize = 0;
    if (!g->ceEngine ||
        !gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_CE_GET_FAULT_METHOD_BUFFER_SIZE_,
                      &mthdSize, sizeof(mthdSize), &st) || !mthdSize || mthdSize > 0x100000)
        return false;
    g->mthdSize = mthdSize;
    return true;
}

// The kernel's copy-engine channel (chid 1) in the kernel VA space, privileged: it scrubs
// VRAM with physical addressing.
bool NVBringup::createUtilChannel()
{
    GspState *g = gsp_;
    uint32_t st = 0;
    TestChan &c = g->util;
    if (!g->ceEngine || !g->mthdSize ||
        !createChannel(c, 0xc46f0001, 1, g->ceEngine, 0x20100000, g->mthdSize))
        return false;
    g->hChannel = c.handle;
    g->chidUsed[0] = g->chidUsed[1] = g->chidUsed[2] = true;
    if (!tlbFlush(g->pd3))
        LOG("GSP: TLB invalidate did not complete (continuing)");
    if (!scheduleChannel(c))
        return false;
    uint32_t ceAlloc[NVB0B5_ALLOC_SIZE / 4] = { NVB0B5_ALLOC_VERSION_1, g->ceEngine };
    if (!gspRmAlloc(g->hClient, c.handle, 0xc5b50001, chip_->arch->cls.copy, ceAlloc, sizeof(ceAlloc), &st))
        return false;
    g->utilOk = true;
    LOG("GSP: utility copy channel ready (chid 1, engine 0x%x)", g->ceEngine);
    return true;
}

// Zeroes VRAM with the utility channel: CE memset, physical destination (NVIDIA's
// channel_utils memset: remap constant A, one-byte components, LINE_LENGTH_IN in bytes).
bool NVBringup::scrubVram(uint64_t pa, uint64_t size)
{
    GspState *g = gsp_;
    if (!g->utilOk)
        return false;
    const uint32_t subch = 4;
    const uint64_t chunk = 1ull << 30;
    for (uint64_t off = 0; off < size; off += chunk) {
        uint64_t n = size - off < chunk ? size - off : chunk;
        uint64_t dst = pa + off;
        uint32_t payload = 0x5c000000u | (++g->utilSeq & 0xffffff);
        const uint32_t push[] = {
            pbHeader(subch, 0x000, 1), chip_->arch->cls.copy,
            pbHeader(subch, NVC5B5_SET_DST_PHYS_MODE_, 1), 0,                          // LOCAL_FB
            pbHeader(subch, 0x408, 2), (uint32_t)(dst >> 32), (uint32_t)dst,           // OFFSET_OUT
            pbHeader(subch, NVC5B5_SET_REMAP_CONST_A_, 1), 0,
            pbHeader(subch, NVC5B5_SET_REMAP_COMPONENTS_, 1), NVC5B5_REMAP_CONST_A_1B,
            pbHeader(subch, 0x418, 2), (uint32_t)n, 1,                                 // LINE_LENGTH_IN, LINE_COUNT
            pbHeader(subch, 0x240, 3), (uint32_t)(g->util.vaSem >> 32), (uint32_t)g->util.vaSem, payload,
            pbHeader(subch, 0x300, 1), NVC5B5_LAUNCH_MEMSET_PHYS,
        };
        if (!submitAndWait(g->util, push, sizeof(push) / 4, payload, 5000, false)) {
            LOG("GSP: VRAM scrub of 0x%llx+0x%llx did not complete", (unsigned long long)dst, (unsigned long long)n);
            return false;
        }
    }
    return true;
}

// Boot-time check (nvtest=1): pattern through PRAMIN, scrub, read back zeros.
bool NVBringup::testScrub()
{
    GspState *g = gsp_;
    const uint64_t size = 0x20000;
    uint64_t pa = nv_vram_alloc(g->vram, size, 0x10000);
    if (!pa)
        return false;
    for (uint64_t off = 0; off < size; off += 4)
        praminWr32(pa + off, 0xdeadbeef ^ (uint32_t)off);
    praminRestore();
    uint64_t t0 = mach_absolute_time();
    bool ok = scrubVram(pa, size);
    uint64_t t1 = mach_absolute_time(), ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    uint32_t bad = 0;
    for (uint64_t off = 0; ok && off < size; off += 4)
        bad += praminRd32(pa + off) != 0;
    praminRestore();
    nv_vram_free(g->vram, pa);
    ok = ok && !bad;
    LOG("GSP: scrub test %s: 128 KiB zeroed by the copy engine in %llu us (%u words not zero)",
        ok ? "passed" : "FAILED", (unsigned long long)(ns / 1000), bad);
    setProperty("NVScrubTest", ok ? "passed" : "failed");
    return ok;
}

// GR context buffer sizes; allocates the buffers shared by all graphics channels.
bool NVBringup::initGrGlobal()
{
    GspState *g = gsp_;
    uint32_t st = 0;
    if (!g->hIntClient || !g->hIntSubdevice)
        return false;
    uint8_t *info = (uint8_t *)IOMallocZero(NV2080_GR_CTXBUF_INFO_SIZE);
    if (!info)
        return false;
    bool got = gspRmControl(g->hIntClient, g->hIntSubdevice, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_,
                            info, NV2080_GR_CTXBUF_INFO_SIZE, &st);
    uint32_t sz[NV2080_GR_CTXBUF_ENGINE_COUNT], al[NV2080_GR_CTXBUF_ENGINE_COUNT];
    for (uint32_t i = 0; i < NV2080_GR_CTXBUF_ENGINE_COUNT; i++) {
        memcpy(&sz[i], info + i * NV2080_GR_CTXBUF_ENTRY_SIZE, 4);
        memcpy(&al[i], info + i * NV2080_GR_CTXBUF_ENTRY_SIZE + 4, 4);
        if (sz[i] == 0xffffffffu)
            sz[i] = 0;
    }
    IOFree(info, NV2080_GR_CTXBUF_INFO_SIZE);
    if (!got || !sz[GR_CTXBUF_GRAPHICS] || !sz[GR_CTXBUF_PATCH])
        return false;
    g->grMainSize = ((sz[GR_CTXBUF_GRAPHICS] + 0xfffu) & ~0xfffu) + 0x1000 * 64;   // + subcontext headers
    g->grMainAlign = al[GR_CTXBUF_GRAPHICS];
    g->grPatchSize = (sz[GR_CTXBUF_PATCH] + 0xfffu) & ~0xfffu;
    if (g->grMainSize > SLOT_PATCH - SLOT_MAIN || g->grPatchSize > KVA_CTX_SLOT - SLOT_PATCH)
        return false;

    const GrGlobalBuf want[kMaxGrGlobal] = {
        { "bundle_cb", PROMOTE_ID_BUNDLE_CB, 0, sz[GR_CTXBUF_BUNDLE_CB], 0, NV_MMU_PTE_PRIVILEGE, false },
        { "pagepool", PROMOTE_ID_PAGEPOOL, 0, sz[GR_CTXBUF_PAGEPOOL_GLOBAL], 0, NV_MMU_PTE_PRIVILEGE, false },
        { "attrib_cb", PROMOTE_ID_ATTRIBUTE_CB, 0, sz[GR_CTXBUF_ATTRIBUTE_CB], 0, NV_MMU_PTE_PRIVILEGE, false },
        { "rtv_cb", PROMOTE_ID_RTV_CB_GLOBAL, 0, sz[GR_CTXBUF_RTV_CB_GLOBAL], 0, NV_MMU_PTE_PRIVILEGE, false },
        { "privmap", PROMOTE_ID_PRIV_ACCESS_MAP, 0, sz[GR_CTXBUF_PRIV_ACCESS_MAP], 0, NV_MMU_PTE_READ_ONLY, true },
        { "unres_privmap", PROMOTE_ID_UNRESTRICTED_PRIV_ACCESS_MAP, 0, sz[GR_CTXBUF_PRIV_ACCESS_MAP], 0,
          NV_MMU_PTE_READ_ONLY, true },
    };
    const uint32_t wantAlign[kMaxGrGlobal] = {
        al[GR_CTXBUF_BUNDLE_CB], al[GR_CTXBUF_PAGEPOOL_GLOBAL], al[GR_CTXBUF_ATTRIBUTE_CB],
        al[GR_CTXBUF_RTV_CB_GLOBAL], al[GR_CTXBUF_PRIV_ACCESS_MAP], al[GR_CTXBUF_PRIV_ACCESS_MAP],
    };
    uint64_t off = 0;
    g->nGrGlobal = 0;
    for (uint32_t i = 0; i < kMaxGrGlobal; i++) {
        if (!want[i].size)
            continue;
        GrGlobalBuf b = want[i];
        b.size = (b.size + 0xfff) & ~0xfffull;
        uint64_t align = 0x1000;
        while (align < wantAlign[i] && align < 0x200000)
            align <<= 1;
        if (align < 0x10000)
            align = 0x10000;
        off = (off + align - 1) & ~(align - 1);
        b.off = off;
        b.pa = nv_vram_alloc(g->vram, b.size, align);
        if (!b.pa || !scrubVram(b.pa, b.size)) {
            LOG("GSP: GR global buffer %s (0x%llx bytes): allocation failed", b.name, (unsigned long long)b.size);
            return false;
        }
        off += b.size;
        g->grGlobal[g->nGrGlobal++] = b;
        LOG("GSP: GR global buffer %-13s 0x%08llx bytes: VRAM 0x%llx, VA +0x%llx", b.name,
            (unsigned long long)b.size, (unsigned long long)b.pa, (unsigned long long)b.off);
    }
    if (off > KVA_GRGLOBAL_MAX)
        return false;
    g->grGlobalSpan = off;
    g->grReady = true;
    LOG("GSP: GR contexts: main 0x%x bytes (align 0x%x), patch 0x%x; shared buffers 0x%llx bytes",
        g->grMainSize, g->grMainAlign, g->grPatchSize, (unsigned long long)off);
    return true;
}

// GPC/TPC/SM counts for nv_device_info (NVK sizes shader local memory with them).
void NVBringup::queryGrInfo()
{
    GspState *g = gsp_;
    uint32_t st = 0;
    uint8_t p[NV2080_GR_INFO_V2_SIZE] = {};
    const uint32_t idx[] = { GR_INFO_SHADER_PIPE_COUNT, GR_INFO_SHADER_PIPE_SUB_COUNT, GR_INFO_SM_VERSION,
                             GR_INFO_MAX_WARPS_PER_SM, GR_INFO_LITTER_NUM_GPCS, GR_INFO_LITTER_NUM_SM_PER_TPC,
                             GR_INFO_GPU_CORE_COUNT };
    const uint32_t n = sizeof(idx) / 4;
    put32(p, NV2080_GR_INFO_V2_listSize, n);
    for (uint32_t i = 0; i < n; i++)
        put32(p, NV2080_GR_INFO_V2_list + i * NV2080_GR_INFO_ENTRY_SIZE, idx[i]);
    uint32_t v[sizeof(idx) / 4] = {};
    if (gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GR_GET_INFO_V2_, p, sizeof(p), &st))
        for (uint32_t i = 0; i < n; i++)
            memcpy(&v[i], p + NV2080_GR_INFO_V2_list + i * NV2080_GR_INFO_ENTRY_SIZE + 4, 4);

    uint8_t m[NV2080_GPC_MASK_SIZE] = {};
    uint32_t gpcMask = 0, tpcs = 0;
    if (gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GR_GET_GPC_MASK_, m, sizeof(m), &st))
        memcpy(&gpcMask, m + NV2080_GPC_MASK_gpcMask, 4);
    for (uint32_t gpc = 0; gpc < 32; gpc++) {
        if (!(gpcMask & (1u << gpc)))
            continue;
        uint8_t t[NV2080_TPC_MASK_SIZE] = {};
        put32(t, NV2080_TPC_MASK_gpcId, gpc);
        uint32_t tpcMask = 0;
        if (gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GR_GET_TPC_MASK_, t, sizeof(t), &st))
            memcpy(&tpcMask, t + NV2080_TPC_MASK_tpcMask, 4);
        tpcs += (uint32_t)__builtin_popcount(tpcMask);
    }
    g->gpcCount = (uint8_t)__builtin_popcount(gpcMask);
    g->tpcCount = (uint16_t)(tpcs ? tpcs : v[0]);
    g->smPerTpc = (uint8_t)(v[5] ? v[5] : 2);
    g->maxWarps = (uint8_t)(v[3] ? v[3] : 32);
    // RM reports the SM hardware revision (TU117: 0x703); compilers want the ISA level, one per
    // architecture (nv_hal.cpp: 7.5 for Turing), as nouveau/NVK derive it from the chipset.
    g->sm = chip_ ? chip_->arch->sm : v[2] ? (uint8_t)((v[2] >> 8) * 10 + (v[2] & 0xff)) : 75;
    LOG("GSP: GR units: GPC mask 0x%x (%u GPCs), %u TPCs, %u SM/TPC, %u warps/SM, SM version 0x%x (sm%u), "
        "shader pipes %u/%u, %u cores", gpcMask, g->gpcCount, g->tpcCount, g->smPerTpc, g->maxWarps, v[2], g->sm,
        v[0], v[1], v[6]);
}

// Video decode: NVDEC0 present (engine list), its ENG_DESC (device info table) and the size of
// its falcon context buffer (constructed falcon info), as nouveau's r570 fifo.c finds them.
// Read-only queries; any failure leaves nvdecCtxSize 0 (no VDEC contexts, cls_vdec 0).
void NVBringup::initNvdec()
{
    GspState *g = gsp_;
    uint32_t st = 0;
    g->nvdecCtxSize = 0;
    bool present = false;
    for (uint32_t i = 0; i < g->nEngines; i++)
        present |= g->engines[i] == NV2080_ENGINE_TYPE_NVDEC0_;
    if (!present) {
        LOG("GSP: no NVDEC0: video decode unavailable");
        return;
    }
    uint8_t *t = (uint8_t *)IOMallocZero(NV2080_DEVINFO_SIZE);
    if (!t)
        return;
    uint32_t engDesc = 0;
    bool found = false, more = true;
    for (uint32_t base = 0; more && !found && base < 256; base += NV2080_DEVINFO_MAX) {
        memset(t, 0, NV2080_DEVINFO_SIZE);
        put32(t, NV2080_DEVINFO_baseIndex, base);
        if (!gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_FIFO_GET_DEVICE_INFO_TABLE_, t,
                          NV2080_DEVINFO_SIZE, &st))
            break;
        uint32_t n = 0;
        memcpy(&n, t + NV2080_DEVINFO_numEntries, 4);
        more = t[NV2080_DEVINFO_bMore] != 0;
        for (uint32_t i = 0; i < n && i < NV2080_DEVINFO_MAX; i++) {
            const uint8_t *e = t + NV2080_DEVINFO_entries + i * NV2080_DEVINFO_ENTRY_SIZE;
            uint32_t rmType = 0;
            memcpy(&rmType, e + 4 * ENGINE_INFO_TYPE_RM_ENGINE_TYPE_, 4);
            if (rmType == RM_ENGINE_TYPE_NVDEC0_) {
                memcpy(&engDesc, e + 4 * ENGINE_INFO_TYPE_ENG_DESC_, 4);
                found = true;
                break;
            }
        }
    }
    IOFree(t, NV2080_DEVINFO_SIZE);
    if (!found) {
        LOG("GSP: NVDEC0 not in the device info table: video decode unavailable");
        return;
    }
    uint8_t *f = (uint8_t *)IOMallocZero(NV2080_FALCON_INFO_SIZE);
    if (!f)
        return;
    uint32_t size = 0;
    if (gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GPU_GET_CONSTRUCTED_FALCON_INFO_, f,
                     NV2080_FALCON_INFO_SIZE, &st)) {
        uint32_t n = 0;
        memcpy(&n, f + NV2080_FALCON_INFO_count, 4);
        for (uint32_t i = 0; i < n && i < NV2080_FALCON_INFO_MAX; i++) {
            const uint8_t *e = f + NV2080_FALCON_INFO_table + i * NV2080_FALCON_ENTRY_SIZE;
            uint32_t d = 0;
            memcpy(&d, e + NV2080_FALCON_engDesc, 4);
            if (d == engDesc) {
                memcpy(&size, e + NV2080_FALCON_ctxBufferSize, 4);
                break;
            }
        }
    }
    IOFree(f, NV2080_FALCON_INFO_SIZE);
    // A context buffer goes in the context's SLOT_MAIN..SLOT_PATCH range of its VA slot.
    if (!size || size > SLOT_PATCH - SLOT_MAIN) {
        LOG("GSP: NVDEC0 (ENG_DESC 0x%x): context buffer size %u unusable: video decode unavailable",
            engDesc, size);
        return;
    }
    g->nvdecCtxSize = size;
    LOG("GSP: NVDEC0: ENG_DESC 0x%x, context buffer %u bytes, class 0x%x", engDesc, size,
        chip_->arch->cls.vdec);
}

// Display, step B0 (boot-arg nvdisp=1): what GSP-RM knows about the display outputs, read-only.
// As nouveau's r535_disp_oneinit up to its queries: display instance memory registered with
// GSP-RM (WRITE_INST_MEM, on the internal client), NV04_DISPLAY_COMMON, then the heads, the
// supported displays and, per display, its OR (type, protocol), connector, connect state and
// EDID, and what each head shows. No channels, modesets, backlight state or DP mode changes.
// Results: the log, NVDisplayProbe (summary) and NVDisplayEDID-<displayId> (raw EDID).
void NVBringup::probeDisplay()
{
    GspState *g = gsp_;
    uint32_t arm = 0, st = 0;
    PE_parse_boot_argn("nvdisp", &arm, sizeof(arm));
    // The display pipe (dispPipeArmed_) needs the engine up at boot, lit or not: it counts as nvdisp=2.
    if (dispPipeArmed_)
        arm = 2;
    if (arm != 1 && arm != 2)
        return;
    uint32_t modeId = 0, modeProto = 0;     // nvdisp=2: the first connected TMDS output and its EDID
    char sum[512];
    uint32_t pos = 0;
#define ADD(...) do { if (pos < sizeof(sum)) pos += (uint32_t)snprintf(sum + pos, sizeof(sum) - pos, __VA_ARGS__); } while (0)
    sum[0] = 0;

    g->dispInst = nv_vram_alloc(g->vram, 0x10000, 0x10000);
    if (!g->dispInst) {
        LOG("GSP: display: no VRAM for instance memory");
        return;
    }
    uint8_t wi[NV2080_DISP_INST_SIZE] = {};
    put64(wi, NV2080_DISP_INST_physAddr, g->dispInst);
    put64(wi, NV2080_DISP_INST_size, 0x10000);
    put32(wi, NV2080_DISP_INST_addrSpace, 2);           // ADDR_FBMEM
    put32(wi, NV2080_DISP_INST_cacheAttr, 2);           // NV_MEMORY_WRITECOMBINED
    if (!gspRmControl(g->hIntClient, g->hIntSubdevice, NV2080_CTRL_CMD_INTERNAL_DISPLAY_WRITE_INST_MEM_, wi,
                      sizeof(wi), &st)) {
        setProperty("NVDisplayProbe", "WRITE_INST_MEM failed");
        return;
    }
    uint32_t h = g->nextHandle;
    g->nextHandle += 8;
    if (!gspRmAlloc(g->hClient, g->hDevice, h, NV04_DISPLAY_COMMON_CLASS, nullptr, 0, &st)) {
        setProperty("NVDisplayProbe", "NV04_DISPLAY_COMMON allocation failed");
        return;
    }
    g->hDisp = h;
    auto ctl = [&](uint32_t cmd, void *p, uint32_t size) { return gspRmControl(g->hClient, g->hDisp, cmd, p, size, &st); };

    uint8_t nh[NV0073_NUM_HEADS_SIZE] = {}, hm[NV0073_HEAD_MASK_SIZE] = {}, su[NV0073_SUPPORTED_SIZE] = {};
    uint32_t heads = 0, headMask = 0, mask = 0, maskDdc = 0;
    if (ctl(NV0073_CTRL_CMD_SYSTEM_GET_NUM_HEADS_, nh, sizeof(nh)))
        memcpy(&heads, nh + NV0073_NUM_HEADS_numHeads, 4);
    if (ctl(NV0073_CTRL_CMD_SPECIFIC_GET_ALL_HEAD_MASK_, hm, sizeof(hm)))
        memcpy(&headMask, hm + NV0073_HEAD_MASK_headMask, 4);
    if (ctl(NV0073_CTRL_CMD_SYSTEM_GET_SUPPORTED_, su, sizeof(su))) {
        memcpy(&mask, su + NV0073_SUPPORTED_displayMask, 4);
        memcpy(&maskDdc, su + NV0073_SUPPORTED_displayMaskDDC, 4);
    }
    LOG("GSP: display: %u heads (mask 0x%x), displays 0x%x (DDC 0x%x)", heads, headMask, mask, maskDdc);
    ADD("heads %u (0x%x), displays 0x%x;", heads, headMask, mask);

    for (uint32_t bit = 0; bit < 32; bit++) {
        const uint32_t id = 1u << bit;
        if (!(mask & id))
            continue;
        uint8_t oi[NV0073_OR_INFO_SIZE] = {};
        put32(oi, NV0073_OR_INFO_displayId, id);
        uint32_t orIndex = ~0u, orType = 0, proto = 0, loc = 0, dcb = 0;
        bool lit = false;
        if (ctl(NV0073_CTRL_CMD_SPECIFIC_OR_GET_INFO_, oi, sizeof(oi))) {
            memcpy(&orIndex, oi + NV0073_OR_INFO_index, 4);
            memcpy(&orType, oi + NV0073_OR_INFO_type, 4);
            memcpy(&proto, oi + NV0073_OR_INFO_protocol, 4);
            memcpy(&loc, oi + NV0073_OR_INFO_location, 4);
            memcpy(&dcb, oi + NV0073_OR_INFO_dcbIndex, 4);
            lit = oi[NV0073_OR_INFO_bIsLitByVbios] != 0;
        }
        uint8_t cd[NV0073_CONNECTOR_SIZE] = {};
        put32(cd, NV0073_CONNECTOR_displayId, id);
        uint32_t nConn = 0, connType = 0, connIndex = 0, platform = 0;
        if (ctl(NV0073_CTRL_CMD_SPECIFIC_GET_CONNECTOR_DATA_, cd, sizeof(cd))) {
            memcpy(&nConn, cd + NV0073_CONNECTOR_count, 4);
            memcpy(&connIndex, cd + NV0073_CONNECTOR_data, 4);
            memcpy(&connType, cd + NV0073_CONNECTOR_data + 4, 4);
            memcpy(&platform, cd + NV0073_CONNECTOR_platform, 4);
        }
        uint8_t cs[NV0073_CONNECT_SIZE] = {};
        put32(cs, NV0073_CONNECT_displayMask, id);
        uint32_t conn = 0;
        bool connected = ctl(NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE_, cs, sizeof(cs)) &&
                         (memcpy(&conn, cs + NV0073_CONNECT_displayMask, 4), (conn & id) != 0);
        LOG("GSP: display 0x%x: OR type %u index %u protocol %u location %u dcb %u%s; connector type 0x%x "
            "index %u (%u connectors, platform %u); %s", id, orType, orIndex, proto, loc, dcb,
            lit ? " lit by VBIOS" : "", connType, connIndex, nConn, platform,
            connected ? "connected" : "not connected");
        ADD(" 0x%x: OR %u/%u proto %u conn 0x%x %s", id, orType, orIndex, proto, connType,
            connected ? "connected" : "-");
        // TMDS outputs (HDMI/DVI) are what nvdisp=2 lights, now or when one is plugged in.
        if ((proto == 1 || proto == 2 || proto == 5) && g->disp.nCand < 4) {
            g->disp.candId[g->disp.nCand] = id;
            g->disp.candProto[g->disp.nCand++] = proto;
        }
        if (!connected)
            continue;
        uint8_t *ed = (uint8_t *)IOMallocZero(NV0073_EDID_SIZE);
        if (!ed)
            continue;
        put32(ed, NV0073_EDID_displayId, id);
        uint32_t n = 0;
        if (ctl(NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2_, ed, NV0073_EDID_SIZE))
            memcpy(&n, ed + NV0073_EDID_bufferSize, 4);
        if (n >= 128 && n <= NV0073_EDID_MAX) {
            const uint8_t *e = ed + NV0073_EDID_buffer;
            // EDID 1.x: manufacturer (3 letters in 5-bit fields), product code, preferred mode.
            const uint16_t m = (uint16_t)(e[8] << 8 | e[9]);
            const char mfg[4] = { (char)('@' + ((m >> 10) & 31)), (char)('@' + ((m >> 5) & 31)),
                                  (char)('@' + (m & 31)), 0 };
            const uint32_t hact = (uint32_t)e[56] | (uint32_t)(e[58] & 0xf0) << 4;
            const uint32_t vact = (uint32_t)e[59] | (uint32_t)(e[61] & 0xf0) << 4;
            LOG("GSP: display 0x%x: EDID %u bytes, %s %04x, preferred %ux%u", id, n, mfg, e[10] | e[11] << 8,
                hact, vact);
            ADD(" EDID %s %ux%u", mfg, hact, vact);
            if (!modeId && (proto == 1 || proto == 2 || proto == 5)) {
                modeId = id;
                modeProto = proto;
                // The whole EDID, extensions included (the CTA block says whether the sink takes HDMI).
                g->dispModeEdidLen = n < kNvEdidMax ? n : kNvEdidMax;
                memcpy(g->dispModeEdid, e, g->dispModeEdidLen);
            }
            char key[32];
            snprintf(key, sizeof(key), "NVDisplayEDID-%x", id);
            if (OSData *d = OSData::withBytes(e, n)) {
                setProperty(key, d);
                d->release();
            }
        } else {
            LOG("GSP: display 0x%x: no EDID (%u bytes)", id, n);
        }
        ADD(";");
        IOFree(ed, NV0073_EDID_SIZE);
    }
    for (uint32_t head = 0; head < heads && head < 8; head++) {
        uint8_t ac[NV0073_ACTIVE_SIZE] = {};
        put32(ac, NV0073_ACTIVE_head, head);
        uint32_t active = 0;
        if (ctl(NV0073_CTRL_CMD_SYSTEM_GET_ACTIVE_, ac, sizeof(ac)))
            memcpy(&active, ac + NV0073_ACTIVE_displayId, 4);
        LOG("GSP: display: head %u shows 0x%x", head, active);
        if (active)
            ADD(" head %u: 0x%x", head, active);
    }
    setProperty("NVDisplayProbe", sum);
#undef ADD
    g->disp.wanted = arm == 2;      // initBar1 runs after this; dispInit and dispSetMode once it has
    if (arm == 2 && modeId) {
        g->dispModeId = modeId;
        g->dispModeProto = modeProto;
    } else {
        g->dispModeEdidLen = 0;
    }
}

// Display, step B1 (boot-arg nvdisp=2): lighting the HDMI/DVI output, at boot and on hot-plug. Follows
// nouveau's r535/r570 path for Turing: c57d core + c57e window channels (HEAD 0, WINDOW 0), no
// window-immediate channel; HDMI mode and InfoFrames as NVKMS for an HDMI sink. GSP-RM owns the
// OR/SOR and the PHY; this writes the channel methods.
//   dispInit     once per GSP boot: instance memory (RAMHT, context DMAs), LUTs, root, channels, core init,
//                the hot-plug event
//   dispSetMode  per connect: the mode from the EDID within the link's limits, SOR, HDMI or DVI, head timing
//                and range, the surface (colour bars), window 0, UPDATEs, InfoFrames
//   dispBlank    per disconnect: window 0 and the SOR detached
//   dispPower    DPMS (NVFramebuffer)
//   dispHotplugTick  from the GSP poller: the outputs' connect state after a hot-plug event, or polled (debounced)
// What nvdisplay 3 requires, found on hardware: the DMA header takes the method's byte offset (bits 13:2);
// window 0 needs an identity ILUT (else UPDATE INVALID_STATE 0x2d), the head an identity OLUT (0x2e);
// scanout surfaces take a big-page context DMA. Results: the log, NVDisplayModeset, NVDisplayFB (the lit
// surface and mode, for nvvdisplay) and NVDisplayGen (bumped whenever the lit display comes or goes).

static const uint32_t kDispHandleSync = 0xf0000000, kDispHandleVram = 0xf0000001, kDispHandleFb = 0xf0000002;

static inline void dispW32(NVDispMem &m, uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(m.cpu + off) = v;
}

// A VRAM block (pa = 0: allocated here), CPU-mapped uncached through BAR1, zeroed.
bool NVBringup::dispMemAlloc(NVDispMem &m, uint64_t size, uint64_t pa)
{
    GspState *g = gsp_;
    m.size = size;
    m.pa = pa ? pa : nv_vram_alloc(g->vram, size, 0x10000);
    if (!m.pa || !bar1Map(m.pa, size, &m.bar1))
        return false;
    IODeviceMemory *bar1 = pci_->getDeviceMemoryWithIndex(1);
    m.map = bar1 ? bar1->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, m.bar1, size)
                 : nullptr;
    if (!m.map)
        return false;
    m.cpu = (volatile uint8_t *)m.map->getVirtualAddress();
    for (uint64_t o = 0; o < size; o += 4)
        *(volatile uint32_t *)(m.cpu + o) = 0;
    return true;
}

// One method on the core (wndw false) or window channel, kicked on its own: the exception registers
// (gv100_disp_exception: stat = type << 12 | mthd >> 2, data, code) then name the method the engine
// objects to; acked as nouveau does. Near the end of the 4 KiB push buffer, a JUMP back to 0
// (nv50_dmac_wind; GET is at the old position then, never 0).
void NVBringup::dispMthd(bool wndw, uint32_t m, uint32_t v)
{
    GspState::DispHw &d = gsp_->disp;
    NVDispMem &pb = wndw ? d.pbWndw : d.pbCore;
    uint32_t &cur = wndw ? d.wndwCur : d.coreCur;
    const uint32_t user = wndw ? 0x690000 : 0x680000, chid = wndw ? 1 : 0;
    if ((cur + 2) * 4 + 8 > pb.size) {
        dispW32(pb, cur * 4, 1u << 29);                     // OPCODE_JUMP, offset 0
        bar1Flush();
        cur = 0;
        wr32(user, 0);
        for (int i = 0; i < 1000 && rd32(user + 4) != 0; i++)
            IODelay(10);
    }
    dispW32(pb, cur * 4, 0u << 29 | 1u << 18 | m);          // METHOD_OFFSET is bits 13:2: the byte offset as is
    dispW32(pb, cur * 4 + 4, v);
    cur += 2;
    bar1Flush();                    // push buffer fetches are not coherent with BAR1
    wr32(user, cur * 4);
    for (int i = 0; i < 1000 && rd32(user + 4) != cur * 4; i++)
        IODelay(10);
    const uint32_t stat = rd32(0x611020 + chid * 12);
    if ((stat >> 12) & 7) {
        if (d.nErr++ < 12)
            LOG("GSP: display: %s method 0x%04x data 0x%08x -> exception stat 0x%08x data 0x%08x code 0x%08x",
                wndw ? "window" : "core", m, v, stat, rd32(0x611024 + chid * 12), rd32(0x611028 + chid * 12));
        wr32(0x611020 + chid * 12, 0x90000000);
    }
}

// Waits for the channel to have fetched everything up to PUT.
bool NVBringup::dispKick(bool wndw)
{
    const uint32_t user = wndw ? 0x690000 : 0x680000;
    for (int i = 0; i < 2000; i++) {
        if (rd32(user + 4) == rd32(user))
            return true;
        IOSleep(1);
    }
    LOG("GSP: display: %s channel did not consume its push buffer: PUT 0x%x GET 0x%x", wndw ? "window" : "core",
        rd32(user), rd32(user + 4));
    return false;
}

// Core UPDATE (interlocked with the windows in windowMask), waiting for its notifier. A notifier that
// stays unwritten is not fatal: the update may have run anyway.
bool NVBringup::dispCoreUpdate(uint32_t windowMask)
{
    GspState::DispHw &d = gsp_->disp;
    *(volatile uint32_t *)d.sync.cpu = 0;                                  // NOT_BEGUN
    dispMthd(false, 0x20c, 0u | 0u << 4 | 1u << 12);                       // NOTIFIER_CONTROL: write, offset 0, enable
    dispMthd(false, 0x218, 0);                                             // SET_INTERLOCK_FLAGS (cursors)
    dispMthd(false, 0x21c, windowMask);                                    // SET_WINDOW_INTERLOCK_FLAGS
    dispMthd(false, 0x200, 1);                                             // UPDATE
    dispMthd(false, 0x20c, 0);                                             // NOTIFY disable
    if (!dispKick(false))
        return false;
    for (int i = 0; i < 2000; i++) {
        if ((*(volatile uint32_t *)d.sync.cpu >> 30) == 2)                 // NV_DISP_NOTIFIER _0 STATUS: FINISHED
            return true;
        IOSleep(1);
    }
    LOG("GSP: display: warning: core notifier not written (0x%08x); going on", *(volatile uint32_t *)d.sync.cpu);
    IOSleep(100);
    return true;
}

// The exception registers, the channels' PUT/GET, head 0's assembly and armed state and its raster position.
void NVBringup::dispDump()
{
    for (uint32_t chid = 0; chid < 2; chid++)
        LOG("GSP: display: exception chid %u: stat 0x%08x data 0x%08x code 0x%08x", chid, rd32(0x611020 + chid * 12),
            rd32(0x611024 + chid * 12), rd32(0x611028 + chid * 12));
    LOG("GSP: display: core PUT 0x%x GET 0x%x; window PUT 0x%x GET 0x%x", rd32(0x680000), rd32(0x680004),
        rd32(0x690000), rd32(0x690004));
    for (uint32_t pass = 0; pass < 2; pass++) {
        // gv100_head_state: 0x682xxx is the core channel's assembly state, 0x68axxx the armed (active) one.
        const uint32_t base = pass ? 0x68a000 : 0x682000;
        LOG("GSP: display: head 0 %s: raster 0x%08x sync 0x%08x blankE 0x%08x blankS 0x%08x clk %u",
            pass ? "armed" : "asm", rd32(base + 0x64), rd32(base + 0x68), rd32(base + 0x6c), rd32(base + 0x70),
            rd32(base + 0x0c));
    }
    const uint32_t v0 = rd32(0x616330);
    IOSleep(20);
    LOG("GSP: display: head 0 raster line %u, 20 ms later %u", v0 & 0xffff, rd32(0x616330) & 0xffff);
}

// Once per GSP boot, after BAR1 is up: everything that doesn't depend on the display that is plugged in.
bool NVBringup::dispInit()
{
    GspState *g = gsp_;
    GspState::DispHw &d = g->disp;
    uint32_t st = 0;
    if (d.ready)
        return true;
    if (!d.wanted || !g->hDisp || !g->dispInst)
        return false;
    const char *what = "";
#define FAIL(msg) do { what = msg; goto fail; } while (0)
    if (!dispMemAlloc(d.inst, 0x10000, g->dispInst) || !dispMemAlloc(d.pbCore, 0x1000, 0) ||
        !dispMemAlloc(d.pbWndw, 0x1000, 0) || !dispMemAlloc(d.sync, 0x1000, 0) || !dispMemAlloc(d.ilut, 0x10000, 0) ||
        !dispMemAlloc(d.olut, 0x10000, 0))
        FAIL("VRAM allocation or BAR1 mapping failed");

    // Input LUT: nvdisplay 3 needs one to convert a fixed-point surface to the pipe's FP16 (nvkms
    // EvoFlipC5Common). Identity, DIRECT10: a 0x20-byte VSS header, then 1024 entries of FP16 R, G, B
    // (8 bytes each) and the last one repeated (EvoSetupIdentityBaseLutC5, wndwc57e_ilut_load).
    {
        auto unorm10ToFp16 = [](uint32_t i) -> uint16_t {
            if (!i)
                return 0;
            uint64_t n = i;
            const uint64_t den = 1023;
            int e = 0;
            while (n < den) {
                n <<= 1;
                e--;
            }
            uint32_t man = (uint32_t)(((n - den) * 1024 + den / 2) / den);
            if (man == 1024) {
                man = 0;
                e++;
            }
            return (uint16_t)((uint32_t)(e + 15) << 10 | man);
        };
        for (uint32_t i = 0; i <= 1024; i++) {
            const uint32_t v = unorm10ToFp16(i < 1024 ? i : 1023);
            dispW32(d.ilut, 0x20 + i * 8 + 0, v | v << 16);   // R, G
            dispW32(d.ilut, 0x20 + i * 8 + 4, v);             // B
        }
    }
    // Output LUT: nvdisplay 3 heads take one too (nouveau headc57d olut_identity, nvkms
    // EvoSetupIdentityOutputLutC5): a 0x20-byte VSS header, then 1024 entries of 16-bit fixed point
    // R, G, B (i << 6) and the last one repeated.
    for (uint32_t i = 0; i <= 1024; i++) {
        const uint32_t v = (i < 1024 ? i : 1023) << 6;
        dispW32(d.olut, 0x20 + i * 8 + 0, v | v << 16);
        dispW32(d.olut, 0x20 + i * 8 + 4, v);
    }

    // Context DMAs and the RAMHT, in the display instance memory: the RAMHT is the first 0x2000 bytes
    // (1024 entries of handle + context), then 24-byte gv100 DMA objects on 16-byte alignment. The VRAM
    // ones cover all of VRAM, so surfaces allocated on later hot-plugs are inside them.
    {
        const uint32_t instSync = 0x2000, instVram = 0x2020, instFb = 0x2040;
        auto dmaObj = [&](uint32_t off, uint64_t start, uint64_t limit, uint32_t flags) {
            dispW32(d.inst, off + 0x00, flags);
            dispW32(d.inst, off + 0x04, (uint32_t)(start >> 8));
            dispW32(d.inst, off + 0x08, (uint32_t)(start >> 40));
            dispW32(d.inst, off + 0x0c, (uint32_t)(limit >> 8));
            dispW32(d.inst, off + 0x10, (uint32_t)(limit >> 40));
        };
        const uint64_t vramLimit = ((g->vram->limit + 1 + 0xfffff) & ~0xfffffull) - 1;
        dmaObj(instSync, d.sync.pa, d.sync.pa + 0xfff, 0x45);  // VRAM | RW | small pages, pitch kind
        dmaObj(instVram, 0, vramLimit, 0x45);
        // Scanout (ISO) surfaces take a big-page context DMA (nv50_wndw_ctxdma_new: GF119_DMA_V0_PAGE_LP).
        dmaObj(instFb, 0, vramLimit, 0x05);
        // nvkm_ramht_insert: hash(chid, handle) with linear probing; context = chid << 25 | client | inst << 9.
        bool used[1024] = {};
        auto ramhtInsert = [&](uint32_t chid, uint32_t handle, uint32_t instOff) {
            uint32_t hash = 0;
            for (uint32_t h = handle; h; h >>= 10)
                hash ^= h & 1023;
            hash ^= chid << 6;
            hash &= 1023;
            for (uint32_t n = 0; n < 1024; n++, hash = (hash + 1) & 1023)
                if (!used[hash]) {
                    used[hash] = true;
                    dispW32(d.inst, hash * 8 + 0, handle);
                    dispW32(d.inst, hash * 8 + 4, chid << 25 | (g->hClient & 0x3fff) | instOff << 9);
                    return true;
                }
            return false;
        };
        // chid.user: core 0, window n 1 + n.
        if (!ramhtInsert(0, kDispHandleSync, instSync) || !ramhtInsert(0, kDispHandleVram, instVram) ||
            !ramhtInsert(1, kDispHandleSync, instSync) || !ramhtInsert(1, kDispHandleVram, instVram) ||
            !ramhtInsert(1, kDispHandleFb, instFb))
            FAIL("RAMHT full");
        if (g->hClient & 0x2000)        // the context ORs inst << 9 over the client handle's bit 13
            LOG("GSP: display: warning: client handle 0x%x has bit 13 set, which collides with the DMA object offset",
                g->hClient);
        bar1Flush();
    }

    // RM objects: the display root and the two channels (r570 struct layouts).
    d.hRoot = g->nextHandle;
    g->nextHandle += 8;
    if (!gspRmAlloc(g->hClient, g->hDevice, d.hRoot, 0xc570 /* TU102_DISP */, nullptr, 0, &st)) {
        LOG("GSP: display: TU102_DISP allocation failed (status 0x%x)", st);
        d.hRoot = 0;
        FAIL("TU102_DISP allocation failed");
    }
    {
        uint8_t man[8] = {};            // NV0073_CTRL_CMD_DP_SET_MANUAL_DISPLAYPORT: as r535_disp_oneinit
        gspRmControl(g->hClient, g->hDisp, 0x731365, man, sizeof(man), &st);
    }
    for (int w = 0; w < 2; w++) {
        const uint32_t hclass = w ? 0xc57e : 0xc57d;
        NVDispMem &pb = w ? d.pbWndw : d.pbCore;
        uint8_t cp[56] = {};            // NV2080_CTRL_INTERNAL_DISPLAY_CHANNEL_PUSHBUFFER (r570 layout)
        put32(cp, 0, 2);                // ADDR_FBMEM
        put64(cp, 8, pb.pa);
        put64(cp, 16, pb.size - 1);
        put32(cp, 28, hclass);
        put32(cp, 32, 0);               // channel instance
        cp[36] = 1;                     // valid
        put32(cp, 40, 1);               // pbTargetAperture: PHYS_NVM
        put32(cp, 44, 0);               // channelPBSize: 4 KiB
        put32(cp, 48, 1);               // subDeviceId: BIT(0)
        if (!gspRmControl(g->hIntClient, g->hIntSubdevice, 0x20800a58, cp, sizeof(cp), &st)) {
            LOG("GSP: display: DISPLAY_CHANNEL_PUSHBUFFER class 0x%x failed (status 0x%x)", hclass, st);
            FAIL("channel push buffer setup failed");
        }
        uint8_t args[40] = {};          // NV50VAIO_CHANNELDMA_ALLOCATION_PARAMETERS (r570): instance 0, offset 0
        put32(args, 32, 1);             // subDeviceId: BIT(0)
        if (!gspRmAlloc(g->hClient, d.hRoot, hclass << 16, hclass, args, sizeof(args), &st)) {
            LOG("GSP: display: channel class 0x%x allocation failed (status 0x%x)", hclass, st);
            FAIL("channel allocation failed");
        }
    }
    d.coreCur = d.wndwCur = 0;
    // The cursor channel, head 0 (nouveau r535_curs_init; NV50VAIO_CHANNELPIO_ALLOCATION_PARAMETERS: instance,
    // hObjectNotify, pControl), and its image surface. Without them there is no hardware cursor, nothing else.
    {
        uint8_t args[16] = {};
        d.cursChan = gspRmAlloc(g->hClient, d.hRoot, 0xc57a0000, 0xc57a, args, sizeof(args), &st);
        if (!d.cursChan)
            LOG("GSP: display: cursor channel allocation failed (status 0x%x): no hardware cursor", st);
        else if (!dispMemAlloc(d.curs, 256 * 256 * 4, 0)) {
            LOG("GSP: display: no VRAM for the cursor: no hardware cursor");
            OSSafeReleaseNULL(d.curs.map);
            d.curs = NVDispMem();
        }
    }

    // Core init: notifier ctxdma and the usage bounds of the eight windows (corec57d_init).
    dispMthd(false, 0x208, kDispHandleSync);
    for (uint32_t i = 0; i < 8; i++) {
        dispMthd(false, 0x1004 + i * 0x80, 0xf);                           // RGB packed 1/2/4/8 bpp
        dispMthd(false, 0x1008 + i * 0x80, 0);
        dispMthd(false, 0x1010 + i * 0x80, 0x7fff | 1u << 16 | 1u << 20);  // pixels/line, ILUT, 2 taps
    }
    if (!dispKick(false))
        FAIL("core init not consumed");

    // The mode and the scanout surface while nothing is lit: the last lit mode (it survives dispBlank and a GSP
    // reload), else CEA-861 1920x1080@60 (VIC 16: 2200x1125 total, 148.5 MHz, positive syncs). dispSetMode
    // allocates the surface at a modeset; this one exists from here, so the nub has its address and size.
    // dispLost reset d.fb, so after a reload it is a new allocation.
    if (!d.htotal) {
        d.w = 1920;
        d.h = 1080;
        d.pitch = (d.w * 4 + 255) & ~255u;
        d.pclkKhz = 148500;
        d.htotal = 2200;
        d.vtotal = 1125;
        d.hsyncStart = 1920 + 88;
        d.hsyncWidth = 44;
        d.vsyncStart = 1080 + 4;
        d.vsyncWidth = 5;
        d.syncFlags = 0;
    }
    {
        const uint64_t need = (uint64_t)d.pitch * ((d.h + 15) & ~15u);
        NVDispMem nf;
        if (!dispMemAlloc(nf, need, 0))
            FAIL("no VRAM for the scanout surface");
        OSSafeReleaseNULL(d.fb.map);
        d.fb = nf;
    }
    d.ready = true;
    dispHpdArm();
    LOG("GSP: display: engine ready (root, core and window channels); watching %u output(s)", d.nCand);
    for (uint32_t i = 0; i < d.nCand; i++)
        LOG("GSP: display:   0x%x (protocol %u)", d.candId[i], d.candProto[i]);
    return true;
fail:
    LOG("GSP: display: init FAILED: %s", what);
    setProperty("NVDisplayModeset", what);
    if (d.hRoot)
        dispDump();
    return false;
#undef FAIL
}

// Lights displayId with the EDID's first detailed timing: SOR, head, the surface (colour bars until
// something draws there), window 0.
bool NVBringup::dispSetMode(uint32_t displayId, uint32_t rmProto, const uint8_t *edid, uint32_t edidLen, bool relight)
{
    GspState *g = gsp_;
    GspState::DispHw &d = g->disp;
    uint32_t st = 0, sorIdx = ~0u;
    const uint32_t head = 0;
    const char *what = "";
#define FAIL(msg) do { what = msg; goto fail; } while (0)
    if (!d.ready || edidLen < 128)
        return false;
    nv_sink_caps sink;
    nv_edid_parse(edid, edidLen, &sink);

    // Mode: the first of the sink's timings (nv_edid_timings: the preferred detailed timing, the other detailed
    // timings, the CTA short video descriptors, 640x480p60) that the link can carry. The port's limit is GSP-RM's
    // (GET_PCLK_LIMIT, the OR's maximum pixel clock), at most 340 MHz: above that HDMI 2.0 needs scrambling and
    // SCDC (SET_HDMI_SINK_CAPS), which this doesn't set up. A sink without an HDMI VSDB gets single-link DVI,
    // 165 MHz. Other timings must also fit the sink's own maximum (HDMI Forum VSDB, HDMI VSDB, range limits);
    // the preferred one is taken at its word. If nothing fits, the preferred timing, as before.
    {
    nv_edid_timing cand[24];
    const uint32_t nCand = nv_edid_timings(edid, edidLen, &sink, cand, 24);
    uint32_t orLimit = 0;
    {
        uint8_t pl[20] = {};            // NV0073_CTRL_SPECIFIC_GET_PCLK_LIMIT_PARAMS: sub, displayId, pclk, or, vb
        put32(pl, 4, displayId);
        if (gspRmControl(g->hClient, g->hDisp, 0x73028a, pl, sizeof(pl), &st, true))
            memcpy(&orLimit, pl + 12, 4);
        else
            LOG("GSP: display: GET_PCLK_LIMIT failed (status 0x%x); assuming 340 MHz", st);
    }
    uint32_t linkMax = 340000;
    if (orLimit >= 25000 && orLimit < linkMax)      // kHz; anything below VGA's clock is not believed
        linkMax = orLimit;
    if (!sink.hdmi && linkMax > 165000)
        linkMax = 165000;
    const uint32_t sinkMax = nv_sink_max_tmds_khz(&sink);
    // The preferred timing if it fits; else the biggest that fits at 50 Hz or more (a 4K TV beyond the port: 1080p60
    // rather than 4K30), then the highest refresh up to 60 Hz, then EDID order.
    const nv_edid_timing *mode = nullptr;
    uint64_t best = 0;
    for (uint32_t i = 0; i < nCand && !(mode && mode == &cand[0]); i++) {
        const nv_edid_timing &c = cand[i];
        const bool preferred = i == 0 && c.source == NV_EDID_SRC_DTD;
        const uint32_t lim = preferred || sinkMax > linkMax ? linkMax : sinkMax;
        const char *why = c.interlaced ? "interlaced"
                        : !c.hact || !c.vact || !c.hsw || !c.vsw || c.hact > 8192 || c.vact > 8192 ? "unusable"
                        : c.pclkKhz > lim ? "pixel clock above the limit" : nullptr;
        if (why) {
            if (i < 6)
                LOG("GSP: display: timing %u (%ux%u%s, %u kHz, VIC %u) skipped: %s (limit %u kHz)", i, c.hact, c.vact,
                    c.interlaced ? "i" : "p", c.pclkKhz, c.vic, why, lim);
            continue;
        }
        const uint32_t hz = (uint32_t)((uint64_t)c.pclkKhz * 1000 / ((uint64_t)c.htotal * c.vtotal));
        const uint64_t score = (uint64_t)(hz >= 49) << 63 | (uint64_t)c.hact * c.vact << 16 |
                               (uint64_t)(hz <= 61 ? hz : 0) << 8 | (255 - (i < 255 ? i : 255));
        if (!mode || score > best) {
            mode = &c;
            best = score;
        }
    }
    if (!mode && nCand && !cand[0].interlaced && cand[0].hact && cand[0].vact && cand[0].hsw && cand[0].vsw) {
        LOG("GSP: display: no timing within the limits; trying the preferred one anyway");
        mode = &cand[0];
    }
    if (!mode) {
        LOG("GSP: display: EDID has no usable progressive timing (%u candidates)", nCand);
        FAIL("EDID has no usable timing");
    }
    LOG("GSP: display: timing %u of %u (source %u), port limit %u kHz (OR %u), sink %u kHz",
        (uint32_t)(mode - cand), nCand, mode->source, linkMax, orLimit, sinkMax);
    const uint32_t pclkKhz = mode->pclkKhz, hact = mode->hact, vact = mode->vact;
    const uint32_t hso = mode->hfp, hsw = mode->hsw, vso = mode->vfp, vsw = mode->vsw;
    const bool nhsync = mode->nhsync, nvsync = mode->nvsync;
    const uint32_t htotal = mode->htotal, vtotal = mode->vtotal;
    const uint32_t hsyncs = hact + hso, vsyncs = vact + vso;
    const uint32_t pitch = (hact * 4 + 255) & ~255u;

    // HDMI or DVI signal, and the RGB quantization range (nvkms-hdmi.c nvDpyIsHdmiEvo,
    // CalculateVideoInfoFrameColorFormat). HDMI when the sink has an HDMI VSDB; boot-arg nvhdmi=0 keeps DVI.
    // The head sends full-range RGB (0-255) unless the sink expects limited range: an HDMI sink, a CTA video
    // format other than 640x480 (VIC > 1), and no QS bit (it can't be told). Then the head compresses to 16-235
    // (OCSC1 and the procamp's CEA range, as NVKMS's EvoSetProcAmpC5/EvoSetOCsc1C5) and the AVI InfoFrame says
    // "default", which for a CTA format is limited. With QS the head stays full and the AVI InfoFrame says so.
    // Boot-arg nvrange: 0 automatic (default), 1 always full, 2 always limited.
    uint32_t hdmiArg = 1, rangeArg = 0;
    PE_parse_boot_argn("nvhdmi", &hdmiArg, sizeof(hdmiArg));
    PE_parse_boot_argn("nvrange", &rangeArg, sizeof(rangeArg));
    bool hdmi = sink.hdmi && hdmiArg != 0;
    uint8_t aspect = 0;
    const uint8_t vic = nv_cta_vic(mode, &sink, &aspect);
    LOG("GSP: display: lighting 0x%x (protocol %u): %ux%u, pclk %u kHz, h %u/%u/%u/%u v %u/%u/%u/%u, "
        "hsync %c vsync %c", displayId, rmProto, hact, vact, pclkKhz, hact, hso, hsw, htotal, vact, vso, vsw, vtotal,
        nhsync ? '-' : '+', nvsync ? '-' : '+');

    // The surface. A bigger mode gets a new one; the old one's VRAM stays allocated, since a user
    // mapping of it (NVMAC_DISPLAY_MAP) may outlive this.
    const uint64_t need = (uint64_t)pitch * ((vact + 15) & ~15u);
    NVDispMem *bufs[] = { &d.fb, &d.fbB, &d.fbC, &d.fbD };    // NVMAC_DISPLAY_FLIP picks one of the first three
    for (NVDispMem *b : bufs) {
        if (b->size < need) {
            NVDispMem nf;
            if (!dispMemAlloc(nf, need, 0))
                FAIL("no VRAM for the surface");
            OSSafeReleaseNULL(b->map);
            *b = nf;
        }
        // Colour bars show the output is lit before anything draws. On a display-pipe boot, black: WindowServer
        // shows the framebuffer's contents for a moment when it takes the display over (mirrored, on both screens).
        // A relight (DPMS on) keeps what is there.
        if (relight && b->size >= need)
            continue;
        static const uint32_t bars[8] = { 0xffffff, 0xffff00, 0x00ffff, 0x00ff00, 0xff00ff, 0xff0000, 0x0000ff, 0x000000 };
        for (uint32_t y = 0; y < vact; y++)
            for (uint32_t x = 0; x < hact; x++) {
                const bool frame = x < 16 || y < 16 || x >= hact - 16 || y >= vact - 16;
                dispW32(*b, y * pitch + x * 4, dispPipeArmed_ ? 0 : frame ? 0xffffff : bars[x * 8 / hact]);
            }
    }
    bar1Flush();
    d.front = 0;
    d.pipeArmed = ~0u;
    d.pipeShowAt = 0;
    d.pipeFreeAt[0] = d.pipeFreeAt[1] = d.pipeFreeAt[2] = 0;

    // SOR: DFP_ASSIGN_SOR for the display, as r535_outp_acquire.
    {
        uint8_t as[80] = {};
        put32(as, 4, displayId);
        if (!gspRmControl(g->hClient, g->hDisp, 0x731152, as, sizeof(as), &st)) {
            LOG("GSP: display: DFP_ASSIGN_SOR failed (status 0x%x)", st);
            FAIL("DFP_ASSIGN_SOR failed");
        }
        for (uint32_t i = 0; i < 4; i++) {
            uint32_t mask;
            memcpy(&mask, as + 40 + i * 8, 4);
            if (mask & displayId) {
                sorIdx = i;
                break;
            }
        }
        if (sorIdx == ~0u)
            FAIL("no SOR assigned");
    }

    // HDMI or DVI for this modeset (NV0073_CTRL_SPECIFIC_SET_HDMI_ENABLE_PARAMS: u8 subDeviceInstance, u32 displayId,
    // u8 enable): after the SOR assignment, before the modeset, as NVKMS's nvDpyUpdateHdmiPreModesetEvo. Told for
    // every HDMI sink, and once more to turn it off after one.
    if (sink.hdmi || d.hdmiOn) {
        uint8_t he[12] = {};
        put32(he, 4, displayId);
        he[8] = hdmi ? 1 : 0;
        if (!gspRmControl(g->hClient, g->hDisp, 0x730273, he, sizeof(he), &st)) {
            LOG("GSP: display: SET_HDMI_ENABLE(%u) failed (status 0x%x): DVI signal", he[8], st);
            hdmi = false;
        }
        d.hdmiOn = hdmi;
    }
    const bool limited = rangeArg == 2 || (rangeArg == 0 && hdmi && vic > 1 && !sink.qs);
    const uint8_t aviQ = !sink.qs ? 0 : limited ? 1 : 2;         // AVI Q: default, limited, full

    // SOR control, head raster, clock, output resource, OLUT (headc57d_*), window ownership; UPDATE.
    dispMthd(false, 0x300 + sorIdx * 0x20, rmProto << 8 | 1u << head);           // SOR_SET_CONTROL: protocol, owner
    dispMthd(false, 0x2020 + head * 0x400, displayId);                            // HEAD_SET_DISPLAY_ID
    dispMthd(false, 0x204c + head * 0x400, hact | vact << 16);                    // viewport in
    dispMthd(false, 0x2058 + head * 0x400, hact | vact << 16);                    // viewport out
    dispMthd(false, 0x2064 + head * 0x400, htotal | vtotal << 16);                // raster size
    dispMthd(false, 0x2068 + head * 0x400, (hsw - 1) | (vsw - 1) << 16);          // sync end
    {
        const uint32_t hblankE = htotal - hsyncs - 1, vblankE = vtotal - vsyncs - 1;
        dispMthd(false, 0x206c + head * 0x400, hblankE | vblankE << 16);          // blank end
        d.vbE = vblankE;
        d.vbS = vblankE + vact;
        dispMthd(false, 0x2070 + head * 0x400, (hblankE + hact) | (vblankE + vact) << 16);   // blank start
    }
    dispMthd(false, 0x2074 + head * 0x400, 0u << 16 | 1);                         // blank2 (undocumented, as nouveau)
    dispMthd(false, 0x2008 + head * 0x400, 0);                                    // progressive
    dispMthd(false, 0x200c + head * 0x400, pclkKhz * 1000);                       // pixel clock
    dispMthd(false, 0x2028 + head * 0x400, pclkKhz * 1000);                       // pixel clock max
    dispMthd(false, 0x2030 + head * 0x400, 4u | 1u << 4 | 1u << 8 | 1u << 12);    // head usage bounds
    dispMthd(false, 0x2004 + head * 0x400, (nhsync ? 4u : 0) | (nvsync ? 8u : 0) | 4u << 4 | 0x3fu << 26);
    dispMthd(false, 0x2000 + head * 0x400, limited ? 1u << 28 : 0);              // procamp: RGB, VESA or CEA range
    if (limited || d.rangeLimited) {
        // Limited range: OCSC1 scales by 219/256 and adds 16/256 (NVKMS RGBToLimitedRangeRGB, S15.16), the clamp
        // keeps 16-235 (12 bits). Back to full: OCSC1 off, no clamp. Not written at all for full range from the
        // start, as before.
        static const uint32_t ocsc1[12] = { 0xdb04, 0, 0, 0x1000, 0, 0xdb04, 0, 0x1000, 0, 0, 0xdb04, 0x1000 };
        const uint32_t clamp = limited ? 0x100u | 0xeb0u << 16 : 0u | 0xfffu << 16;
        dispMthd(false, 0x2238 + head * 0x400, clamp);                            // CLAMP_RANGE_GREEN
        dispMthd(false, 0x223c + head * 0x400, clamp);                            // CLAMP_RANGE_RED_BLUE
        dispMthd(false, 0x229c + head * 0x400, limited ? 1 : 0);                  // OCSC1CONTROL
        if (limited)
            for (uint32_t i = 0; i < 12; i++)
                dispMthd(false, 0x22a0 + head * 0x400 + i * 4, ocsc1[i]);         // OCSC1COEFFICIENT_C00..C23
        d.rangeLimited = limited;
    }
    dispMthd(false, 0x2280 + head * 0x400, (4u + 1025u) << 8 | 2u << 2 | 1u);     // OLUT: size, DIRECT10, interp
    dispMthd(false, 0x2284 + head * 0x400, 0xffffffff);                           // OLUT FP norm scale
    dispMthd(false, 0x2288 + head * 0x400, kDispHandleVram);                      // OLUT context DMA
    dispMthd(false, 0x228c + head * 0x400, (uint32_t)(d.olut.pa >> 8));           // OLUT offset
    for (uint32_t i = 0; i < 8; i++)                                              // window i -> head i / 2
        dispMthd(false, 0x1000 + i * 0x80, i >> 1);
    if (!dispCoreUpdate(0))
        FAIL("core update (head, SOR, window owner) did not complete");

    // Window 0: the surface (wndwc37e_image_set, blend_set), interlocked with the core.
    dispMthd(true, 0x308, 1u | 0u << 4);                                          // present: interval 1
    dispMthd(true, 0x224, hact | vact << 16);                                     // size
    dispMthd(true, 0x228, 0u | 1u << 4);                                          // storage: pitch layout
    dispMthd(true, 0x22c, 0xe6);                                                  // params: X8R8G8B8
    dispMthd(true, 0x230, pitch >> 6);                                            // planar storage 0: pitch
    dispMthd(true, 0x240, kDispHandleFb);                                         // context DMA ISO 0
    dispMthd(true, 0x260, (uint32_t)(d.fb.pa >> 8));                              // offset 0
    dispMthd(true, 0x290, 0);                                                     // point in
    dispMthd(true, 0x298, hact | vact << 16);                                     // size in
    dispMthd(true, 0x2a4, hact | vact << 16);                                     // size out
    dispMthd(true, 0x2a8, 1u | 1u << 4);                                          // input scaler: 2 taps
    {
        static const uint32_t fmtIdentity[12] = { 0x10000, 0, 0, 0, 0, 0x10000, 0, 0, 0, 0, 0x10000, 0 };
        for (uint32_t i = 0; i < 12; i++)
            dispMthd(true, 0x400 + i * 4, fmtIdentity[i]);                        // FMT matrix: identity (RGB)
    }
    dispMthd(true, 0x440, (4u + 1025u) << 8 | 2u << 2);                           // ILUT: size, DIRECT10
    dispMthd(true, 0x444, kDispHandleVram);                                       // ILUT context DMA
    dispMthd(true, 0x448, (uint32_t)(d.ilut.pa >> 8));                            // ILUT offset
    dispMthd(true, 0x59c, 0);                                                     // CSC11 off (identity)
    dispMthd(true, 0x2ec, 0u | 255u << 4);                                        // composition: depth 255
    dispMthd(true, 0x2f0, 255);                                                   // constant alpha K1
    dispMthd(true, 0x2f4, 0x4422);                                                // src K1, dst NEG_K1
    dispMthd(true, 0x2f8, 0xffff0000);
    dispMthd(true, 0x2fc, 0xffff0000);
    dispMthd(true, 0x300, 0xffff0000);
    dispMthd(true, 0x304, 0xffff0000);
    dispMthd(true, 0x370, 1);                                                     // interlock with the core
    dispMthd(true, 0x374, 1);                                                     // and window 0
    dispMthd(true, 0x200, 1);                                                     // UPDATE
    if (!dispKick(true))
        FAIL("window push buffer not consumed");
    if (!dispCoreUpdate(1))
        FAIL("interlocked core update did not complete");

    d.litId = displayId;
    d.sorIdx = sorIdx;
    d.proto = rmProto;
    if (edid != d.edid)
        memcpy(d.edid, edid, sink.len);
    d.edidLen = sink.len;
    d.edidValid = true;
    d.w = hact;
    d.h = vact;
    d.pitch = pitch;
    d.pclkKhz = pclkKhz;
    d.htotal = htotal;
    d.vtotal = vtotal;
    d.hsyncStart = hsyncs;
    d.hsyncWidth = hsw;
    d.vsyncStart = vsyncs;
    d.vsyncWidth = vsw;
    d.syncFlags = (nhsync ? 1u : 0) | (nvsync ? 2u : 0);
    g->dispLit = true;
    g->dispFbBar1 = d.fb.bar1;
    g->dispFbSize = d.fb.size;
    g->dispW = hact;
    g->dispH = vact;
    g->dispPitch = pitch;
    LOG("GSP: display: 0x%x lit: %ux%u on head %u -> SOR %u, surface 0x%llx", displayId, hact, vact, head, sorIdx,
        (unsigned long long)d.fb.pa);
    setProperty("NVDisplayModeset", "ok");
    {
        // What the sink's EDID says (nv_edid): for the test plan and the HDMI decisions below.
        char si[256];
        snprintf(si, sizeof(si), "%s, %u bytes (%u ext%s); %s; HDMI VSDB %s (max TMDS %u MHz)%s; range limit %u MHz; "
                 "VCDB %s (QS %u, S_IT %u, S_CE %u); audio %s (%u SADs); native VIC %u, %u VICs",
                 sink.name[0] ? sink.name : "?", sink.len, sink.nExt, sink.headerOk ? "" : ", BAD HEADER",
                 sink.cta ? "CTA-861" : "no CTA block", sink.hdmi ? "yes" : "no", sink.hdmiMaxTmdsKhz / 1000,
                 sink.hf ? ", HF-VSDB" : "", sink.rangeMaxPclkKhz / 1000, sink.vcdb ? "yes" : "no", sink.qs,
                 sink.sIT, sink.sCE, sink.basicAudio ? "basic" : "none", sink.nSad, sink.nativeVic, sink.nVic);
        LOG("GSP: display: sink: %s", si);
        setProperty("NVDisplaySink", si);
    }
    // InfoFrames once the head and the SOR run (NVKMS nvUpdateInfoFrames after the modeset).
    d.vic = vic;
    d.dpmsOff = d.dpmsBySor = false;
    {
        char hs[128];
        if (hdmi && dispHdmiInfoFrames(head, sink, vic, aspect, aviQ))
            snprintf(hs, sizeof(hs), "HDMI, AVI VIC %u Q %u, range %s, SPD", vic, aviQ, limited ? "limited" : "full");
        else if (hdmi)
            snprintf(hs, sizeof(hs), "HDMI without InfoFrames (SF registers unusable), range %s",
                     limited ? "limited" : "full");
        else
            snprintf(hs, sizeof(hs), "DVI (%s), range %s",
                     !sink.hdmi ? "no HDMI VSDB" : hdmiArg ? "SET_HDMI_ENABLE failed" : "nvhdmi=0",
                     limited ? "limited" : "full");
        if (!hdmi)
            dispHdmiInfoFramesOff(head);
        LOG("GSP: display: signal: %s", hs);
        setProperty("NVDisplaySignal", hs);
    }
    return true;
    }
fail:
    LOG("GSP: display: lighting 0x%x FAILED: %s", displayId, what);
    dispDump();
    setProperty("NVDisplayModeset", what);
    return false;
#undef FAIL
}

// ---- HDMI InfoFrames -----------------------------------------------------------------------------------
//
// On Turing NVKMS sends InfoFrames through the display's SF registers, not through RM (hdmipacket nvhdmipkt.c: the
// C57x display uses the NVHDMIPKT_C571_CLASS, which keeps 9171's register writers; nvhdmipkt_0073.c's
// SET_OD_PACKET path is only for pre-Kepler). NV_PDISP_SF_USER_0 is at 0x6f0000 on nvdisplay 3
// (swref/published/disp/v03_00/dev_disp.h, kdispGetDisplaySfUserBaseAndSize_v03_00; the cl9171.h offsets are
// relative to it), 0x400 per head. Per packet slot (index 0 AVI, 1 generic, 3 GCP, 4 vendor-specific) at
// index * 64: CTRL (bit 0 enable, bit 9 hardware checksum), STATUS, HEADER (HB0-2), then subpacks of PB0-3 / PB4-6.
// Turing only, like the rest of this display code (C57x classes); Ampere's nvdisplay 4 moves things.
static const uint32_t kSfUser = 0x6f0000;
enum { kSfAvi = 0, kSfGeneric = 1, kSfGcp = 3, kSfVsi = 4 };

static bool sfUsable(uint32_t v)
{
    return v != 0xffffffff && (v & 0xfff00000) != 0xbad00000;      // a PRI error reads as 0xbadXXXXX
}

// AVI (with the VIC and quantization range), SPD, the HDMI vendor-specific InfoFrame for an HDMI 1.4 4K format, and
// the general control packet that clears AVMUTE, which also switches the link's HDMI data islands on
// (CTRL_HDMI, as nvhdmipkt_9171.c does on the first unmuting GCP). False if the SF registers don't read sanely,
// so nothing is written.
bool NVBringup::dispHdmiInfoFrames(uint32_t head, const nv_sink_caps &sink, uint8_t vic, uint8_t aspect, uint8_t q)
{
    GspState *g = gsp_;
    const uint32_t sf = kSfUser + head * 0x400;
    const uint32_t avi0 = rd32(sf + kSfAvi * 64), gcp0 = rd32(sf + kSfGcp * 64);
    LOG("GSP: display: SF head %u before: AVI ctrl 0x%08x, generic 0x%08x, GCP 0x%08x subpack 0x%08x, VSI 0x%08x",
        head, avi0, rd32(sf + kSfGeneric * 64), gcp0, rd32(sf + 0xcc), rd32(sf + kSfVsi * 64));
    if (!sfUsable(avi0) || !sfUsable(gcp0)) {
        LOG("GSP: display: SF registers at 0x%x don't read sanely: no InfoFrames", sf);
        return false;
    }
    // hdmiPacketWrite9171: disable the slot, write header and subpacks, enable every frame with HW checksum.
    auto send = [&](uint32_t idx, const uint8_t *pkt, uint32_t subpacks) {
        const uint32_t ctrl = sf + idx * 64, hdr = ctrl + 8;
        wr32(ctrl, rd32(ctrl) & ~1u);
        wr32(hdr, pkt[0] | (uint32_t)pkt[1] << 8 | (uint32_t)pkt[2] << 16);
        for (uint32_t i = 0; i < subpacks; i++) {
            const uint8_t *b = pkt + 3 + i * 7;
            wr32(hdr + 4 + i * 8, b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24);
            wr32(hdr + 8 + i * 8, b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16);
        }
        wr32(ctrl, 1u | 1u << 9);
    };
    // HDMI 1.4 sinks know 3840/4096x2160p24-30 only by HDMI_VIC, in the vendor-specific InfoFrame, with AVI VIC 0
    // (HDMI 1.4b 8.2.3.1); HDMI 2.x sinks (HF-VSDB) take the CTA VIC.
    const uint8_t hdmiVic = sink.hf ? 0 : nv_hdmi_vic(vic);
    uint8_t avi[17];
    nv_avi_infoframe(avi, hdmiVic ? 0 : vic, aspect, q);
    send(kSfAvi, avi, 2);
    uint8_t pkt[31];
    nv_spd_infoframe(pkt, "NVIDIA", "GeForce (macOS)");
    send(kSfGeneric, pkt, 4);
    if (hdmiVic) {
        nv_hdmi_vsi_infoframe(pkt, hdmiVic);
        send(kSfVsi, pkt, 4);
    } else {
        wr32(sf + kSfVsi * 64, rd32(sf + kSfVsi * 64) & ~1u);
    }
    // GCP: clear AVMUTE (its reset value is SET_AVMUTE). CTRL_HDMI {u8 sub, u32 displayId, NvBool enable} first.
    uint32_t st = 0;
    wr32(sf + kSfGcp * 64, rd32(sf + kSfGcp * 64) & ~1u);
    uint8_t ch[12] = {};
    put32(ch, 4, g->disp.litId);
    ch[8] = 1;
    if (!gspRmControl(g->hClient, g->hDisp, 0x730274, ch, sizeof(ch), &st))
        LOG("GSP: display: CTRL_HDMI failed (status 0x%x)", st);
    wr32(sf + 0xcc, 0x10);                                                        // SB0 CLR_AVMUTE, SB1/SB2 0
    wr32(sf + kSfGcp * 64, 1u | 1u << 9);
    LOG("GSP: display: AVI %02x %02x %02x | %02x %02x %02x %02x %02x %02x (VIC %u, HDMI_VIC %u); SF after: AVI ctrl "
        "0x%08x, generic 0x%08x, GCP 0x%08x, VSI 0x%08x", avi[0], avi[1], avi[2], avi[3], avi[4], avi[5], avi[6],
        avi[7], avi[8], vic, hdmiVic, rd32(sf), rd32(sf + kSfGeneric * 64), rd32(sf + kSfGcp * 64),
        rd32(sf + kSfVsi * 64));
    return true;
}

// The packets off (DVI, or the display gone): each slot's enable bit cleared, if the registers read sanely.
void NVBringup::dispHdmiInfoFramesOff(uint32_t head)
{
    const uint32_t sf = kSfUser + head * 0x400;
    if (!sfUsable(rd32(sf)))
        return;
    static const uint32_t slots[] = { kSfAvi, kSfGeneric, kSfGcp, kSfVsi };
    for (uint32_t idx : slots) {
        const uint32_t v = rd32(sf + idx * 64);
        if (sfUsable(v) && (v & 1))
            wr32(sf + idx * 64, v & ~1u);
    }
}

// The lit display went away: window 0 and the SOR detached (wndwc37e_image_clr; SOR_SET_CONTROL owner none),
// so the head scans out nothing. The surface stays allocated.
void NVBringup::dispBlank()
{
    GspState *g = gsp_;
    GspState::DispHw &d = g->disp;
    if (!d.ready || !d.litId)
        return;
    dispMthd(true, 0x308, 0);                                                     // present control
    dispMthd(true, 0x240, 0);                                                     // context DMA ISO 0: none
    dispMthd(true, 0x370, 1);
    dispMthd(true, 0x374, 1);
    dispMthd(true, 0x200, 1);
    dispKick(true);
    cursorLive(false);
    if (d.cursOn) {
        dispMthd(false, 0x209c, 0xcf);                                            // CONTROL_CURSOR: disabled
        dispMthd(false, 0x2088, 0);                                               // CONTEXT_DMA_CURSOR: none
        d.cursOn = false;
    }
    if (d.sorIdx != ~0u)
        dispMthd(false, 0x300 + d.sorIdx * 0x20, 0);                              // SOR_SET_CONTROL: no owner
    dispCoreUpdate(1);
    dispHdmiInfoFramesOff(0);
    d.dpmsOff = d.dpmsBySor = false;
    LOG("GSP: display: 0x%x unplugged: window 0 and SOR %u detached", d.litId, d.sorIdx);
    d.litId = 0;
    d.sorIdx = ~0u;
    g->dispLit = false;
    setProperty("NVDisplayModeset", "unplugged");
}

// NV_DISPLAY_POWER_FN, DPMS: macOS put the display to sleep (on false) or woke it. Off is NVKMS's for a TMDS output
// (nvRmSetDpmsEvo): SET_MONITOR_POWER (0x730295; u32 subDeviceInstance, displayId, powerState, headIdx, NvBool
// bForceMonitorState), which keeps the head, the mode and the surfaces and stops the link, so the sink sees no
// signal and goes to standby. If GSP-RM refuses it, the SOR is detached as at an unplug, without forgetting the
// display, and waking relights it (dispSetMode, keeping the surfaces' contents). The hot-plug poll leaves a
// sleeping display alone: it stays connected and lit as far as macOS is concerned. Boot-arg nvdpms=0: ignored.
IOReturn NVBringup::dispPower(bool on)
{
    uint32_t arg = 1;
    if (PE_parse_boot_argn("nvdpms", &arg, sizeof(arg)) && !arg)
        return kIOReturnUnsupported;
    if (!gspLock_)
        return kIOReturnNotReady;
    IOLockLock(gspLock_);
    GspState *g = gsp_;
    if (!g || !g->booted || !g->disp.ready || !g->disp.litId) {
        IOLockUnlock(gspLock_);
        return kIOReturnNotReady;
    }
    GspState::DispHw &d = g->disp;
    IOReturn r = kIOReturnSuccess;
    uint32_t st = 0;
    if (on != d.dpmsOff) {                          // already in that state
        IOLockUnlock(gspLock_);
        return kIOReturnSuccess;
    }
    uint8_t mp[20] = {};
    put32(mp, 4, d.litId);
    put32(mp, 8, on ? 1 : 0);                       // NV0073_CTRL_SPECIFIC_SET_MONITOR_POWER_ON / _OFF
    put32(mp, 12, 0);                               // head 0
    if (!on) {
        if (gspRmControl(g->hClient, g->hDisp, 0x730295, mp, sizeof(mp), &st)) {
            d.dpmsBySor = false;
        } else {
            LOG("GSP: display: SET_MONITOR_POWER off failed (status 0x%x): detaching the SOR instead", st);
            cursorLive(false);
            if (d.sorIdx != ~0u)
                dispMthd(false, 0x300 + d.sorIdx * 0x20, 0);                      // SOR_SET_CONTROL: no owner
            dispCoreUpdate(0);
            d.dpmsBySor = true;
            d.cursOn = false;                       // re-enabled after the relight
        }
        d.dpmsOff = true;
    } else {
        bool lit = false;
        if (!d.dpmsBySor) {
            lit = gspRmControl(g->hClient, g->hDisp, 0x730295, mp, sizeof(mp), &st);
            if (!lit)
                LOG("GSP: display: SET_MONITOR_POWER on failed (status 0x%x): relighting", st);
        }
        if (!lit) {
            lit = dispSetMode(d.litId, d.proto, d.edid, d.edidLen, true);
            if (lit)
                cursorEnableLocked();
        }
        if (lit)
            d.dpmsOff = d.dpmsBySor = false;
        else
            r = kIOReturnError;
    }
    LOG("GSP: display: power %s (%s)%s", on ? "on" : "off", d.dpmsBySor ? "SOR" : "SET_MONITOR_POWER",
        r ? ": FAILED" : "");
    setProperty("NVDisplayPower", d.dpmsOff ? (d.dpmsBySor ? "off (SOR detached)" : "off") : "on");
    IOLockUnlock(gspLock_);
    return r;
}

// The display's EDID, the base block and its extensions (GSP-RM reads it over DDC), up to kNvEdidMax bytes.
bool NVBringup::dispReadEdid(uint32_t displayId, uint8_t *edid, uint32_t *len)
{
    GspState *g = gsp_;
    uint32_t st = 0, n = 0;
    uint8_t *ed = (uint8_t *)IOMallocZero(NV0073_EDID_SIZE);
    if (!ed)
        return false;
    put32(ed, NV0073_EDID_displayId, displayId);
    if (gspRmControl(g->hClient, g->hDisp, NV0073_CTRL_CMD_SPECIFIC_GET_EDID_V2_, ed, NV0073_EDID_SIZE, &st))
        memcpy(&n, ed + NV0073_EDID_bufferSize, 4);
    const bool ok = n >= 128 && n <= NV0073_EDID_MAX;
    *len = 0;
    if (ok) {
        *len = n < kNvEdidMax ? n : kNvEdidMax;
        memcpy(edid, ed + NV0073_EDID_buffer, *len);
    }
    IOFree(ed, NV0073_EDID_SIZE);
    return ok;
}

// ---- Hot-plug events ------------------------------------------------------------------------------------
//
// GSP-RM handles the HPD interrupts and tells its clients through NV2080_NOTIFIERS_HOTPLUG (cl2080_notification.h)
// on the subdevice: an event object of class NV01_EVENT_KERNEL_CALLBACK_EX (NV0005_ALLOC_PARAMETERS: hParentClient,
// hSrcResource = the subdevice, the notifier, hClass, notifyIndex with NV01_EVENT_CLIENT_RM so GSP-RM posts it
// back to us, data), enabled with NV2080_CTRL_CMD_EVENT_SET_NOTIFICATION (event, action REPEAT), as nouveau's
// r535 disp does. GSP-RM then sends NV_VGPU_MSG_EVENT_POST_EVENT (rpc_post_event_v17_00), which the status queue
// delivers within 100 ms (the poller; any RPC wait drains it too). Without it the poll runs once a second as
// before. Boot-arg nvhpdevent=0: no event object.
void NVBringup::dispHpdArm()
{
    GspState *g = gsp_;
    GspState::DispHw &d = g->disp;
    uint32_t arg = 1, st = 0;
    if (d.hpdEvent || (PE_parse_boot_argn("nvhpdevent", &arg, sizeof(arg)) && !arg))
        return;
    const uint32_t h = g->nextHandle;
    g->nextHandle += 8;
    uint8_t ev[24] = {};                            // NV0005_ALLOC_PARAMETERS (data, an NvP64, at 16)
    put32(ev, 0, g->hClient);                       // hParentClient
    put32(ev, 4, g->hSubdevice);                    // hSrcResource: the notifier
    put32(ev, 8, 0x7e);                             // hClass: NV01_EVENT_KERNEL_CALLBACK_EX
    put32(ev, 12, 0x04000000u | 1);                 // NV01_EVENT_CLIENT_RM | NV2080_NOTIFIERS_HOTPLUG
    if (!gspRmAlloc(g->hClient, g->hSubdevice, h, 0x7e, ev, sizeof(ev), &st)) {
        LOG("GSP: display: hot-plug event object refused (status 0x%x): polling once a second", st);
        return;
    }
    uint8_t sn[20] = {};                            // NV2080_CTRL_EVENT_SET_NOTIFICATION_PARAMS
    put32(sn, 0, 1);                                // event: NV2080_NOTIFIERS_HOTPLUG
    put32(sn, 4, 2);                                // action: REPEAT
    if (!gspRmControl(g->hClient, g->hSubdevice, 0x20800301, sn, sizeof(sn), &st)) {
        LOG("GSP: display: hot-plug SET_NOTIFICATION failed (status 0x%x): polling once a second", st);
        gspRmFree(g->hClient, g->hSubdevice, h, &st);
        return;
    }
    d.hpdEvent = h;
    d.hpdBurst = 0;
    LOG("GSP: display: hot-plug events on (event 0x%x); polling every 5 s as a fallback", h);
    setProperty("NVDisplayHotplug", "events");
}

// NV_VGPU_MSG_EVENT_POST_EVENT, from handleGspMessage (gspLock_ held). rpc_post_event_v17_00: hClient, hEvent,
// notifyIndex, data, info16 (u16), status, eventDataSize, bNotifyList (u8), eventData[] at 29; for HOTPLUG,
// Nv2080HotplugNotification { plugDisplayMask, unplugDisplayMask }. Only logged: the poll reads the state itself,
// sampling every 300 ms from the next tick until the change has settled.
void NVBringup::dispHpdEvent(const uint8_t *p, uint32_t len)
{
    GspState *g = gsp_;
    uint32_t hClient = 0, hEvent = 0, index = 0, size = 0, plug = 0, unplug = 0;
    if (len >= 28) {
        memcpy(&hClient, p, 4);
        memcpy(&hEvent, p + 4, 4);
        memcpy(&index, p + 8, 4);
        memcpy(&size, p + 24, 4);
    }
    if (!g || !g->disp.hpdEvent || hEvent != g->disp.hpdEvent) {
        static uint32_t logged;
        if (logged++ < 8)
            LOG("GSP: event: client 0x%x event 0x%x index 0x%x (%u bytes of data) ignored", hClient, hEvent, index,
                size);
        return;
    }
    if (size >= 8 && len >= 29 + 8) {
        memcpy(&plug, p + 29, 4);
        memcpy(&unplug, p + 33, 4);
    }
    GspState::DispHw &d = g->disp;
    d.hpdEvents++;
    d.hpdBurst = 6;
    d.hpdNext = d.tick + 1;
    LOG("GSP: display: hot-plug event %u: plug 0x%x unplug 0x%x", d.hpdEvents, plug, unplug);
    setProperty("NVDisplayHotplugEvents", d.hpdEvents, 32);
}

// From the GSP poller (gspLock_ held), every 100 ms. Samples the outputs' connect state once a second; with hot-plug
// events (dispHpdArm) every 300 ms for a while after an event and every 5 s otherwise. A change must hold for two
// samples (HPD bounces while a cable goes in). Keeps the lit display while it stays connected, else lights the first
// connected TMDS output.
void NVBringup::dispHotplugTick()
{
    GspState *g = gsp_;
    GspState::DispHw &d = g->disp;
    if (!d.ready || !d.nCand)
        return;
    d.tick++;
    if (d.dpmsOff)          // asleep (dispPower): GSP-RM's state may say anything; checked again once woken
        return;
    if (!d.hpdEvent) {
        if (d.tick % 10)
            return;
    } else if (d.hpdBurst) {
        if ((int32_t)(d.tick - d.hpdNext) < 0)
            return;
        d.hpdBurst--;
        d.hpdNext = d.tick + 3;
    } else if (d.tick % 50) {
        return;
    }
    // Another sample soon (a change waiting for its second sample, an EDID not readable yet), events or not.
    auto again = [&](uint32_t ticks) {
        if (d.hpdEvent && !d.hpdBurst) {
            d.hpdBurst = 1;
            d.hpdNext = d.tick + ticks;
        }
    };
    uint32_t mask = 0, st = 0, conn = 0;
    for (uint32_t i = 0; i < d.nCand; i++)
        mask |= d.candId[i];
    uint8_t cs[NV0073_CONNECT_SIZE] = {};
    put32(cs, NV0073_CONNECT_displayMask, mask);
    // While lit, GSP-RM's cached state (METHOD_CACHED; it handles the hot-plug interrupts: an unplug showed in
    // 1-2 s). The full detection held gspLock_ 20-67 ms each time, DDC and load detection disabled or not,
    // stalling every GPU submission behind it (NVPollHold, 2026-10-12). Nothing lit, the full detection, as before.
    if (d.litId)
        put32(cs, 4, 1u | 1u << 4 | 1u << 5);
    if (!gspRmControl(g->hClient, g->hDisp, NV0073_CTRL_CMD_SYSTEM_GET_CONNECT_STATE_, cs, sizeof(cs), &st,
                      true))      // once a second: logged only when it fails
        return;
    memcpy(&conn, cs + NV0073_CONNECT_displayMask, 4);
    uint32_t want = 0, wantProto = 0;
    if (d.litId && (conn & d.litId)) {
        want = d.litId;
    } else {
        for (uint32_t i = 0; i < d.nCand && !want; i++)
            if (conn & d.candId[i]) {
                want = d.candId[i];
                wantProto = d.candProto[i];
            }
    }
    if (!(conn & d.failId))
        d.failId = 0;
    if (want == d.litId || (want && want == d.failId)) {
        d.pendCount = 0;
        return;
    }
    if (want != d.pendId || !d.pendCount) {
        d.pendId = want;
        d.pendCount = 1;
        again(3);
        return;
    }
    d.pendCount = 0;
    const bool wasLit = d.litId != 0;
    if (wasLit)
        dispBlank();
    if (want) {
        uint8_t *edid = (uint8_t *)IOMallocZero(kNvEdidMax);
        uint32_t edidLen = 0;
        if (!edid || !dispReadEdid(want, edid, &edidLen)) {
            if (edid)
                IOFree(edid, kNvEdidMax);
            LOG("GSP: display: 0x%x connected, but its EDID can't be read yet", want);     // tried again in 2 s
            again(20);
            if (wasLit)
                dispPublish();
            return;
        }
        LOG("GSP: display: 0x%x connected", want);
        const bool lit = dispSetMode(want, wantProto, edid, edidLen);
        IOFree(edid, kNvEdidMax);
        if (!lit) {
            d.failId = want;
            if (wasLit)
                dispPublish();
            return;
        }
        cursorEnableLocked();
    }
    dispPublish();
}

// NVDisplayFB (the lit surface and mode, for nvvdisplay) or its absence, and NVDisplayGen, bumped on every
// change so user space notices a replug with the same mode too.
void NVBringup::dispPublish()
{
    GspState *g = gsp_;
    dispGen_++;
    setProperty("NVDisplayGen", dispGen_, 32);
    IODeviceMemory *bar1 = pci_ ? pci_->getDeviceMemoryWithIndex(1) : nullptr;
    // Armed, the nub is up whenever the engine is (lit or not, Online says which); otherwise only while lit.
    const bool up = g && (g->disp.litId || (dispPipeArmed_ && g->disp.ready));
    if (!up || !bar1) {
        removeProperty("NVDisplayFB");
        dispNubWant(nullptr, false);
        return;
    }
    GspState::DispHw &d = g->disp;
    OSDictionary *dict = OSDictionary::withCapacity(16);
    if (!dict)
        return;
    auto num = [&](const char *k, uint64_t v, unsigned bits) {
        if (OSNumber *n = OSNumber::withNumber(v, bits)) {
            dict->setObject(k, n);
            n->release();
        }
    };
    // The CPU physical address: getPhysicalAddress() goes through the system mapper (VT-d) and gives an I/O
    // address that is RAM to the CPU. NVFramebuffer handed it to WindowServer as the aperture: kernel panic.
    num("PhysAddr", bar1->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone) + d.fb.bar1, 64);
    num("Size", d.fb.size, 64);
    num("Width", d.w, 32);
    num("Height", d.h, 32);
    num("Pitch", d.pitch, 32);
    num("PixelClockKHz", d.pclkKhz, 32);
    num("HTotal", d.htotal, 32);
    num("VTotal", d.vtotal, 32);
    num("HSyncStart", d.hsyncStart, 32);
    num("HSyncWidth", d.hsyncWidth, 32);
    num("VSyncStart", d.vsyncStart, 32);
    num("VSyncWidth", d.vsyncWidth, 32);
    num("Flags", d.syncFlags, 32);
    num("DisplayID", d.litId, 32);
    if (d.cursChan && d.curs.cpu)
        num("CursorSize", 256, 32);         // NVFramebuffer offers a hardware cursor (NV_CURSOR_*_FN)
    if (d.edidValid) {              // the last lit display's: it stays while unlit (Online false)
        if (OSData *e = OSData::withBytes(d.edid, d.edidLen)) {
            dict->setObject("EDID", e);
            e->release();
        }
    }
    setProperty("NVDisplayFB", dict);
    dispNubWant(dict, d.litId != 0);
    dict->release();
}

// NVFramebuffer's provider: a nub under the GPU's PCI device (NVBringup's provider), so NVFramebuffer is a client
// at depth 2 of it. Published while a display is lit, carrying NVDisplayFB. NVFramebuffer (display/) matches it
// as soon as it appears, so nothing waits for the modeset, and a replug or unplug replaces or removes it.
// Armed (dispPipeArmed_): published once the display engine is up, lit or not, carrying Online too. A change
// updates its NVDisplayFB and Online and sends kNVDisplayMsgState to its clients, instead of replacing it.
class NVDisplayNub : public IOService {
    OSDeclareDefaultStructors(NVDisplayNub)
};
OSDefineMetaClassAndStructors(NVDisplayNub, IOService)

// The message to NVFramebuffer's message(): the nub's NVDisplayFB or Online changed ('NVDB'; display/src/NVFramebuffer.cpp
// has the same value).
static const UInt32 kNVDisplayMsgState = 0x4e564442;

// NVFramebuffer instead of nvvdisplay: boot-arg nvfb=1, or for one boot the NVRAM variable nvfb-once
// (`sudo nvram nvfb-once=1`), deleted here the first time a display is lit, so a boot that hangs on the
// framebuffer is followed by a normal one. The display pipe gate (dispPipeArmed_) wants it too.
bool NVBringup::fbWanted()
{
    if (fbWanted_ >= 0)
        return fbWanted_ == 1;
    uint32_t arg = 0;
    fbWanted_ = ((PE_parse_boot_argn("nvfb", &arg, sizeof(arg)) && arg) || dispPipeArmed_) ? 1 : 0;
    if (!fbWanted_ && nvramOneShot("nvfb-once"))
        fbWanted_ = 1;
    if (fbWanted_ && safeMode_) {
        LOG("safe mode (boot-attempt guard): NVFramebuffer not wanted this boot");
        fbWanted_ = 0;
    }
    return fbWanted_ == 1;
}

// An NVRAM one-shot: read once, and if it is set, deleted here and checked. The delete is the one `nvram -d`
// does: setProperty(kIONVRAMDeletePropertyKey) doesn't delete, setProperties() with it and
// kIONVRAMSyncNowPropertyKey does. True if the variable was set, which makes it apply to this boot only.
IORegistryEntry *NVBringup::nvramOptions()
{
    // OpenCore injects this kext, so start() can run before AppleEFINVRAM has loaded the OS's variables into /options:
    // then a variable set with `nvram` is missing here and the one-shot is silently ignored (2026-10-10 13:06 and 13:14,
    // with nvdpipe-once=1 still set after boot). Wait for the controller first; it is up within a second of boot.
    static bool nvramReady = false;
    if (!nvramReady) {
        OSDictionary *match = IOService::serviceMatching("AppleEFINVRAM");
        IOService *ctl = match ? IOService::waitForMatchingService(match, 10ULL * NSEC_PER_SEC) : nullptr;
        OSSafeReleaseNULL(match);
        if (ctl) {
            ctl->release();
        } else {
            LOG("nvram: AppleEFINVRAM not up after 10 s; reading /options anyway");
        }
        nvramReady = true;
    }
    return IORegistryEntry::fromPath("/options", gIODTPlane);
}

bool NVBringup::nvramOneShot(const char *name)
{
    IORegistryEntry *opts = nvramOptions();
    OSObject *v = opts ? opts->copyProperty(name) : nullptr;
    if (!v) {
        OSSafeReleaseNULL(opts);
        return false;
    }
    v->release();
    OSString *key = OSString::withCString(name);
    static const char *const keys[] = { kIONVRAMDeletePropertyKey, kIONVRAMSyncNowPropertyKey };
    for (const char *k : keys) {
        OSDictionary *req = key ? OSDictionary::withCapacity(1) : nullptr;
        if (req) {
            req->setObject(k, key);
            opts->setProperties(req);
            req->release();
        }
    }
    OSSafeReleaseNULL(key);
    OSObject *left = opts->copyProperty(name);
    if (left) {
        LOG("nvram %s: set, for this boot; the variable could NOT be deleted (run sudo nvram -d %s)", name, name);
    } else {
        LOG("nvram %s: set, for this boot; the variable is deleted", name);
    }
    OSSafeReleaseNULL(left);
    OSSafeReleaseNULL(opts);
    return true;
}

// The boot-attempt guard (boot-arg nvguard=1, off by default). NVRAM nv-boot-try counts the boots that started this kext
// and never reached a healthy desktop: it goes up here at every start, and the boot daemon (tools/daemon/gsp_boot.sh)
// deletes it once WindowServer has run for a minute. A panic, a hang or a login loop leaves it set. At kNVGuardTries
// unfinished boots this boot is a safe one: the experimental gates (nvdpipe, nvdpipe-once, nvfb, nvfb-once) are ignored,
// NVSafeMode is published, and the auxiliary kexts stay out (nv_stack_admit). The normal kext still starts, since it is
// what the desktop has run on for weeks; boot-arg -nvoff leaves the GPU alone entirely.
// It fails open: a missing or unreadable NVRAM means no safe mode, and a write that fails is logged and changes nothing.
// The variable is in Apple's NVRAM namespace, like nvdpipe-once (OpenCore's NVRAM Delete lists only boot-args there).
void NVBringup::bootGuard()
{
    static const uint32_t kNVGuardTries = 2;
    static const char kVar[] = "nv-boot-try";
    uint32_t arm = 0;
    if (!PE_parse_boot_argn("nvguard", &arm, sizeof(arm)) || arm != 1)
        return;
    IORegistryEntry *opts = nvramOptions();
    if (!opts) {
        LOG("boot guard: no /options; off this boot");
        return;
    }
    // `sudo nvram nv-boot-try=3` stores the text as data; this kext writes one decimal digit.
    uint32_t tries = 0;
    OSObject *v = opts->copyProperty(kVar);
    const char *p = nullptr;
    unsigned len = 0;
    if (OSData *d = OSDynamicCast(OSData, v)) {
        p = (const char *)d->getBytesNoCopy();
        len = d->getLength();
    } else if (OSString *str = OSDynamicCast(OSString, v)) {
        p = str->getCStringNoCopy();
        len = str->getLength();
    }
    for (unsigned i = 0; p && i < len && p[i] >= '0' && p[i] <= '9' && tries < 100; i++)
        tries = tries * 10 + (uint32_t)(p[i] - '0');
    OSSafeReleaseNULL(v);

    const uint32_t next = tries < 9 ? tries + 1 : 9;
    const char digit = (char)('0' + next);
    OSData *val = OSData::withBytes(&digit, 1);
    OSString *key = OSString::withCString(kVar);
    bool wrote = false;
    if (val && key) {
        OSDictionary *req = OSDictionary::withCapacity(1);
        if (req) {
            req->setObject(key, val);
            wrote = opts->setProperties(req) == kIOReturnSuccess;
            req->release();
        }
        if ((req = OSDictionary::withCapacity(1))) {
            req->setObject(kIONVRAMSyncNowPropertyKey, key);
            opts->setProperties(req);
            req->release();
        }
    }
    OSSafeReleaseNULL(val);
    OSSafeReleaseNULL(key);
    OSSafeReleaseNULL(opts);

    if (tries >= kNVGuardTries) {
        safeMode_ = true;
        setProperty(NV_SAFE_MODE_KEY, kOSBooleanTrue);
        LOG("boot guard: %u boots in a row didn't reach a healthy desktop: SAFE MODE, the experimental gates and the "
            "auxiliary kexts are off this boot (sudo nvram -d %s, or tools/nvctl boot-ok, to clear)", tries, kVar);
    } else {
        LOG("boot guard: boot attempt %u of %u before safe mode%s", next, kNVGuardTries + 1,
            wrote ? "" : " (the counter could NOT be written: the guard can't trip)");
    }
}

// Records the nub's state for dispNubSync: fb is NVDisplayFB, nullptr none; online says a display is lit. Caller
// holds gspLock_.
void NVBringup::dispNubWant(OSDictionary *fb, bool online)
{
    if (fb)
        fb->retain();
    OSSafeReleaseNULL(dispWant_);
    dispWant_ = fb;
    dispWantOnline_ = online;
    dispWantGen_++;
}

// Brings dispNub_ to the newest state recorded by dispNubWant. Callers have released gspLock_: registerService()
// and terminate() run IORegistry matching and the clients' start() and stop() on this thread, and a client that
// calls back into NVBringup (through a user client, or the IOAccelerator family's gate) takes gspLock_, which
// deadlocks if it is held here. dispNubLock_ orders the applies; each one catches up to the newest generation,
// so a rapid unplug and replug ends on the last state. Armed, a nub that is up is updated in place (properties,
// then the message to NVFramebuffer), so an unplug or replug never terminates it; messageClients runs here too.
void NVBringup::dispNubSync()
{
    if (!dispNubLock_ || !gspLock_)
        return;
    IOLockLock(dispNubLock_);
    for (;;) {
        IOLockLock(gspLock_);
        OSDictionary *want = dispWant_;
        const bool online = dispWantOnline_;
        const uint32_t gen = dispWantGen_;
        if (want)
            want->retain();
        IOLockUnlock(gspLock_);
        if (gen == dispNubGen_) {
            OSSafeReleaseNULL(want);
            break;
        }
        if (dispPipeArmed_ && dispNub_ && want) {
            // Armed: the nub stays. A change of display or mode is its properties, and NVFramebuffer is told.
            dispNub_->setProperty("NVDisplayFB", want);
            dispNub_->setProperty("Online", online ? kOSBooleanTrue : kOSBooleanFalse);
            dispNub_->messageClients(kNVDisplayMsgState);
            LOG("display: NVDisplayNub updated (online %s)", online ? "yes" : "no");
        } else {
            if (dispNub_) {
                dispNub_->terminate();
                OSSafeReleaseNULL(dispNub_);
            }
            if (want && pci_ && fbWanted()) {
                NVDisplayNub *n = OSTypeAlloc(NVDisplayNub);
                if (n && !n->init())
                    OSSafeReleaseNULL(n);
                if (n) {
                    n->setProperty("NVDisplayFB", want);
                    if (dispPipeArmed_) {
                        n->setProperty("NVDisplayPipeArmed", kOSBooleanTrue);
                        n->setProperty("Online", online ? kOSBooleanTrue : kOSBooleanFalse);
                    }
                    if (n->attach(pci_)) {
                        n->registerService();
                        dispNub_ = n;
                        LOG("display: published NVDisplayNub for NVFramebuffer");
                        bootBusyRelease("display nub registered");
                    } else {
                        n->release();
                    }
                }
            }
        }
        dispNubGen_ = gen;
        OSSafeReleaseNULL(want);
    }
    IOLockUnlock(dispNubLock_);
}

// shutdownHw (stop, or didTerminate when the PCI device goes). The nub is a client of the PCI device, not of
// NVBringup, so nothing terminates it with us: it goes here, before the device is closed.
void NVBringup::dispNubStop()
{
    if (gspLock_) {
        IOLockLock(gspLock_);
        dispNubWant(nullptr, false);
        IOLockUnlock(gspLock_);
    }
    dispNubSync();
}

// NVMAC_DISPLAY_FLIP: window 0 to the other buffer, applied by the display engine at the next vblank
// (PRESENT_CONTROL non-tearing). The window notifier, at 0x100 in the sync page, leaves NOT_BEGUN once the
// flip has latched, i.e. the old buffer is no longer scanned out. The methods go in under gspLock_; the
// wait (up to a frame) is outside it, through a retained mapping, so other GPU work isn't held up.
IOReturn NVBringup::displayFlip(GpuConn *c, uint32_t buffer)
{
    if (buffer > 2)
        return kIOReturnBadArgument;
    IOLockLock(gspLock_);
    GspState *g = gsp_;
    if (!g || !g->booted || c->dead || !g->dispLit || !g->disp.ready) {
        IOLockUnlock(gspLock_);
        return kIOReturnNotReady;
    }
    GspState::DispHw &d = g->disp;
    NVDispMem &f = buffer == 2 ? d.fbC : buffer ? d.fbB : d.fb;
    if (!f.pa || !d.sync.map) {
        IOLockUnlock(gspLock_);
        return kIOReturnNotReady;
    }
    IOMemoryMap *keep = d.sync.map;
    keep->retain();
    volatile uint32_t *ntfy = (volatile uint32_t *)(d.sync.cpu + 0x100);
    ntfy[0] = 0;                                                                  // NOT_BEGUN
    ntfy[1] = ntfy[2] = ntfy[3] = 0;
    dispMthd(true, 0x21c, kDispHandleSync);                                       // CONTEXT_DMA_NOTIFIER
    dispMthd(true, 0x220, 0x100u | 0u);                                           // NOTIFIER_CONTROL: write, 0x100
    dispMthd(true, 0x260, (uint32_t)(f.pa >> 8));                                 // offset 0: the buffer
    dispMthd(true, 0x370, 0);                                                     // not interlocked with the core
    dispMthd(true, 0x374, 0);
    dispMthd(true, 0x200, 1);                                                     // UPDATE
    d.front = buffer;
    IOLockUnlock(gspLock_);
    // Latched once the notifier leaves NOT_BEGUN, or once window 0's armed offset is the new buffer: 0x690000 +
    // method is the window channel's assembly state, + 0x800 the armed one (nvkm gv100_disp_wndw_mthd, .prev).
    // Armed is what the next vblank makes active, so then the raster has to pass a whole vblank too: only after
    // that is the old buffer out of scanout. Head 0's line is 0x616330 (gv100_head_rgpos); the active lines
    // lie between blank end and blank start, the vblank around the wrap to 0.
    const uint32_t want = (uint32_t)(f.pa >> 8);
    IOReturn r = kIOReturnTimeout;
    int how = 0;                            // 1 notifier, 2 armed state; 3 armed, no vblank seen
    int latched = 0;
    uint32_t prev = ~0u, waited = 0, latchLine = ~0u;
    bool active0 = false, sawBlank = false;
    for (int i = 0; i < 100 && !how; i++) { // a 0xffffffff read (GPU gone) ends it too
        IOLockLock(gspLock_);
        const bool up = gsp_ && gsp_->booted && gsp_->disp.ready;
        if (up && !latched)
            latched = (ntfy[0] >> 30) ? 1 : rd32(0x690a60) == want ? 2 : 0;
        if (up && latched) {
            const uint32_t v = rd32(0x616330) & 0xffff;
            const bool active = v > gsp_->disp.vbE && v <= gsp_->disp.vbS;
            if (latchLine == ~0u)
                latchLine = v;
            if (!active)
                sawBlank = active0;         // a vblank after an active line seen since latching
            else if (!active0)
                active0 = true;             // latched in a vblank: count from the next active line
            else if (sawBlank || v < prev)  // active again after a vblank, or wrapped past one between samples
                how = latched;
            if (active)
                prev = v;
            if (!how && ++waited > 25)      // the raster doesn't seem to move: take the armed state as is
                how = 3;
        }
        IOLockUnlock(gspLock_);
        if (!up)
            break;
        if (!how)
            IOSleep(1);
    }
    if (how)
        r = kIOReturnSuccess;
    IOLockLock(gspLock_);
    if (gsp_ && gsp_->booted && gsp_->disp.ready) {
        GspState::DispHw &dd = gsp_->disp;
        dd.flipBy[how]++;
        // The raster line when the flip was seen latched, for the first flips: spread over the frame if
        // latching is the UPDATE's arrival, near the vblank if it's the vblank itself.
        if (dd.nLatch < 24 && latchLine != ~0u) {
            dd.latchLine[dd.nLatch++] = (uint16_t)latchLine;
            char l[24 * 6 + 64];
            int o = snprintf(l, sizeof(l), "active %u-%u:", dd.vbE + 1, dd.vbS);
            for (uint32_t k = 0; k < dd.nLatch && o < (int)sizeof(l) - 6; k++)
                o += snprintf(l + o, sizeof(l) - (size_t)o, " %u", dd.latchLine[k]);
            setProperty("NVDisplayFlipLines", l);
        }
        if (how != 1 && how != 2 && dd.flipLogged < 3) {     // what the notifier and the window state say
            dd.flipLogged++;
            char msg[256];
            snprintf(msg, sizeof(msg),
                     "flip to %u by %s: ntfy %08x %08x %08x %08x; offset asy %08x armed %08x (want %08x); "
                     "ntfy ctl asy %08x/%08x armed %08x/%08x; exc %08x %08x %08x; PUT %x GET %x",
                     buffer, how ? "armed state, no vblank seen" : "nothing (timeout)", ntfy[0], ntfy[1], ntfy[2], ntfy[3],
                     rd32(0x690260), rd32(0x690a60), want, rd32(0x69021c), rd32(0x690220), rd32(0x690a1c),
                     rd32(0x690a20), rd32(0x61102c), rd32(0x611030), rd32(0x611034), rd32(0x690000),
                     rd32(0x690004));
            LOG("GSP: display: %s", msg);
            setProperty("NVDisplayFlipDiag", msg);
        }
        char counts[128];
        snprintf(counts, sizeof(counts), "notifier %u, armed state %u, armed without vblank %u, timed out %u",
                 dd.flipBy[1], dd.flipBy[2], dd.flipBy[3], dd.flipBy[0]);
        setProperty("NVDisplayFlips", counts);
    }
    IOLockUnlock(gspLock_);
    keep->release();
    return r;
}

// ---- The hardware cursor (nv_pipe_copy.h) ----------------------------------------------------------
//
// Head 0's cursor (nvdisplay 3: NVKMS EvoSetCursorImageC3, MoveCursorC3; nouveau headc37d_curs_set): a 256x256
// A8R8G8B8 surface in VRAM through the core channel's VRAM context DMA, blended without premultiplication
// (convertCursorImage hands straight alpha), positioned through the cursor channel's PIO area at BAR0 0x6d8000
// (NV_UDISP_FE_CHN_ASSY_BASEADR_CURS(0)). The position is the image's top-left in head coordinates; IOFramebuffer
// has already taken the hot spot off.

static const uint32_t kCursUser = 0x6d8000, kCursMax = 256;

void NVBringup::cursorLive(bool live)
{
    if (!cursLock_)
        return;
    IOSimpleLockLock(cursLock_);
    cursLive_ = live;
    if (live && gsp_) {
        cursHideX_ = (int32_t)gsp_->disp.w;
        cursHideY_ = (int32_t)gsp_->disp.h;
    }
    IOSimpleLockUnlock(cursLock_);
}

void NVBringup::cursorEnableLocked()
{
    GspState *g = gsp_;
    if (!g || !g->disp.ready || !g->disp.litId)
        return;
    GspState::DispHw &d = g->disp;
    if (!d.cursChan || !d.curs.cpu || !d.cursHave || d.cursOn)
        return;
    dispMthd(false, 0x2088, kDispHandleVram);                                // CONTEXT_DMA_CURSOR(0, 0)
    dispMthd(false, 0x2090, (uint32_t)(d.curs.pa >> 8));                     // OFFSET_CURSOR(0, 0)
    dispMthd(false, 0x2098, 0);                                              // PRESENT_CONTROL_CURSOR: MONO
    dispMthd(false, 0x209c, 1u << 31 | 3u << 8 | 0xcf);                      // CONTROL_CURSOR: on, 256x256, A8R8G8B8
    dispMthd(false, 0x20a0, 0xff | 5u << 8 | 7u << 12);                      // COMPOSITION: K1 255, K1*src over (1-K1*src)
    dispCoreUpdate(0);
    d.cursOn = true;
    cursorLive(true);
    static uint32_t logged;
    if (logged++ < 4)
        LOG("GSP: display: hardware cursor on (surface 0x%llx)", (unsigned long long)d.curs.pa);
}

// NV_CURSOR_IMAGE_FN: width x height straight-alpha ARGB pixels, packed; the rest of the 256x256 is transparent.
IOReturn NVBringup::cursorImage(const uint32_t *argb, uint32_t width, uint32_t height)
{
    if (!argb || !width || !height || width > kCursMax || height > kCursMax || !gspLock_)
        return kIOReturnBadArgument;
    IOLockLock(gspLock_);
    GspState *g = gsp_;
    if (!g || !g->booted || !g->disp.ready || !g->disp.cursChan || !g->disp.curs.cpu) {
        IOLockUnlock(gspLock_);
        return kIOReturnNotReady;
    }
    GspState::DispHw &d = g->disp;
    for (uint32_t y = 0; y < kCursMax; y++)
        for (uint32_t x = 0; x < kCursMax; x++)
            dispW32(d.curs, (y * kCursMax + x) * 4, x < width && y < height ? argb[y * width + x] : 0);
    bar1Flush();
    d.cursHave = true;
    cursorEnableLocked();               // once per modeset; later images replace the pixels in place
    IOLockUnlock(gspLock_);
    return kIOReturnSuccess;
}

// NV_CURSOR_MOVE_FN: no gspLock_, no sleeping. A hidden cursor is moved off the head.
IOReturn NVBringup::cursorMove(int32_t x, int32_t y, bool visible)
{
    if (!cursLock_)
        return kIOReturnNotReady;
    IOSimpleLockLock(cursLock_);
    if (!cursLive_) {
        IOSimpleLockUnlock(cursLock_);
        return kIOReturnNotReady;
    }
    if (!visible) {
        x = cursHideX_;
        y = cursHideY_;
    }
    x = x < -(int32_t)kCursMax ? -(int32_t)kCursMax : x > 0x7fff ? 0x7fff : x;
    y = y < -(int32_t)kCursMax ? -(int32_t)kCursMax : y > 0x7fff ? 0x7fff : y;
    bool room = false;
    for (int i = 0; i < 100 && !(room = (rd32(kCursUser + 0x008) & 0x3f) >= 2); i++)     // FREE count
        IODelay(1);
    if (room) {
        wr32(kCursUser + 0x208, (uint32_t)(y & 0xffff) << 16 | (uint32_t)(x & 0xffff));   // HOT_SPOT_POINT_OUT(0)
        wr32(kCursUser + 0x200, 0);                                                     // UPDATE
    }
    IOSimpleLockUnlock(cursLock_);
    return room ? kIOReturnSuccess : kIOReturnBusy;
}

// ---- The display pipe's frame copy (nv_pipe_copy.h) -----------------------------------------------
//
// NVMetalAccel's NVDisplayPipe calls this for every transaction: the copy engine copies plane 0 of WindowServer's
// frame (system memory) into the lit scanout surface (VRAM) on the utility channel, instead of a CPU copy through
// the BAR1 aperture inside WindowServer's submit (~6 ms a 1080p frame, and CoreDisplay marks every frame wholly
// dirty). The source is mapped into the kernel VA space once per surface (read-only, coherent system memory) and
// kept, with the surface retained and its pages wired, until another surface needs the slot or GSP-RM goes away.

IOReturn NVBringup::callPlatformFunction(const OSSymbol *functionName, bool waitForFunction,
                                         void *param1, void *param2, void *param3, void *param4)
{
    if (functionName && functionName->isEqualTo(NV_PIPE_COPY_FN))
        return pipeCopy((OSObject *)param1, (IOMemoryDescriptor *)param2, (nv_pipe_copy *)param3);
    if (functionName && functionName->isEqualTo(NV_VBLANK_LINES_FN)) {
        if (param1)
            *(uint32_t *)param1 = dispLinesToVblank();
        return kIOReturnSuccess;
    }
    if (functionName && functionName->isEqualTo(NV_CURSOR_IMAGE_FN))
        return cursorImage((const uint32_t *)param1, (uint32_t)(uintptr_t)param2, (uint32_t)(uintptr_t)param3);
    if (functionName && functionName->isEqualTo(NV_CURSOR_MOVE_FN))
        return cursorMove((int32_t)(intptr_t)param1, (int32_t)(intptr_t)param2, param3 != nullptr);
    if (functionName && functionName->isEqualTo(NV_DISPLAY_POWER_FN))
        return dispPower(param1 != nullptr);
    return IOService::callPlatformFunction(functionName, waitForFunction, param1, param2, param3, param4);
}

IOReturn NVBringup::setProperties(OSObject *properties)
{
    OSDictionary *d = OSDynamicCast(OSDictionary, properties);
    OSBoolean *b = d ? OSDynamicCast(OSBoolean, d->getObject("NVPipeFlip")) : nullptr;
    if (!b)
        return kIOReturnUnsupported;
    pipeFlipOff_ = !b->getValue();
    setProperty("NVPipeFlip", b->getValue());
    LOG("GSP: display pipe: flips %s", b->getValue() ? "on" : "off");
    return kIOReturnSuccess;
}

void NVBringup::pipeSrcDrop(uint32_t i, bool unmap)
{
    PipeSrc &s = pipeSrc_[i];
    if (unmap && s.npages && gsp_ && gsp_->pd3) {
        const nv_mmu_ops ops = { this, mmuAlloc, mmuRd64, mmuWr64, nullptr, false };
        nv_mmu_unmap_4k(&ops, gsp_->pd3, kPipeVa + i * kPipeVaSlot, s.npages * PAGE_4K);
        praminRestore();
        tlbFlush(gsp_->pd3);
    }
    if (s.dma) {
        s.dma->clearMemoryDescriptor(true);
        s.dma->release();
    }
    if (s.pages)
        IOFree(s.pages, s.npages * sizeof(uint64_t));
    if (s.md) {
        s.md->complete(kIODirectionInOut);
        s.md->release();
    }
    OSSafeReleaseNULL(s.key);
    s = PipeSrc();
}

// unmap false: the VA space is going away with GSP-RM (freeGsp), only our references are dropped.
void NVBringup::pipeSrcFlush(bool unmap)
{
    for (uint32_t i = 0; i < kPipeSrcs; i++)
        if (pipeSrc_[i].key)
            pipeSrcDrop(i, unmap);
}

int NVBringup::pipeSrcMap(OSObject *key, IOMemoryDescriptor *md)
{
    GspState *g = gsp_;
    uint32_t slot = 0;
    for (uint32_t i = 0; i < kPipeSrcs; i++) {
        if (pipeSrc_[i].key == key && pipeSrc_[i].md == md) {
            pipeSrc_[i].lastUse = ++pipeUse_;
            return (int)i;
        }
        if (pipeSrc_[i].lastUse < pipeSrc_[slot].lastUse)
            slot = i;
    }
    pipeSrcDrop(slot, true);                        // the least recently used, or an empty slot

    const uint64_t size = (md->getLength() + PAGE_4K - 1) & ~(uint64_t)(PAGE_4K - 1);
    if (!size || size > kPipeVaSlot)
        return -1;
    PipeSrc &s = pipeSrc_[slot];
    // InOut though only read: xnu marks a descriptor wired for kIODirectionOut alone 'prepared read only' and,
    // while it stays wired, refuses every later prepare that may write (kIOReturnNotWritable). The source is
    // WindowServer's IOSurface, which the Intel GPU wires InOut to scan it out in mirror mode: held here
    // read-only, the laptop panel flashed black ('IOAccelSysMemory::wire(): memory failed to prepare', 2026-10-12)
    if (md->prepare(kIODirectionInOut) != kIOReturnSuccess)
        return -1;
    md->retain();
    s.md = md;
    key->retain();
    s.key = key;
    s.npages = size / PAGE_4K;
    s.pages = (uint64_t *)IOMalloc(s.npages * sizeof(uint64_t));
    s.dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0, IODMACommand::kMapped, 0, PAGE_4K);
    if (!s.pages || !s.dma || s.dma->setMemoryDescriptor(md, true) != kIOReturnSuccess) {
        pipeSrcDrop(slot, false);
        return -1;
    }
    uint64_t off = 0, n = 0;
    while (off < md->getLength() && n < s.npages) {
        IODMACommand::Segment64 seg[16];
        UInt32 nseg = 16;
        if (s.dma->gen64IOVMSegments(&off, seg, &nseg) != kIOReturnSuccess || !nseg)
            break;
        for (UInt32 k = 0; k < nseg; k++) {
            if (seg[k].fIOVMAddr & (PAGE_4K - 1)) {     // a source not page-aligned: not ours to map
                n = 0;
                off = ~0ull;
                break;
            }
            for (uint64_t a = 0; a < seg[k].fLength && n < s.npages; a += PAGE_4K)
                s.pages[n++] = seg[k].fIOVMAddr + a;
        }
    }
    const nv_mmu_ops ops = { this, mmuAlloc, mmuRd64, mmuWr64, nullptr, false };
    const nv_mmu_target t = { true, 0, s.pages, 0, NV_MMU_PTE_READ_ONLY, 0 };
    bool ok = n == s.npages && nv_mmu_map(&ops, g->pd3, kPipeVa + slot * kPipeVaSlot, size, &t);
    praminRestore();
    if (!ok) {
        LOG("GSP: display pipe: mapping a %llu KiB source failed (%llu of %llu pages)",
            (unsigned long long)(size >> 10), (unsigned long long)n, (unsigned long long)s.npages);
        pipeSrcDrop(slot, false);
        return -1;
    }
    tlbFlush(g->pd3);
    s.lastUse = ++pipeUse_;
    static uint32_t logged;
    if (logged++ < 8)
        LOG("GSP: display pipe: source %p mapped at VA 0x%llx (%llu KiB)", key,
            (unsigned long long)(kPipeVa + slot * kPipeVaSlot), (unsigned long long)(size >> 10));
    return (int)slot;
}

// The per-frame timeline of the pipe (see GspState::DispHw): accumulates, published every 120 flips as
// NVPipeTiming ("us mean/max"). Two mach_absolute_time reads per flip; the property is built once per 120.
template <class D>
static bool pipeTiming(D &dd, uint64_t tSubmit, uint64_t tCopied, uint64_t showAt, char *m, size_t cap)
{
    const uint64_t tNow = mach_absolute_time();
    auto us = [](uint64_t abs) { uint64_t ns = 0; absolutetime_to_nanoseconds(abs, &ns); return (uint32_t)(ns / 1000); };
    const uint32_t copy = us(tCopied - tSubmit), queue = us(tNow - tCopied), lead = us(showAt > tNow ? showAt - tNow : 0);
    dd.tmN++;
    dd.tmCopySum += copy;
    dd.tmQueueSum += queue;
    dd.tmLeadSum += lead;
    if (copy > dd.tmCopyMax)
        dd.tmCopyMax = copy;
    if (queue > dd.tmQueueMax)
        dd.tmQueueMax = queue;
    if (lead > dd.tmLeadMax)
        dd.tmLeadMax = lead;
    if (copy > 2500)
        dd.tmSlow++;
    if (dd.tmLastFlip) {
        const uint32_t gap = us(tSubmit - dd.tmLastFlip);
        dd.tmGapN++;
        dd.tmGapSum += gap;
        if (gap > dd.tmGapMax)
            dd.tmGapMax = gap;
    }
    dd.tmLastFlip = tNow;
    if (dd.tmN >= 120) {
        snprintf(m, cap, "%llu flips: copy %llu/%u us, queue %llu/%u us, latch in %llu/%u us, gap %llu/%u us, %llu slow",
                 (unsigned long long)dd.tmN, (unsigned long long)(dd.tmCopySum / dd.tmN), dd.tmCopyMax,
                 (unsigned long long)(dd.tmQueueSum / dd.tmN), dd.tmQueueMax,
                 (unsigned long long)(dd.tmLeadSum / dd.tmN), dd.tmLeadMax,
                 (unsigned long long)(dd.tmGapN ? dd.tmGapSum / dd.tmGapN : 0), dd.tmGapMax,
                 (unsigned long long)dd.tmSlow);
        dd.tmN = dd.tmCopySum = dd.tmQueueSum = dd.tmLeadSum = dd.tmGapSum = dd.tmGapN = dd.tmSlow = 0;
        dd.tmCopyMax = dd.tmQueueMax = dd.tmLeadMax = dd.tmGapMax = 0;
        return true;
    }
    return false;
}

IOReturn NVBringup::pipeCopy(OSObject *key, IOMemoryDescriptor *md, nv_pipe_copy *a)
{
    if (!key || !md || !a || a->version != NV_PIPE_COPY_VERSION || a->x1 <= a->x0 || a->y1 <= a->y0 ||
        a->srcPitch < a->x1 * 4)
        return kIOReturnBadArgument;
    if ((uint64_t)a->srcPitch * (a->y1 - 1) + (uint64_t)a->x1 * 4 > md->getLength())
        return kIOReturnBadArgument;
    if (!gspLock_)                              // stopping
        return kIOReturnNotReady;
    IOLockLock(gspLock_);
    GspState *g = gsp_;
    IOReturn r = kIOReturnNotReady;
    if (!g || !g->booted || !g->utilOk || !g->pd3 || !g->dispLit || !g->disp.ready) {
        IOLockUnlock(gspLock_);
        return r;
    }
    GspState::DispHw &d = g->disp;
    // Presented with a flip at the vblank (window 0, non-tearing, flips queue in order): the copy goes into a
    // buffer that is neither the one window 0 was last flipped to nor one still on screen or queued (pipeFreeAt).
    // With three buffers one is free unless two flips are already queued, and then this waits for the next one
    // to be shown (at most a frame). Writing into a queued buffer showed frames out of order (2026-10-12).
    const uint64_t need = (uint64_t)d.pitch * (d.h - 1) + (uint64_t)d.w * 4;
    const bool flips = d.fbB.pa && d.fbC.pa && d.fbD.pa && d.fbB.size >= need && d.fbC.size >= need &&
                       d.fbD.size >= need && d.fb.size >= need && !pipeFlipOff_ &&
                       d.vtotal && d.pclkKhz && d.htotal;
    if (!d.fb.pa || a->dstWidth != d.w || a->dstHeight != d.h || a->dstPitch != d.pitch ||
        a->x1 > d.w || a->y1 > d.h || (uint64_t)d.pitch * (a->y1 - 1) + (uint64_t)a->x1 * 4 > d.fb.size) {
        IOLockUnlock(gspLock_);
        return r;
    }
    auto pickFree = [](GspState::DispHw &h, uint64_t now, uint64_t *soonest) -> int {
        int best = -1;
        *soonest = ~0ull;
        for (uint32_t b = 0; b < 3; b++) {
            if (b == h.pipeArmed)
                continue;
            if (now >= h.pipeFreeAt[b]) {
                if (best < 0 || h.pipeFreeAt[b] < h.pipeFreeAt[best])
                    best = (int)b;
            } else if (h.pipeFreeAt[b] < *soonest) {
                *soonest = h.pipeFreeAt[b];
            }
        }
        return best;
    };
    uint64_t waitedAbs = 0, soonest = 0;
    int target = 0;
    if (flips && (target = pickFree(d, mach_absolute_time(), &soonest)) < 0) {
        const uint64_t w0 = mach_absolute_time();
        IOLockUnlock(gspLock_);
        for (int i = 0; i < 40 && mach_absolute_time() < soonest; i++)  // at most a frame; 40 ms if the clock is off
            IOSleep(1);
        waitedAbs = mach_absolute_time() - w0;
        IOLockLock(gspLock_);
        g = gsp_;
        // A mode set while unlocked may have changed the geometry or reallocated the buffers: copying with the
        // checks above would write past them. Refused (the caller copies itself); the next frame is checked anew
        if (!g || !g->booted || !g->utilOk || !g->pd3 || !g->dispLit || !g->disp.ready ||
            g->disp.w != a->dstWidth || g->disp.h != a->dstHeight || g->disp.pitch != a->dstPitch ||
            !g->disp.fb.pa || g->disp.fb.size < need || g->disp.fbB.size < need || g->disp.fbC.size < need ||
            g->disp.fbD.size < need || pipeFlipOff_) {
            IOLockUnlock(gspLock_);
            return r;
        }
        if ((target = pickFree(g->disp, ~0ull, &soonest)) < 0)          // the clock was off: take the oldest
            target = (int)((g->disp.pipeArmed + 1) % 3);       // pipeArmed < 3 here: two were in use
    }
    GspState::DispHw &dd = g->disp;
    NVDispMem *const now[3] = { &dd.fbB, &dd.fbC, &dd.fbD };    // g may be a new GspState after the wait
    const NVDispMem &f = flips ? *now[target] : dd.fb;
    // A flip shows the whole buffer, which holds an older frame: the whole frame is copied, not the dirty rect.
    const uint32_t x0 = flips ? 0 : a->x0, y0 = flips ? 0 : a->y0;
    const int slot = pipeSrcMap(key, md);
    if (slot < 0) {
        IOLockUnlock(gspLock_);
        return kIOReturnNoMemory;
    }
    const uint64_t in = kPipeVa + (uint64_t)slot * kPipeVaSlot + (uint64_t)y0 * a->srcPitch + (uint64_t)x0 * 4;
    const uint64_t out = f.pa + (uint64_t)y0 * dd.pitch + (uint64_t)x0 * 4;
    const uint32_t subch = 4, payload = 0x5d000000u | (++g->utilSeq & 0xffffff);
    TestChan &c = g->util;
    const uint32_t push[] = {
        pbHeader(subch, 0x000, 1), chip_->arch->cls.copy,                              // SET_OBJECT
        pbHeader(subch, NVC5B5_SET_DST_PHYS_MODE_, 1), 0,                              // LOCAL_FB
        pbHeader(subch, 0x400, 4), (uint32_t)(in >> 32), (uint32_t)in, (uint32_t)(out >> 32), (uint32_t)out,
        pbHeader(subch, 0x410, 4), a->srcPitch, dd.pitch, (a->x1 - x0) * 4, a->y1 - y0,  // PITCH_IN/OUT,
        pbHeader(subch, 0x240, 3), (uint32_t)(c.vaSem >> 32), (uint32_t)c.vaSem, payload, // LINE_LENGTH, COUNT
        // LAUNCH_DMA: non-pipelined, flush, release one word, pitch -> pitch, multi-line, virtual source,
        // physical destination
        pbHeader(subch, 0x300, 1), 2u | 1u << 2 | 1u << 3 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 13,
    };
    // Flipping, the aperture (fb) gets the frame too, VRAM to VRAM after the first copy: in mirror mode the other
    // display's picture is read from it, and holding every third frame it flickered (2026-10-12).
    const uint64_t vin = f.pa, vout = dd.fb.pa;
    const uint32_t payload2 = 0x5d000000u | (++g->utilSeq & 0xffffff);
    const uint32_t push2[] = {
        pbHeader(subch, 0x000, 1), chip_->arch->cls.copy,
        pbHeader(subch, 0x260, 1), 0,                                                  // SET_SRC_PHYS_MODE: LOCAL_FB
        pbHeader(subch, NVC5B5_SET_DST_PHYS_MODE_, 1), 0,
        pbHeader(subch, 0x400, 4), (uint32_t)(vin >> 32), (uint32_t)vin, (uint32_t)(vout >> 32), (uint32_t)vout,
        pbHeader(subch, 0x410, 4), dd.pitch, dd.pitch, (a->x1 - x0) * 4, a->y1 - y0,
        pbHeader(subch, 0x240, 3), (uint32_t)(c.vaSem >> 32), (uint32_t)c.vaSem, payload2,
        // physical source and destination
        pbHeader(subch, 0x300, 1), 2u | 1u << 2 | 1u << 3 | 1u << 7 | 1u << 8 | 1u << 9 | 1u << 12 | 1u << 13,
    };
    // One pushbuffer and one wait for both copies: they run in order on the same channel (non-pipelined, flushed),
    // and the second reads what the first wrote. Its semaphore value is the one waited for; the first's stays
    // readable for the diagnosis below. The copy takes 1-2 ms: spin through it rather than sleep a tick or two.
    static const uint32_t kPipeSpinUs = 2500;
    uint32_t both[sizeof(push) / 4 + sizeof(push2) / 4];
    memcpy(both, push, sizeof(push));
    memcpy(both + sizeof(push) / 4, push2, sizeof(push2));
    const uint64_t t0 = mach_absolute_time();
    bool ok = flips ? submitAndWait(c, both, sizeof(both) / 4, payload2, 100, false, kPipeSpinUs)
                    : submitAndWait(c, push, sizeof(push) / 4, payload, 100, false, kPipeSpinUs);
    if (!ok && flips) {                         // the first copy done and the aperture copy not: still a flip
        const uint32_t sem = praminRd32(c.sem);
        praminRestore();
        if (sem == payload) {
            ok = true;
            LOG("GSP: display pipe: copy into the aperture did not complete");
        }
    }
    const uint64_t t1 = mach_absolute_time();
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(t1 - t0, &ns);
    a->gpuUs = (uint32_t)(ns / 1000);
    if (!ok)                                    // what the slot held is suspect: map it again next time
        pipeSrcDrop((uint32_t)slot, true);
    if (ok && flips && (uint32_t)target != dd.pipeArmed) {
        if (dd.pipeArmed == ~0u)                    // window 0 still shows fb, which isn't one of the three
            dd.pipeArmed = 3;
        dispMthd(true, 0x260, (uint32_t)(f.pa >> 8));                     // window 0, offset 0: the buffer
        dispMthd(true, 0x370, 0);                                         // not interlocked with the core
        dispMthd(true, 0x374, 0);
        dispMthd(true, 0x200, 1);                                         // UPDATE: latched at the next vblank
        uint32_t lines = dispLinesToVblank();
        if (!lines)
            lines = dd.vtotal;
        // Shown at the next vblank (two lines' margin), or a frame after the flip before it if that one is still
        // queued; the buffer before is on screen until then.
        uint64_t showAt = mach_absolute_time() + dispLinesAbs(lines + 2);
        const uint64_t frame = dispLinesAbs(dd.vtotal);
        if (dd.pipeShowAt && showAt < dd.pipeShowAt + frame)
            showAt = dd.pipeShowAt + frame;
        if (dd.pipeArmed < 3)
            dd.pipeFreeAt[dd.pipeArmed] = showAt;
        dd.pipeShowAt = showAt;
        char tm[160];
        if (pipeTiming(dd, t0, t1, showAt, tm, sizeof(tm)))
            setProperty("NVPipeTiming", tm);
        dd.pipeArmed = (uint32_t)target;
        dd.front = (uint32_t)target;
        dd.pipeFlips++;
        if (waitedAbs) {
            uint64_t wns = 0;
            absolutetime_to_nanoseconds(waitedAbs, &wns);
            dd.pipeWaits++;
            dd.pipeWaitUs += wns / 1000;
        }
        if (dd.pipeFlips % 120 == 1) {
            char m[96];
            snprintf(m, sizeof(m), "%llu flips; %llu waited for a free buffer, %llu us in all",
                     (unsigned long long)dd.pipeFlips, (unsigned long long)dd.pipeWaits,
                     (unsigned long long)dd.pipeWaitUs);
            setProperty("NVPipeFlips", m);
        }
    }
    if (ok && !flips && dd.pipeArmed != ~0u) {         // flips switched off: window 0 back to fb, which has the frame
        dispMthd(true, 0x260, (uint32_t)(dd.fb.pa >> 8));
        dispMthd(true, 0x370, 0);
        dispMthd(true, 0x374, 0);
        dispMthd(true, 0x200, 1);
        dd.pipeArmed = ~0u;
        dd.pipeShowAt = 0;
        dd.pipeFreeAt[0] = dd.pipeFreeAt[1] = dd.pipeFreeAt[2] = mach_absolute_time() + dispLinesAbs(2 * dd.vtotal);
    }
    IOLockUnlock(gspLock_);
    return ok ? kIOReturnSuccess : kIOReturnTimeout;
}

// Head 0's raster: lines until the next vblank starts (the line after blank start, vbS), a whole frame when it
// starts now; 0 when the display isn't lit. The raster line is 0x616330 (gv100_head_rgpos). No lock: one read.
uint32_t NVBringup::dispLinesToVblank()
{
    GspState *g = gsp_;
    if (!g || !g->dispLit || !g->disp.ready || !g->disp.vtotal)
        return 0;
    const uint32_t vt = g->disp.vtotal, line = rd32(0x616330) & 0xffff;
    if (line >= vt)
        return 0;
    const uint32_t n = (g->disp.vbS + 1 + vt - line) % vt;
    return n ? n : vt;
}

uint64_t NVBringup::dispLinesAbs(uint32_t lines)
{
    GspState *g = gsp_;
    uint64_t abs = 0;
    if (g && g->disp.pclkKhz)
        nanoseconds_to_absolutetime((uint64_t)lines * g->disp.htotal * 1000000ull / g->disp.pclkKhz, &abs);
    return abs;
}

// GSP-RM is going down (sleep, idle, unload): the display engine with it. The kernel mappings go; the
// VRAM goes with the heap. User space sees the display gone.
void NVBringup::dispLost()
{
    GspState *g = gsp_;
    if (!g || !g->disp.ready)
        return;
    GspState::DispHw &d = g->disp;
    cursorLive(false);
    NVDispMem *mems[] = { &d.inst, &d.pbCore, &d.pbWndw, &d.sync, &d.ilut, &d.olut, &d.fb, &d.fbB, &d.fbC, &d.fbD, &d.curs };
    for (NVDispMem *m : mems)
        OSSafeReleaseNULL(m->map);
    // The scanout surfaces are in the VRAM heap, which is rebuilt at the next boot: forgotten here, so that
    // dispInit and dispSetMode allocate them again instead of writing through the released mappings.
    NVDispMem *surfaces[] = { &d.fb, &d.fbB, &d.fbC, &d.fbD, &d.curs };
    for (NVDispMem *m : surfaces)
        *m = NVDispMem();
    d.cursChan = d.cursOn = d.cursHave = false;     // the channel goes with the client
    d.hdmiOn = d.rangeLimited = d.dpmsOff = d.dpmsBySor = false;   // GSP-RM and the display engine start over
    d.hpdEvent = d.hpdBurst = 0;                    // the event object goes with the client
    d.ready = false;
    d.litId = 0;
    g->dispLit = false;
    dispPublish();
    setProperty("NVDisplayModeset", "GSP-RM unloaded");
}

// Item 4 (non-stall interrupts), step 1: read-only probe. Asks GSP-RM which interrupt vectors the
// CPU services, keeps GR0's and the copy engines' non-stall vectors for intrHwOn, and logs the CPU
// interrupt tree as GSP-RM left it (and NVDEC0's, where video contexts run). No register writes.
void NVBringup::probeIntr()
{
    GspState *g = gsp_;
    uint32_t st = 0;
    memset(intrLeaf_, 0, sizeof(intrLeaf_));
    memset(intrTop_, 0, sizeof(intrTop_));
    if (!g->hIntClient || !g->hIntSubdevice)
        return;
    uint8_t *p = (uint8_t *)IOMallocZero(NV2080_INTR_TABLE_SIZE);
    if (!p)
        return;
    if (!gspRmControl(g->hIntClient, g->hIntSubdevice, NV2080_CTRL_CMD_INTERNAL_INTR_GET_KERNEL_TABLE_,
                      p, NV2080_INTR_TABLE_SIZE, &st)) {
        LOG("intr probe: INTR_GET_KERNEL_TABLE failed (status 0x%x)", st);
        setProperty("NVIntrProbe", "table query failed");
        IOFree(p, NV2080_INTR_TABLE_SIZE);
        return;
    }
    uint32_t n = 0;
    memcpy(&n, p, 4);
    if (n > NV2080_INTR_TABLE_MAX)
        n = NV2080_INTR_TABLE_MAX;
    LOG("intr probe: kernel table has %u entries (engine: pmc mask, stall vector, non-stall vector)", n);
    uint32_t grNs = NV_INTR_VECTOR_INVALID, decNs = NV_INTR_VECTOR_INVALID, ceNs[10], nCe = 0;
    char line[256];
    uint32_t len = 0;
    for (uint32_t k = 0; k < n; k++) {
        const uint8_t *e = p + 4 + k * NV2080_INTR_TABLE_ENTRY_SIZE;
        uint16_t eng;
        uint32_t pmc, vs, vn;
        memcpy(&eng, e, 2);
        memcpy(&pmc, e + 4, 4);
        memcpy(&vs, e + 8, 4);
        memcpy(&vn, e + 12, 4);
        if (eng == MC_ENGINE_IDX_GR0_)
            grNs = vn;
        if (eng == MC_ENGINE_IDX_NVDEC0_)
            decNs = vn;
        if (eng >= MC_ENGINE_IDX_CE0_ && eng < MC_ENGINE_IDX_CE0_ + 10 && vn != NV_INTR_VECTOR_INVALID && nCe < 10)
            ceNs[nCe++] = (uint32_t)(eng - MC_ENGINE_IDX_CE0_) << 16 | vn;
        // GR0, the copy engines and NVDEC0 are where our channels run: their non-stall vectors are
        // ours (without NVDEC0's, waits on video contexts fell back to the 2 ms deadline).
        bool ours = eng == MC_ENGINE_IDX_GR0_ || eng == MC_ENGINE_IDX_NVDEC0_ ||
                    (eng >= MC_ENGINE_IDX_CE0_ && eng < MC_ENGINE_IDX_CE0_ + 10);
        if (ours && vn != NV_INTR_VECTOR_INVALID && vn < 8 * 32) {
            intrLeaf_[vn / 32] |= 1u << (vn % 32);
            intrTop_[vn / 64 / 32] |= 1u << (vn / 64 % 32);
        }
        len += (uint32_t)snprintf(line + len, sizeof(line) - len, "  %u: %x %d %d", eng, pmc,
                        vs == NV_INTR_VECTOR_INVALID ? -1 : (int)vs, vn == NV_INTR_VECTOR_INVALID ? -1 : (int)vn);
        if ((k & 3) == 3 || k + 1 == n) {
            LOG("intr probe:%s", line);
            len = 0;
        }
    }
    len = 0;
    for (uint32_t c = 0; c < NV2080_INTR_CATEGORY_COUNT; c++) {
        const uint8_t *m = p + NV2080_INTR_TABLE_SUBTREE_OFF + c * 2;
        len += (uint32_t)snprintf(line + len, sizeof(line) - len, " %u:%d-%d", c, m[0] == 0xff ? -1 : m[0], m[1] == 0xff ? -1 : m[1]);
    }
    LOG("intr probe: subtree map per category:%s", line);
    IOFree(p, NV2080_INTR_TABLE_SIZE);

    len = (uint32_t)snprintf(line, sizeof(line), "GR0 %d", grNs == NV_INTR_VECTOR_INVALID ? -1 : (int)grNs);
    for (uint32_t c = 0; c < nCe; c++)
        len += (uint32_t)snprintf(line + len, sizeof(line) - len, ", CE%u %u", ceNs[c] >> 16, ceNs[c] & 0xffff);
    if (decNs != NV_INTR_VECTOR_INVALID)
        len += (uint32_t)snprintf(line + len, sizeof(line) - len, ", NVDEC0 %u", decNs);
    LOG("intr probe: non-stall vectors: %s", line);
    setProperty("NVIntrNonStall", line);

    LOG("intr probe: TOP %08x %08x  TOP_EN %08x %08x", rd32(NV_CPU_INTR_TOP(0)), rd32(NV_CPU_INTR_TOP(1)),
        rd32(NV_CPU_INTR_TOP_EN_SET(0)), rd32(NV_CPU_INTR_TOP_EN_SET(1)));
    len = 0;
    for (uint32_t i = 0; i < NV_CPU_INTR_LEAF_COUNT; i++)
        len += (uint32_t)snprintf(line + len, sizeof(line) - len, " %u:%08x/%08x", i, rd32(NV_CPU_INTR_LEAF(i)),
                        rd32(NV_CPU_INTR_LEAF_EN_SET(i)));
    LOG("intr probe: LEAF pending/enabled:%s", line);
    uint16_t cmd = pci_->configRead16(kIOPCIConfigCommand);
    uint8_t msiCap = 0;                         // returns the capability register; offset via the pointer
    uint16_t msiCtl = pci_->findPCICapability(kIOPCICapabilityIDMSI, &msiCap) ? pci_->configRead16(msiCap + 2u) : 0;
    LOG("intr probe: PCI command 0x%04x (INTx %s), MSI cap at 0x%x, control 0x%04x (%s, %u vectors capable)",
        cmd, (cmd & 0x400) ? "disabled" : "enabled", msiCap, msiCtl, (msiCtl & 1) ? "enabled" : "disabled",
        1u << ((msiCtl >> 1) & 7));
    setProperty("NVIntrProbe", grNs != NV_INTR_VECTOR_INVALID ? "ok" : "no GR non-stall vector");
}

// ---- Item 4: non-stall interrupts ---------------------------------------------------------
//
// On after each GSP-RM boot unless boot-arg nvintr=0; `sudo nvgsp intr on|off` switches at runtime. On: stale pending bits of our vectors are cleared,
// their LEAF_EN bits and subtree TOP_EN bits set, and the MSI source enabled. EXEC then ends
// each submission with NON_STALL_INTERRUPT, the engine's non-stall vector fires, and the filter
// (primary interrupt context: register access only) clears our leaf bits and re-arms the MSI
// through the PCI config mirror (Turing's MSI EOI; without it only the first MSI arrives). The action (own work loop) wakes SYNC_WAIT. A storm (more than
// kIntrStormPerSec) turns TOP off and SYNC_WAIT goes back to polling. Off before any GSP-RM
// unload (sleep, restart, nvgsp unload) and on again after the next boot if still wanted.

static const uint32_t kIntrStormPerSec = 100000;

void NVBringup::wakeWaiters()
{
    IOLockLock(waitLock_);
    waitGen_++;
    IOLockWakeup(waitLock_, (event_t)&waitGen_, false);
    IOLockUnlock(waitLock_);
}

bool NVBringup::intrFilter(IOFilterInterruptEventSource *)
{
    if (!intrOn_)
        return false;
    uint32_t hit = 0;
    for (uint32_t i = 0; i < NV_CPU_INTR_LEAF_COUNT; i++)
        if (intrLeaf_[i]) {
            uint32_t p = rd32(NV_CPU_INTR_LEAF(i)) & intrLeaf_[i];
            if (p) {
                wr32(NV_CPU_INTR_LEAF(i), p);           // write 1 to clear
                hit |= p;
            }
        }
    uint64_t now = mach_absolute_time();
    if (now - intrWinStart_ > intrWinLen_) {
        intrWinStart_ = now;
        intrWinCount_ = 0;
    }
    if (++intrWinCount_ > kIntrStormPerSec) {
        intrOn_ = false;
        for (uint32_t i = 0; i < 2; i++)
            if (intrTop_[i])
                wr32(NV_CPU_INTR_TOP_EN_CLEAR(i), intrTop_[i]);
        intrStorms_++;
        return true;                                    // the action reports it
    }
    wr32(NV_XVE_MSI_REARM, 0);                          // re-arm: the next pending vector sends an MSI
    if (!hit) {
        intrSpurious_++;
        return false;
    }
    intrCount_++;
    return true;
}

void NVBringup::intrAction(IOInterruptEventSource *, int)
{
    wakeWaiters();
    if (intrStorms_ != intrStormsLogged_) {
        intrStormsLogged_ = intrStorms_;
        LOG("intr: more than %u interrupts/s: turned off at TOP; SYNC_WAIT polls again (nvgsp intr on "
            "to retry)", kIntrStormPerSec);
        setProperty("NVIntr", "off (storm)");
    }
}

bool NVBringup::intrHwOn()
{
    if (!gsp_ || !gsp_->booted || !bar0_)
        return false;
    uint32_t any = 0;
    for (uint32_t i = 0; i < NV_CPU_INTR_LEAF_COUNT; i++)
        any |= intrLeaf_[i];
    if (!any || !(intrTop_[0] | intrTop_[1])) {
        LOG("intr: no non-stall vectors from the probe; staying off");
        return false;
    }
    if (!intrES_) {
        int idx = -1;
        for (int i = 0; i < 8; i++) {
            int type = 0;
            if (pci_->getInterruptType(i, &type) != kIOReturnSuccess)
                break;
            if (type & kIOInterruptTypePCIMessaged) {
                idx = i;
                break;
            }
        }
        if (idx < 0) {
            LOG("intr: no MSI interrupt source on the device; staying off");
            return false;
        }
        intrWL_ = IOWorkLoop::workLoop();
        if (intrWL_)
            intrES_ = IOFilterInterruptEventSource::filterInterruptEventSource(this,
                OSMemberFunctionCast(IOInterruptEventSource::Action, this, &NVBringup::intrAction),
                OSMemberFunctionCast(IOFilterInterruptEventSource::Filter, this, &NVBringup::intrFilter),
                pci_, idx);
        if (!intrES_ || intrWL_->addEventSource(intrES_) != kIOReturnSuccess) {
            LOG("intr: could not set up the MSI event source; staying off");
            intrRelease();
            return false;
        }
        nanoseconds_to_absolutetime(1000000000ull, &intrWinLen_);
        LOG("intr: MSI source is interrupt index %d", idx);
    }
    for (uint32_t i = 0; i < NV_CPU_INTR_LEAF_COUNT; i++)
        if (intrLeaf_[i]) {
            wr32(NV_CPU_INTR_LEAF(i), intrLeaf_[i]);        // drop stale pending bits
            wr32(NV_CPU_INTR_LEAF_EN_SET(i), intrLeaf_[i]);
        }
    intrWinStart_ = 0;
    intrWinCount_ = 0;
    intrOn_ = true;
    if (!intrESOn_) {
        intrES_->enable();
        intrESOn_ = true;
    }
    for (uint32_t i = 0; i < 2; i++)
        if (intrTop_[i])
            wr32(NV_CPU_INTR_TOP_EN_SET(i), intrTop_[i]);
    wr32(NV_XVE_MSI_REARM, 0);                  // in case an earlier MSI was never acknowledged
    uint8_t msiCap = 0;
    uint16_t msiCtl = pci_->findPCICapability(kIOPCICapabilityIDMSI, &msiCap) ? pci_->configRead16(msiCap + 2u) : 0;
    LOG("intr: non-stall interrupts on: leaf 0 mask 0x%08x, TOP_EN 0x%08x; MSI control 0x%04x (%s)",
        intrLeaf_[0], rd32(NV_CPU_INTR_TOP_EN_SET(0)), msiCtl, (msiCtl & 1) ? "enabled" : "disabled");
    setProperty("NVIntr", "on");
    return true;
}

void NVBringup::intrHwOff()
{
    if (!intrOn_ && !intrESOn_)
        return;
    intrOn_ = false;                        // the filter stops re-arming
    if (intrES_ && intrESOn_) {
        intrES_->disable();
        intrESOn_ = false;
    }
    IODelay(20);                            // a filter already running on another CPU finishes
    if (bar0_) {
        for (uint32_t i = 0; i < 2; i++)
            if (intrTop_[i])
                wr32(NV_CPU_INTR_TOP_EN_CLEAR(i), intrTop_[i]);
        for (uint32_t i = 0; i < NV_CPU_INTR_LEAF_COUNT; i++)
            if (intrLeaf_[i])
                wr32(NV_CPU_INTR_LEAF_EN_CLEAR(i), intrLeaf_[i]);
    }
    LOG("intr: non-stall interrupts off (%u interrupts, %u spurious, %u storms so far)",
        intrCount_, intrSpurious_, intrStorms_);
    setProperty("NVIntr", intrWanted_ ? "wanted (GPU down)" : "off");
    if (waitLock_)
        wakeWaiters();                      // waiters switch back to short polling steps
}

void NVBringup::intrRelease()
{
    if (intrES_) {
        if (intrESOn_)
            intrES_->disable();
        intrESOn_ = false;
        if (intrWL_)
            intrWL_->removeEventSource(intrES_);
        OSSafeReleaseNULL(intrES_);
    }
    OSSafeReleaseNULL(intrWL_);
}

IOReturn NVBringup::setIntr(uint32_t mode, uint64_t out[4])
{
    if (!gspLock_ || !waitLock_)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    IOLockLock(gspLock_);
    if (mode == 1) {
        intrWanted_ = true;
        if (!intrOn_ && gsp_ && gsp_->booted && !intrHwOn()) {
            intrWanted_ = false;
            r = kIOReturnUnsupported;
        } else if (!intrOn_) {
            setProperty("NVIntr", "wanted (GPU down)");
        }
    } else if (mode == 0) {
        intrWanted_ = false;
        intrHwOff();
        setProperty("NVIntr", "off");
    } else if (mode != 2) {
        r = kIOReturnBadArgument;
    }
    out[0] = intrOn_;
    out[1] = intrCount_;
    out[2] = intrSpurious_;
    out[3] = intrStorms_;
    IOLockUnlock(gspLock_);
    return r;
}

// BAR1: GSP-RM's root must be the one the BAR1 block uses; we take [16 MiB, end) of the
// aperture (firmware consoles live at the start) and hang our tables under that root.
bool NVBringup::initBar1()
{
    GspState *g = gsp_;
    if (!g->bar1Pde || (g->bar1Pde & 0xfff))
        return false;
    uint32_t blk = rd32(NV_PBUS_BAR1_BLOCK);
    uint64_t inst = (uint64_t)(blk & 0x0fffffff) << 12;
    if (!(blk >> 31) || ((blk >> 28) & 3))
        return false;
    uint32_t pdbLo = praminRd32(inst + 0x200), pdbHi = praminRd32(inst + 0x204);
    praminRestore();
    uint64_t pdb = (uint64_t)(pdbLo & 0xfffff000) | ((uint64_t)pdbHi << 32);
    IODeviceMemory *bar1 = pci_->getDeviceMemoryWithIndex(1);
    uint64_t len = bar1 ? bar1->getLength() : 0;
    if (pdb != g->bar1Pde || (pdbLo & 3) || len < 0x2000000) {
        LOG("GSP: BAR1 unusable (PDB 0x%llx, root 0x%llx, aperture 0x%llx)", (unsigned long long)pdb,
            (unsigned long long)g->bar1Pde, (unsigned long long)len);
        return false;
    }
    if (!g->bar1Heap)
        g->bar1Heap = (nv_vram_heap *)IOMalloc(sizeof(nv_vram_heap));
    if (!g->bar1Heap || !nv_vram_init(g->bar1Heap, 0x1000000, len - 1))
        return false;
    g->bar1Aperture = len;
    g->bar1Phys = bar1->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);    // CPU physical, not the mapper's
    LOG("GSP: BAR1 window for CPU mappings: 0x1000000..0x%llx of %llu MiB at phys 0x%llx",
        (unsigned long long)(len - 1), (unsigned long long)(len >> 20), (unsigned long long)g->bar1Phys);

    g->bar1DummyPa = mmuAlloc(this, 0x10000);      // zeroed through PRAMIN; held slices point here
    if (!g->bar1DummyPa)
        LOG("GSP: BAR1: dummy page unavailable; held slices stay unmapped until freed");
    if (!bar1ReserveHeld()) {
        LOG("GSP: BAR1: not every held slice could be reserved; BAR1 stays off until the driver reloads");
        return false;
    }

    if (!ptPoolInit())
        LOG("GSP: page-table pool unavailable; page tables go through PRAMIN");
    return true;
}

// ---- Page-table pool ----------------------------------------------------------------------

enum : uint8_t { kPoolNew = 0, kPoolInUse = 1, kPoolFreed = 2 };   // per 4 KiB table

bool NVBringup::ptPoolInit()
{
    GspState *g = gsp_;
    const uint64_t size = 16ull << 20;              // 4096 tables: ~8 GiB of 4 KiB-page VA
    uint64_t pa = nv_vram_alloc(g->vram, size, 0x10000), va = 0;
    if (!pa)
        return false;
    if (!bar1Map(pa, size, &va)) {
        nv_vram_free(g->vram, pa);
        return false;
    }
    IODeviceMemory *bar1 = pci_->getDeviceMemoryWithIndex(1);
    IOMemoryMap *map = bar1 ? bar1->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, va, size)
                            : nullptr;
    uint64_t *shadow = (uint64_t *)IOMallocZero(size);
    uint8_t *used = (uint8_t *)IOMallocZero(size >> 12);
    if (!map || !shadow || !used) {
        OSSafeReleaseNULL(map);
        if (shadow)
            IOFree(shadow, size);
        if (used)
            IOFree(used, size >> 12);
        bar1Unmap(va, size);
        nv_vram_free(g->vram, pa);
        return false;
    }
    g->ptPoolPa = pa;
    g->ptPoolBar1 = va;
    g->ptPoolSize = size;
    g->ptPoolMap = map;
    g->ptPoolCpu = (volatile uint64_t *)map->getVirtualAddress();
    g->ptPoolShadow = shadow;
    g->ptPoolUsed = used;
    g->ptPoolNext = 0;
    LOG("GSP: page-table pool: %llu MiB of VRAM at 0x%llx, CPU-mapped via BAR1 0x%llx",
        (unsigned long long)(size >> 20), (unsigned long long)pa, (unsigned long long)va);
    return true;
}

// Before BAR1 goes away (bar1Cleanup): every connection is gone, so no table is in use.
void NVBringup::ptPoolFini()
{
    GspState *g = gsp_;
    if (!g || !g->ptPoolSize)
        return;
    g->ptPoolCpu = nullptr;                         // fast paths off first
    OSSafeReleaseNULL(g->ptPoolMap);
    IOFree(g->ptPoolShadow, g->ptPoolSize);
    IOFree(g->ptPoolUsed, g->ptPoolSize >> 12);
    g->ptPoolShadow = nullptr;
    g->ptPoolUsed = nullptr;
    bar1Unmap(g->ptPoolBar1, g->ptPoolSize);
    nv_vram_free(g->vram, g->ptPoolPa);
    g->ptPoolPa = g->ptPoolBar1 = g->ptPoolSize = 0;
}

uint64_t NVBringup::ptPoolAlloc()
{
    GspState *g = gsp_;
    if (!g || !g->ptPoolCpu)
        return 0;
    uint32_t n = (uint32_t)(g->ptPoolSize >> 12);
    for (uint32_t k = 0; k < n; k++) {
        uint32_t i = (g->ptPoolNext + k) % n;
        uint8_t st = g->ptPoolUsed[i];
        if (st == kPoolInUse)
            continue;
        g->ptPoolUsed[i] = kPoolInUse;
        g->ptPoolNext = i + 1;
        uint64_t *sh = g->ptPoolShadow + (uint64_t)i * 512;
        volatile uint64_t *hw = g->ptPoolCpu + (uint64_t)i * 512;
        for (uint32_t e = 0; e < 512; e++) {
            // never used: VRAM holds whatever was there before, clear it all; freed: the
            // shadow says which entries are still set
            if (st == kPoolNew || sh[e]) {
                sh[e] = 0;
                hw[e] = 0;
            }
        }
        return g->ptPoolPa + ((uint64_t)i << 12);
    }
    return 0;                                       // full: the caller falls back to PRAMIN
}

void NVBringup::tableFree(uint64_t addr)
{
    GspState *g = gsp_;
    if (!g)
        return;
    if (g->ptPoolUsed && addr - g->ptPoolPa < g->ptPoolSize)
        g->ptPoolUsed[(addr - g->ptPoolPa) >> 12] = kPoolFreed;
    else
        nv_vram_free(g->vram, addr);
}

// nouveau g84_bar_flush: CPU writes through BAR1 are visible in VRAM once this completes.
bool NVBringup::bar1Flush()
{
    wr32(NV_UFLUSH_FB_FLUSH, 1);
    for (int i = 0; i < 2000; i++) {
        if (!(rd32(NV_UFLUSH_FB_FLUSH) & 2))
            return true;
        IODelay(1);
    }
    return false;
}

bool NVBringup::bar1Map(uint64_t pa, uint64_t size, uint64_t *va)
{
    GspState *g = gsp_;
    if (!g->bar1Heap || g->bar1Off)
        return false;
    uint64_t v = nv_vram_alloc(g->bar1Heap, size, size >= 0x10000 ? 0x10000 : 0x1000);
    if (!v)
        return false;
    const nv_mmu_ops ops = { this, bar1MmuAlloc, mmuRd64, mmuWr64, bar1MmuLinked, false };
    nv_mmu_target t = { false, pa, nullptr, 0, 0, 0 };
    bool ok = nv_mmu_map(&ops, g->bar1Pde, v, size, &t);
    praminRestore();
    if (!ok) {
        nv_vram_free(g->bar1Heap, v);
        return false;
    }
    tlbFlush(g->bar1Pde, true);
    *va = v;
    return true;
}

void NVBringup::bar1Unmap(uint64_t va, uint64_t size)
{
    GspState *g = gsp_;
    const nv_mmu_ops ops = { this, bar1MmuAlloc, mmuRd64, mmuWr64, bar1MmuLinked, false };
    nv_mmu_unmap_4k(&ops, g->bar1Pde, va, size);
    praminRestore();
    tlbFlush(g->bar1Pde, true);
    nv_vram_free(g->bar1Heap, va);
}

// Unhooks every table we linked into GSP-RM's BAR1 root and frees them.
void NVBringup::bar1Cleanup()
{
    GspState *g = gsp_;
    ptPoolFini();
    if (!g || (!g->bar1Links.n && !g->bar1Tables.n))
        return;
    for (uint32_t i = 0; i < g->bar1Links.n; i++)
        mmuWr64(this, g->bar1Links.v[i], 0);
    praminRestore();
    bool flushed = tlbFlush(g->bar1Pde, true);
    for (uint32_t i = 0; i < g->bar1Tables.n; i++)
        nv_vram_free(g->vram, g->bar1Tables.v[i]);
    LOG("GSP: BAR1: %u links into GSP-RM's tables cleared, %u tables freed%s", g->bar1Links.n,
        g->bar1Tables.n, flushed ? "" : "; TLB invalidate did not complete");
    g->bar1Links.free();
    g->bar1Tables.free();
    if (g->bar1Heap)
        nv_vram_init(g->bar1Heap, g->bar1Heap->base, g->bar1Heap->limit);
}

// Clears the PTEs of [va, va + size) and points every 4 KiB page at the dummy page, read-only
// (every held slice shares it, so it must not carry data from one duplicate to another), in
// 64 KiB chunks to keep the number of mapping calls low. False if some page was left
// unmapped instead (no dummy, or a table couldn't be allocated). Does not flush the TLB; the
// caller does that once for however many slices it touches. Caller holds gspLock_.
static bool bar1PointAtDummy(const nv_mmu_ops &ops, uint64_t pdb, uint64_t dummyPa,
                             uint64_t va, uint64_t size)
{
    nv_mmu_unmap_4k(&ops, pdb, va, size);
    if (!dummyPa)
        return false;
    bool ok = true;
    for (uint64_t off = 0; off < size; ) {
        uint64_t chunk = size - off < 0x10000 ? size - off : 0x10000;
        ok &= nv_mmu_map_4k(&ops, pdb, va + off, dummyPa, chunk, NV_MMU_PTE_READ_ONLY);
        off += chunk;
    }
    return ok;
}

// Promises one held-slice entry and `size` held bytes to memory memMap is about to CPU-map,
// growing the list if needed, so memFree never has to allocate and a slice can't go
// untracked (and be handed out again after the next GSP-RM boot). Refused once held BAR1
// would pass half the window, so the rest stays available to every process. Caller holds
// gspLock_.
IOReturn NVBringup::bar1HoldCommit(uint64_t size)
{
    GspState *g = gsp_;
    uint64_t window = g->bar1Heap ? g->bar1Heap->limit - g->bar1Heap->base + 1 - g->ptPoolSize : 0;
    if (bar1HoldBytes_ + size > window / 2) {
        if (!bar1CapLogged_)
            LOG("GSP: BAR1: %llu MiB held by CPU mappings, half the window: refusing further CPU mappings "
                "of VRAM until the driver reloads", (unsigned long long)(bar1HoldBytes_ >> 20));
        bar1CapLogged_ = true;
        return kIOReturnNoResources;
    }
    if (bar1HeldN_ + bar1HeldRes_ >= bar1HeldCap_) {
        uint32_t nc = bar1HeldCap_ ? bar1HeldCap_ * 2 : 64;
        Bar1Held *nh = (Bar1Held *)IOMalloc(nc * sizeof(Bar1Held));
        if (!nh)
            return kIOReturnNoMemory;
        if (bar1Held_) {
            memcpy(nh, bar1Held_, bar1HeldN_ * sizeof(Bar1Held));
            IOFree(bar1Held_, bar1HeldCap_ * sizeof(Bar1Held));
        }
        bar1Held_ = nh;
        bar1HeldCap_ = nc;
    }
    bar1HeldRes_++;
    bar1HoldBytes_ += size;
    return kIOReturnSuccess;
}

void NVBringup::bar1HoldUncommit(uint64_t size)
{
    bar1HeldRes_--;
    bar1HoldBytes_ -= size;
}

// A CPU-mapped BAR1 slice is being freed (memFree): a duplicate of its mapping may still be
// live in some task the driver doesn't know about, so its range must never map real VRAM
// again. Re-points its PTEs at the dummy page (a leftover duplicate reads only zeroes and
// its writes are dropped), flushes the BAR1 TLB, and keeps the range out of bar1Heap for as
// long as the driver is loaded, except for reuse by `owner` (bar1Reuse). Caller holds gspLock_.
void NVBringup::bar1Hold(uint64_t va, uint64_t size, uint64_t owner)
{
    GspState *g = gsp_;
    const nv_mmu_ops ops = { this, bar1MmuAlloc, mmuRd64, mmuWr64, bar1MmuLinked, false };
    if (!bar1PointAtDummy(ops, g->bar1Pde, g->bar1DummyPa, va, size))
        LOG("GSP: BAR1: held slice 0x%llx (%llu bytes) partly left unmapped instead of on the dummy page",
            (unsigned long long)va, (unsigned long long)size);
    praminRestore();
    if (!tlbFlush(g->bar1Pde, true))
        LOG("GSP: BAR1: TLB invalidate did not complete holding slice 0x%llx", (unsigned long long)va);
    if (bar1HeldRes_ && bar1HeldN_ < bar1HeldCap_) {    // always: promised (bar1HoldCommit)
        bar1HeldRes_--;
        bar1Held_[bar1HeldN_++] = { va, size, owner };
    } else                                    // still allocated in bar1Heap: safe until the next boot
        LOG("GSP: BAR1: held slice 0x%llx not tracked", (unsigned long long)va);
}

// New CPU-mappable memory of connection `owner` at VRAM `pa`: takes the smallest of its held
// slices of at least `size` bytes (and at most twice that, so a big slice isn't spent on a
// small buffer) and maps its first `size` bytes to `pa`; the rest stays on the dummy page.
// A duplicate of the slice's old mapping came from this connection's process, so all it can
// see is more of that process's memory. The slice's entry and bytes stay promised: the new
// memory counts as CPU-mapped and goes back through bar1Hold. Caller holds gspLock_.
bool NVBringup::bar1Reuse(uint64_t owner, uint64_t pa, uint64_t size, uint64_t *va, uint64_t *sliceSize)
{
    GspState *g = gsp_;
    if (!g->bar1Heap || g->bar1Off)
        return false;
    uint32_t best = bar1HeldN_;
    for (uint32_t i = 0; i < bar1HeldN_; i++) {
        const Bar1Held &s = bar1Held_[i];
        if (s.owner == owner && s.size >= size && s.size / 2 <= size &&
            (best == bar1HeldN_ || s.size < bar1Held_[best].size))
            best = i;
    }
    if (best == bar1HeldN_)
        return false;
    Bar1Held s = bar1Held_[best];
    const nv_mmu_ops ops = { this, bar1MmuAlloc, mmuRd64, mmuWr64, bar1MmuLinked, false };
    nv_mmu_unmap_4k(&ops, g->bar1Pde, s.va, size);
    nv_mmu_target t = { false, pa, nullptr, 0, 0, 0 };
    bool ok = nv_mmu_map(&ops, g->bar1Pde, s.va, size, &t);
    if (!ok)                                // all-or-nothing: put the dummy back
        bar1PointAtDummy(ops, g->bar1Pde, g->bar1DummyPa, s.va, size);
    praminRestore();
    if (!tlbFlush(g->bar1Pde, true))
        LOG("GSP: BAR1: TLB invalidate did not complete reusing held slice 0x%llx", (unsigned long long)s.va);
    if (!ok)
        return false;
    bar1Held_[best] = bar1Held_[--bar1HeldN_];
    bar1HeldRes_++;
    *va = s.va;
    *sliceSize = s.size;
    return true;
}

// After a GSP-RM boot resets the BAR1 heap (bar1Cleanup): keeps every held slice's range out
// of it and re-points its PTEs at the new dummy page, before ptPoolInit can claim the same
// addresses. If any slice can't be reserved, its range is free in the heap and may be handed
// out under a stale duplicate, so BAR1 is switched off for this boot (bar1Off). Caller holds
// gspLock_.
bool NVBringup::bar1ReserveHeld()
{
    GspState *g = gsp_;
    const nv_mmu_ops ops = { this, bar1MmuAlloc, mmuRd64, mmuWr64, bar1MmuLinked, false };
    bool ok = true;
    for (uint32_t i = 0; i < bar1HeldN_; i++) {
        Bar1Held &s = bar1Held_[i];
        if (!nv_vram_reserve(g->bar1Heap, s.va, s.size)) {
            LOG("GSP: BAR1: held slice 0x%llx (%llu bytes) could not be reserved this boot",
                (unsigned long long)s.va, (unsigned long long)s.size);
            ok = false;
            continue;
        }
        if (!bar1PointAtDummy(ops, g->bar1Pde, g->bar1DummyPa, s.va, s.size))
            LOG("GSP: BAR1: held slice 0x%llx (%llu bytes) partly left unmapped instead of on the dummy page",
                (unsigned long long)s.va, (unsigned long long)s.size);
    }
    praminRestore();
    if (bar1HeldN_ && !tlbFlush(g->bar1Pde, true))
        LOG("GSP: BAR1: TLB invalidate did not complete reserving held slices");
    if (!ok)
        g->bar1Off = true;
    return ok;
}

// PD3/PD2/PD1 above GSP-RM's range, FERMI_VASPACE_A, then GSP-RM's reserved PDEs
// (Phase 4 step 4). Returns the root, or 0 (tables stay with ops' allocator).
uint64_t NVBringup::vasCreate(uint32_t hVas, const nv_mmu_ops *ops)
{
    GspState *g = gsp_;
    const uint64_t serverVa = 0x100000000ull, serverSize = 0x20000000ull;
    uint32_t st = 0;
    uint64_t pd3 = ops->alloc(ops->ctx, nv_mmu_level_bytes(NV_MMU_PD3));
    uint64_t pd2 = ops->alloc(ops->ctx, nv_mmu_level_bytes(NV_MMU_PD2));
    uint64_t pd1 = ops->alloc(ops->ctx, nv_mmu_level_bytes(NV_MMU_PD1));
    if (!pd3 || !pd2 || !pd1) {
        praminRestore();
        return 0;
    }
    ops->wr64(ops->ctx, pd3 + 8 * nv_mmu_index(serverVa, NV_MMU_PD3), nv_mmu_pde_vram(pd2));
    ops->wr64(ops->ctx, pd2 + 8 * nv_mmu_index(serverVa, NV_MMU_PD2), nv_mmu_pde_vram(pd1));
    praminRestore();

    uint8_t va[NV_VASPACE_ALLOC_SIZE] = {};
    uint64_t vaSize = 1ull << 40;
    uint32_t bigPage = 0x10000;
    memcpy(va + NV_VASPACE_vaSize, &vaSize, 8);
    memcpy(va + NV_VASPACE_bigPageSize, &bigPage, 4);
    if (!gspRmAlloc(g->hClient, g->hDevice, hVas, FERMI_VASPACE_A_CLASS, va, sizeof(va), &st))
        return 0;
    uint64_t outBase = 0, outSize = 0;
    memcpy(&outBase, va + NV_VASPACE_vaBase, 8);
    memcpy(&outSize, va + NV_VASPACE_vaSize, 8);
    if (!g->vaLimit) {
        g->vaBase = outBase;
        g->vaLimit = outBase + outSize;
    }

    uint8_t cp[NV90F1_COPY_PDES_SIZE] = {};
    uint64_t pageSize = 1ull << nv_mmu_level_shift(NV_MMU_PD1), lo = serverVa, hi = serverVa + serverSize - 1;
    uint32_t nLevels = 3;
    memcpy(cp + NV90F1_COPY_PDES_hSubDevice, &g->hSubdevice, 4);
    memcpy(cp + NV90F1_COPY_PDES_pageSize, &pageSize, 8);
    memcpy(cp + NV90F1_COPY_PDES_virtAddrLo, &lo, 8);
    memcpy(cp + NV90F1_COPY_PDES_virtAddrHi, &hi, 8);
    memcpy(cp + NV90F1_COPY_PDES_numLevelsToCopy, &nLevels, 4);
    const uint64_t lvAddr[3] = { pd3, pd2, pd1 };
    for (uint32_t i = 0; i < nLevels; i++) {
        uint8_t *l = cp + NV90F1_COPY_PDES_levels + i * NV90F1_COPY_PDES_LEVEL_SIZE;
        uint64_t sz = nv_mmu_level_bytes(NV_MMU_PD3 + (int)i);
        uint32_t aperture = 1;                      // GMMU_APERTURE_VIDEO
        memcpy(l + (NV90F1_LEVEL_physAddress - NV90F1_COPY_PDES_levels), &lvAddr[i], 8);
        memcpy(l + (NV90F1_LEVEL_size - NV90F1_COPY_PDES_levels), &sz, 8);
        memcpy(l + (NV90F1_LEVEL_aperture - NV90F1_COPY_PDES_levels), &aperture, 4);
        l[NV90F1_LEVEL_pageShift - NV90F1_COPY_PDES_levels] = (uint8_t)nv_mmu_level_shift(NV_MMU_PD3 + (int)i);
    }
    if (!gspRmControl(g->hClient, hVas, NV90F1_CTRL_CMD_VASPACE_COPY_SERVER_RESERVED_PDES_, cp, sizeof(cp), &st)) {
        gspRmFree(g->hClient, g->hDevice, hVas, &st);
        return 0;
    }
    return pd3;
}

// TURING_CHANNEL_GPFIFO_A under the device (Phase 4 step 5). kernel: admin privilege (may
// use physical addressing); otherwise user privilege, and privileged host methods denied.
bool NVBringup::rmAllocChannel(uint32_t handle, uint32_t chid, uint32_t engine, uint32_t hVas,
                               uint64_t ringVa, uint32_t ringEntries, uint64_t inst, uint64_t userd,
                               uint64_t mthd, bool kernel)
{
    GspState *g = gsp_;
    uint32_t st = 0;
    uint8_t ch[NV_CHAN_ALLOC_SIZE] = {};
    put64(ch, NV_CHAN_gpFifoOffset, ringVa);
    put32(ch, NV_CHAN_gpFifoEntries, ringEntries);
    put32(ch, NV_CHAN_flags, ((chid % 8) << 8) |      // USERD_INDEX_VALUE 10:8 (INDEX_FIXED 11 = 0)
                             ((chid / 8) << 12) |     // USERD_INDEX_PAGE_VALUE 20:12
                             (1u << 21) |             // USERD_INDEX_PAGE_FIXED
                             (kernel ? 0 : 1u << 22));   // DENY_AUTH_LEVEL_PRIV
    put32(ch, NV_CHAN_hVASpace, hVas);
    put32(ch, NV_CHAN_engineType, engine);
    putMemDesc(ch, NV_CHAN_instanceMem, inst, 0x1000);
    putMemDesc(ch, NV_CHAN_userdMem, userd, 0x200);
    putMemDesc(ch, NV_CHAN_ramfcMem, inst, 0x200);
    putMemDesc(ch, NV_CHAN_mthdbufMem, mthd, g->mthdSize);
    put32(ch, NV_CHAN_internalFlags, (kernel ? 1u : 0u) |   // PRIVILEGE ADMIN / USER
                                     (1u << 2) |            // ERROR_NOTIFIER_TYPE NONE
                                     (1u << 4));            // ECC_ERROR_NOTIFIER_TYPE NONE
    return gspRmAlloc(g->hClient, g->hDevice, handle, chip_->arch->cls.gpfifo, ch, sizeof(ch), &st);
}

bool NVBringup::rmScheduleChannel(uint32_t handle, uint32_t engine, uint32_t *token)
{
    GspState *g = gsp_;
    uint32_t st = 0, bind = engine;
    if (!gspRmControl(g->hClient, handle, NVA06F_CTRL_CMD_BIND_, &bind, sizeof(bind), &st))
        return false;
    uint8_t sched[NVA06F_SCHEDULE_SIZE] = {};
    sched[NVA06F_SCHEDULE_bEnable] = 1;
    return gspRmControl(g->hClient, handle, NVA06F_CTRL_CMD_GPFIFO_SCHEDULE_, sched, sizeof(sched), &st) &&
           gspRmControl(g->hClient, handle, NVC36F_CTRL_CMD_GPFIFO_GET_WORK_SUBMIT_TOKEN_, token,
                        sizeof(*token), &st);
}

int NVBringup::allocChid()
{
    for (uint32_t i = kFirstUserChid; i < kMaxChid; i++)
        if (!gsp_->chidUsed[i]) {
            gsp_->chidUsed[i] = true;
            return (int)i;
        }
    return -1;
}

// RC_TRIGGERED from GSP-RM (under gspLock_): the channel is dead; so is the device, for Vulkan.
void NVBringup::markChidLost(uint32_t chid, uint32_t exceptType)
{
    GspState *g = gsp_;
    if (!g || chid >= kMaxChid || !g->chidOwner[chid])
        return;
    GpuCtx *x = g->chidOwner[chid];
    x->lost = true;
    x->exceptType = exceptType;
    x->conn->anyLost = true;
    LOG("GSP: context %u of connection %u (chid %u) lost", x->handle, x->conn->id, chid);
}

// PTIMER (ns); the high word is read twice around the low one.
uint64_t NVBringup::gpuTime()
{
    uint32_t hi, lo;
    do {
        hi = rd32(0x9410);
        lo = rd32(0x9400);
    } while (hi != rd32(0x9410));
    return ((uint64_t)hi << 32) | lo;
}

// ---- Connections ------------------------------------------------------------------------

// Type 0: the admin boot/log client (IOUserClientClass); NVMAC_CLIENT_TYPE: a GPU client.
IOReturn NVBringup::newUserClient(task_t owningTask, void *securityID, UInt32 type,
                                  OSDictionary *properties, IOUserClient **handler)
{
    if (type != NVMAC_CLIENT_TYPE && type != NVMAC_CONTROL_TYPE)
        return IOService::newUserClient(owningTask, securityID, type, properties, handler);
    // WindowServer on a display-pipe boot gets the IOAccel-named class, the only kind its sandbox lets it open
    const bool ws = dispPipeArmed_ && NVGpuUserClient::callerIsWindowServer();
    NVGpuUserClient *c = ws ? OSTypeAlloc(IOAccelNVGpuUserClient) : OSTypeAlloc(NVGpuUserClient);
    if (!c)
        return kIOReturnNoMemory;
    c->allowWindowServer = ws;                // both types: NVK lists the GPU through the control client first
    if (!c->initWithTask(owningTask, securityID, type, properties)) {
        c->release();
        return kIOReturnNotPrivileged;
    }
    if (!c->attach(this)) {
        c->release();
        return kIOReturnError;
    }
    if (!c->start(this)) {
        c->detach(this);
        c->release();
        return kIOReturnNoDevice;
    }
    *handler = c;
    return kIOReturnSuccess;
}

// Powers the GPU on first when it is off (auto mode); powerLock_ keeps an idle power-off out
// until the connection exists.
IOReturn NVBringup::gpuOpen(task_t task, GpuConn **out)
{
    if (!powerLock_)
        return kIOReturnNotReady;
    IOLockLock(powerLock_);
    IOReturn r = kIOReturnSuccess;
    if (pwrMode_ == kModeOff)
        r = kIOReturnNotReady;              // switched off by the user
    else if (pwrState_ == kPwrOff && !gpuPowerOn("GPU opened"))
        r = kIOReturnNotResponding;
    if (r == kIOReturnSuccess)
        r = gpuOpenLocked(task, out);
    idleSince_ = 0;
    IOLockUnlock(powerLock_);
    return r;
}

IOReturn NVBringup::gpuOpenLocked(task_t task, GpuConn **out)
{
    IOLockLock(gspLock_);
    GspState *g = gsp_;
    IOReturn r = kIOReturnNoDevice;
    GpuConn *c = nullptr;
    uint32_t id = kMaxConns;
    if (!g || !g->booted || !g->hClient || !g->utilOk || !g->vaLimit)
        goto out;
    for (uint32_t i = 0; i < kMaxConns; i++)
        if (!g->conns[i]) {
            id = i;
            break;
        }
    r = kIOReturnNoResources;
    if (id == kMaxConns)
        goto out;
    r = kIOReturnNoMemory;
    c = new GpuConn;
    if (!c)
        goto out;
    c->drv = this;
    c->task = task;
    c->serial = ++connSerial_;
    c->id = id;
    c->hVas = 0x90f10100 + id;
    if (c->syncs.alloc(NVMAC_SYNC_COUNT * 8, false, true))
        goto fail;
    {
        const nv_mmu_ops ops = connOps(c, connMmuAlloc, connMmuRd64, connMmuWr64);
        r = kIOReturnNoDevice;
        c->noPool = true;       // GSP-RM writes its reserved PDEs into PD3..PD1
        c->pd3 = vasCreate(c->hVas, &ops);
        c->noPool = false;
        if (!c->pd3)
            goto fail;
        g->conns[id] = c;       // from here on gpuTeardown undoes everything
        nv_mmu_target sync = { true, 0, c->syncs.pages, 0, 0, 0 };
        bool ok = nv_mmu_map(&ops, c->pd3, KVA_SYNC, c->syncs.size, &sync);
        for (uint32_t i = 0; ok && i < g->nGrGlobal; i++) {
            nv_mmu_target t = { false, g->grGlobal[i].pa, nullptr, 0, g->grGlobal[i].pte, 0 };
            ok = nv_mmu_map(&ops, c->pd3, KVA_GRGLOBAL + g->grGlobal[i].off, g->grGlobal[i].size, &t);
        }
        praminRestore();
        tlbFlush(c->pd3);
        if (!ok) {
            gpuTeardown(c, "setup failed");
            r = kIOReturnNoMemory;
            goto out;
        }
    }
    LOG("GSP: connection %u opened (VA space 0x%x, PD3 0x%llx)", id, c->hVas, (unsigned long long)c->pd3);
    *out = c;
    c = nullptr;
    r = kIOReturnSuccess;
    goto out;
fail:
    if (c->hVas && c->pd3) {
        uint32_t st = 0;
        gspRmFree(g->hClient, g->hDevice, c->hVas, &st);
    }
    for (uint32_t i = 0; i < c->tables.n; i++)
        tableFree(c->tables.v[i]);
    c->tables.free();
    c->syncs.free();
    delete c;
    c = nullptr;
out:
    if (c && r != kIOReturnSuccess)
        c->dead = true;         // torn down above; freed by gpuRelease
    IOLockUnlock(gspLock_);
    if (c && r != kIOReturnSuccess)
        gpuRelease(c);
    return r;
}

// Everything the connection has on the GPU. Caller holds gspLock_.
void NVBringup::gpuTeardown(GpuConn *c, const char *why)
{
    GspState *g = gsp_;
    if (c->dead)
        return;
    uint32_t nCtx = 0, nMem = 0;
    for (uint32_t i = 0; i < kMaxCtx; i++)
        if (c->ctxs[i]) {
            ctxDestroy(c->ctxs[i]);
            nCtx++;
        }
    for (uint32_t i = 0; i < c->memCap; i++)
        if (c->mems[i]) {
            memFree(c, c->mems[i], false);  // no channels left; the PTEs go with the VA space
            nMem++;
        }
    OSSafeReleaseNULL(c->syncMap);
    OSSafeReleaseNULL(c->dispMap);
    uint32_t st = 0;
    if (g && g->hClient && c->pd3)
        gspRmFree(g->hClient, g->hDevice, c->hVas, &st);
    for (uint32_t i = 0; g && i < c->tables.n; i++)
        tableFree(c->tables.v[i]);
    c->tables.free();
    c->pd3 = 0;
    c->nBinds = 0;
    if (g && g->conns[c->id] == c)
        g->conns[c->id] = nullptr;
    c->dead = true;
    LOG("GSP: connection %u closed (%s): %u contexts, %u memory objects freed", c->id, why, nCtx, nMem);
}

void NVBringup::gpuTeardownAll(const char *why)
{
    GspState *g = gsp_;
    for (uint32_t i = 0; g && i < kMaxConns; i++)
        if (g->conns[i])
            gpuTeardown(g->conns[i], why);
}

void NVBringup::gpuClose(GpuConn *c)
{
    IOLockLock(gspLock_);
    gpuTeardown(c, "closed");
    IOLockUnlock(gspLock_);
    wakeWaiters();                          // waiters see dead
}

// No call of this connection is running any more (the user client is being freed).
void NVBringup::gpuRelease(GpuConn *c)
{
    if (!c->dead) {
        IOLockLock(gspLock_);
        gpuTeardown(c, "released");
        IOLockUnlock(gspLock_);
    }
    c->syncs.free();
    if (c->mems)
        IOFree(c->mems, c->memCap * sizeof(GpuMem *));
    if (c->binds)
        IOFree(c->binds, c->bindCap * sizeof(Binding));
    delete c;
}

// ---- Memory -----------------------------------------------------------------------------

static NVBringup::GpuMem *findMem(NVBringup::GpuConn *c, uint64_t handle)
{
    return handle && handle <= c->memCap ? c->mems[handle - 1] : nullptr;
}

// A free slot in c->mems (grown if needed), or an error.
static IOReturn memSlot(NVBringup::GpuConn *c, uint32_t *out)
{
    uint32_t slot = c->memCap;
    for (uint32_t i = 0; i < c->memCap; i++)
        if (!c->mems[i]) {
            slot = i;
            break;
        }
    if (slot == c->memCap) {
        uint32_t nc = c->memCap ? c->memCap * 2 : 256;
        if (nc > 65536)
            return kIOReturnNoResources;
        NVBringup::GpuMem **nm = (NVBringup::GpuMem **)IOMallocZero(nc * sizeof(NVBringup::GpuMem *));
        if (!nm)
            return kIOReturnNoMemory;
        if (c->mems) {
            memcpy(nm, c->mems, c->memCap * sizeof(NVBringup::GpuMem *));
            IOFree(c->mems, c->memCap * sizeof(NVBringup::GpuMem *));
        }
        c->mems = nm;
        c->memCap = nc;
    }
    *out = slot;
    return kIOReturnSuccess;
}

// NVMAC_MEM_IMPORT: the caller's own memory as a GART-like object. The range is wired
// (prepare) on the caller's task and its pages DMA-mapped; bindings then use them like
// GART pages. Writable memory only: the GPU may write it.
IOReturn NVBringup::memImport(GpuConn *c, uint64_t addr, uint64_t size, uint32_t *handle, uint64_t *outSize)
{
    if (!size || ((addr | size) & 0xfff) || size > NVMAC_IMPORT_MAX || addr + size < addr)
        return kIOReturnBadArgument;
    uint32_t slot;
    IOReturn r = memSlot(c, &slot);
    if (r != kIOReturnSuccess)
        return r;
    GpuMem *m = new GpuMem;
    if (!m)
        return kIOReturnNoMemory;
    m->flags = NVMAC_MEM_GART;
    m->size = size;
    m->userMd = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)addr, (mach_vm_size_t)size,
                                                     kIODirectionInOut, c->task);
    r = kIOReturnVMError;
    if (!m->userMd)
        goto fail;
    if ((r = m->userMd->prepare(kIODirectionInOut)) != kIOReturnSuccess) {
        OSSafeReleaseNULL(m->userMd);       // not prepared: nothing to complete
        goto fail;
    }
    {
        DmaBuf &d = m->sys;
        d.size = size;
        d.npages = size / PAGE_4K;
        r = kIOReturnNoMemory;
        d.pages = (uint64_t *)IOMalloc(d.npages * sizeof(uint64_t));
        if (!d.pages)
            goto fail;
        r = kIOReturnVMError;
        d.dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0, IODMACommand::kMapped, 0, PAGE_4K);
        if (!d.dma || d.dma->setMemoryDescriptor(m->userMd, true) != kIOReturnSuccess)
            goto fail;
        uint64_t off = 0, n = 0;
        while (off < size) {
            IODMACommand::Segment64 seg[16];
            UInt32 nseg = 16;
            if (d.dma->gen64IOVMSegments(&off, seg, &nseg) != kIOReturnSuccess || !nseg)
                goto fail;
            for (UInt32 k = 0; k < nseg; k++) {
                if ((seg[k].fIOVMAddr | seg[k].fLength) & (PAGE_4K - 1))
                    goto fail;
                for (uint64_t a = 0; a < seg[k].fLength && n < d.npages; a += PAGE_4K)
                    d.pages[n++] = seg[k].fIOVMAddr + a;
            }
        }
        if (n != d.npages)
            goto fail;
    }
    m->handle = slot + 1;
    c->mems[slot] = m;
    *handle = m->handle;
    *outSize = size;
    return kIOReturnSuccess;
fail:
    m->sys.free();
    if (m->userMd) {
        m->userMd->complete(kIODirectionInOut);
        OSSafeReleaseNULL(m->userMd);
    }
    delete m;
    return r;
}

IOReturn NVBringup::memAlloc(GpuConn *c, uint64_t size, uint64_t align, uint32_t flags, uint32_t kind,
                             uint32_t *handle, uint64_t *outSize)
{
    GspState *g = gsp_;
    bool vram = flags & NVMAC_MEM_VRAM, gart = flags & NVMAC_MEM_GART;
    if (!size || vram == gart || (flags & ~(uint32_t)(NVMAC_MEM_VRAM | NVMAC_MEM_GART | NVMAC_MEM_CAN_MAP)) || userKind(kind) < 0 ||
        (align & (align - 1)) || align > 0x200000 || size > (vram ? (4ull << 30) : (1ull << 30)))
        return kIOReturnBadArgument;
    size = (size + 0xfff) & ~0xfffull;
    if (align < 0x1000)
        align = 0x1000;
    if (vram && size >= 0x10000 && align < 0x10000)
        align = 0x10000;                        // lets VM_BIND use 64 KiB pages

    uint32_t slot;
    IOReturn sr = memSlot(c, &slot);
    if (sr != kIOReturnSuccess)
        return sr;

    GpuMem *m = new GpuMem;
    if (!m)
        return kIOReturnNoMemory;
    m->flags = flags | (gart ? NVMAC_MEM_CAN_MAP : 0);
    m->size = size;
    m->kind = (uint8_t)userKind(kind);
    IOReturn r = kIOReturnNoMemory;
    if (vram) {
        m->vram = nv_vram_alloc(g->vram, size, align);
        if (!m->vram)
            goto fail;
        r = kIOReturnIOError;
        if (!scrubVram(m->vram, size))
            goto fail;
        if (flags & NVMAC_MEM_CAN_MAP) {
            r = kIOReturnNoMemory;
            if (bar1Reuse(c->serial, m->vram, size, &m->bar1Va, &m->bar1Size))
                m->cpuMapped = true;        // a held slice: back to bar1Hold when freed
            else if (bar1Map(m->vram, size, &m->bar1Va))
                m->bar1Size = size;
            else
                goto fail;
            m->cpuMd = IOMemoryDescriptor::withPhysicalAddress((IOPhysicalAddress)(g->bar1Phys + m->bar1Va),
                                                               (IOByteCount)size, kIODirectionInOut);
            if (!m->cpuMd)
                goto fail;
        }
    } else {
        if (m->sys.alloc(size, false, true))
            goto fail;
        m->cpuMd = m->sys.mem;
        m->cpuMd->retain();
    }
    m->handle = slot + 1;
    c->mems[slot] = m;
    *handle = m->handle;
    *outSize = size;
    return kIOReturnSuccess;
fail:
    OSSafeReleaseNULL(m->cpuMd);
    if (m->bar1Va && m->cpuMapped)
        bar1Hold(m->bar1Va, m->bar1Size, c->serial);
    else if (m->bar1Va)
        bar1Unmap(m->bar1Va, m->bar1Size);
    if (m->vram)
        nv_vram_free(g->vram, m->vram);
    m->sys.free();
    delete m;
    return r;
}

// Unbinds it everywhere, drops the CPU mapping and BAR1 window, frees the memory. Memory that
// was ever CPU-mapped (memMap) may have a duplicate mapping the driver no longer knows about
// (e.g. mach_vm_remap into another task), so its BAR1 slice is held rather than freed: its PTEs
// are pointed at the shared read-only dummy page first, so a leftover duplicate reads only
// zeroes, and only then is the VRAM itself freed for reuse. Memory that was never CPU-mapped
// can't have been duplicated and keeps the immediate bar1Unmap path.
void NVBringup::memFree(GpuConn *c, GpuMem *m, bool unbind)
{
    GspState *g = gsp_;
    if (unbind && c->pd3 && !c->dead)
        unbindRange(c, USER_VA_START, USER_VA_END - USER_VA_START, m);
#if NV_BAR1_REDIRECT
    if (m->userMap && m->bar1Va) {
        IOBufferMemoryDescriptor *zero = nullptr;
        if (NV_BAR1_REDIRECT == 1) {
            zero = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, m->size, 0x1000);
            if (zero)
                bzero(zero->getBytesNoCopy(), m->size);
        }
        IOReturn rr = NV_BAR1_REDIRECT == 1 && !zero ? kIOReturnNoMemory : m->userMap->redirect(zero, 0, 0);
        LOG("GSP: BAR1 experiment: redirect of 0x%llx (%llu KiB) onto %s: 0x%x", (unsigned long long)m->bar1Va,
            (unsigned long long)(m->size >> 10), NV_BAR1_REDIRECT == 1 ? "zeroed memory" : "nothing", rr);
        OSSafeReleaseNULL(zero);
    }
    m->cpuMapped = false;               // no hold: the slice goes straight back to bar1Heap
#endif
    OSSafeReleaseNULL(m->userMap);
    OSSafeReleaseNULL(m->cpuMd);
    if (m->bar1Va) {
        if (m->cpuMapped)
            bar1Hold(m->bar1Va, m->bar1Size, c->serial);
        else
            bar1Unmap(m->bar1Va, m->bar1Size);
    }
    if (m->vram && !m->borrowed)
        nv_vram_free(g->vram, m->vram);
    m->sys.free();
    if (m->userMd) {                    // unwire the caller's pages (after the DMA mapping is gone)
        m->userMd->complete(kIODirectionInOut);
        OSSafeReleaseNULL(m->userMd);
    }
    if (m->handle && m->handle <= c->memCap && c->mems[m->handle - 1] == m)
        c->mems[m->handle - 1] = nullptr;
    delete m;
}

IOReturn NVBringup::memMap(GpuConn *c, GpuMem *m, uint64_t *addr)
{
    if (!m->cpuMd)
        return kIOReturnBadArgument;
    bool promised = !NV_BAR1_REDIRECT && !m->cpuMapped && m->bar1Va;
    if (promised) {
        IOReturn r = bar1HoldCommit(m->bar1Size);   // memFree must be able to hold the slice
        if (r != kIOReturnSuccess)
            return r;
    }
    if (!m->userMap) {
        IOOptionBits opt = kIOMapAnywhere | (m->bar1Va ? kIOMapWriteCombineCache : kIOMapDefaultCache);
        if (NV_BAR1_REDIRECT && m->bar1Va)
            opt |= kIOMapUnique;        // redirect() needs a mapping of its own
        m->userMap = m->cpuMd->createMappingInTask(c->task, 0, opt, 0, m->size);
        if (!m->userMap) {
            if (promised)
                bar1HoldUncommit(m->bar1Size);
            return kIOReturnVMError;
        }
    }
    if (m->bar1Va)
        m->cpuMapped = true;            // only such memory can have a CPU-side duplicate
    *addr = m->userMap->getAddress();
    return kIOReturnSuccess;
}

// ---- Bindings -----------------------------------------------------------------------------

// First binding with va + size > addr.
static uint32_t firstBindAfter(const NVBringup::GpuConn *c, uint64_t addr)
{
    uint32_t lo = 0, hi = c->nBinds;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (c->binds[mid].va + c->binds[mid].size <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

static bool insertBind(NVBringup::GpuConn *c, uint32_t at, const Binding &b)
{
    if (c->nBinds == c->bindCap) {
        uint32_t nc = c->bindCap ? c->bindCap * 2 : 256;
        if (nc > (1u << 20))
            return false;
        Binding *nb = (Binding *)IOMalloc(nc * sizeof(Binding));
        if (!nb)
            return false;
        if (c->binds) {
            memcpy(nb, c->binds, c->nBinds * sizeof(Binding));
            IOFree(c->binds, c->bindCap * sizeof(Binding));
        }
        c->binds = nb;
        c->bindCap = nc;
    }
    memmove(&c->binds[at + 1], &c->binds[at], (c->nBinds - at) * sizeof(Binding));
    c->binds[at] = b;
    c->nBinds++;
    return true;
}

// Removes [va, va + size) from the bindings (only those of `only`, if given), clearing the
// PTEs. The caller flushes the TLB (memFree's callers do it via the next bind or teardown;
// here it is done at the end).
IOReturn NVBringup::unbindRange(GpuConn *c, uint64_t va, uint64_t size, GpuMem *only)
{
    const nv_mmu_ops ops = connOps(c, connMmuAlloc, connMmuRd64, connMmuWr64);
    uint64_t end = va + size;
    bool any = false;
    for (uint32_t i = firstBindAfter(c, va); i < c->nBinds && c->binds[i].va < end;) {
        Binding b = c->binds[i];
        if (only && b.mem != only) {
            i++;
            continue;
        }
        uint64_t bEnd = b.va + b.size;
        uint64_t cs = b.va > va ? b.va : va, ce = bEnd < end ? bEnd : end;
        nv_mmu_unmap_4k(&ops, c->pd3, cs, ce - cs);
        any = true;
        if (b.va < cs && bEnd > ce) {               // split in two
            c->binds[i].size = cs - b.va;
            Binding tail = { ce, bEnd - ce, b.memOff + (ce - b.va), b.mem };
            if (!insertBind(c, i + 1, tail)) {      // keep the table consistent: drop the tail's PTEs too
                nv_mmu_unmap_4k(&ops, c->pd3, ce, bEnd - ce);
            }
            i += 2;
        } else if (b.va < cs) {
            c->binds[i].size = cs - b.va;
            i++;
        } else if (bEnd > ce) {
            c->binds[i].va = ce;
            c->binds[i].memOff += ce - b.va;
            c->binds[i].size = bEnd - ce;
            i++;
        } else {
            memmove(&c->binds[i], &c->binds[i + 1], (c->nBinds - i - 1) * sizeof(Binding));
            c->nBinds--;
        }
    }
    praminRestore();
    if (any)
        tlbFlush(c->pd3);
    return kIOReturnSuccess;
}

IOReturn NVBringup::vmBind(GpuConn *c, const uint8_t *buf, uint32_t len)
{
    nvmac_bind_hdr h;
    if (len < sizeof(h))
        return kIOReturnBadArgument;
    memcpy(&h, buf, sizeof(h));
    if (!h.count || h.count > NVMAC_MAX_BIND_OPS || len != sizeof(h) + h.count * sizeof(nvmac_bind_op))
        return kIOReturnBadArgument;
    const nv_mmu_ops ops = connOps(c, connMmuAlloc, connMmuRd64, connMmuWr64);
    IOReturn r = kIOReturnSuccess;
    bool mapped = false;
    for (uint32_t k = 0; k < h.count && r == kIOReturnSuccess; k++) {
        nvmac_bind_op o;
        memcpy(&o, buf + sizeof(h) + k * sizeof(o), sizeof(o));
        if (((o.va | o.range | o.mem_offset) & 0xfff) || !o.range || o.va < USER_VA_START ||
            o.va + o.range < o.va || o.va + o.range > USER_VA_END || o.va + o.range > gsp_->vaLimit) {
            r = kIOReturnBadArgument;
            break;
        }
        uint32_t op = o.op & 0xff;
        if ((o.op & ~0x1ffffu) || (op == NVMAC_UNBIND && (o.op >> 8))) {
            r = kIOReturnBadArgument;
            break;
        }
        if (op == NVMAC_UNBIND) {
            // Unbinds may not end inside a 64 KiB page (the kernel chose the page size).
            uint64_t e = o.va + o.range;
            if (((o.va & 0xffff) && nv_mmu_lookup_big(&ops, c->pd3, o.va)) ||
                ((e & 0xffff) && nv_mmu_lookup_big(&ops, c->pd3, e))) {
                r = kIOReturnBadArgument;
                break;
            }
            r = unbindRange(c, o.va, o.range, nullptr);
            continue;
        }
        GpuMem *m = op == NVMAC_BIND ? findMem(c, o.mem) : nullptr;
        if (!m || o.mem_offset + o.range < o.mem_offset || o.mem_offset + o.range > m->size) {
            r = kIOReturnBadArgument;
            break;
        }
        uint32_t at = firstBindAfter(c, o.va);
        if (at < c->nBinds && c->binds[at].va < o.va + o.range) {
            r = kIOReturnBadArgument;               // overlaps an existing binding
            break;
        }
        int kind = (o.op & (1u << 16)) ? userKind((o.op >> 8) & 0xff) : m->kind;
        if (kind < 0) {
            r = kIOReturnBadArgument;
            break;
        }
        nv_mmu_target t = { m->vram == 0, m->vram + o.mem_offset, m->sys.pages, o.mem_offset / 0x1000, 0, (uint8_t)kind };
        Binding b = { o.va, o.range, o.mem_offset, m };
        if (!nv_mmu_map(&ops, c->pd3, o.va, o.range, &t)) {
            r = kIOReturnNoMemory;
            break;
        }
        if (!insertBind(c, at, b)) {
            nv_mmu_unmap_4k(&ops, c->pd3, o.va, o.range);
            r = kIOReturnNoMemory;
            break;
        }
        mapped = true;
    }
    praminRestore();
    if (mapped)
        tlbFlush(c->pd3);
    return r;
}

// ---- Contexts -----------------------------------------------------------------------------

static NVBringup::GpuCtx *findCtx(NVBringup::GpuConn *c, uint64_t handle)
{
    return handle && handle <= kMaxCtx ? c->ctxs[handle - 1] : nullptr;
}

static int allocSync(NVBringup::GpuConn *c, uint8_t use)
{
    for (uint32_t i = 0; i < NVMAC_SYNC_COUNT; i++)
        if (!c->syncUsed[i]) {
            c->syncUsed[i] = use;
            *syncSlot(c, i) = 0;
            return (int)i;
        }
    return -1;
}

IOReturn NVBringup::ctxCreate(GpuConn *c, uint32_t engines, uint32_t *handle)
{
    GspState *g = gsp_;
    uint32_t st = 0;
    if (!engines ||
        (engines & ~(uint32_t)(NVMAC_ENGINE_COPY | NVMAC_ENGINE_3D | NVMAC_ENGINE_COMPUTE | NVMAC_ENGINE_VDEC)))
        return kIOReturnBadArgument;
    bool gr = engines & (NVMAC_ENGINE_3D | NVMAC_ENGINE_COMPUTE);
    bool vdec = engines & NVMAC_ENGINE_VDEC;
    if (vdec && engines != NVMAC_ENGINE_VDEC)
        return kIOReturnBadArgument;            // an NVDEC channel carries only the decoder
    if ((gr && !g->grReady) || (vdec && !g->nvdecCtxSize))
        return kIOReturnUnsupported;
    uint32_t index = kMaxCtx;
    for (uint32_t i = 0; i < kMaxCtx; i++)
        if (!c->ctxs[i]) {
            index = i;
            break;
        }
    if (index == kMaxCtx)
        return kIOReturnNoResources;
    int chid = allocChid();
    if (chid < 0)
        return kIOReturnNoResources;
    int seqSync = allocSync(c, 2);
    if (seqSync < 0) {
        g->chidUsed[chid] = false;
        return kIOReturnNoResources;
    }

    GpuCtx *x = new GpuCtx;
    if (!x) {
        g->chidUsed[chid] = false;
        c->syncUsed[seqSync] = 0;
        return kIOReturnNoMemory;
    }
    x->conn = c;
    x->index = index;
    x->handle = index + 1;
    x->engines = engines;
    x->chid = (uint32_t)chid;
    x->seqSync = (uint32_t)seqSync;
    x->kva = KVA_CTX + index * KVA_CTX_SLOT;
    x->engineType = gr ? NV2080_ENGINE_TYPE_GR0_ : vdec ? NV2080_ENGINE_TYPE_NVDEC0_ : g->ceEngine;
    c->ctxs[index] = x;                         // ctxDestroy cleans up from here on

    const nv_mmu_ops ops = connOps(c, connMmuAlloc, connMmuRd64, connMmuWr64);
    IOReturn r = kIOReturnNoMemory;
    x->inst = mmuAlloc(this, 0x1000);
    x->userd = mmuAlloc(this, 0x1000);
    x->mthd = mmuAlloc(this, g->mthdSize);
    x->ring = mmuAlloc(this, RING_ENTRIES * 8);
    x->push = nv_vram_alloc(g->vram, PUSH_SLOTS * PUSH_SLOT_BYTES, 0x1000);
    if (gr) {
        uint64_t align = 0x1000;
        while (align < g->grMainAlign && align < 0x200000)
            align <<= 1;
        x->main = nv_vram_alloc(g->vram, g->grMainSize, align);
        x->patch = nv_vram_alloc(g->vram, g->grPatchSize, 0x1000);
    }
    const uint64_t vdecBytes = ((uint64_t)g->nvdecCtxSize + 0xfff) & ~0xfffull;   // whole 4 KiB pages
    if (vdec)                                   // NVDEC's falcon context buffer, in MAIN's place
        x->main = nv_vram_alloc(g->vram, vdecBytes, 0x1000);
    bool ok = x->inst && x->userd && x->mthd && x->ring && x->push && (!gr || (x->main && x->patch)) &&
              (!vdec || x->main);
    // The push slots are readable by the connection's GPU work: no leftovers of earlier VRAM users.
    if (ok && !scrubVram(x->push, PUSH_SLOTS * PUSH_SLOT_BYTES)) {
        r = kIOReturnIOError;
        ok = false;
    }
    // The NVDEC context buffer starts zeroed, as nouveau's (nvkm_gpuobj_new with zero).
    if (ok && vdec && !scrubVram(x->main, vdecBytes)) {
        r = kIOReturnIOError;
        ok = false;
    }
    if (ok) {
        nv_mmu_target ring = { false, x->ring, nullptr, 0, NV_MMU_PTE_READ_ONLY, 0 };
        nv_mmu_target push = { false, x->push, nullptr, 0, NV_MMU_PTE_READ_ONLY, 0 };
        ok = nv_mmu_map(&ops, c->pd3, x->kva + SLOT_RING, RING_ENTRIES * 8, &ring) &&
             nv_mmu_map(&ops, c->pd3, x->kva + SLOT_PUSH, PUSH_SLOTS * PUSH_SLOT_BYTES, &push);
        if (ok && gr) {
            nv_mmu_target mainT = { false, x->main, nullptr, 0, NV_MMU_PTE_PRIVILEGE, 0 };
            // Privileged like MAIN: context switches apply the register writes listed in it.
            nv_mmu_target patchT = { false, x->patch, nullptr, 0, NV_MMU_PTE_PRIVILEGE, 0 };
            ok = nv_mmu_map(&ops, c->pd3, x->kva + SLOT_MAIN, g->grMainSize, &mainT) &&
                 nv_mmu_map(&ops, c->pd3, x->kva + SLOT_PATCH, g->grPatchSize, &patchT);
        }
        if (ok && vdec) {
            // Not privileged, as nouveau maps it (r535_flcn_ctor). Only this channel's own NVDEC
            // work could reach it, and the buffer holds only this channel's decoder state.
            nv_mmu_target ctxT = { false, x->main, nullptr, 0, 0, 0 };
            ok = nv_mmu_map(&ops, c->pd3, x->kva + SLOT_MAIN, vdecBytes, &ctxT);
        }
    }
    praminRestore();
    if (!ok)
        goto fail;
    tlbFlush(c->pd3);

    r = kIOReturnNoDevice;
    x->hChannel = g->nextHandle;
    g->nextHandle += 8;
    if (!rmAllocChannel(x->hChannel, x->chid, x->engineType, c->hVas, x->kva + SLOT_RING, RING_ENTRIES,
                        x->inst, x->userd, x->mthd, false)) {
        x->hChannel = 0;
        goto fail;
    }
    if (gr) {
        // Promote the context buffers (Phase 4 step 7), then the objects.
        uint8_t pr[NV2080_PROMOTE_SIZE] = {};
        uint32_t n = 0;
        auto entry = [&](uint32_t id, uint64_t va, uint64_t pa, uint64_t size, bool init) {
            uint8_t *e = pr + NV2080_PROMOTE_entries + n++ * NV2080_PROMOTE_ENTRY_SIZE;
            put64(e, NV2080_PE_gpuVirtAddr, va);
            uint16_t id16 = (uint16_t)id;
            memcpy(e + NV2080_PE_bufferId, &id16, 2);
            if (init) {
                put64(e, NV2080_PE_gpuPhysAddr, pa);
                put64(e, NV2080_PE_size, size);
                put32(e, NV2080_PE_physAttr, NV2080_CTRL_GPU_INITIALIZE_CTX_VIDMEM_UNCACHED);
                e[NV2080_PE_bInitialize] = 1;
            }
        };
        entry(PROMOTE_ID_MAIN, x->kva + SLOT_MAIN, x->main, g->grMainSize, true);
        entry(PROMOTE_ID_PATCH, x->kva + SLOT_PATCH, x->patch, g->grPatchSize, true);
        for (uint32_t i = 0; i < g->nGrGlobal && n < NV2080_PROMOTE_MAX; i++)
            entry(g->grGlobal[i].id, KVA_GRGLOBAL + g->grGlobal[i].off, g->grGlobal[i].pa, g->grGlobal[i].size,
                  g->grGlobal[i].init);
        put32(pr, NV2080_PROMOTE_engineType, NV2080_ENGINE_TYPE_GR0_);
        put32(pr, NV2080_PROMOTE_hChanClient, g->hClient);
        put32(pr, NV2080_PROMOTE_hObject, x->hChannel);
        put32(pr, NV2080_PROMOTE_entryCount, n);
        if (!gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GPU_PROMOTE_CTX_, pr, sizeof(pr), &st))
            goto fail;
        if ((engines & NVMAC_ENGINE_3D) &&
            !gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 1, chip_->arch->cls.eng3d, nullptr, 0, &st))
            goto fail;
        if ((engines & NVMAC_ENGINE_COMPUTE) &&
            !gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 2, chip_->arch->cls.compute, nullptr, 0, &st))
            goto fail;
        if (engines & NVMAC_ENGINE_COPY) {
            // A copy object on a graphics channel needs a copy engine on the graphics runlist
            // (GRCE); GSP-RM rejects the others. Try each once, then remember the winner.
            bool got = false;
            for (uint32_t i = 0; i <= g->nEngines && !got; i++) {
                uint32_t t = i == 0 ? g->grCeEngine : g->engines[i - 1];
                if (!t || !isCopyEngine(t) || (i && t == g->grCeEngine))
                    continue;
                uint32_t ceAlloc[NVB0B5_ALLOC_SIZE / 4] = { NVB0B5_ALLOC_VERSION_1, t };
                got = gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 3, chip_->arch->cls.copy,
                                 ceAlloc, sizeof(ceAlloc), &st);
                if (got && g->grCeEngine != t) {
                    g->grCeEngine = t;
                    LOG("GSP: copy engine 0x%x accepts copy objects on graphics channels", t);
                }
            }
            if (!got)
                goto fail;
        }
        if (!rmScheduleChannel(x->hChannel, x->engineType, &x->token))
            goto fail;
    } else if (vdec) {
        // As nouveau with r570: bind and schedule the channel, promote the falcon context buffer
        // (the single-buffer form of PROMOTE_CTX), then allocate the decoder (r535_flcn_bind,
        // r535_nvdec_alloc). No copy object: NVDEC channels have no copy engine.
        if (!rmScheduleChannel(x->hChannel, x->engineType, &x->token))
            goto fail;
        uint8_t pr[NV2080_PROMOTE_SIZE] = {};
        put32(pr, NV2080_PROMOTE_engineType, NV2080_ENGINE_TYPE_NVDEC0_);
        put32(pr, NV2080_PROMOTE_hClient, g->hClient);
        put32(pr, NV2080_PROMOTE_ChID, x->chid);
        put32(pr, NV2080_PROMOTE_hChanClient, g->hClient);
        put32(pr, NV2080_PROMOTE_hObject, x->hChannel);
        put64(pr, NV2080_PROMOTE_virtAddress, x->kva + SLOT_MAIN);
        put64(pr, NV2080_PROMOTE_size, g->nvdecCtxSize);
        if (!gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GPU_PROMOTE_CTX_, pr, sizeof(pr), &st))
            goto fail;
        uint8_t bsp[NV_BSP_ALLOC_SIZE] = {};
        put32(bsp, NV_BSP_size, NV_BSP_ALLOC_SIZE);
        put32(bsp, NV_BSP_engineInstance, 0);
        if (!gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 4, chip_->arch->cls.vdec, bsp, sizeof(bsp),
                        &st))
            goto fail;
    } else {
        if (!rmScheduleChannel(x->hChannel, x->engineType, &x->token))
            goto fail;
        uint32_t ceAlloc[NVB0B5_ALLOC_SIZE / 4] = { NVB0B5_ALLOC_VERSION_1, g->ceEngine };
        if (!gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 3, chip_->arch->cls.copy, ceAlloc,
                        sizeof(ceAlloc), &st))
            goto fail;
    }
    g->chidOwner[x->chid] = x;
    *handle = x->handle;
    LOG("GSP: connection %u context %u: chid %u, engines 0x%x, token 0x%x", c->id, x->handle, x->chid, engines,
        x->token);
    return kIOReturnSuccess;
fail:
    LOG("GSP: connection %u: context creation failed (engines 0x%x)", c->id, engines);
    ctxDestroy(x);
    return r;
}

void NVBringup::ctxDestroy(GpuCtx *x)
{
    GspState *g = gsp_;
    GpuConn *c = x->conn;
    uint32_t st = 0;
    if (x->hChannel && g->hClient)
        gspRmFree(g->hClient, g->hDevice, x->hChannel, &st);   // preempts; its objects go with it
    if (g->chidOwner[x->chid] == x)
        g->chidOwner[x->chid] = nullptr;
    if (c->pd3) {
        const nv_mmu_ops ops = connOps(c, connMmuAlloc, connMmuRd64, connMmuWr64);
        nv_mmu_unmap_4k(&ops, c->pd3, x->kva, KVA_CTX_SLOT);
        praminRestore();
        tlbFlush(c->pd3);
    }
    const uint64_t mem[] = { x->inst, x->userd, x->mthd, x->ring, x->push, x->main, x->patch };
    for (uint64_t a : mem)
        if (a)
            nv_vram_free(g->vram, a);
    g->chidUsed[x->chid] = false;
    c->syncUsed[x->seqSync] = 0;
    c->ctxs[x->index] = nullptr;
    delete x;
}

// ---- Submission ---------------------------------------------------------------------------

// One host semaphore operation (SEM_ADDR_LO..SEM_EXECUTE): 6 words.
static uint32_t semOp(uint32_t *w, uint64_t addr, uint64_t value, uint32_t exec)
{
    w[0] = pbHeader(0, NVC46F_SEM_ADDR_LO_, 5);
    w[1] = (uint32_t)addr;
    w[2] = (uint32_t)(addr >> 32);
    w[3] = (uint32_t)value;
    w[4] = (uint32_t)(value >> 32);
    w[5] = exec;
    return 6;
}

// ---- P-state boost -----------------------------------------------------------------------

static uint64_t msSince(uint64_t then, uint64_t now)
{
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(now - then, &ns);
    return ns / 1000000;
}

bool NVBringup::perfBoostLocked(uint32_t cmd, uint32_t sec, uint32_t *status)
{
    GspState *g = gsp_;
    *status = 0xffffffff;
    if (!g || !g->booted || !g->hSubdevice || cmd > NVMAC_BOOST_TO_MAX)
        return false;
    NV2080_CTRL_INTERNAL_PERF_BOOST_SET_PARAMS_2X_ p = {};
    p.flags = (uint8_t)cmd;
    p.duration = sec;
    boostSent_[cmd]++;
    bool ok = gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_INTERNAL_PERF_BOOST_SET_2X_,
                           &p, sizeof(p), status, true);
    if (ok)
        boostCount_++;
    return ok;
}

uint32_t NVBringup::pstateLocked()
{
    GspState *g = gsp_;
    uint32_t p = 0, st = 0;
    if (!g || !g->booted || !g->hSubdevice ||
        !gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE_, &p, sizeof(p), &st, true) ||
        !p)
        return ~0u;
    return (uint32_t)__builtin_ctz(p);
}

bool NVBringup::gpuBusyLocked()
{
    GspState *g = gsp_;
    for (uint32_t i = 0; g && i < kMaxConns; i++) {
        GpuConn *c = g->conns[i];
        if (!c || c->dead)
            continue;
        for (uint32_t j = 0; j < kMaxCtx; j++) {
            GpuCtx *x = c->ctxs[j];
            if (x && !x->lost && *syncSlot(c, x->seqSync) < x->submitted)
                return true;
        }
    }
    return false;
}

// Requests are sent with the lock held (GSP-RM RPCs are serialised by it anyway); one costs a
// GSP round trip, so they only go out when the level changes or a boost is about to run out.
// A refusal turns the policy off: GSP-RM keeps the clocks, and the log says so once.
static const uint32_t kBoostTickMs = 20, kBoostHoldSec = 2;

uint32_t NVBringup::boostHeldLocked()
{
    if (boostPolicy_ == NVMAC_PERF_POLICY_FIXED && boostLevel_ && boostLast_ &&
        msSince(boostLast_, mach_absolute_time()) >= boostSec_ * 1000ull)
        return NVMAC_BOOST_CLEAR;
    return boostLevel_;
}

void NVBringup::boostArmLocked()
{
    if (boostTicking_ || boostStopping_ || !boostCall_)
        return;
    boostTicking_ = true;
    uint64_t dl = 0;
    clock_interval_to_deadline(kBoostTickMs, kMillisecondScale, &dl);
    boostTickAt_ = dl;
    thread_call_enter_delayed(boostCall_, dl);
}

void NVBringup::boostThunk(thread_call_param_t self, thread_call_param_t)
{
    NVBringup *d = (NVBringup *)self;
    IOLockLock(d->gspLock_);
    const bool up = d->gsp_ && d->gsp_->booted;
    // A boost request queued by an EXEC (autoBoostLocked): the RPC goes out here, not on the submitting thread.
    if (d->boostPend_ != ~0u) {
        const uint32_t want = d->boostPend_, sec = d->boostPendSec_;
        d->boostPend_ = ~0u;
        uint32_t st = 0;
        if (up && d->boostPolicy_ != NVMAC_PERF_POLICY_OFF && !d->perfBoostLocked(want, sec, &st)) {
            d->log("perf: P-state boost refused (status 0x%x); clocks left to GSP-RM\n", st);
            d->boostPolicy_ = NVMAC_PERF_POLICY_OFF;
            d->boostLevel_ = 0;
        }
    }
    if (d->boostTicking_) {
        // The call may have been entered early for the request above: the sampler keeps its own 20 ms grid.
        uint64_t now = mach_absolute_time();
        if (up && now < d->boostTickAt_ && !d->boostStopping_) {
            thread_call_enter_delayed(d->boostCall_, d->boostTickAt_);
        } else {
            d->boostTicking_ = false;
            if (up)
                d->boostTickLocked();
            else
                d->boostLevel_ = 0;         // GSP-RM went away, and its boosts with it
        }
    } else if (!up) {
        d->boostLevel_ = 0;
    }
    IOLockUnlock(d->gspLock_);
}

void NVBringup::autoBoostLocked(bool backlog)
{
    uint64_t now = mach_absolute_time();
    uint32_t st = 0, want;
    switch (boostPolicy_) {
    case NVMAC_PERF_POLICY_FIXED:
        if (boostLevel_ == NVMAC_BOOST_TO_MAX && boostLast_ && msSince(boostLast_, now) < boostSec_ * 500ull)
            return;
        want = NVMAC_BOOST_TO_MAX;
        break;
    case NVMAC_PERF_POLICY_ADAPTIVE:
        boostLastBusy_ = now;
        want = backlog ? NVMAC_BOOST_TO_MAX : boostLevel_ ? boostLevel_ : boostBurst_;
        if (want <= boostLevel_) {          // held already; the sampler renews and clears it
            boostArmLocked();
            return;
        }
        break;
    default:
        return;
    }
    boostLast_ = now;
    // The RPC is a GSP round trip (about a millisecond): queued for boostCall_ so EXEC doesn't wait for it. The
    // level is taken as held right away (the next EXEC doesn't ask again); a refusal turns the policy off from
    // boostThunk, as it did here.
    if (want != NVMAC_BOOST_CLEAR) {
        if (!boostCall_ || boostStopping_)
            return;
        boostPend_ = want;
        boostPendSec_ = boostPolicy_ == NVMAC_PERF_POLICY_FIXED ? boostSec_ : kBoostHoldSec;
        thread_call_enter(boostCall_);
    }
    (void)st;
    boostLevel_ = want;
    if (boostPolicy_ == NVMAC_PERF_POLICY_ADAPTIVE)
        boostArmLocked();                   // even with no burst boost: the sampler may escalate
}

// Every 20 ms while the adaptive policy holds (or watches for) a boost.
void NVBringup::boostTickLocked()
{
    if (boostPolicy_ != NVMAC_PERF_POLICY_ADAPTIVE) {
        boostEwma_ = 0;
        return;                             // the policy changed; whatever is held runs out
    }
    uint64_t now = mach_absolute_time();
    bool busy = gpuBusyLocked();
    boostEwma_ = (3 * boostEwma_ + (busy ? 1000 : 0)) / 4;
    if (busy)
        boostLastBusy_ = now;
    uint32_t st = 0;
    if (msSince(boostLastBusy_, now) >= boostIdleMs_) {
        if (boostLevel_ != NVMAC_BOOST_CLEAR)
            perfBoostLocked(NVMAC_BOOST_CLEAR, 0, &st);
        boostLevel_ = NVMAC_BOOST_CLEAR;
        boostEwma_ = 0;
        return;                             // the next EXEC starts over
    }
    uint32_t want = boostLevel_;
    if (boostEwma_ >= boostBusyPct_ * 10)
        want = NVMAC_BOOST_TO_MAX;
    // Renew a TO_MAX before it runs out. A 1LEVEL boost is left to run out instead (whether asking
    // again stacks a level is GSP-RM's business); the next EXEC after it asks afresh.
    bool renew = want == boostLevel_ && want == NVMAC_BOOST_TO_MAX && msSince(boostLast_, now) >= 1000;
    if (boostLevel_ == NVMAC_BOOST_1LEVEL && msSince(boostLast_, now) >= kBoostHoldSec * 1000ull) {
        boostLevel_ = NVMAC_BOOST_CLEAR;    // ran out: nothing held now
        if (want == NVMAC_BOOST_1LEVEL)
            want = NVMAC_BOOST_CLEAR;
    }
    if (want != boostLevel_ || renew) {
        boostLast_ = now;
        if (!perfBoostLocked(want, kBoostHoldSec, &st)) {
            LOG("perf: P-state boost refused (status 0x%x); clocks left to GSP-RM", st);
            boostPolicy_ = NVMAC_PERF_POLICY_OFF;
            boostLevel_ = 0;
            return;
        }
        boostLevel_ = want;
    }
    boostArmLocked();
}

IOReturn NVBringup::exec(GpuConn *c, const uint8_t *buf, uint32_t len, uint64_t *seqOut)
{
    nvmac_exec_hdr h;
    if (len < sizeof(h))
        return kIOReturnBadArgument;
    memcpy(&h, buf, sizeof(h));
    if (h.n_wait > NVMAC_MAX_WAITS || h.n_push > NVMAC_MAX_PUSHES || h.n_signal > NVMAC_MAX_SIGNALS ||
        len != sizeof(h) + (h.n_wait + h.n_signal) * sizeof(nvmac_sync_point) + h.n_push * sizeof(nvmac_push))
        return kIOReturnBadArgument;
    const nvmac_sync_point *waits = (const nvmac_sync_point *)(buf + sizeof(h));
    const nvmac_push *pushes = (const nvmac_push *)(waits + h.n_wait);
    const nvmac_sync_point *signals = (const nvmac_sync_point *)(pushes + h.n_push);
    for (uint32_t i = 0; i < h.n_wait + h.n_signal; i++) {
        const nvmac_sync_point &s = i < h.n_wait ? waits[i] : signals[i - h.n_wait];
        if (!s.sync || s.sync > NVMAC_SYNC_COUNT || c->syncUsed[s.sync - 1] != 1)
            return kIOReturnBadArgument;
    }
    for (uint32_t i = 0; i < h.n_push; i++) {
        const nvmac_push &p = pushes[i];
        if (((p.va | p.size) & 3) || !p.size || p.size >= (8u << 20) || (p.flags & ~(uint32_t)NVMAC_PUSH_NO_PREFETCH) ||
            p.va < USER_VA_START || p.va + p.size > USER_VA_END)
            return kIOReturnBadArgument;
    }

    // Room in the ring (GPGet from USERD) and a free kernel push slot (seqno); the ring can
    // only fill up behind slow work, so wait with the lock dropped, up to 5 s.
    uint32_t need = h.n_push + (h.n_wait ? 1 : 0) + 1;
    GpuCtx *x = nullptr;
    for (uint32_t tries = 0;; tries++) {
        x = findCtx(c, h.ctx);
        if (!x)
            return kIOReturnBadArgument;
        if (x->lost || c->dead)
            return kIOReturnNoDevice;
        // Entries up to the end of the last completed EXEC were consumed (no reliance on
        // USERD's GPGet write-back).
        uint64_t done = *syncSlot(c, x->seqSync);
        uint32_t room = RING_ENTRIES - 1;
        if (done < x->submitted) {
            uint32_t consumed = done ? x->putAfter[done % PUSH_SLOTS] : 0;
            room = (consumed + RING_ENTRIES - x->put - 1) % RING_ENTRIES;
        }
        if (room >= need && x->submitted + 1 - done < PUSH_SLOTS)
            break;
        if (tries >= 5000)
            return kIOReturnBusy;
        IOLockUnlock(gspLock_);
        IOSleep(1);
        IOLockLock(gspLock_);
        if (!gsp_ || !gsp_->booted)
            return kIOReturnNoDevice;
    }

    bool backlog = *syncSlot(c, x->seqSync) < x->submitted;  // earlier work still queued
    uint64_t seq = ++x->submitted;
    uint32_t slot = (uint32_t)(seq % PUSH_SLOTS);
    uint64_t pushPa = x->push + slot * PUSH_SLOT_BYTES, pushVa = x->kva + SLOT_PUSH + slot * PUSH_SLOT_BYTES;
    uint32_t w[128];
    uint64_t gp[NVMAC_MAX_PUSHES + 2];
    uint32_t ngp = 0;
    auto gpEntry = [](uint64_t va, uint32_t words, bool noPrefetch) {
        return ((uint64_t)(((uint32_t)(va >> 32) & 0xff) | (words << 10) | (noPrefetch ? 1u << 31 : 0)) << 32) |
               ((uint32_t)va & ~3u);
    };

    // Before: GPU-side waits (acquire until value >= wanted, switching away meanwhile).
    if (h.n_wait) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < h.n_wait; i++)
            n += semOp(w + n, KVA_SYNC + 8 * (waits[i].sync - 1), waits[i].value, NVC46F_SEM_ACQ_GEQ_64);
        for (uint32_t i = 0; i < n; i++)
            praminWr32(pushPa + 4 * i, w[i]);
        gp[ngp++] = gpEntry(pushVa, n, false);
    }
    for (uint32_t i = 0; i < h.n_push; i++)
        gp[ngp++] = gpEntry(pushes[i].va, pushes[i].size / 4, pushes[i].flags & NVMAC_PUSH_NO_PREFETCH);

    // After: wait for idle, system membar, then the signals and our seqno (64-bit releases).
    {
        uint32_t n = 0;
        w[n++] = pbHeader(0, NVC46F_WFI_, 1);
        w[n++] = 1;                                          // SCOPE_ALL
        w[n++] = pbHeader(0, NVC46F_MEM_OP_A_, 4);
        w[n++] = 0;
        w[n++] = 0;
        w[n++] = 0;                                          // MEM_OP_C: SYS_MEMBAR
        w[n++] = NVC46F_MEM_OP_D_MEMBAR;
        for (uint32_t i = 0; i < h.n_signal; i++)
            n += semOp(w + n, KVA_SYNC + 8 * (signals[i].sync - 1), signals[i].value, NVC46F_SEM_RELEASE_64_WFI);
        n += semOp(w + n, KVA_SYNC + 8 * x->seqSync, seq, NVC46F_SEM_RELEASE_64_WFI);
        if (intrOn_) {                                       // wakes SYNC_WAIT (item 4)
            w[n++] = pbHeader(0, NVC46F_NON_STALL_INTERRUPT_, 1);
            w[n++] = 0;
        }
        for (uint32_t i = 0; i < n; i++)
            praminWr32(pushPa + 512 + 4 * i, w[i]);
        gp[ngp++] = gpEntry(pushVa + 512, n, false);
    }

    for (uint32_t i = 0; i < ngp; i++) {
        uint64_t e = x->ring + 8 * x->put;
        praminWr32(e, (uint32_t)gp[i]);
        praminWr32(e + 4, (uint32_t)(gp[i] >> 32));
        x->put = (x->put + 1) % RING_ENTRIES;
    }
    x->putAfter[slot] = (uint16_t)x->put;
    praminWr32(x->userd + NVC46F_USERD_GPPut, x->put);
    (void)praminRd32(x->userd + NVC46F_USERD_GPPut);       // post the writes
    praminRestore();
    wr32(NV_VF_DOORBELL, x->token);
    *seqOut = seq;
    autoBoostLocked(backlog);                                // after the doorbell: work isn't held up
    return kIOReturnSuccess;
}

// Checks the sync values: about 50 us spinning, then sleeps until a wakeup (non-stall interrupt,
// SYNC_SIGNAL, close) or a deadline: 2 ms with interrupts on, 100 us without (polling).
IOReturn NVBringup::syncWait(GpuConn *c, const uint8_t *buf, uint32_t len, uint64_t *index)
{
    nvmac_wait_hdr h;
    if (len < sizeof(h))
        return kIOReturnBadArgument;
    memcpy(&h, buf, sizeof(h));
    if (!h.count || h.count > NVMAC_MAX_WAIT_ENTRIES || len != sizeof(h) + h.count * sizeof(nvmac_sync_point))
        return kIOReturnBadArgument;
    const nvmac_sync_point *e = (const nvmac_sync_point *)(buf + sizeof(h));
    for (uint32_t i = 0; i < h.count; i++)
        if (!e[i].sync || e[i].sync > NVMAC_SYNC_COUNT || !c->syncUsed[e[i].sync - 1])
            return kIOReturnBadArgument;
    bool all = h.flags & NVMAC_WAIT_ALL;
    uint64_t deadline = 0, now = 0;
    nanoseconds_to_absolutetime(h.timeout_ns < (1ull << 62) ? h.timeout_ns : (1ull << 62), &deadline);
    deadline += mach_absolute_time();
    for (uint32_t iter = 0;; iter++) {
        uint64_t gen = waitGen_;                // a wakeup after this point cancels the sleep below
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint32_t met = 0, first = h.count;
        for (uint32_t i = 0; i < h.count; i++)
            if (*syncSlot(c, e[i].sync - 1) >= e[i].value) {
                met++;
                if (first == h.count)
                    first = i;
            }
        if (all ? met == h.count : met > 0) {
            *index = all ? 0 : first;
            return kIOReturnSuccess;
        }
        if (c->dead || c->anyLost)
            return kIOReturnNoDevice;
        now = mach_absolute_time();
        if (!h.timeout_ns || now >= deadline)
            return kIOReturnTimeout;
        if (iter < 50) {
            IODelay(1);
            continue;
        }
        uint64_t step = 0;
        clock_interval_to_deadline(intrOn_ ? 2000 : 100, kMicrosecondScale, &step);
        if (step > deadline)
            step = deadline;
        int wr = THREAD_AWAKENED;
        IOLockLock(waitLock_);
        if (waitGen_ == gen)
            wr = IOLockSleepDeadline(waitLock_, (event_t)&waitGen_, step, THREAD_INTERRUPTIBLE);
        IOLockUnlock(waitLock_);
        if (wr == THREAD_INTERRUPTED)
            return kIOReturnAborted;
    }
}

// ---- Dispatch -------------------------------------------------------------------------------

static IOReturn readStructIn(IOExternalMethodArguments *a, uint8_t **out, uint32_t *len, uint32_t cap)
{
    uint64_t n = a->structureInputDescriptor ? a->structureInputDescriptor->getLength() : a->structureInputSize;
    if (!n || n > cap)
        return kIOReturnBadArgument;
    uint8_t *b = (uint8_t *)IOMalloc(n);
    if (!b)
        return kIOReturnNoMemory;
    if (a->structureInputDescriptor) {
        IOMemoryDescriptor *md = a->structureInputDescriptor;
        IOReturn r = md->prepare(kIODirectionOut);
        uint64_t got = r == kIOReturnSuccess ? md->readBytes(0, b, n) : 0;
        if (r == kIOReturnSuccess)
            md->complete(kIODirectionOut);
        if (got != n) {
            IOFree(b, n);
            return kIOReturnVMError;
        }
    } else {
        memcpy(b, a->structureInput, n);
    }
    *out = b;
    *len = (uint32_t)n;
    return kIOReturnSuccess;
}

#define NEED(in, out) \
    do { if (a->scalarInputCount != (in) || a->scalarOutputCount != (out)) return kIOReturnBadArgument; } while (0)


// The device info NVK builds its physical device from (NVMAC_GET_INFO, NVMAC_POWER_INFO).
void NVBringup::fillInfo(nvmac_info *i)
{
    GspState *g = gsp_;
    *i = {};
    i->version = NVMAC_ABI_VERSION;
    i->features = NVMAC_FEATURES;
    i->device_id = pci_->configRead16(kIOPCIConfigDeviceID);
    i->chipset = (uint16_t)chipset_;
    i->pci_bus = pci_->getBusNumber();
    i->pci_dev = pci_->getDeviceNumber();
    i->pci_func = pci_->getFunctionNumber();
    i->revision = pci_->configRead8(kIOPCIConfigRevisionID);
    i->sm = g->sm;
    i->gpc_count = g->gpcCount;
    i->tpc_count = g->tpcCount;
    i->mp_per_tpc = g->smPerTpc;
    i->max_warps_per_mp = g->maxWarps;
    i->cls_copy = chip_->arch->cls.copy;
    i->cls_eng3d = chip_->arch->cls.eng3d;
    i->cls_compute = chip_->arch->cls.compute;
    i->cls_gpfifo = chip_->arch->cls.gpfifo;
    i->cls_vdec = g->nvdecCtxSize ? chip_->arch->cls.vdec : 0;
    i->vram_size = g->vram->limit - g->vram->base + 1;
    i->vram_used = nv_vram_used(g->vram);
    i->bar1_size = g->bar1Heap ? g->bar1Heap->limit - g->bar1Heap->base + 1 - g->ptPoolSize : 0;
    i->bar1_used = g->bar1Heap ? nv_vram_used(g->bar1Heap) - g->ptPoolSize : 0;
    i->va_start = USER_VA_START;
    i->va_end = g->vaLimit < USER_VA_END ? g->vaLimit : USER_VA_END;
    i->bind_align = 0x1000;
    i->max_pushes = NVMAC_MAX_PUSHES;
    i->max_waits = NVMAC_MAX_WAITS;
    i->max_signals = NVMAC_MAX_SIGNALS;
    i->max_bind_ops = NVMAC_MAX_BIND_OPS;
    i->max_wait_entries = NVMAC_MAX_WAIT_ENTRIES;
    i->sync_count = NVMAC_SYNC_COUNT;
    OSString *name = OSDynamicCast(OSString, getProperty("NVGpuName"));
    strlcpy(i->name, name ? name->getCStringNoCopy() : "NVIDIA GPU", sizeof(i->name));
}

IOReturn NVBringup::gpuCall(GpuConn *c, uint32_t selector, IOExternalMethodArguments *a)
{
    // Lock-free calls first.
    if (selector == NVMAC_GET_TIMESTAMP) {
        NEED(0, 1);
        a->scalarOutput[0] = gpuTime();
        return kIOReturnSuccess;
    }
    if (selector == NVMAC_DISPLAY_FLIP) {
        NEED(1, 0);
        return displayFlip(c, (uint32_t)a->scalarInput[0]);
    }
    if (selector == NVMAC_SYNC_WAIT) {
        NEED(0, 1);
        uint8_t *b;
        uint32_t n;
        IOReturn r = readStructIn(a, &b, &n, sizeof(nvmac_wait_hdr) + NVMAC_MAX_WAIT_ENTRIES * sizeof(nvmac_sync_point));
        if (r != kIOReturnSuccess)
            return r;
        uint64_t idx = 0;
        r = syncWait(c, b, n, &idx);
        IOFree(b, n);
        a->scalarOutput[0] = idx;
        return r;
    }

    uint8_t *b = nullptr;
    uint32_t n = 0;
    if (selector == NVMAC_VM_BIND || selector == NVMAC_EXEC) {
        uint32_t cap = selector == NVMAC_VM_BIND
            ? (uint32_t)(sizeof(nvmac_bind_hdr) + NVMAC_MAX_BIND_OPS * sizeof(nvmac_bind_op))
            : (uint32_t)(sizeof(nvmac_exec_hdr) + (NVMAC_MAX_WAITS + NVMAC_MAX_SIGNALS) * sizeof(nvmac_sync_point) +
                         NVMAC_MAX_PUSHES * sizeof(nvmac_push));
        IOReturn r = readStructIn(a, &b, &n, cap);
        if (r != kIOReturnSuccess)
            return r;
    }

    IOLockLock(gspLock_);
    GspState *g = gsp_;
    IOReturn r = kIOReturnSuccess;
    if (!g || !g->booted || c->dead) {
        r = kIOReturnNoDevice;
        goto out;
    }
    switch (selector) {
    case NVMAC_GET_INFO: {
        if (a->structureOutputSize < sizeof(nvmac_info) || !a->structureOutput) {
            r = kIOReturnBadArgument;
            break;
        }
        nvmac_info i;
        fillInfo(&i);
        memcpy(a->structureOutput, &i, sizeof(i));
        a->structureOutputSize = sizeof(i);
        break;
    }
    case NVMAC_MEM_ALLOC: {
        if (a->scalarInputCount != 4 || a->scalarOutputCount != 2) {
            r = kIOReturnBadArgument;
            break;
        }
        uint32_t h = 0;
        uint64_t size = 0;
        r = memAlloc(c, a->scalarInput[0], a->scalarInput[1], (uint32_t)a->scalarInput[2],
                     (uint32_t)a->scalarInput[3], &h, &size);
        a->scalarOutput[0] = h;
        a->scalarOutput[1] = size;
        break;
    }
    case NVMAC_MEM_IMPORT: {
        if (a->scalarInputCount != 2 || a->scalarOutputCount != 2) {
            r = kIOReturnBadArgument;
            break;
        }
        uint32_t h = 0;
        uint64_t size = 0;
        r = memImport(c, a->scalarInput[0], a->scalarInput[1], &h, &size);
        a->scalarOutput[0] = h;
        a->scalarOutput[1] = size;
        break;
    }
    case NVMAC_MEM_FREE: {
        GpuMem *m = a->scalarInputCount == 1 ? findMem(c, a->scalarInput[0]) : nullptr;
        if (!m)
            r = kIOReturnBadArgument;
        else
            memFree(c, m, true);
        break;
    }
    case NVMAC_MEM_MAP: {
        GpuMem *m = a->scalarInputCount == 1 && a->scalarOutputCount == 2 ? findMem(c, a->scalarInput[0]) : nullptr;
        uint64_t addr = 0;
        r = m ? memMap(c, m, &addr) : kIOReturnBadArgument;
        if (r == kIOReturnSuccess) {
            a->scalarOutput[0] = addr;
            a->scalarOutput[1] = m->size;
        }
        break;
    }
    case NVMAC_MEM_UNMAP: {
        GpuMem *m = a->scalarInputCount == 1 ? findMem(c, a->scalarInput[0]) : nullptr;
        if (!m)
            r = kIOReturnBadArgument;
        else
            OSSafeReleaseNULL(m->userMap);
        break;
    }
    case NVMAC_VM_BIND:
        r = vmBind(c, b, n);
        break;
    case NVMAC_CTX_CREATE: {
        if (a->scalarInputCount != 1 || a->scalarOutputCount != 2) {
            r = kIOReturnBadArgument;
            break;
        }
        uint32_t h = 0;
        r = ctxCreate(c, (uint32_t)a->scalarInput[0], &h);
        a->scalarOutput[0] = h;
        a->scalarOutput[1] = r == kIOReturnSuccess ? c->ctxs[h - 1]->seqSync + 1 : 0;
        break;
    }
    case NVMAC_CTX_DESTROY: {
        GpuCtx *x = a->scalarInputCount == 1 ? findCtx(c, a->scalarInput[0]) : nullptr;
        if (!x)
            r = kIOReturnBadArgument;
        else
            ctxDestroy(x);
        break;
    }
    case NVMAC_EXEC: {
        if (a->scalarOutputCount != 1) {
            r = kIOReturnBadArgument;
            break;
        }
        uint64_t seq = 0;
        r = exec(c, b, n, &seq);
        a->scalarOutput[0] = seq;
        break;
    }
    case NVMAC_SYNC_CREATE: {
        if (a->scalarInputCount != 1 || a->scalarOutputCount != 2) {
            r = kIOReturnBadArgument;
            break;
        }
        int s = allocSync(c, 1);
        if (s < 0) {
            r = kIOReturnNoResources;
            break;
        }
        *syncSlot(c, (uint32_t)s) = a->scalarInput[0];
        a->scalarOutput[0] = (uint64_t)s + 1;
        a->scalarOutput[1] = 8 * (uint64_t)s;
        break;
    }
    case NVMAC_SYNC_DESTROY: {
        uint64_t s = a->scalarInputCount == 1 ? a->scalarInput[0] : 0;
        if (!s || s > NVMAC_SYNC_COUNT || c->syncUsed[s - 1] != 1)
            r = kIOReturnBadArgument;
        else
            c->syncUsed[s - 1] = 0;
        break;
    }
    case NVMAC_SYNC_SIGNAL: {
        uint64_t s = a->scalarInputCount == 2 ? a->scalarInput[0] : 0, v = s ? a->scalarInput[1] : 0;
        if (!s || s > NVMAC_SYNC_COUNT || c->syncUsed[s - 1] != 1) {
            r = kIOReturnBadArgument;
            break;
        }
        volatile uint64_t *p = syncSlot(c, (uint32_t)(s - 1));
        for (;;) {
            uint64_t cur = *p;
            if (cur >= v || OSCompareAndSwap64(cur, v, (volatile UInt64 *)p))
                break;
        }
        wakeWaiters();
        break;
    }
    case NVMAC_DISPLAY_MAP: {
        if (a->scalarOutputCount != 5) {
            r = kIOReturnBadArgument;
            break;
        }
        if (!g->dispLit) {
            r = kIOReturnNotReady;
            break;
        }
        if (c->dispMap && c->dispMapBar1 != g->dispFbBar1)
            OSSafeReleaseNULL(c->dispMap);      // a new surface since (a bigger mode on replug)
        if (!c->dispMap) {
            c->dispMapBar1 = g->dispFbBar1;
            IODeviceMemory *bar1 = pci_->getDeviceMemoryWithIndex(1);
            c->dispMap = bar1 ? bar1->createMappingInTask(c->task, 0, kIOMapAnywhere | kIOMapWriteCombineCache,
                                                          g->dispFbBar1, g->dispFbSize)
                              : nullptr;
        }
        if (!c->dispMap) {
            r = kIOReturnVMError;
            break;
        }
        a->scalarOutput[0] = c->dispMap->getAddress();
        a->scalarOutput[1] = g->dispFbSize;
        a->scalarOutput[2] = g->dispW;
        a->scalarOutput[3] = g->dispH;
        a->scalarOutput[4] = g->dispPitch;
        break;
    }
    case NVMAC_DISPLAY_MEM: {
        const uint64_t buf = a->scalarInputCount ? a->scalarInput[0] : 0;
        if (a->scalarOutputCount != 5 || a->scalarInputCount > 1 || buf > 2) {
            r = kIOReturnBadArgument;
            break;
        }
        const NVDispMem &fbm = buf == 2 ? g->disp.fbC : buf ? g->disp.fbB : g->disp.fb;
        if (!g->dispLit || !fbm.pa) {
            r = kIOReturnNotReady;
            break;
        }
        uint32_t slot;
        if ((r = memSlot(c, &slot)) != kIOReturnSuccess)
            break;
        GpuMem *m = new GpuMem;
        if (!m) {
            r = kIOReturnNoMemory;
            break;
        }
        m->flags = NVMAC_MEM_VRAM;
        m->vram = fbm.pa;
        m->size = fbm.size & ~0xfffull;             // exactly the surface: no neighbouring VRAM
        m->kind = (uint8_t)userKind(0);
        m->borrowed = true;
        m->handle = slot + 1;
        c->mems[slot] = m;
        a->scalarOutput[0] = m->handle;
        a->scalarOutput[1] = m->size;
        a->scalarOutput[2] = g->dispW;
        a->scalarOutput[3] = g->dispH;
        a->scalarOutput[4] = g->dispPitch;
        break;
    }
    case NVMAC_MAP_SYNC_PAGE: {
        if (a->scalarOutputCount != 2) {
            r = kIOReturnBadArgument;
            break;
        }
        if (!c->syncMap)
            c->syncMap = c->syncs.mem->createMappingInTask(c->task, 0, kIOMapAnywhere | kIOMapReadOnly, 0,
                                                           c->syncs.size);
        if (!c->syncMap) {
            r = kIOReturnVMError;
            break;
        }
        a->scalarOutput[0] = c->syncMap->getAddress();
        a->scalarOutput[1] = c->syncs.size;
        break;
    }
    case NVMAC_CTX_STATUS: {
        GpuCtx *x = a->scalarInputCount == 1 && a->scalarOutputCount == 3 ? findCtx(c, a->scalarInput[0]) : nullptr;
        if (!x) {
            r = kIOReturnBadArgument;
            break;
        }
        a->scalarOutput[0] = x->lost ? 1 : 0;
        a->scalarOutput[1] = x->exceptType;
        a->scalarOutput[2] = *syncSlot(c, x->seqSync);
        break;
    }
    default:
        r = kIOReturnBadArgument;
        break;
    }
out:
    IOLockUnlock(gspLock_);
    if (b)
        IOFree(b, n);
    return r;
}

// ---- Monitoring ---------------------------------------------------------------------

// Caller (gspPoll, every 100 ms) holds gspLock_ with GSP-RM running. A sample is busy when
// any live context has EXECs whose seqno has not been released yet (this counts work waiting
// on a GPU-side semaphore too). Every 10 samples, publishes NVStats: utilization over that
// second, temperature, heap usage and client counts. Readable by anyone through ioreg.
void NVBringup::updateStats()
{
    GspState *g = gsp_;
    uint32_t nConns = 0, nCtxs = 0;
    bool busy = false;
    for (uint32_t i = 0; i < kMaxConns; i++) {
        GpuConn *c = g->conns[i];
        if (!c || c->dead)
            continue;
        nConns++;
        for (uint32_t j = 0; j < kMaxCtx; j++) {
            GpuCtx *x = c->ctxs[j];
            if (!x || x->lost)
                continue;
            nCtxs++;
            if (*syncSlot(c, x->seqSync) < x->submitted)
                busy = true;
        }
    }
    statBusy_ += busy;
    if (++statTicks_ < 10)
        return;

    OSDictionary *d = OSDictionary::withCapacity(10);
    if (d) {
        auto put = [d](const char *k, uint64_t v, uint32_t bits) {
            OSNumber *n = OSNumber::withNumber(v, bits);
            if (n) {
                d->setObject(k, n);
                n->release();
            }
        };
        put("Utilization", statBusy_ * 100 / statTicks_, 32);
        // Priv-protected reads return 0xbadf....., which has the valid bit set: rule it out.
        uint32_t ts = rd32(NV_THERM_TSENSOR);
        uint32_t temp32 = (ts & 0x1fff8) >> 3;          // 1/32 °C
        if ((ts & (1u << 29)) && (ts >> 20) != 0xbad && temp32 < 150 * 32)
            put("Temperature", temp32 * 100 / 32, 32);  // centi-°C
        uint64_t heap = g->vram ? g->vram->limit - g->vram->base + 1 : 0;
        put("VramTotal", heap, 64);
        put("VramUsed", g->vram ? nv_vram_used(g->vram) : 0, 64);
        put("Bar1Total", g->bar1Heap ? g->bar1Heap->limit - g->bar1Heap->base + 1 - g->ptPoolSize : 0, 64);
        put("Bar1Used", g->bar1Heap ? nv_vram_used(g->bar1Heap) - g->ptPoolSize : 0, 64);
        put("Bar1Held", bar1HoldBytes_, 64);
        put("Connections", nConns, 32);
        put("Contexts", nCtxs, 32);
        put("Sequence", ++statSeq_, 32);
        setProperty("NVStats", d);
        d->release();
    }
    statTicks_ = statBusy_ = 0;
}
