// Driver-private state shared by NVGsp.cpp (GSP-RM boot, Phase 4) and NVGpu.cpp (the user
// GPU interface, Phase 5). Kernel only.
#pragma once

#include "NVBringup.hpp"
#include "nv_gsp.h"
#include "nv_gsp_rm.h"
#include "nv_gsp_static.h"
#include "nv_vram.h"
#include "nv_mmu.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <string.h>

static const uint64_t PAGE_4K = 0x1000;
static const uint32_t MSG_MAX = GSP_MSG_QUEUE_ELEMENT_SIZE_MAX;

// A kernel buffer the GPU may read and write, mapped through the IOMMU, with the
// device address of each 4 KiB page. userShared: may also be mapped into a user task.
struct DmaBuf {
    IOBufferMemoryDescriptor *mem = nullptr;
    IODMACommand *dma = nullptr;
    uint8_t  *va = nullptr;
    uint64_t  size = 0;
    uint64_t *pages = nullptr;
    uint64_t  npages = 0;

    uint64_t iova() const { return pages ? pages[0] : 0; }

    bool contiguous() const
    {
        for (uint64_t i = 1; i < npages; i++)
            if (pages[i] != pages[0] + i * PAGE_4K)
                return false;
        return npages > 0;
    }

    // Returns nullptr on success, else what failed.
    const char *alloc(uint64_t bytes, bool physContig, bool userShared = false)
    {
        size   = (bytes + PAGE_4K - 1) & ~(PAGE_4K - 1);
        npages = size / PAGE_4K;
        mem = IOBufferMemoryDescriptor::withOptions(
            kIODirectionInOut | (physContig ? kIOMemoryPhysicallyContiguous : 0) |
            (userShared ? kIOMemoryKernelUserShared : 0), size, PAGE_4K);
        if (!mem)
            return "allocation";
        va = (uint8_t *)mem->getBytesNoCopy();
        memset(va, 0, size);
        pages = (uint64_t *)IOMalloc(npages * sizeof(uint64_t));
        if (!pages)
            return "allocation";
        dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0,
                                              IODMACommand::kMapped, 0, PAGE_4K);
        if (!dma || dma->setMemoryDescriptor(mem, true) != kIOReturnSuccess)
            return "IOMMU mapping";
        uint64_t off = 0, n = 0;
        while (off < size) {
            IODMACommand::Segment64 seg[16];
            UInt32 nseg = 16;
            if (dma->gen64IOVMSegments(&off, seg, &nseg) != kIOReturnSuccess || !nseg)
                return "IOMMU segments";
            for (UInt32 s = 0; s < nseg; s++) {
                if ((seg[s].fIOVMAddr | seg[s].fLength) & (PAGE_4K - 1))
                    return "unaligned IOMMU segment";
                for (uint64_t a = 0; a < seg[s].fLength && n < npages; a += PAGE_4K)
                    pages[n++] = seg[s].fIOVMAddr + a;
            }
        }
        return n == npages ? nullptr : "IOMMU page count";
    }

    void free()
    {
        if (dma) {
            dma->clearMemoryDescriptor(true);
            dma->release();
        }
        OSSafeReleaseNULL(mem);
        if (pages)
            IOFree(pages, npages * sizeof(uint64_t));
        *this = DmaBuf();
    }
};

// A growable array of 64-bit values (VRAM addresses of tables, link entries).
struct U64Vec {
    uint64_t *v = nullptr;
    uint32_t n = 0, cap = 0;

    bool push(uint64_t x)
    {
        if (n == cap) {
            uint32_t nc = cap ? cap * 2 : 64;
            uint64_t *nv = (uint64_t *)IOMalloc(nc * sizeof(uint64_t));
            if (!nv)
                return false;
            if (v) {
                memcpy(nv, v, n * sizeof(uint64_t));
                IOFree(v, cap * sizeof(uint64_t));
            }
            v = nv;
            cap = nc;
        }
        v[n++] = x;
        return true;
    }

    void free()
    {
        if (v)
            IOFree(v, cap * sizeof(uint64_t));
        *this = U64Vec();
    }
};

// One kernel-owned channel (copy-engine utility channel, boot-time tests): its memory
// (VRAM), GPU VAs of ring/pushbuffer/semaphore, doorbell token and ring position.
struct NVBringup::TestChan {
    uint32_t handle = 0, chid = 0, engine = 0, token = 0, put = 0;
    uint64_t inst = 0, userd = 0, mthd = 0, ring = 0, pb = 0, sem = 0;
    uint64_t vaRing = 0, vaPb = 0, vaSem = 0;
};

// GR context buffers shared by every graphics channel (legacy mode: allocated once per GPU,
// mapped into each VA space; kgraphicsAllocGlobalCtxBuffers, nouveau r535_gr_oneinit).
struct GrGlobalBuf {
    const char *name;
    uint32_t id;            // PROMOTE_ID_*
    uint64_t pa, size, off; // VRAM, bytes, offset from the global-buffer VA base
    uint32_t pte;           // NV_MMU_PTE_* flags
    bool     init;          // promoted with PA (GSP-RM initializes it)
};

enum { kMaxConns = 32, kMaxChid = 256, kMaxGrGlobal = 6 };

struct NVBringup::GspState {
    IOBufferMemoryDescriptor *fw[kFwCount] = {};
    DmaBuf elf, radix, bl, sig, meta, args, rmargs, shm, logs[NV_GSP_LOG_COUNT];
    nv_gsp_shm_layout shml = {};
    uint32_t msgRptr = 0, rxSeq = 0;
    uint32_t txSeq = 2;             // SET_SYSTEM_INFO and SET_REGISTRY were 0 and 1
    uint32_t waitFunction = 0, waitResult = 0;  // reply awaited by waitGspReply
    bool     waitDone = false;
    uint8_t *waitBuf = nullptr;                 // optional copy of the reply payload
    uint32_t waitCap = 0, waitLen = 0;
    uint32_t hClient = 0, hDevice = 0, hSubdevice = 0;  // our RM objects (Phase 4)
    uint32_t hVaSpace = 0;
    uint64_t pd3 = 0;               // root of the kernel VA space's page tables (VRAM)
    uint64_t vaBase = 0, vaLimit = 0;   // GSP-RM's usable VA range of a VA space [base, limit)
    uint32_t hChannel = 0;          // copy-engine utility channel (Phase 4 step 5)
    uint32_t mthdSize = 0;          // CE fault method buffer size, per channel
    uint32_t hIntClient = 0, hIntSubdevice = 0;   // GSP-RM's internal client (static info)
    uint64_t bar1Pde = 0;           // GSP-RM's BAR1 root page directory (static info)
    // BAR1 (Phase 4 step 6, Phase 5): our VA allocator over the aperture, the tables we hung
    // into GSP-RM's root and where; all undone before GSP-RM unloads.
    nv_vram_heap *bar1Heap = nullptr;
    uint64_t bar1Aperture = 0, bar1Phys = 0;
    U64Vec   bar1Tables, bar1Links;
    // A small zeroed VRAM block (initBar1) that held slices (NVBringup::bar1Held_) point at
    // read-only instead of the VRAM they used to back, so a leftover CPU duplicate reads only
    // zeroes. Fresh every boot; held slices are re-pointed at the new one (bar1ReserveHeld).
    uint64_t bar1DummyPa = 0;
    // Set when a held slice could not be reserved this boot (initBar1): its range is free in
    // bar1Heap but may still be mapped by a stale duplicate, so bar1Map refuses everything.
    bool     bar1Off = false;
    // Page-table pool: PD0s and PTs of connection VA spaces come from this VRAM, mapped once
    // through BAR1 for direct (uncached) CPU writes, with a CPU shadow for reads. Only we
    // write these tables (GSP-RM fills PDEs into the top levels, which stay outside).
    uint64_t ptPoolPa = 0, ptPoolBar1 = 0, ptPoolSize = 0;
    IOMemoryMap *ptPoolMap = nullptr;
    volatile uint64_t *ptPoolCpu = nullptr;
    uint64_t *ptPoolShadow = nullptr;
    uint8_t  *ptPoolUsed = nullptr;         // one byte per 4 KiB table
    uint32_t  ptPoolNext = 0;
    nv_vram_heap *vram = nullptr;   // host-owned VRAM (usable FB region from static info)
    bool     linked = false;        // the GSP has created the status queue
    uint8_t *msgBuf = nullptr;      // MSG_MAX bytes: one message being built or read
    bool touched = false;   // GSP/SEC2 were reset or started: no retry until the next cold boot
    bool booted  = false;
    uint32_t appVersion = 0;    // the RISC-V bootloader's app version (NV_FALCON_OS, written at boot and resume)
    bool leadWas = false, leadSet = false;

    // Phase 5: engines, the utility channel, GR context buffers, user connections
    uint32_t engines[32] = {}, nEngines = 0;
    uint32_t ceEngine = 0;          // copy engine of the utility channel
    uint32_t grCeEngine = 0;        // copy engine accepted on a graphics channel (found on first use)
    TestChan util;
    bool     utilOk = false;
    uint32_t utilSeq = 0;
    uint32_t grMainSize = 0, grMainAlign = 0, grPatchSize = 0;
    GrGlobalBuf grGlobal[kMaxGrGlobal] = {};
    uint32_t nGrGlobal = 0;
    uint64_t grGlobalSpan = 0;
    bool     grReady = false;
    uint32_t nvdecCtxSize = 0;      // NVDEC0 falcon context buffer per channel; 0 = no NVDEC
    uint64_t dispInst = 0;          // display instance memory (64 KiB VRAM), 0 = display not set up
    uint32_t hDisp = 0;             // NV04_DISPLAY_COMMON under our device
    uint32_t dispModeId = 0, dispModeProto = 0;   // nvdisp=2: the output and EDID modesetDisplay uses
    uint8_t  dispModeEdid[128] = {};
    // nvdisp=2: the display engine as set up once per GSP boot (dispInit) and the output it lights
    // (dispSetMode, from the boot probe or the hot-plug poll). Kernel CPU mappings through BAR1.
    struct DispHw {
        NVDispMem inst, pbCore, pbWndw, sync, ilut, olut, fb, fbB;      // fb, fbB: the two scanout buffers
        uint32_t front = 0;             // which one window 0 scans out (0: fb)
        uint32_t hRoot = 0;
        uint32_t coreCur = 0, wndwCur = 0;      // push buffer positions, in words
        bool     wanted = false, ready = false;
        uint32_t nCand = 0, candId[4] = {}, candProto[4] = {};   // the TMDS outputs (HDMI/DVI)
        uint32_t litId = 0, sorIdx = ~0u;
        uint32_t pendId = 0, pendCount = 0, tick = 0;            // hot-plug debounce
        uint32_t failId = 0;            // an output that failed to light: not retried until it's unplugged
        uint32_t nErr = 0;
        // The lit mode (dispSetMode): the EDID's preferred timing and the surface layout.
        uint8_t  edid[128] = {};
        uint32_t proto = 0, w = 0, h = 0, pitch = 0, pclkKhz = 0, htotal = 0, vtotal = 0;
        uint32_t hsyncStart = 0, hsyncWidth = 0, vsyncStart = 0, vsyncWidth = 0, syncFlags = 0;
    } disp;
    bool     dispLit = false;          // a modeset scans out: the GPU stays powered
    uint64_t dispFbBar1 = 0, dispFbSize = 0;   // the scanout surface: BAR1 offset and size
    uint32_t dispW = 0, dispH = 0, dispPitch = 0;
    uint16_t tpcCount = 0;
    uint8_t  gpcCount = 0, smPerTpc = 0, maxWarps = 0, sm = 0;
    GpuConn *conns[kMaxConns] = {};
    GpuCtx  *chidOwner[kMaxChid] = {};  // chid -> user context (RC events), 0 = free
    uint32_t nextHandle = 0xcaf00000;
    bool     chidUsed[kMaxChid] = {};

    // keepLogs: after a failed boot the falcons are stopped, and the log buffers
    // stay readable (tools/nvgsp logs) until the driver stops.
    void freeDma(bool keepLogs = false)
    {
        DmaBuf *all[] = { &elf, &radix, &bl, &sig, &meta, &args, &rmargs, &shm,
                          &logs[0], &logs[1], &logs[2], &logs[3] };
        for (DmaBuf *b : all)
            if (!keepLogs || b < &logs[0] || b > &logs[NV_GSP_LOG_COUNT - 1])
                b->free();
    }
};

static inline void put32(uint8_t *p, uint32_t off, uint32_t v) { memcpy(p + off, &v, 4); }
static inline void put64(uint8_t *p, uint32_t off, uint64_t v) { memcpy(p + off, &v, 8); }

static inline void putMemDesc(uint8_t *p, uint32_t off, uint64_t base, uint64_t size)
{
    put64(p, off + NV_MEMDESC_base, base);
    put64(p, off + NV_MEMDESC_size, size);
    put32(p, off + NV_MEMDESC_addressSpace, 2);     // ADDR_FBMEM
    put32(p, off + NV_MEMDESC_cacheAttrib, 1);      // NV_MEMORY_UNCACHED
}

// Volta+ host method header: incrementing, count words, subchannel, method address.
static inline uint32_t pbHeader(uint32_t subch, uint32_t method, uint32_t count)
{
    return (1u << 29) | (count << 16) | (subch << 13) | (method >> 2);
}

static inline bool isCopyEngine(uint32_t t)
{
    return (t >= NV2080_ENGINE_TYPE_COPY0_ && t < NV2080_ENGINE_TYPE_COPY0_ + 10) ||
           (t >= 0x34 && t < 0x34 + NV2080_ENGINE_TYPE_COPY_SIZE_ - 10);   // COPY10.. (cl2080_notification.h)
}
