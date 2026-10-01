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
#include "NVGpuUserClient.hpp"

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
    uint8_t  kind = 0;
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
    if (!gspRmAlloc(g->hClient, c.handle, 0xc5b50001, TURING_DMA_COPY_A_CLASS, ceAlloc, sizeof(ceAlloc), &st))
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
            pbHeader(subch, 0x000, 1), TURING_DMA_COPY_A_CLASS,
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
    // RM reports the SM hardware revision (TU117: 0x703); compilers want the ISA level, which
    // is 7.5 for every Turing chip (chipset 0x16x), as nouveau/NVK derive it from the chipset.
    g->sm = (chipset_ & 0xff0) == 0x160 ? 75 : v[2] ? (uint8_t)((v[2] >> 8) * 10 + (v[2] & 0xff)) : 75;
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
        NVC4B0_VIDEO_DECODER_CLASS);
}

// Item 4 (non-stall interrupts), step 1: read-only probe. Asks GSP-RM which interrupt vectors the
// CPU services, keeps GR0's and the copy engines' non-stall vectors for intrHwOn, and logs the CPU
// interrupt tree as GSP-RM left it. No register writes.
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
    uint32_t grNs = NV_INTR_VECTOR_INVALID, ceNs[10], nCe = 0;
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
        if (eng >= MC_ENGINE_IDX_CE0_ && eng < MC_ENGINE_IDX_CE0_ + 10 && vn != NV_INTR_VECTOR_INVALID && nCe < 10)
            ceNs[nCe++] = (uint32_t)(eng - MC_ENGINE_IDX_CE0_) << 16 | vn;
        // GR0 and the copy engines are where our channels run: their non-stall vectors are ours.
        bool ours = eng == MC_ENGINE_IDX_GR0_ || (eng >= MC_ENGINE_IDX_CE0_ && eng < MC_ENGINE_IDX_CE0_ + 10);
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
    g->bar1Phys = bar1->getPhysicalAddress();
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
    return gspRmAlloc(g->hClient, g->hDevice, handle, TURING_CHANNEL_GPFIFO_A_CLASS, ch, sizeof(ch), &st);
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
    NVGpuUserClient *c = OSTypeAlloc(NVGpuUserClient);
    if (!c)
        return kIOReturnNoMemory;
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
        GpuMem **nm = (GpuMem **)IOMallocZero(nc * sizeof(GpuMem *));
        if (!nm)
            return kIOReturnNoMemory;
        if (c->mems) {
            memcpy(nm, c->mems, c->memCap * sizeof(GpuMem *));
            IOFree(c->mems, c->memCap * sizeof(GpuMem *));
        }
        c->mems = nm;
        c->memCap = nc;
    }

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
    if (m->vram)
        nv_vram_free(g->vram, m->vram);
    m->sys.free();
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
            !gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 1, TURING_A_CLASS, nullptr, 0, &st))
            goto fail;
        if ((engines & NVMAC_ENGINE_COMPUTE) &&
            !gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 2, TURING_COMPUTE_A_CLASS, nullptr, 0, &st))
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
                got = gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 3, TURING_DMA_COPY_A_CLASS,
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
        if (!gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 4, NVC4B0_VIDEO_DECODER_CLASS, bsp, sizeof(bsp),
                        &st))
            goto fail;
    } else {
        if (!rmScheduleChannel(x->hChannel, x->engineType, &x->token))
            goto fail;
        uint32_t ceAlloc[NVB0B5_ALLOC_SIZE / 4] = { NVB0B5_ALLOC_VERSION_1, g->ceEngine };
        if (!gspRmAlloc(g->hClient, x->hChannel, x->hChannel + 3, TURING_DMA_COPY_A_CLASS, ceAlloc,
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
    i->cls_copy = TURING_DMA_COPY_A_CLASS;
    i->cls_eng3d = TURING_A_CLASS;
    i->cls_compute = TURING_COMPUTE_A_CLASS;
    i->cls_gpfifo = TURING_CHANNEL_GPFIFO_A_CLASS;
    i->cls_vdec = g->nvdecCtxSize ? NVC4B0_VIDEO_DECODER_CLASS : 0;
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
