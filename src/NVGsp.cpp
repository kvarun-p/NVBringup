// Phase 3d step 3: boot GSP-RM (r570.144) on Turing, triggered by tools/nvgsp.
//
// Order follows NVIDIA's kgspInitRm / kgspBootstrap_TU102 (r570): FRTS already
// ran at boot, then
//   1. build the sysmem structures: ELF behind radix3 page tables, bootloader,
//      signature, WPR meta, LibOS args, RMARGS, log buffers, queues;
//   2. queue SET_SYSTEM_INFO and SET_REGISTRY (kgspQueueAsyncInitRpcs);
//   3. reset the GSP falcon (on Turing it then runs RISC-V), LibOS args -> its mailboxes;
//   4. reset SEC2, PIO-load booter_load (signature patched), WPR meta -> its
//      mailboxes, run it; booter copies GSP-RM into WPR2 and starts the RISC-V core;
//   5. app version -> GSP FALCON_OS, check RISC-V active;
//   6. service the status queue (sequencer, prints, errors) until GSP_INIT_DONE.
//
// Every buffer the GPU touches is mapped through the IOMMU (VT-d), so the GPU can
// reach only these buffers; addresses given to the GPU are device (IOVA) addresses.

#include <IOKit/IONVRAM.h>
#include "NVBringup.hpp"
#include "nv_regs.h"
#include "nv_gsp.h"
#include "nv_gsp_rm.h"
#include "nv_vbios.h"
#include "nv_fwsec.h"
#include "nv_gsp_static.h"
#include "nv_vram.h"
#include "nv_mmu.h"
#include "nv_gsp_state.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <pexpert/pexpert.h>
#include <string.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)
#define super IOService

static const char *const kFwNames[] = { "GSP ELF", "GSP bootloader", "booter_load", "booter_unload" };

static inline uint32_t vrd32(const uint8_t *p) { return *(volatile const uint32_t *)p; }
static inline void vwr32(uint8_t *p, uint32_t v) { *(volatile uint32_t *)p = v; }

// ---- Firmware from user space -------------------------------------------------

IOReturn NVBringup::setFirmware(uint32_t kind, IOMemoryDescriptor *src, uint64_t len)
{
    if (kind >= kFwCount || !len || len > (64ull << 20))
        return kIOReturnBadArgument;
    IOLockLock(gspLock_);
    IOReturn r = kIOReturnSuccess;
    IOBufferMemoryDescriptor *buf = nullptr;
    if (!gsp_)
        gsp_ = new GspState;
    if (!gsp_) {
        r = kIOReturnNoMemory;
    } else if (gsp_->touched) {
        r = kIOReturnBusy;
    } else if (!(buf = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut, len, PAGE_4K))) {
        r = kIOReturnNoMemory;
    } else if (src->readBytes(0, buf->getBytesNoCopy(), len) != len) {
        r = kIOReturnIOError;
        buf->release();
    } else {
        OSSafeReleaseNULL(gsp_->fw[kind]);
        gsp_->fw[kind] = buf;
        LOG("GSP: %s received, %llu bytes", kFwNames[kind], (unsigned long long)len);
    }
    IOLockUnlock(gspLock_);
    return r;
}

IOReturn NVBringup::readGspLog(uint32_t index, IOMemoryDescriptor *dst, uint64_t *len)
{
    if (index >= NV_GSP_LOG_COUNT)
        return kIOReturnBadArgument;
    IOLockLock(gspLock_);
    IOReturn r = kIOReturnNotReady;
    if (gsp_ && gsp_->logs[index].va) {
        uint64_t n = dst->getLength() < gsp_->logs[index].size ? dst->getLength() : gsp_->logs[index].size;
        *len = dst->writeBytes(0, gsp_->logs[index].va, n);
        r = *len == n ? kIOReturnSuccess : kIOReturnIOError;
    }
    IOLockUnlock(gspLock_);
    return r;
}

// Stops the GSP (so nothing DMAs into the buffers any more) and releases everything.
void NVBringup::freeGsp(bool stopFalcons)
{
    if (!gsp_)
        return;
    if (stopFalcons && gsp_->touched && bar0_) {
        falconReset(NV_PGSP_BASE);
        falconReset(NV_PSEC_BASE);
    }
    gsp_->freeDma();
    if (gsp_->leadSet && pci_)
        pci_->setBusLeadEnable(gsp_->leadWas);
    for (IOBufferMemoryDescriptor *&f : gsp_->fw)
        OSSafeReleaseNULL(f);
    if (gsp_->msgBuf)
        IOFree(gsp_->msgBuf, MSG_MAX);
    if (gsp_->vram)
        IOFree(gsp_->vram, sizeof(nv_vram_heap));
    delete gsp_;
    gsp_ = nullptr;
}

// ---- Boot ------------------------------------------------------------------------

IOReturn NVBringup::bootGsp()
{
    IOLockLock(gspLock_);
    IOReturn r = kIOReturnError;
    GspState *g = gsp_;
    uint8_t *tmp = nullptr;     // g->msgBuf, kept for the poller

    // ---- Gates: explicit opt-in, a fresh FRTS from this boot, nothing tried yet.
    uint32_t arm = 0;
    const char *why = nullptr;
    if (!PE_parse_boot_argn("nvgsp", &arm, sizeof(arm)) || arm != 1)
        why = "boot-arg nvgsp=1 not set";
    else if (!g || !g->fw[kFwGspElf] || !g->fw[kFwGspBootloader] || !g->fw[kFwBooterLoad] || !g->fw[kFwBooterUnload])
        why = "firmware not loaded";
    else if (g->booted)
        why = "GSP-RM already running";
    else if (g->touched)
        why = "an earlier attempt ran this boot; shut down fully before retrying";
    else if (const char *c = chipRefusal())
        why = c;
    else if (!frtsOk_)
        why = "FRTS did not succeed this boot";
    else if (nv_wpr2_addr(rd32(NV_PFB_PRI_MMU_WPR2_ADDR_LO)) != frtsAddr_)
        why = "WPR2 no longer starts at the FRTS region";
    else if (!(grd(NV_FALCON_HWCFG2) & NV_HWCFG2_RISCV))
        why = "GSP has no RISC-V core";
    if (why) {
        LOG("GSP: not booting: %s", why);
        IOLockUnlock(gspLock_);
        return kIOReturnNotPermitted;
    }

    // ---- Parse firmware (same code as the host tool)
    const uint8_t *elfFile = (const uint8_t *)g->fw[kFwGspElf]->getBytesNoCopy();
    uint64_t elfLen = g->fw[kFwGspElf]->getLength();
    uint64_t imgOff, imgSize, sigOff, sigSize;
    const char *sigName;
    nv_gspbl gbl;
    nv_wpr2_layout lay;
    nv_radix3_sizes rs;
    // One ELF per firmware family; the signature for the GSP's boot ROM differs per chip group.
    sigName = chip_->elf_sig;
    if (!nv_elf64_section(elfFile, elfLen, NV_GSP_ELF_IMAGE, &imgOff, &imgSize) ||
        !nv_elf64_section(elfFile, elfLen, sigName, &sigOff, &sigSize)) {
        LOG("GSP: ELF lacks .fwimage or %s", sigName);
        goto fail;
    }
    if (!nv_gspbl_parse((const uint8_t *)g->fw[kFwGspBootloader]->getBytesNoCopy(),
                        (uint32_t)g->fw[kFwGspBootloader]->getLength(), &gbl)) {
        LOG("GSP: %s", gbl.err);
        goto fail;
    }
    if (!chip_->arch->wpr2_layout(vramSize_, vgaAddr_, imgSize, gbl.size, sizeof(GspFwWprMeta), &lay) ||
        lay.frts_addr != frtsAddr_) {
        LOG("GSP: WPR2 layout inconsistent with FRTS at 0x%llx", (unsigned long long)frtsAddr_);
        goto fail;
    }
    if (!nv_radix3_plan(imgSize, &rs))
        goto fail;
    LOG("GSP: layout: WPR2 0x%llx..0x%llx, heap 0x%llx (%llu MiB), ELF 0x%llx, boot 0x%llx",
        (unsigned long long)lay.wpr2_addr, (unsigned long long)(lay.wpr2_addr + lay.wpr2_size),
        (unsigned long long)lay.heap_addr, (unsigned long long)(lay.heap_size >> 20),
        (unsigned long long)lay.elf_addr, (unsigned long long)lay.boot_addr);

    // ---- 1. Sysmem structures
    {
        struct { DmaBuf *b; uint64_t size; bool contig; const char *name; } plan[] = {
            { &g->elf,    imgSize,                     false, "ELF" },
            { &g->radix,  rs.table_pages * PAGE_4K,    false, "radix3" },
            { &g->bl,     gbl.size,                    true,  "bootloader" },
            { &g->sig,    sigSize,                     true,  "signature" },
            { &g->meta,   PAGE_4K,                     true,  "WPR meta" },
            { &g->args,   LIBOS_INIT_ARGUMENTS_SIZE,   true,  "LibOS args" },
            { &g->rmargs, PAGE_4K,                     true,  "RMARGS" },
            { &g->shm,    0,                           false, "queues" },
            { &g->logs[0], NV_GSP_LOG_SIZE, true, "LOGINIT" }, { &g->logs[1], NV_GSP_LOG_SIZE, true, "LOGINTR" },
            { &g->logs[2], NV_GSP_LOG_SIZE, true, "LOGRM" },   { &g->logs[3], NV_GSP_LOG_SIZE, true, "LOGMNOC" },
        };
        nv_gsp_shm_plan(&g->shml);
        plan[7].size = g->shml.total;
        for (auto &p : plan) {
            if (const char *err = p.b->alloc(p.size, p.contig)) {
                LOG("GSP: %s buffer (0x%llx bytes): %s failed", p.name, (unsigned long long)p.size, err);
                goto fail;
            }
            if (p.contig && !p.b->contiguous()) {
                LOG("GSP: %s buffer is not contiguous for the GPU", p.name);
                goto fail;
            }
        }
    }
    memcpy(g->elf.va, elfFile + imgOff, imgSize);
    memcpy(g->sig.va, elfFile + sigOff, sigSize);
    memcpy(g->bl.va, gbl.img, gbl.size);
    nv_radix3_fill(&rs, (uint64_t *)g->radix.va, g->radix.pages, g->elf.pages);

    // Queues: the PTE array for the whole area comes first (message_queue_cpu.c).
    memcpy(g->shm.va, g->shm.pages, g->shml.pte_count * sizeof(uint64_t));
    {
        msgqTxHeader tx;
        nv_gsp_cmdq_init(&g->shml, &tx);
        memcpy(g->shm.va + g->shml.cmdq_off, &tx, sizeof(tx));
    }
    // Log buffers: put pointer, then the buffer's own page addresses; LibOS gets page 0.
    uint64_t logPa[NV_GSP_LOG_COUNT];
    for (uint32_t i = 0; i < NV_GSP_LOG_COUNT; i++) {
        memcpy(g->logs[i].va + 8, g->logs[i].pages, g->logs[i].npages * sizeof(uint64_t));
        logPa[i] = g->logs[i].iova();
    }
    nv_gsp_rmargs_fill((GSP_ARGUMENTS_CACHED *)g->rmargs.va, &g->shml, g->shm.iova());
    nv_gsp_libos_args_fill((LibosMemoryRegionInitArgument *)g->args.va, logPa,
                           g->rmargs.iova(), g->rmargs.size);
    {
        nv_gsp_sysmem sm = {};
        sm.radix3_pa = g->radix.iova();
        sm.elf_size  = imgSize;
        sm.bl_pa = g->bl.iova();
        sm.bl_size = gbl.size;
        sm.bl_code_off = gbl.code_off;
        sm.bl_data_off = gbl.data_off;
        sm.bl_manifest_off = gbl.manifest_off;
        sm.sig_pa = g->sig.iova();
        sm.sig_size = sigSize;
        nv_gsp_wpr_meta_fill((GspFwWprMeta *)g->meta.va, &lay, &sm);
    }
    LOG("GSP: sysmem ready: meta 0x%llx, LibOS args 0x%llx, radix3 0x%llx (%llu pages), queues 0x%llx",
        (unsigned long long)g->meta.iova(), (unsigned long long)g->args.iova(),
        (unsigned long long)g->radix.iova(), (unsigned long long)rs.table_pages,
        (unsigned long long)g->shm.iova());

    // ---- 2. Queue SET_SYSTEM_INFO and SET_REGISTRY
    if (!g->msgBuf)
        g->msgBuf = (uint8_t *)IOMalloc(MSG_MAX);
    if (!(tmp = g->msgBuf))
        goto fail;
    {
        GspSystemInfo si;
        nv_gsp_pci_info pi = {};
        IODeviceMemory *m;
        if ((m = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0))) pi.bar0_pa = m->getPhysicalAddress();
        if ((m = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress1))) pi.bar1_pa = m->getPhysicalAddress();
        if ((m = pci_->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress3))) pi.bar3_pa = m->getPhysicalAddress();
        pi.bus = pci_->getBusNumber();
        pi.dev = pci_->getDeviceNumber();
        pi.fn  = pci_->getFunctionNumber();
        pi.vendor    = pci_->configRead16(kIOPCIConfigVendorID);
        pi.device    = pci_->configRead16(kIOPCIConfigDeviceID);
        pi.subvendor = pci_->configRead16(kIOPCIConfigSubSystemVendorID);
        pi.subdevice = pci_->configRead16(kIOPCIConfigSubSystemID);
        pi.revision  = pci_->configRead8(kIOPCIConfigRevisionID);
        nv_gsp_sysinfo_fill(&si, &pi);

        uint8_t reg[256];
        uint32_t regLen = nv_gsp_registry_pack(reg, sizeof(reg), nv_gsp_default_registry,
                                               nv_gsp_default_registry_count);
        uint8_t *cmdq = g->shm.va + g->shml.cmdq_off;
        uint32_t n;
        if (!regLen ||
            !(n = nv_gsp_msg_build(tmp, MSG_MAX, 0, NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO, &si, sizeof(si))) ||
            !nv_gsp_cmdq_push(cmdq, tmp, n, 0) ||
            !(n = nv_gsp_msg_build(tmp, MSG_MAX, 1, NV_VGPU_MSG_FUNCTION_SET_REGISTRY, reg, regLen)) ||
            !nv_gsp_cmdq_push(cmdq, tmp, n, 0)) {
            LOG("GSP: queueing the init messages failed");
            goto fail;
        }
        LOG("GSP: queued SET_SYSTEM_INFO (BAR0 0x%llx, %02x:%02x.%x) and SET_REGISTRY (%u bytes)",
            (unsigned long long)pi.bar0_pa, pi.bus, pi.dev, pi.fn, regLen);
    }

    // GSP-RM raises interrupts we don't service yet; keep legacy INTx from firing
    // on a shared line. Messages are polled instead.
    pci_->configWrite16(kIOPCIConfigCommand, pci_->configRead16(kIOPCIConfigCommand) | 0x0400);
    g->leadWas = pci_->setBusLeadEnable(true);
    g->leadSet = true;
    OSSynchronizeIO();
    wr32(NV_PGSP_QUEUE_HEAD(0), 0);     // doorbell, as NVIDIA does after each push

    // ---- 3-5. Start GSP-RM (HAL; on Turing booter_load on SEC2 starts the GSP's RISC-V core)
    g->touched = true;
    if (!(this->*hal_->gspStart)(gbl.app_version))
        goto fail;

    // ---- 6. Status queue until GSP_INIT_DONE
    {
        const uint32_t limitMs = 30000;
        uint32_t ms = 0;
        int res = 0;
        while (ms < limitMs && (res = serviceStatusQueue()) == 0) {
            IOSleep(1);
            ms++;
        }
        if (res < 0)
            goto fail;
        if (res == 0) {
            LOG("GSP: FAILED: no GSP_INIT_DONE within %u s (queue %s; GSP mailbox0 0x%x, RISC-V %s)",
                limitMs / 1000, g->linked ? "up" : "never came up", grd(NV_FALCON_MAILBOX0),
                (rd32(NV_PRISCV_GSP_BASE + NV_PRISCV_CORE_SWITCH_RISCV_STATUS) & 1) ? "active" : "stopped");
            goto fail;
        }
    }

    g->booted = true;
    boostLast_ = 0;         // a new GSP-RM holds no boost
    boostLevel_ = 0;
    LOG("GSP: SUCCESS: GSP-RM initialized");
    if (!sleepNotifier_)
        sleepNotifier_ = registerPrioritySleepWakeInterest(&NVBringup::sleepHandler, this);
    getGspStaticInfo();     // Phase 4, first RPC; failures are logged, GSP-RM stays up
    createRmObjects();
    setProperty("NVGspResult", "running");
    startGspPoller();
    r = kIOReturnSuccess;
    goto out;

fail:
    // Stop the falcons before the buffers they may be reading are unmapped.
    if (g && g->touched) {
        falconReset(NV_PGSP_BASE);
        falconReset(NV_PSEC_BASE);
        LOG("GSP: falcons stopped; shut down fully before booting Windows or retrying");
    }
    if (g) {
        g->freeDma(g->touched);
        if (g->leadSet) {
            pci_->setBusLeadEnable(g->leadWas);
            g->leadSet = false;
        }
    }
    setProperty("NVGspResult", "failed");

out:
    IOLockUnlock(gspLock_);
    return r;
}

// Handles every message waiting in the status queue. Returns -1 on a fatal
// error, 1 if GSP_INIT_DONE (result 0) was among them, else 0. Caller holds gspLock_.
int NVBringup::serviceStatusQueue()
{
    GspState *g = gsp_;
    uint8_t *cmdq = g->shm.va + g->shml.cmdq_off;
    uint8_t *msgq = g->shm.va + g->shml.msgq_off;
    int result = 0;

    if (!g->linked) {
        msgqTxHeader h;
        memcpy(&h, msgq, sizeof(h));
        if (!(g->linked = nv_gsp_msgq_linked(&h, (uint32_t)g->shml.msgq_size)))
            return 0;
        LOG("GSP: status queue up");
    }
    for (;;) {
        uint32_t wptr = vrd32(msgq + offsetof(msgqTxHeader, writePtr));
        if (wptr == g->msgRptr)
            return result;
        OSSynchronizeIO();
        uint32_t n = nv_gsp_msgq_read(msgq, g->msgRptr, g->msgBuf, MSG_MAX);
        const char *bad = n ? nv_gsp_msg_check(g->msgBuf, n * GSP_MSG_QUEUE_ELEMENT_SIZE_MIN) : "bad element";
        if (bad) {
            LOG("GSP: FAILED: status message at %u: %s", g->msgRptr, bad);
            return -1;
        }
        const GSP_MSG_QUEUE_ELEMENT *e = (const GSP_MSG_QUEUE_ELEMENT *)g->msgBuf;
        if (e->seqNum != g->rxSeq)
            LOG("GSP: note: sequence %u, expected %u", e->seqNum, g->rxSeq);
        g->rxSeq = e->seqNum + 1;
        int res = handleGspMessage(e, g->msgBuf + GSP_MSG_QUEUE_ELEMENT_HDR_SIZE + sizeof(rpc_message_header_v),
                                   e->rpc.length - (uint32_t)sizeof(rpc_message_header_v));
        msgqTxHeader h;
        memcpy(&h, msgq, sizeof(h));
        g->msgRptr = (g->msgRptr + n) % h.msgCount;
        OSSynchronizeIO();
        vwr32(cmdq + g->shml.rx_hdr_off, g->msgRptr);    // our read pointer (SWAP_RX)
        if (res < 0)
            return -1;
        if (res > 0)
            result = 1;
    }
}

// After GSP_INIT_DONE, GSP-RM keeps sending events (prints, errors, NOCAT
// records); an undrained queue fills after 63 entries and stalls it. Poll every
// 100 ms until the driver stops. It gets its own work loop: a sequencer can
// sleep for seconds, which must not block a work loop shared with other drivers.
void NVBringup::startGspPoller()
{
    if (!gspTimer_) {
        gspWorkLoop_ = IOWorkLoop::workLoop();
        gspTimer_ = IOTimerEventSource::timerEventSource(this, [](OSObject *owner, IOTimerEventSource *) {
            ((NVBringup *)owner)->gspPoll();
        });
        if (!gspWorkLoop_ || !gspTimer_ || gspWorkLoop_->addEventSource(gspTimer_) != kIOReturnSuccess) {
            LOG("GSP: could not start the message poller");
            OSSafeReleaseNULL(gspTimer_);
            OSSafeReleaseNULL(gspWorkLoop_);
            return;
        }
    }
    gspTimer_->setTimeoutMS(100);
}

void NVBringup::gspPoll()
{
    IOLockLock(gspLock_);
    bool again = gsp_ && gsp_->booted;
    if (again && serviceStatusQueue() < 0) {
        LOG("GSP: message poller stopped after an error");
        again = false;
    }
    if (again) {
        updateStats();
        idleTick();
    }
    IOLockUnlock(gspLock_);
    if (again)
        gspTimer_->setTimeoutMS(100);
}

void NVBringup::stopGspPoller()
{
    if (!gspTimer_)
        return;
    gspTimer_->cancelTimeout();
    gspWorkLoop_->removeEventSource(gspTimer_);     // waits for a running poll to finish
    OSSafeReleaseNULL(gspTimer_);
    OSSafeReleaseNULL(gspWorkLoop_);
}

// Returns 1 for GSP_INIT_DONE (success), -1 to abort, 0 to keep going.
int NVBringup::handleGspMessage(const GSP_MSG_QUEUE_ELEMENT *e, const uint8_t *p, uint32_t len)
{
    GspState *g = gsp_;
    if (g->waitFunction && e->rpc.function == g->waitFunction) {
        g->waitResult = e->rpc.rpc_result;
        g->waitDone = true;
        g->waitLen = len < g->waitCap ? len : g->waitCap;
        if (g->waitBuf)
            memcpy(g->waitBuf, p, g->waitLen);
        LOG("GSP: reply to 0x%x: result 0x%x", e->rpc.function, e->rpc.rpc_result);
        return 0;
    }
    switch (e->rpc.function) {
    case NV_VGPU_MSG_EVENT_GSP_INIT_DONE:
        LOG("GSP: GSP_INIT_DONE, result 0x%x", e->rpc.rpc_result);
        return e->rpc.rpc_result == 0 ? 1 : -1;

    case NV_VGPU_MSG_EVENT_GSP_RUN_CPU_SEQUENCER:
        return runSequencer(p, len) ? 0 : -1;

    case NV_VGPU_MSG_EVENT_OS_ERROR_LOG:
        if (len >= sizeof(rpc_os_error_log_v17_00)) {
            const rpc_os_error_log_v17_00 *m = (const rpc_os_error_log_v17_00 *)p;
            char s[sizeof(m->errString) + 1];
            memcpy(s, m->errString, sizeof(m->errString));
            s[sizeof(m->errString)] = 0;
            LOG("GSP: error log: type 0x%x: %s", m->exceptType, s);
        }
        return 0;

    case NV_VGPU_MSG_EVENT_UCODE_LIBOS_PRINT:
        if (len >= sizeof(rpc_ucode_libos_print_v1E_08)) {
            const rpc_ucode_libos_print_v1E_08 *m = (const rpc_ucode_libos_print_v1E_08 *)p;
            uint32_t n = m->libosPrintBufSize;
            if (n > len - sizeof(*m)) n = len - (uint32_t)sizeof(*m);
            if (n > 200) n = 200;
            char s[201];
            memcpy(s, p + sizeof(*m), n);
            s[n] = 0;
            LOG("GSP: print: %s", s);
        }
        return 0;

    case NV_VGPU_MSG_EVENT_GSP_POST_NOCAT_RECORD:
        if (len >= sizeof(NV2080CtrlNocatJournalInsertRecord)) {
            NV2080CtrlNocatJournalInsertRecord r;
            memcpy(&r, p, sizeof(r));
            r.source[sizeof(r.source) - 1] = 0;
            r.faultingEngine[sizeof(r.faultingEngine) - 1] = 0;
            uint32_t dl = r.diagBufferLen < sizeof(r.diagBuffer) ? r.diagBufferLen : (uint32_t)sizeof(r.diagBuffer);
            char hex[3 * 24 + 1] = "";
            for (uint32_t i = 0; i < dl && i < 24; i++)
                snprintf(hex + 3 * i, 4, " %02x", r.diagBuffer[i]);
            LOG("GSP: NOCAT %s: bugcheck 0x%x source '%s' subsystem %u error 0x%llx engine '%s' tdr %u, diag %u bytes:%s",
                nv_nocat_type_name(r.recType), r.bugcheck, r.source, r.subsystem,
                (unsigned long long)r.errorCode, r.faultingEngine, r.tdrReason, r.diagBufferLen, hex);
            // GR records carry the exception registers (protobuf); dump them whole.
            if (!strncmp(r.source, "GR", 2) && dl > 24) {
                char line[3 * 32 + 1];
                for (uint32_t o = 24; o < dl; o += 32) {
                    uint32_t n = dl - o < 32 ? dl - o : 32;
                    for (uint32_t i = 0; i < n; i++)
                        snprintf(line + 3 * i, 4, " %02x", r.diagBuffer[o + i]);
                    LOG("GSP:   diag +%u:%s", o, line);
                }
            }
        } else {
            LOG("GSP: NOCAT record too short (%u bytes)", len);
        }
        return 0;

    case NV_VGPU_MSG_EVENT_RC_TRIGGERED:
        // rpc_rc_triggered_v17_02: engine, chid, gfid, level, exceptType, scope, u16, fault lo/hi/type
        if (len >= 40) {
            uint32_t w[10];
            memcpy(w, p, sizeof(w));
            LOG("GSP: RC triggered: engine 0x%x chid %u exceptType 0x%x (level %u, scope %u), "
                "MMU fault addr 0x%08x%08x type 0x%x",
                w[0], w[1], w[4], w[3], w[5], w[8], w[7], w[9]);
            setProperty("NVGspLastRC", w[4], 32);
            markChidLost(w[1], w[4]);
        } else {
            LOG("GSP: RC triggered (%u bytes)", len);
        }
        return 0;

    default:
        LOG("GSP: message 0x%x (%u bytes, result 0x%x) ignored", e->rpc.function, len, e->rpc.rpc_result);
        return 0;
    }
}

// Executes a GSP_RUN_CPU_SEQUENCER buffer (kgspExecuteSequencerBuffer, r570):
// register writes/polls on behalf of GSP-RM, and GSP core control.
bool NVBringup::runSequencer(const uint8_t *p, uint32_t len)
{
    if (len < sizeof(rpc_run_cpu_sequencer_v17_00))
        return false;
    rpc_run_cpu_sequencer_v17_00 hdr;
    memcpy(&hdr, p, sizeof(hdr));
    uint32_t avail = (len - (uint32_t)sizeof(hdr)) / 4;
    if (!hdr.bufferSizeDWord || hdr.cmdIndex >= hdr.bufferSizeDWord || hdr.cmdIndex > avail) {
        LOG("GSP: sequencer: bad buffer (size %u, used %u, have %u)", hdr.bufferSizeDWord, hdr.cmdIndex, avail);
        return false;
    }
    const uint32_t *cmds = (const uint32_t *)(p + sizeof(hdr));
    const uint64_t barLen = bar0_->getLength();
    LOG("GSP: sequencer: %u dwords", hdr.cmdIndex);

    // Consecutive writes to one register (PIO uploads through IMEMD/DMEMD) are
    // logged as a single line.
    struct WriteRun { uint32_t addr, count, first, last; } run = {};
    auto flushWrites = [this](WriteRun *r) {
        if (r->count == 1)
            LOG("GSP: seq: write 0x%06x = 0x%08x", r->addr, r->first);
        else if (r->count > 1)
            LOG("GSP: seq: write 0x%06x x%u (first 0x%08x, last 0x%08x)", r->addr, r->count, r->first, r->last);
        r->count = 0;
    };

    uint32_t pos = 0;
    nv_seq_op op;
    while (pos < hdr.cmdIndex) {
        if (!nv_gsp_seq_next(cmds, hdr.cmdIndex, &pos, &op)) {
            flushWrites(&run);
            LOG("GSP: sequencer: bad command at dword %u", pos);
            return false;
        }
        uint32_t addr = op.args[0];
        if (op.opcode != GSP_SEQ_BUF_OPCODE_REG_WRITE || addr != run.addr)
            flushWrites(&run);
        bool regOp = op.opcode <= GSP_SEQ_BUF_OPCODE_REG_POLL || op.opcode == GSP_SEQ_BUF_OPCODE_REG_STORE;
        if (regOp && (addr & 3 || addr + 4ull > barLen)) {
            LOG("GSP: sequencer: %s address 0x%x outside BAR0", nv_gsp_seq_name(op.opcode), addr);
            return false;
        }
        switch (op.opcode) {
        case GSP_SEQ_BUF_OPCODE_REG_WRITE:
            if (!run.count) {
                run.addr  = addr;
                run.first = op.args[1];
            }
            run.count++;
            run.last = op.args[1];
            wr32(addr, op.args[1]);
            break;
        case GSP_SEQ_BUF_OPCODE_REG_MODIFY: {
            uint32_t v = (rd32(addr) & ~op.args[1]) | op.args[2];
            LOG("GSP: seq: modify 0x%06x mask 0x%08x val 0x%08x -> 0x%08x", addr, op.args[1], op.args[2], v);
            wr32(addr, v);
            break;
        }
        case GSP_SEQ_BUF_OPCODE_REG_POLL: {
            uint32_t limit = op.args[3] ? op.args[3] : 4000000, us = 0, v;
            if (limit > 10000000)
                limit = 10000000;
            while (((v = rd32(addr)) & op.args[1]) != op.args[2]) {
                if (us >= limit) {
                    LOG("GSP: seq: poll 0x%06x & 0x%08x == 0x%08x timed out (0x%08x, error 0x%x)",
                        addr, op.args[1], op.args[2], v, op.args[4]);
                    return false;
                }
                IODelay(10);
                us += 10;
            }
            LOG("GSP: seq: poll 0x%06x & 0x%08x == 0x%08x ok after %u us", addr, op.args[1], op.args[2], us);
            break;
        }
        case GSP_SEQ_BUF_OPCODE_DELAY_US:
            LOG("GSP: seq: delay %u us", op.args[0]);
            if (op.args[0] > 1000)
                IOSleep((op.args[0] + 999) / 1000);
            else
                IODelay(op.args[0]);
            break;
        case GSP_SEQ_BUF_OPCODE_REG_STORE:
            LOG("GSP: seq: store 0x%06x (0x%08x) -> save[%u]", addr, rd32(addr), op.args[1]);
            break;
        case GSP_SEQ_BUF_OPCODE_CORE_RESET:
            LOG("GSP: seq: core reset");
            if (!falconReset(NV_PGSP_BASE))
                return false;
            gwr(NV_FALCON_FBIF_CTL, grd(NV_FALCON_FBIF_CTL) | 0x80);
            gwr(NV_FALCON_DMACTL, 0);
            break;
        case GSP_SEQ_BUF_OPCODE_CORE_START:
            LOG("GSP: seq: core start");
            falconStart(NV_PGSP_BASE);
            break;
        case GSP_SEQ_BUF_OPCODE_CORE_WAIT_FOR_HALT:
            LOG("GSP: seq: core wait for halt");
            if (!falconWaitHalted(4000, NV_PGSP_BASE)) {
                LOG("GSP: seq: GSP did not halt");
                return false;
            }
            break;
        case GSP_SEQ_BUF_OPCODE_CORE_RESUME:
            flushWrites(&run);
            if (!(this->*hal_->gspResume)())
                return false;
            break;
        default:
            LOG("GSP: sequencer: %s not supported", nv_gsp_seq_name(op.opcode));
            return false;
        }
    }
    flushWrites(&run);
    return true;
}

// ---- Teardown ------------------------------------------------------------------------

// Queues one RPC and rings the doorbell. Caller holds gspLock_.
bool NVBringup::sendGspRpc(uint32_t function, const void *payload, uint32_t len)
{
    GspState *g = gsp_;
    uint8_t *cmdq = g->shm.va + g->shml.cmdq_off;
    uint8_t *msgq = g->shm.va + g->shml.msgq_off;
    uint32_t n = nv_gsp_msg_build(g->msgBuf, MSG_MAX, g->txSeq, function, payload, len);
    uint32_t theirRptr = vrd32(msgq + g->shml.rx_hdr_off);     // GSP's cmdq read pointer (SWAP_RX)
    if (!n || !nv_gsp_cmdq_push(cmdq, g->msgBuf, n, theirRptr)) {
        LOG("GSP: could not queue RPC 0x%x", function);
        return false;
    }
    g->txSeq++;
    OSSynchronizeIO();
    wr32(NV_PGSP_QUEUE_HEAD(0), 0);
    return true;
}

// Services the status queue until the reply to `function` arrives.
bool NVBringup::waitGspReply(uint32_t function, uint32_t ms, uint32_t *result,
                             uint8_t *buf, uint32_t cap, uint32_t *len)
{
    GspState *g = gsp_;
    g->waitBuf = buf;
    g->waitCap = buf ? cap : 0;
    g->waitLen = 0;
    g->waitFunction = function;
    g->waitDone = false;
    for (uint32_t t = 0; t < ms && !g->waitDone; t++) {
        if (serviceStatusQueue() < 0)
            break;
        if (!g->waitDone)
            IOSleep(1);
    }
    g->waitFunction = 0;
    g->waitBuf = nullptr;
    *result = g->waitResult;
    if (len)
        *len = g->waitLen;
    return g->waitDone;
}

IOReturn NVBringup::unloadGsp(const char *why)
{
    stopGspPoller();                // must not run while we own the queues
    IOLockLock(gspLock_);
    IOReturn r = unloadGspLocked(why);
    IOLockUnlock(gspLock_);
    return r;
}

// r570 kgspUnloadRm (normal unload) + kgspTeardown_TU102:
//   UNLOADING_GUEST_DRIVER RPC -> wait for GSP mailbox0 = 0x80000000 (suspended)
//   -> reset GSP -> FWSEC-SB (restores the VBIOS's pre-OS apps) -> booter_unload on
//   SEC2 with mailboxes 0xff (clears WPR2). Then the GPU is as the firmware left it.
IOReturn NVBringup::unloadGspLocked(const char *why)
{
    GspState *g = gsp_;
    if (!g || !g->booted) {
        LOG("GSP: unload (%s): GSP-RM not running", why);
        return kIOReturnNotReady;
    }
    LOG("GSP: unloading (%s)", why);
    bool ok = true;
    intrHwOff();                    // stays wanted: back on after the next boot (wake)

    // User connections lose their contexts, memory and VA spaces (device lost); then our
    // entries come out of GSP-RM's BAR1 tables (they point into our heap).
    gpuTeardownAll(why);
    bar1Cleanup();

    // 0. Free our client; GSP-RM frees its device, subdevice and channels with it.
    if (g->hClient) {
        uint32_t st = 0;
        LOG("GSP: freeing client 0x%x: %s", g->hClient,
            gspRmFree(g->hClient, 0, g->hClient, &st) ? "ok" : "failed");
        g->hClient = g->hDevice = g->hSubdevice = g->hVaSpace = g->hChannel = 0;
        g->pd3 = 0;
    }
    g->utilOk = g->grReady = false;
    g->nvdecCtxSize = 0;
    g->dispInst = 0;                // the VRAM heap is rebuilt at the next boot
    g->hDisp = 0;                   // freed with the client
    g->util = TestChan();
    g->nGrGlobal = 0;
    memset(g->chidUsed, 0, sizeof(g->chidUsed));
    memset(g->chidOwner, 0, sizeof(g->chidOwner));
    setProperty("NVGpuInterface", "unloaded");
    removeProperty("NVStats");
    statTicks_ = statBusy_ = 0;

    // 1. Ask GSP-RM to unload, wait for its reply and for the core to suspend.
    struct { uint8_t bInPMTransition, bGc6Entering; uint32_t newLevel; } arg = { 0, 0, 0 };
    uint32_t res = 0;
    if (!sendGspRpc(NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER, &arg, sizeof(arg)) ||
        !waitGspReply(NV_VGPU_MSG_FUNCTION_UNLOADING_GUEST_DRIVER, 5000, &res, nullptr, 0, nullptr)) {
        LOG("GSP: unload: no reply to UNLOADING_GUEST_DRIVER; tearing down anyway");
        ok = false;
    } else if (res) {
        LOG("GSP: unload: UNLOADING_GUEST_DRIVER returned 0x%x", res);
        ok = false;
    }
    uint32_t ms = 0;
    while (grd(NV_FALCON_MAILBOX0) != 0x80000000 && ms < 5000) {
        IOSleep(1);
        ms++;
    }
    if (ms >= 5000) {
        LOG("GSP: unload: GSP did not suspend (mailbox0 0x%x)", grd(NV_FALCON_MAILBOX0));
        ok = false;
    } else {
        LOG("GSP: unload: GSP suspended after %u ms", ms);
    }
    g->booted = false;

    // 2-3. Hand the GPU back to the VBIOS and clear WPR2 (HAL; on Turing FWSEC-SB, booter_unload).
    if (!(this->*hal_->gspTeardown)())
        ok = false;
    uint32_t hi = rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    LOG("GSP: after unload WPR2 hi 0x%08x (%s)", hi, (hi >> 4) ? "STILL SET" : "cleared");
    if (hi >> 4)
        ok = false;

    // 4. Nothing on the GPU uses our buffers any more (log buffers stay readable).
    falconReset(NV_PGSP_BASE);
    g->freeDma(true);
    if (g->leadSet) {
        pci_->setBusLeadEnable(g->leadWas);
        g->leadSet = false;
    }
    frtsOk_ = false;    // WPR2 is gone: another GSP boot would need FRTS again
    setProperty("NVGspResult", ok ? "unloaded" : "unload failed");
    LOG("GSP: unload %s", ok ? "complete" : "finished with errors (shut down fully before Windows)");
    return ok ? kIOReturnSuccess : kIOReturnError;
}

// ---- Teardown marker in NVRAM -----------------------------------------------------------
// The power-event teardown runs when nothing can observe it (the log is gone after the
// restart), so its result goes into one NVRAM variable of ours; the next boot logs it as
// NVLastTeardown and deletes it. OpenCore keeps native NVRAM here (WriteFlash = true).

static const char kTeardownVar[] = "nvbringup-teardown";

static IODTNVRAM *copyNvram()
{
    IORegistryEntry *e = IORegistryEntry::fromPath("/options", gIODTPlane);
    IODTNVRAM *nv = OSDynamicCast(IODTNVRAM, e);
    if (!nv)
        OSSafeReleaseNULL(e);
    return nv;
}

void NVBringup::recordTeardown(const char *why, IOReturn r)
{
    char s[96];
    snprintf(s, sizeof(s), "%s: %s (0x%x), WPR2 hi 0x%08x", why, r == kIOReturnSuccess ? "ok" : "FAILED", r,
             rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI));
    IODTNVRAM *nv = copyNvram();
    const OSSymbol *k = OSSymbol::withCString(kTeardownVar);
    OSData *d = OSData::withBytes(s, (unsigned)strlen(s));
    bool ok = nv && k && d && nv->setProperty(k, d);
    if (ok)
        nv->sync();
    LOG("teardown marker %s: %s", ok ? "written" : "NOT written", s);
    OSSafeReleaseNULL(d);
    OSSafeReleaseNULL(k);
    OSSafeReleaseNULL(nv);
}

void NVBringup::logLastTeardown()
{
    IODTNVRAM *nv = copyNvram();
    const OSSymbol *k = OSSymbol::withCString(kTeardownVar);
    OSObject *o = nv && k ? nv->copyProperty(k) : nullptr;
    OSData *d = OSDynamicCast(OSData, o);
    if (d && d->getLength()) {
        char s[97] = {};
        memcpy(s, d->getBytesNoCopy(), d->getLength() < 96 ? d->getLength() : 96);
        LOG("previous teardown: %s", s);
        setProperty("NVLastTeardown", s);
        nv->removeProperty(k);
        nv->sync();
    } else {
        LOG("previous teardown: none recorded");
        setProperty("NVLastTeardown", "none recorded");
    }
    OSSafeReleaseNULL(o);
    OSSafeReleaseNULL(k);
    OSSafeReleaseNULL(nv);
}

// Shutdown and restart: leave the GPU clean for the next OS (Windows).
void NVBringup::systemWillShutdown(IOOptionBits specifier)
{
    powerSleep(specifier);          // an idle (powered-off) GPU goes back on for the next OS
    if (gspLock_ && gsp_ && gsp_->booted) {
        const char *why = specifier == kIOMessageSystemWillRestart ? "restart" : "shutdown";
        recordTeardown(why, unloadGsp(why));
    }
    super::systemWillShutdown(specifier);
}

// Sleep, power-off and restart would take the GPU away from under GSP-RM (and a restart
// would leave WPR2 set): tear it down first. systemWillShutdown() above only reaches drivers
// in the power plane, which this one is not; the priority sleep/wake interest also gets the
// halt/restart messages (IOPMrootDomain::handlePlatformHaltRestart notifies it first).
IOReturn NVBringup::sleepHandler(void *target, void *, UInt32 messageType, IOService *,
                                 void *, vm_size_t)
{
    NVBringup *self = (NVBringup *)target;
    const char *why = messageType == kIOMessageSystemWillSleep    ? "sleep" :
                      messageType == kIOMessageSystemWillPowerOff ? "power off" :
                      messageType == kIOMessageSystemWillRestart  ? "restart" : nullptr;
    if (why)
        self->powerSleep(messageType);     // an idle (powered-off) GPU is powered on without GSP-RM
    if (why && self->gsp_ && self->gsp_->booted) {
        IOReturn r = self->unloadGsp(why);
        self->recordTeardown(why, r);
        // Only a clean unload leaves the GPU in a state we know how to boot from again.
        self->resumePending_ = messageType == kIOMessageSystemWillSleep && r == kIOReturnSuccess;
    }
    if (messageType == kIOMessageSystemHasPoweredOn)
        self->powerWake();                 // and goes back off after wake
    if (messageType == kIOMessageSystemHasPoweredOn && self->resumePending_) {
        self->resumePending_ = false;
        if (!self->resumeCall_)
            self->resumeCall_ = thread_call_allocate(&NVBringup::resumeThunk, self);
        if (self->resumeCall_)
            thread_call_enter(self->resumeCall_);       // don't hold up the wake
        else
            self->LOG("wake: no thread call; GSP-RM stays down");
    }
    return kIOReturnSuccess;
}

void NVBringup::resumeThunk(thread_call_param_t self, thread_call_param_t)
{
    ((NVBringup *)self)->resumeGsp("wake");
}

// After wake: the platform powered the GPU off in sleep, so it comes back as from a cold
// boot. Wait for it, rerun FWSEC-FRTS (same gates as at boot), give GspState a fresh start
// with the firmware the daemon uploaded at boot, and boot GSP-RM again.
void NVBringup::resumeGsp(const char *why)
{
    LOG("%s: bringing GSP-RM back", why);
    uint32_t b0 = 0xffffffff;
    for (int i = 0; i < 100 && (b0 = rd32(NV_PMC_BOOT_0)) == 0xffffffff; i++)
        IOSleep(50);
    if (b0 == 0xffffffff) {
        LOG("%s: GPU not responding (BOOT_0 reads 0xffffffff); GSP-RM stays down", why);
        setProperty("NVGspResult", "down after wake");
        return;
    }
    IOLockLock(gspLock_);
    IOBufferMemoryDescriptor *fw[kFwCount] = {};
    if (gsp_)
        for (uint32_t i = 0; i < kFwCount; i++) {
            fw[i] = gsp_->fw[i];
            gsp_->fw[i] = nullptr;
        }
    freeGsp(false);                 // the unload already stopped the falcons
    gsp_ = new GspState;
    for (uint32_t i = 0; i < kFwCount; i++) {
        if (gsp_)
            gsp_->fw[i] = fw[i];
        else
            OSSafeReleaseNULL(fw[i]);
    }
    IOLockUnlock(gspLock_);

    readVbios();                    // reruns FWSEC-FRTS
    if (!frtsOk_) {
        LOG("%s: FRTS did not succeed; GSP-RM stays down", why);
        setProperty("NVGspResult", "down after wake");
        return;
    }
    IOReturn r = bootGsp();
    LOG("%s: GSP-RM boot %s (0x%x)", why, r == kIOReturnSuccess ? "ok" : "FAILED", r);
}

// ---- Phase 4: first RPCs ---------------------------------------------------------------

template <typename T> static T rdField(const uint8_t *p, uint32_t off)
{
    T v;
    memcpy(&v, p + off, sizeof(T));
    return v;
}

static void copyString(char *dst, uint32_t cap, const uint8_t *src, uint32_t n)
{
    uint32_t i = 0;
    for (; i + 1 < cap && i < n && src[i]; i++)
        dst[i] = (src[i] >= 0x20 && src[i] < 0x7f) ? (char)src[i] : '?';
    dst[i] = 0;
}

// GET_GSP_STATIC_INFO (rpcGetGspStaticInfo_v14_00): the request is a zeroed
// GspStaticConfigInfo (1656 bytes in r570.144), the reply the filled-in one.
// Layout from src/nv_gsp_static.h, generated from NVIDIA's headers. Caller holds gspLock_.
bool NVBringup::getGspStaticInfo()
{
    uint8_t *info = (uint8_t *)IOMallocZero(GSC_SIZE);
    uint32_t res = 0, len = 0;
    bool ok = info && sendGspRpc(NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, info, GSC_SIZE) &&
              waitGspReply(NV_VGPU_MSG_FUNCTION_GET_GSP_STATIC_INFO, 5000, &res, info, GSC_SIZE, &len);
    if (!ok || res || len < GSC_SIZE) {
        LOG("GSP: GET_GSP_STATIC_INFO failed (%s, result 0x%x, %u bytes)",
            ok ? "reply" : "no reply", res, len);
        if (info)
            IOFree(info, GSC_SIZE);
        return false;
    }
    if (OSData *d = OSData::withBytes(info, GSC_SIZE)) {
        setProperty("NVGspStaticInfo", d);
        d->release();
    }

    char name[65], shortName[65];
    copyString(name, sizeof(name), info + GSC_gpuNameString, 64);
    copyString(shortName, sizeof(shortName), info + GSC_gpuShortNameString, 64);
    LOG("GSP: static info: \"%s\" (%s)%s, VBIOS %s, subsystem %04x:%04x",
        name, shortName, info[GSC_bIsMobile] ? ", mobile" : "",
        info[GSC_bVbiosValid] ? "valid" : "invalid",
        rdField<uint32_t>(info, GSC_vbiosSubVendor), rdField<uint32_t>(info, GSC_vbiosSubDevice));
    LOG("GSP: FB %llu MiB, bus %u bits, RAM type %u, FBP mask 0x%llx, FBIO mask 0x%llx, L2 %u KiB",
        (unsigned long long)(rdField<uint64_t>(info, GSC_fb_length) >> 20),
        rdField<uint32_t>(info, GSC_fb_bus_width), rdField<uint32_t>(info, GSC_fb_ram_type),
        (unsigned long long)rdField<uint64_t>(info, GSC_fbp_mask),
        (unsigned long long)rdField<uint64_t>(info, GSC_fbio_mask),
        rdField<uint32_t>(info, GSC_l2_cache_size) >> 10);
    gsp_->bar1Pde = rdField<uint64_t>(info, GSC_bar1PdeBase);
    gsp_->hIntClient = rdField<uint32_t>(info, GSC_hInternalClient);
    gsp_->hIntSubdevice = rdField<uint32_t>(info, GSC_hInternalSubdevice);
    LOG("GSP: BAR1 PDE 0x%llx, BAR2 PDE 0x%llx; internal client 0x%x device 0x%x subdevice 0x%x",
        (unsigned long long)rdField<uint64_t>(info, GSC_bar1PdeBase),
        (unsigned long long)rdField<uint64_t>(info, GSC_bar2PdeBase),
        rdField<uint32_t>(info, GSC_hInternalClient), rdField<uint32_t>(info, GSC_hInternalDevice),
        rdField<uint32_t>(info, GSC_hInternalSubdevice));

    const uint8_t *fbr = info + GSC_fbRegionInfoParams;
    uint32_t n = rdField<uint32_t>(fbr, FBR_NUM);
    LOG("GSP: %u FB region(s)", n);
    uint64_t heapBase = 0, heapLimit = 0;
    for (uint32_t i = 0; i < n && i < FBR_MAX; i++) {
        const uint8_t *r = fbr + FBR_REGIONS + i * FBR_REGION_SIZE;
        LOG("GSP:   0x%09llx..0x%09llx reserved 0x%llx perf %u%s%s%s", 
            (unsigned long long)rdField<uint64_t>(r, FBR_BASE), (unsigned long long)rdField<uint64_t>(r, FBR_LIMIT),
            (unsigned long long)rdField<uint64_t>(r, FBR_RESERVED), rdField<uint32_t>(r, FBR_PERFORMANCE),
            r[FBR_COMPRESSED] ? " compressible" : "", r[FBR_ISO] ? " ISO" : "", r[FBR_PROTECTED] ? " protected" : "");
        // The host's VRAM: the largest region with nothing reserved and not protected.
        uint64_t b = rdField<uint64_t>(r, FBR_BASE), l = rdField<uint64_t>(r, FBR_LIMIT);
        if (!rdField<uint64_t>(r, FBR_RESERVED) && !r[FBR_PROTECTED] && l > b && l - b > heapLimit - heapBase)
            heapBase = b, heapLimit = l;
    }
    IOFree(info, GSC_SIZE);

    GspState *g = gsp_;
    if (!g->vram)
        g->vram = (nv_vram_heap *)IOMalloc(sizeof(nv_vram_heap));
    if (!g->vram || !nv_vram_init(g->vram, (heapBase + 0xfff) & ~0xfffull, heapLimit | 0xfff)) {
        LOG("GSP: no usable VRAM region for the host heap");
        return false;
    }
    LOG("GSP: host VRAM heap 0x%llx..0x%llx (%llu MiB)", (unsigned long long)g->vram->base,
        (unsigned long long)g->vram->limit, (unsigned long long)((g->vram->limit - g->vram->base + 1) >> 20));
    return true;
}

// ---- Phase 4 step 2: RM objects --------------------------------------------------------

// GSP_RM_ALLOC (rpcRmApiAlloc_GSP): success needs the RPC result 0; on failure the
// payload's status says why. Caller holds gspLock_.
bool NVBringup::gspRmAlloc(uint32_t hClient, uint32_t hParent, uint32_t hObject, uint32_t hClass,
                           void *params, uint32_t size, uint32_t *status)
{
    uint32_t total = (uint32_t)sizeof(rpc_gsp_rm_alloc_v03_00) + size;
    uint8_t *buf = (uint8_t *)IOMallocZero(total);
    if (!buf)
        return false;
    rpc_gsp_rm_alloc_v03_00 a = { hClient, hParent, hObject, hClass, 0, size, 0, {} };
    memcpy(buf, &a, sizeof(a));
    if (size)
        memcpy(buf + sizeof(a), params, size);
    uint32_t res = 0, len = 0;
    bool ok = sendGspRpc(NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, buf, total) &&
              waitGspReply(NV_VGPU_MSG_FUNCTION_GSP_RM_ALLOC, 5000, &res, buf, total, &len) &&
              len >= sizeof(a);
    uint32_t st = ok ? ((rpc_gsp_rm_alloc_v03_00 *)buf)->status : 0xffffffff;
    *status = res ? (st ? st : res) : 0;
    if (ok && res == 0 && size && len >= total)
        memcpy(params, buf + sizeof(a), size);
    LOG("GSP: alloc class 0x%04x handle 0x%08x (parent 0x%08x): %s, result 0x%x status 0x%x",
        hClass, hObject, hParent, !ok ? "no reply" : res ? "FAILED" : "ok", res, st);
    IOFree(buf, total);
    return ok && res == 0;
}

// GSP_RM_CONTROL: params are sent and the reply's params copied back.
bool NVBringup::gspRmControl(uint32_t hClient, uint32_t hObject, uint32_t cmd,
                             void *params, uint32_t size, uint32_t *status, bool quiet)
{
    uint32_t total = (uint32_t)sizeof(rpc_gsp_rm_control_v03_00) + size;
    uint8_t *buf = (uint8_t *)IOMallocZero(total);
    if (!buf)
        return false;
    rpc_gsp_rm_control_v03_00 c = { hClient, hObject, cmd, 0, size, 0 };
    memcpy(buf, &c, sizeof(c));
    if (size)
        memcpy(buf + sizeof(c), params, size);
    uint32_t res = 0, len = 0;
    bool ok = sendGspRpc(NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, buf, total) &&
              waitGspReply(NV_VGPU_MSG_FUNCTION_GSP_RM_CONTROL, 5000, &res, buf, total, &len) &&
              len >= total;
    uint32_t st = ok ? ((rpc_gsp_rm_control_v03_00 *)buf)->status : 0xffffffff;
    *status = res ? (st ? st : res) : 0;
    if (ok && res == 0 && size)
        memcpy(params, buf + sizeof(c), size);
    if (!quiet || !ok || res)
        LOG("GSP: control 0x%08x on 0x%08x: %s, result 0x%x status 0x%x",
            cmd, hObject, !ok ? "no reply" : res ? "FAILED" : "ok", res, st);
    IOFree(buf, total);
    return ok && res == 0;
}

// FREE (NVOS00): freeing a client frees everything under it.
bool NVBringup::gspRmFree(uint32_t hRoot, uint32_t hParent, uint32_t hObject, uint32_t *status)
{
    rpc_free_v03_00 f = { hRoot, hParent, hObject, 0 };
    uint32_t res = 0, len = 0;
    bool ok = sendGspRpc(NV_VGPU_MSG_FUNCTION_FREE, &f, sizeof(f)) &&
              waitGspReply(NV_VGPU_MSG_FUNCTION_FREE, 5000, &res, (uint8_t *)&f, sizeof(f), &len);
    *status = res ? (f.status ? f.status : res) : 0;
    return ok && res == 0;
}

// Client -> device -> subdevice, with nouveau's handles, then one control call
// (GPU_GET_NAME_STRING) on the subdevice to prove the objects work.
bool NVBringup::createRmObjects()
{
    GspState *g = gsp_;
    const uint32_t hClient = 0xc1d00001, hDevice = 0xde1d0000, hSubdevice = 0x5d1d0000;
    uint32_t st = 0;

    uint8_t root[NV0000_ALLOC_SIZE] = {};
    uint32_t pid = 0xffffffff;
    memcpy(root + NV0000_hClient, &hClient, 4);
    memcpy(root + NV0000_processID, &pid, 4);
    strlcpy((char *)root + NV0000_processName, "NVBringup", 100);
    if (!gspRmAlloc(hClient, hClient, hClient, NV01_ROOT_CLASS, root, sizeof(root), &st))
        return false;
    g->hClient = hClient;

    uint8_t dev[NV0080_ALLOC_SIZE] = {};
    memcpy(dev + NV0080_hClientShare, &hClient, 4);     // deviceId 0
    if (!gspRmAlloc(hClient, hClient, hDevice, NV01_DEVICE_0_CLASS, dev, sizeof(dev), &st))
        return false;
    g->hDevice = hDevice;

    uint8_t sub[NV2080_ALLOC_SIZE] = {};                 // subDeviceId 0
    if (!gspRmAlloc(hClient, hDevice, hSubdevice, NV20_SUBDEVICE_0_CLASS, sub, sizeof(sub), &st))
        return false;
    g->hSubdevice = hSubdevice;

    uint8_t name[NV2080_GET_NAME_SIZE] = {};             // flags 0: ASCII
    if (!gspRmControl(hClient, hSubdevice, NV2080_CTRL_CMD_GPU_GET_NAME_STRING_, name, sizeof(name), &st))
        return false;
    char s[65];
    copyString(s, sizeof(s), name + NV2080_GET_NAME_string, 64);
    LOG("GSP: RM objects ready (client 0x%x, device 0x%x, subdevice 0x%x); GPU_GET_NAME_STRING: \"%s\"",
        hClient, hDevice, hSubdevice, s);
    setProperty("NVGspClient", hClient, 32);
    setProperty("NVGpuName", s);

    // Phase 5 setup: kernel VA space, engines, utility copy channel, GR context buffers,
    // GR unit counts, BAR1. The boot-time tests of Phase 4 only run with nvtest=1.
    uint32_t tests = 0;
    PE_parse_boot_argn("nvtest", &tests, sizeof(tests));
    if (tests)
        testVram();
    bool ready = createVaSpace() && initEngines() && createUtilChannel();
    if (ready) {
        if (!initGrGlobal())
            LOG("GSP: GR context buffers unavailable: graphics/compute contexts disabled");
        queryGrInfo();
        initNvdec();
        probeDisplay();
        probeIntr();
        if (!initBar1())
            LOG("GSP: BAR1 unavailable: CPU-mappable VRAM disabled");
    }
    if (ready && tests) {
        testScrub();
        testCopyEngine();
        testCompute();
        testBar1();
    }
    if (ready) {                    // what Vulkan lists while the GPU is powered off (NVMAC_POWER_INFO)
        fillInfo(&infoCache_);
        infoCached_ = true;
    }
    if (ready && intrWanted_)
        intrHwOn();                 // switched on before this boot (e.g. before sleep)
    LOG("GSP: GPU interface %s", ready ? "ready" : "NOT available");
    setProperty("NVGpuInterface", ready ? "ready" : "failed");
    return true;
}

// ---- Phase 4 step 3: CPU access to VRAM through the PRAMIN window ----------------------
//
// BAR0 0x700000..0x7fffff shows 1 MiB of VRAM starting at NV_PBUS_BAR0_WINDOW's base
// (64 KiB granularity). NVIDIA's CPU-RM uses the same window while GSP-RM runs
// (kbusSetBAR0WindowVidOffset_GM107). Caller holds gspLock_.

void NVBringup::praminSelect(uint64_t vram)
{
    uint32_t want = (uint32_t)(vram >> 16) & 0xffffff;      // target 0 = VRAM
    if (!praminSaved_) {
        praminOrig_  = rd32(NV_PBUS_BAR0_WINDOW);
        praminSaved_ = true;
        praminCur_   = praminOrig_;
    }
    if (praminCur_ != want) {
        wr32(NV_PBUS_BAR0_WINDOW, want);
        (void)rd32(NV_PBUS_BAR0_WINDOW);                     // post the write
        praminCur_ = want;
    }
}

uint32_t NVBringup::praminRd32(uint64_t vram)
{
    praminSelect(vram);
    return rd32(NV_PRAMIN + (uint32_t)(vram & 0xffff));
}

void NVBringup::praminWr32(uint64_t vram, uint32_t v)
{
    praminSelect(vram);
    wr32(NV_PRAMIN + (uint32_t)(vram & 0xffff), v);
}

void NVBringup::praminRestore()
{
    if (praminSaved_ && praminCur_ != praminOrig_) {
        wr32(NV_PBUS_BAR0_WINDOW, praminOrig_);
        (void)rd32(NV_PBUS_BAR0_WINDOW);
    }
    praminSaved_ = false;
}

// Allocates 64 KiB of host VRAM, writes a pattern through PRAMIN, reads it back.
bool NVBringup::testVram()
{
    GspState *g = gsp_;
    if (!g->vram)
        return false;
    const uint64_t size = 0x10000;
    uint64_t addr = nv_vram_alloc(g->vram, size, 0x10000);
    if (!addr) {
        LOG("GSP: VRAM test: allocation failed");
        return false;
    }
    uint32_t before = praminRd32(addr);
    LOG("GSP: VRAM test at 0x%llx (window was 0x%08x), first word before: 0x%08x",
        (unsigned long long)addr, praminOrig_, before);
    auto pattern = [addr](uint32_t i) { return (uint32_t)(i * 0x9e3779b9u) ^ (uint32_t)(addr >> 12) ^ 0x5a5a0000u; };
    for (uint32_t i = 0; i < size / 4; i++)
        praminWr32(addr + 4 * i, pattern(i));
    uint32_t bad = 0, firstBad = 0, got = 0;
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t v = praminRd32(addr + 4 * i);
        if (v != pattern(i) && !bad++) {
            firstBad = i;
            got = v;
        }
    }
    praminRestore();
    nv_vram_free(g->vram, addr);
    if (bad)
        LOG("GSP: VRAM test FAILED: %u of %llu words wrong (first at +0x%x: 0x%08x, expected 0x%08x)",
            bad, (unsigned long long)(size / 4), firstBad * 4, got, pattern(firstBad));
    else
        LOG("GSP: VRAM test passed: 64 KiB written and read back through PRAMIN; window restored");
    setProperty("NVVramTest", bad ? "failed" : "passed");
    return !bad;
}

// ---- Phase 4 step 4: GPU VA space with host-owned page tables ---------------------------
//
// NVIDIA's split VAS (r570 gpu_vaspace.c; nouveau r535 does the same): the host owns the
// page tables in its VRAM; GSP-RM owns the 512 MiB at 4 GiB (SPLIT_VAS_SERVER_RM_MANAGED_VA_*).
// After allocating FERMI_VASPACE_A, the host passes its PD3/PD2/PD1 instances above that
// range with COPY_SERVER_RESERVED_PDES; GSP-RM swaps them into its walker, so its root
// becomes ours, and fills the levels below PD1 for its range itself.
// Table memory is only touched through PRAMIN, and never while an RPC is outstanding.

uint64_t NVBringup::mmuAlloc(void *ctx, uint32_t bytes)
{
    NVBringup *me = (NVBringup *)ctx;
    uint64_t size = (bytes + 0xfffu) & ~0xfffull;
    uint64_t a = nv_vram_alloc(me->gsp_->vram, size, 0x1000);
    for (uint64_t off = 0; a && off < size; off += 4)
        me->praminWr32(a + off, 0);
    return a;
}

uint64_t NVBringup::mmuRd64(void *ctx, uint64_t addr)
{
    NVBringup *me = (NVBringup *)ctx;
    const GspState *g = me->gsp_;
    if (g && g->ptPoolCpu && addr - g->ptPoolPa < g->ptPoolSize)
        return g->ptPoolShadow[(addr - g->ptPoolPa) >> 3];      // pool: from the shadow
    uint64_t lo = me->praminRd32(addr);
    return lo | (uint64_t)me->praminRd32(addr + 4) << 32;
}

// High word first: the valid bit / aperture are in the low word.
void NVBringup::mmuWr64(void *ctx, uint64_t addr, uint64_t value)
{
    NVBringup *me = (NVBringup *)ctx;
    GspState *g = me->gsp_;
    if (g && g->ptPoolCpu && addr - g->ptPoolPa < g->ptPoolSize) {
        uint64_t i = (addr - g->ptPoolPa) >> 3;
        g->ptPoolShadow[i] = value;
        g->ptPoolCpu[i] = value;                    // one 64-bit store: never half-updated
        return;
    }
    me->praminWr32(addr + 4, (uint32_t)(value >> 32));
    me->praminWr32(addr, (uint32_t)value);
}

bool NVBringup::createVaSpace()
{
    GspState *g = gsp_;
    if (!g->vram || !g->hDevice || !g->hSubdevice)
        return false;
    const uint32_t hVaSpace = 0x90f10000;
    const uint64_t testVa = 0x20000000ull, testSize = 0x10000;
    const nv_mmu_ops ops = { this, mmuAlloc, mmuRd64, mmuWr64, nullptr, false };
    uint64_t pd3 = vasCreate(hVaSpace, &ops);
    if (!pd3) {
        LOG("GSP: VA space FAILED");
        setProperty("NVVaSpace", "failed");
        return false;
    }
    g->hVaSpace = hVaSpace;
    g->pd3 = pd3;
    LOG("GSP: VA space 0x%x: PD3 0x%llx, usable VA 0x%llx..0x%llx", hVaSpace, (unsigned long long)pd3,
        (unsigned long long)g->vaBase, (unsigned long long)g->vaLimit);

    // Our own test mapping, read back.
    uint64_t buf = nv_vram_alloc(g->vram, testSize, 0x10000);
    bool mapped = buf && nv_mmu_map_4k(&ops, pd3, testVa, buf, testSize, 0);
    uint32_t good = 0;
    for (uint64_t off = 0; mapped && off < testSize; off += 0x1000)
        good += nv_mmu_pte_addr(nv_mmu_lookup(&ops, pd3, testVa + off)) == buf + off;
    praminRestore();
    bool ok = mapped && good == testSize / 0x1000;
    LOG("GSP: VA space %s (test mapping 0x%llx -> VRAM 0x%llx: %u of %llu PTEs read back)", ok ? "ready" : "FAILED",
        (unsigned long long)testVa, (unsigned long long)buf, good, (unsigned long long)(testSize / 0x1000));
    setProperty("NVVaSpace", ok ? "ready" : "failed");
    return ok;
}

// ---- Phase 4 step 5: a GPFIFO channel and a copy engine ---------------------------------
//
// Follows NVIDIA's CPU-RM for a GSP client (kchannelAllocMem_GM107, kernel_channel.c) and
// nouveau r535: the host supplies zeroed VRAM for the instance block (RAMFC = its first
// 512 bytes), USERD and the method buffer, picks the channel ID (encoded in the USERD
// index flags; GSP-RM keeps chid 0), and GSP-RM fills the instance block. The channel goes
// directly under the device (RM-internal TSG), then BIND, GPFIFO_SCHEDULE, and the doorbell
// token. Everything the copy engine touches is a VA in our VA space.

// TLB invalidate for our PDB (turing dev_vm.h PRIV_MMU_INVALIDATE*, nouveau tu102_vmm_flush).
bool NVBringup::tlbFlush(uint64_t pdb, bool hubOnly)
{
    if (gsp_ && gsp_->ptPoolCpu && !bar1Flush())    // page tables written through BAR1
        LOG("GSP: BAR1 flush did not complete");
    wr32(NV_VF_MMU_INVALIDATE_PDB, (uint32_t)(pdb >> 8));          // addr >> 12 in 31:4, aperture VRAM
    wr32(NV_VF_MMU_INVALIDATE_UPPER_PDB, (uint32_t)(pdb >> 40));
    wr32(NV_VF_MMU_INVALIDATE, NV_VF_MMU_INVALIDATE_TRIGGER | NV_VF_MMU_INVALIDATE_UP_TO_PDE3 |
                               NV_VF_MMU_INVALIDATE_ALL_VA | (hubOnly ? NV_VF_MMU_INVALIDATE_HUBTLB_ONLY : 0));
    for (int i = 0; i < 2000; i++) {
        if (!(rd32(NV_VF_MMU_INVALIDATE) & NV_VF_MMU_INVALIDATE_TRIGGER))
            return true;
        IODelay(5);
    }
    return false;
}

// Allocates the channel's memory and maps ring (4 KiB, 512 entries), pushbuffer and semaphore
// page at vaBase.., then allocates the channel (not yet bound or scheduled).
bool NVBringup::createChannel(TestChan &c, uint32_t handle, uint32_t chid, uint32_t engine,
                              uint64_t vaBase, uint32_t mthdSize)
{
    GspState *g = gsp_;
    const nv_mmu_ops ops = { this, mmuAlloc, mmuRd64, mmuWr64, nullptr, false };
    const uint32_t ringEntries = 512;
    c.handle = handle;
    c.chid = chid;
    c.engine = engine;
    c.inst = mmuAlloc(this, 0x1000);
    c.userd = mmuAlloc(this, 0x1000);
    c.mthd = mmuAlloc(this, mthdSize);
    c.ring = mmuAlloc(this, 0x1000);
    c.pb = mmuAlloc(this, 0x1000);
    c.sem = mmuAlloc(this, 0x1000);
    c.vaRing = vaBase;
    c.vaPb = vaBase + 0x1000;
    c.vaSem = vaBase + 0x2000;
    bool ok = c.inst && c.userd && c.mthd && c.ring && c.pb && c.sem &&
              nv_mmu_map_4k(&ops, g->pd3, c.vaRing, c.ring, 0x1000, 0) &&
              nv_mmu_map_4k(&ops, g->pd3, c.vaPb, c.pb, 0x1000, 0) &&
              nv_mmu_map_4k(&ops, g->pd3, c.vaSem, c.sem, 0x1000, 0);
    praminRestore();
    if (!ok) {
        LOG("GSP: channel 0x%x: memory allocation or mapping failed", handle);
        return false;
    }
    LOG("GSP: channel 0x%x memory: inst 0x%llx userd 0x%llx mthdbuf 0x%llx (%u bytes); ring/pb/sem VA 0x%llx.. "
        "-> 0x%llx/0x%llx/0x%llx", handle, (unsigned long long)c.inst, (unsigned long long)c.userd,
        (unsigned long long)c.mthd, mthdSize, (unsigned long long)vaBase, (unsigned long long)c.ring,
        (unsigned long long)c.pb, (unsigned long long)c.sem);

    return rmAllocChannel(handle, chid, engine, g->hVaSpace, c.vaRing, ringEntries, c.inst, c.userd, c.mthd, true);
}

// BIND, GPFIFO_SCHEDULE, then the doorbell token.
bool NVBringup::scheduleChannel(TestChan &c)
{
    if (!rmScheduleChannel(c.handle, c.engine, &c.token))
        return false;
    LOG("GSP: channel 0x%x (chid %u, engine 0x%x) scheduled; work submit token 0x%x",
        c.handle, c.chid, c.engine, c.token);
    return true;
}

// Writes the pushbuffer, the next GPFIFO entry and GPPut, rings the doorbell, then waits up
// to `ms` for the semaphore word (servicing GSP messages meanwhile). One submission at a time.
bool NVBringup::submitAndWait(TestChan &c, const uint32_t *push, uint32_t words, uint32_t semPayload, uint32_t ms,
                              bool verbose)
{
    const uint32_t ringEntries = 512;
    for (uint32_t i = 0; i < words; i++)
        praminWr32(c.pb + 4 * i, push[i]);
    uint64_t e = c.ring + 8 * c.put;
    praminWr32(e + 0, (uint32_t)c.vaPb & ~3u);                              // GET 31:2
    praminWr32(e + 4, ((uint32_t)(c.vaPb >> 32) & 0xff) | (words << 10));   // GET_HI 7:0, LENGTH 30:10
    c.put = (c.put + 1) % ringEntries;
    praminWr32(c.userd + NVC46F_USERD_GPPut, c.put);
    (void)praminRd32(c.userd + NVC46F_USERD_GPPut);                         // post the writes
    praminRestore();
    wr32(NV_VF_DOORBELL, c.token);
    if (verbose)
        LOG("GSP: channel 0x%x: submitted %u pushbuffer words; doorbell rung", c.handle, words);

    // Spin briefly (small jobs finish in microseconds), then poll every millisecond.
    uint32_t semVal = 0, waited = 0;
    for (uint32_t i = 0; i < 200; i++) {
        semVal = praminRd32(c.sem);
        if (semVal == semPayload)
            break;
        IODelay(5);
    }
    for (; semVal != semPayload && waited < ms; waited++) {
        praminRestore();
        IOSleep(1);
        serviceStatusQueue();
        semVal = praminRd32(c.sem);
    }
    uint32_t gpGet = praminRd32(c.userd + NVC46F_USERD_GPGet);
    praminRestore();
    if (verbose || semVal != semPayload)
        LOG("GSP: channel 0x%x after %u ms: semaphore 0x%08x (want 0x%08x), USERD GPGet %u",
            c.handle, waited, semVal, semPayload, gpGet);
    return semVal == semPayload;
}

bool NVBringup::testCopyEngine()
{
    GspState *g = gsp_;
    const uint32_t subch = 4;
    const uint64_t vaSrc = 0x20103000, vaDst = 0x20104000;
    const uint32_t copyBytes = 0x1000, semPayload = 0xc0ffee01;
    const nv_mmu_ops ops = { this, mmuAlloc, mmuRd64, mmuWr64, nullptr, false };
    auto fail = [this](const char *why) {
        praminRestore();
        LOG("GSP: copy engine test FAILED: %s", why);
        setProperty("NVCopyEngineTest", "failed");
        return false;
    };
    if (!g->utilOk)
        return fail("no utility channel");

    // Source (pattern) and destination (zeroed) buffers in the kernel VA space.
    uint64_t src = mmuAlloc(this, copyBytes), dst = mmuAlloc(this, copyBytes);
    if (!src || !dst)
        return fail("VRAM allocation");
    auto pattern = [](uint32_t i) { return 0xce000000u ^ (i * 0x01000193u); };
    for (uint32_t i = 0; i < copyBytes / 4; i++)
        praminWr32(src + 4 * i, pattern(i));
    if (!nv_mmu_map_4k(&ops, g->pd3, vaSrc, src, copyBytes, 0) ||
        !nv_mmu_map_4k(&ops, g->pd3, vaDst, dst, copyBytes, 0))
        return fail("mapping buffers");
    praminRestore();
    if (!tlbFlush(g->pd3))
        LOG("GSP: TLB invalidate did not complete (continuing)");

    TestChan &c = g->util;
    const uint32_t push[] = {
        pbHeader(subch, 0x000, 1), chip_->arch->cls.copy,                 // SET_OBJECT
        pbHeader(subch, 0x400, 4), (uint32_t)(vaSrc >> 32), (uint32_t)vaSrc, // OFFSET_IN_UPPER/LOWER
                                   (uint32_t)(vaDst >> 32), (uint32_t)vaDst, // OFFSET_OUT_UPPER/LOWER
        pbHeader(subch, 0x418, 2), copyBytes, 1,                            // LINE_LENGTH_IN, LINE_COUNT
        pbHeader(subch, 0x240, 3), (uint32_t)(c.vaSem >> 32), (uint32_t)c.vaSem, semPayload,  // SET_SEMAPHORE_A/B/PAYLOAD
        pbHeader(subch, 0x300, 1),                                           // LAUNCH_DMA:
            2u | (1u << 2) | (1u << 3) | (1u << 7) | (1u << 8),              //  NON_PIPELINED, FLUSH, RELEASE_ONE_WORD,
    };                                                                       //  pitch in/out, virtual in/out
    bool released = submitAndWait(c, push, sizeof(push) / 4, semPayload, 1000);
    uint32_t bad = 0, firstBad = 0, got = 0;
    for (uint32_t i = 0; i < copyBytes / 4; i++) {
        uint32_t v = praminRd32(dst + 4 * i);
        if (v != pattern(i) && !bad++) {
            firstBad = i;
            got = v;
        }
    }
    praminRestore();
    if (!released || bad) {
        if (bad)
            LOG("GSP: destination: %u of %u words wrong (first at +0x%x: 0x%08x, expected 0x%08x)",
                bad, copyBytes / 4, firstBad * 4, got, pattern(firstBad));
        return fail(!released ? "semaphore not released" : "destination mismatch");
    }
    LOG("GSP: copy engine test passed: 4 KiB copied VA 0x%llx -> 0x%llx by CE 0x%x, semaphore released",
        (unsigned long long)vaSrc, (unsigned long long)vaDst, c.engine);
    setProperty("NVCopyEngineTest", "passed");
    return true;
}

// ---- Phase 4 step 7: graphics context and the compute class ------------------------------
//
// NVIDIA's CPU-RM for a GSP client (kernel_graphics_object.c _kgrAlloc, kgrobjPromoteContext,
// kgrobjGetPromoteIds_FWCLIENT, kgrctxPrepare{Initialize,Promote}CtxBuffer): the client
// allocates the GR context buffers, maps them into the channel's VA space, and promotes them
// with NV2080_CTRL_CMD_GPU_PROMOTE_CTX before the GR object is allocated. Our channel has no
// context share, so its group is in legacy mode and the 3D set is promoted:
//   MAIN (PA+VA, init), PATCH (PA+VA, init), BUNDLE_CB / PAGEPOOL / ATTRIBUTE_CB / RTV_CB
//   (VA only), PRIV_ACCESS_MAP and UNRESTRICTED_PRIV_ACCESS_MAP (PA+VA, init).
// Sizes come from NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO on GSP-RM's
// internal subdevice. MAIN adds a 4 KiB header per subcontext (kgraphicsGetMainCtxBufferSize).
// The first GR object makes GSP-RM build the golden context (kgraphicsCreateGoldenImageChannel
// does exactly this: a bare channel plus one object). Buffers are GPU_PRIVILEGED except PATCH
// and the priv access maps, which are mapped read-only.

bool NVBringup::testCompute()
{
    GspState *g = gsp_;
    const nv_mmu_ops ops = { this, mmuAlloc, mmuRd64, mmuWr64, nullptr, false };
    const uint32_t subch = 1, semPayload = 0xc0de0c50, maxSubctx = 64;
    uint32_t st = 0;
    auto fail = [this](const char *why) {
        praminRestore();
        LOG("GSP: compute test FAILED: %s", why);
        setProperty("NVComputeTest", "failed");
        return false;
    };
    if (!g->hIntClient || !g->hIntSubdevice || !g->mthdSize)
        return fail("prerequisites (internal handles, method buffer size)");

    // 1. Context buffer sizes (GR0).
    uint8_t *info = (uint8_t *)IOMallocZero(NV2080_GR_CTXBUF_INFO_SIZE);
    if (!info)
        return fail("no memory");
    bool got = gspRmControl(g->hIntClient, g->hIntSubdevice, NV2080_CTRL_CMD_INTERNAL_STATIC_KGR_GET_CONTEXT_BUFFERS_INFO_,
                            info, NV2080_GR_CTXBUF_INFO_SIZE, &st);
    uint32_t sz[NV2080_GR_CTXBUF_ENGINE_COUNT], al[NV2080_GR_CTXBUF_ENGINE_COUNT];
    for (uint32_t i = 0; i < NV2080_GR_CTXBUF_ENGINE_COUNT; i++) {
        memcpy(&sz[i], info + i * NV2080_GR_CTXBUF_ENTRY_SIZE, 4);
        memcpy(&al[i], info + i * NV2080_GR_CTXBUF_ENTRY_SIZE + 4, 4);
    }
    IOFree(info, NV2080_GR_CTXBUF_INFO_SIZE);
    if (!got)
        return fail("GET_CONTEXT_BUFFERS_INFO");
    auto val = [](uint32_t v) { return v == 0xffffffffu ? 0u : v; };   // NV_U32_MAX = not present
    LOG("GSP: GR ctx buffers (size/align): main 0x%x/0x%x patch 0x%x/0x%x bundle 0x%x/0x%x pagepool 0x%x/0x%x "
        "attrib 0x%x/0x%x rtv 0x%x/0x%x fecs 0x%x/0x%x privmap 0x%x/0x%x",
        sz[GR_CTXBUF_GRAPHICS], al[GR_CTXBUF_GRAPHICS], sz[GR_CTXBUF_PATCH], al[GR_CTXBUF_PATCH],
        sz[GR_CTXBUF_BUNDLE_CB], al[GR_CTXBUF_BUNDLE_CB], sz[GR_CTXBUF_PAGEPOOL_GLOBAL], al[GR_CTXBUF_PAGEPOOL_GLOBAL],
        sz[GR_CTXBUF_ATTRIBUTE_CB], al[GR_CTXBUF_ATTRIBUTE_CB], sz[GR_CTXBUF_RTV_CB_GLOBAL], al[GR_CTXBUF_RTV_CB_GLOBAL],
        sz[GR_CTXBUF_FECS_EVENT], al[GR_CTXBUF_FECS_EVENT], sz[GR_CTXBUF_PRIV_ACCESS_MAP], al[GR_CTXBUF_PRIV_ACCESS_MAP]);

    // 2. Buffers: VRAM, mapped at 0x40100000.. in our VA space.
    struct Buf { const char *name; uint32_t id; uint32_t size, align; uint32_t pte; bool init; uint64_t pa, va; };
    uint32_t mainSize = ((val(sz[GR_CTXBUF_GRAPHICS]) + 0xfffu) & ~0xfffu) + 0x1000 * maxSubctx;
    Buf bufs[] = {
        { "main",      PROMOTE_ID_MAIN,         mainSize,                              al[GR_CTXBUF_GRAPHICS],
          NV_MMU_PTE_PRIVILEGE, true, 0, 0 },
        { "patch",     PROMOTE_ID_PATCH,        val(sz[GR_CTXBUF_PATCH]),               0x1000, 0, true, 0, 0 },
        { "bundle_cb", PROMOTE_ID_BUNDLE_CB,    val(sz[GR_CTXBUF_BUNDLE_CB]),           al[GR_CTXBUF_BUNDLE_CB],
          NV_MMU_PTE_PRIVILEGE, false, 0, 0 },
        { "pagepool",  PROMOTE_ID_PAGEPOOL,     val(sz[GR_CTXBUF_PAGEPOOL_GLOBAL]),     al[GR_CTXBUF_PAGEPOOL_GLOBAL],
          NV_MMU_PTE_PRIVILEGE, false, 0, 0 },
        { "attrib_cb", PROMOTE_ID_ATTRIBUTE_CB, val(sz[GR_CTXBUF_ATTRIBUTE_CB]),        al[GR_CTXBUF_ATTRIBUTE_CB],
          NV_MMU_PTE_PRIVILEGE, false, 0, 0 },
        { "rtv_cb",    PROMOTE_ID_RTV_CB_GLOBAL, val(sz[GR_CTXBUF_RTV_CB_GLOBAL]),      al[GR_CTXBUF_RTV_CB_GLOBAL],
          NV_MMU_PTE_PRIVILEGE, false, 0, 0 },
        { "privmap",   PROMOTE_ID_PRIV_ACCESS_MAP, val(sz[GR_CTXBUF_PRIV_ACCESS_MAP]),  al[GR_CTXBUF_PRIV_ACCESS_MAP],
          NV_MMU_PTE_READ_ONLY, true, 0, 0 },
        { "unres_privmap", PROMOTE_ID_UNRESTRICTED_PRIV_ACCESS_MAP, val(sz[GR_CTXBUF_PRIV_ACCESS_MAP]),
          al[GR_CTXBUF_PRIV_ACCESS_MAP], NV_MMU_PTE_READ_ONLY, true, 0, 0 },
    };
    if (!val(sz[GR_CTXBUF_GRAPHICS]) || !bufs[1].size)
        return fail("no main/patch context size");
    uint64_t va = 0x40100000;
    for (Buf &b : bufs) {
        if (!b.size)
            continue;
        uint64_t size = ((uint64_t)b.size + 0xfff) & ~0xfffull;
        uint64_t align = 0x1000;
        while (align < b.align && align < 0x200000)
            align <<= 1;
        b.pa = nv_vram_alloc(g->vram, size, align);
        va = (va + 0xffff) & ~0xffffull;
        b.va = va;
        if (!b.pa || !nv_mmu_map_4k(&ops, g->pd3, b.va, b.pa, size, b.pte)) {
            praminRestore();
            LOG("GSP: GR buffer %s (0x%x bytes): allocation or mapping failed", b.name, b.size);
            return fail("context buffer allocation");
        }
        va += size;
    }
    praminRestore();
    for (const Buf &b : bufs)
        if (b.size)
            LOG("GSP: GR buffer %-13s 0x%08x bytes: VRAM 0x%llx VA 0x%llx%s%s", b.name, b.size,
                (unsigned long long)b.pa, (unsigned long long)b.va,
                b.pte & NV_MMU_PTE_PRIVILEGE ? " priv" : "", b.pte & NV_MMU_PTE_READ_ONLY ? " ro" : "");

    // 3. The GR channel (chid 2), not yet scheduled; then promote the buffers.
    TestChan c;
    if (!createChannel(c, 0xc46f0002, 2, NV2080_ENGINE_TYPE_GR0_, 0x40000000, g->mthdSize))
        return fail("GR channel alloc");
    uint8_t pr[NV2080_PROMOTE_SIZE] = {};
    uint32_t n = 0;
    for (const Buf &b : bufs) {
        if (!b.size || n >= NV2080_PROMOTE_MAX)
            continue;
        uint8_t *e = pr + NV2080_PROMOTE_entries + n++ * NV2080_PROMOTE_ENTRY_SIZE;
        put64(e, NV2080_PE_gpuVirtAddr, b.va);
        uint16_t id = (uint16_t)b.id;
        memcpy(e + NV2080_PE_bufferId, &id, 2);
        e[NV2080_PE_bNonmapped] = 0;
        if (b.init) {
            put64(e, NV2080_PE_gpuPhysAddr, b.pa);
            put64(e, NV2080_PE_size, b.size);
            put32(e, NV2080_PE_physAttr, NV2080_CTRL_GPU_INITIALIZE_CTX_VIDMEM_UNCACHED);
            e[NV2080_PE_bInitialize] = 1;
        }
    }
    put32(pr, NV2080_PROMOTE_engineType, NV2080_ENGINE_TYPE_GR0_);
    put32(pr, NV2080_PROMOTE_hChanClient, g->hClient);
    put32(pr, NV2080_PROMOTE_hObject, c.handle);
    put32(pr, NV2080_PROMOTE_entryCount, n);
    if (!tlbFlush(g->pd3))
        LOG("GSP: TLB invalidate did not complete (continuing)");
    if (!gspRmControl(g->hClient, g->hSubdevice, NV2080_CTRL_CMD_GPU_PROMOTE_CTX_, pr, sizeof(pr), &st))
        return fail("PROMOTE_CTX");
    LOG("GSP: promoted %u GR context buffers for channel 0x%x", n, c.handle);

    // 4. The compute object (GSP-RM builds the golden context on the first GR object).
    if (!gspRmAlloc(g->hClient, c.handle, 0xc5c00001, chip_->arch->cls.compute, nullptr, 0, &st))
        return fail("TURING_COMPUTE_A alloc");
    if (!scheduleChannel(c))
        return fail("bind/schedule/token");

    // 5. The graphics engine releases a semaphore through the compute class.
    const uint32_t push[] = {
        pbHeader(subch, 0x000, 1), chip_->arch->cls.compute,                  // SET_OBJECT
        pbHeader(subch, NVC5C0_SET_REPORT_SEMAPHORE_A_, 4),                  // SET_REPORT_SEMAPHORE_A..D
            (uint32_t)(c.vaSem >> 32), (uint32_t)c.vaSem, semPayload, NVC5C0_SEMAPHORE_D_RELEASE_ONE_WORD,
    };
    if (!submitAndWait(c, push, sizeof(push) / 4, semPayload, 2000))
        return fail("semaphore not released by the compute class");
    LOG("GSP: compute test passed: TURING_COMPUTE_A on GR0 released the semaphore (context switch works)");
    setProperty("NVComputeTest", "passed");
    return true;
}

// ---- Phase 4 step 6: BAR1 (CPU access to VRAM through the GPU MMU) ------------------------
//
// NVIDIA's CPU-RM for a GSP client (kbusInitBar1_GM107, kbusPatchBar1Pdb_GSPCLIENT): GSP-RM
// owns the BAR1 root page directory (static info bar1PdeBase) and the BAR1 instance block;
// the CPU side builds the levels below the root itself and writes its PTEs directly.
// BAR1 VA x is BAR1 aperture offset x. We check that NV_PBUS_BAR1_BLOCK's instance block
// really uses that root before writing anything, map only unused VA, and undo it at unload.

bool NVBringup::testBar1()
{
    GspState *g = gsp_;
    const uint64_t size = 0x10000;
    auto fail = [this](const char *why) {
        praminRestore();
        LOG("GSP: BAR1 test FAILED: %s", why);
        setProperty("NVBar1Test", "failed");
        return false;
    };
    if (!g->bar1Heap)
        return fail("BAR1 not initialized");
    uint64_t buf = nv_vram_alloc(g->vram, size, 0x10000), va = 0;
    if (!buf)
        return fail("VRAM allocation");
    if (!bar1Map(buf, size, &va)) {
        nv_vram_free(g->vram, buf);
        return fail("writing BAR1 page tables");
    }
    IODeviceMemory *bar1 = pci_->getDeviceMemoryWithIndex(1);
    IOMemoryMap *map = bar1 ? bar1->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache, va, size)
                            : nullptr;
    bool ok = false;
    if (map) {
        volatile uint32_t *p = (volatile uint32_t *)map->getVirtualAddress();
        auto pattern = [buf](uint32_t i) { return 0xba100000u ^ (i * 0x9e3779b1u) ^ (uint32_t)(buf >> 12); };
        uint64_t t0 = mach_absolute_time();
        for (uint32_t i = 0; i < size / 4; i++)
            p[i] = pattern(i);
        (void)p[0];                                                 // flush posted writes
        uint64_t t1 = mach_absolute_time();
        uint32_t badBar = 0, badPramin = 0;
        for (uint32_t i = 0; i < size / 4; i++) {
            badBar += p[i] != pattern(i);
            badPramin += praminRd32(buf + 4 * i) != pattern(i);
        }
        praminWr32(buf, 0x5ca1ab1e);                                // other direction: PRAMIN -> BAR1
        uint32_t back = p[0];
        praminRestore();
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(t1 - t0, &ns);
        LOG("GSP: BAR1 VA 0x%llx -> VRAM 0x%llx: %llu KiB written in %llu us; bad via BAR1 %u, via PRAMIN %u; "
            "PRAMIN write seen via BAR1: %s", (unsigned long long)va, (unsigned long long)buf,
            (unsigned long long)(size >> 10), (unsigned long long)(ns / 1000), badBar, badPramin,
            back == 0x5ca1ab1e ? "yes" : "no");
        ok = !badBar && !badPramin && back == 0x5ca1ab1e;
        map->release();
    }
    bar1Unmap(va, size);
    nv_vram_free(g->vram, buf);
    if (!ok)
        return fail(map ? "data mismatch" : "mapping BAR1 into the kernel");
    LOG("GSP: BAR1 test passed");
    setProperty("NVBar1Test", "passed");
    return true;
}
