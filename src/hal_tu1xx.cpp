// Turing HAL (NVBringup::Hal): the boot sequences r570 runs on TU102..TU117, following NVIDIA's
// _TU102 HAL functions (kflcnReset_TU102, kgspExecuteFwsec_TU102, kgspBootstrap_TU102,
// kgspExecuteSequencerCommand_TU102, kgspTeardown_TU102), cross-checked against nouveau
// (nvkm/subdev/gsp/tu102.c) and nova-core (falcon/hal/tu102.rs).
#include "nv_gsp_state.h"
#include "nv_regs.h"
#include "nv_vbios.h"
#include "nv_fwsec.h"
#include "gen_bootloader.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <string.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

const NVBringup::Hal NVBringup::halTu1xx = {
    &NVBringup::tu1xxFalconReset,
    &NVBringup::tu1xxRunFwsec,
    &NVBringup::tu1xxGspStart,
    &NVBringup::tu1xxGspResume,
    &NVBringup::tu1xxGspTeardown,
    &NVBringup::tu1xxRiscvActive,
};

bool NVBringup::tu1xxRiscvActive()
{
    return rd32(NV_PRISCV_GSP_BASE + NV_PRISCV_CORE_SWITCH_RISCV_STATUS) & 1;
}

// ---- Falcons and FWSEC ---------------------------------------------------------------------

// Engine reset, then wait for IMEM/DMEM scrubbing (nova-core falcon/hal/tu102.rs;
// r570 kgspResetHw_TU102 / ksec2ResetHw_TU102 use the same ENGINE register).
bool NVBringup::tu1xxFalconReset(uint32_t base)
{
    uint32_t e = grd(NV_FALCON_ENGINE, base);
    gwr(NV_FALCON_ENGINE, e | 1, base);
    IODelay(10);
    gwr(NV_FALCON_ENGINE, e & ~1u, base);

    for (uint32_t us = 0; us < 10000; us += 100) {
        if (!(grd(NV_FALCON_DMACTL, base) & 0x6)) {
            gwr(NV_FALCON_RM, rd32(NV_PMC_BOOT_0), base);
            return true;
        }
        IODelay(100);
    }
    LOG("falcon 0x%x: memory scrubbing did not finish (DMACTL 0x%08x)", base, grd(NV_FALCON_DMACTL, base));
    return false;
}

// The bootloader is PIO-loaded; it DMA-reads FWSEC from a buffer mapped through
// the IOMMU, then runs it. The GPU only reads that buffer; FRTS writes go to VRAM.
bool NVBringup::tu1xxRunFwsec(const uint8_t *rom, const nv_fwsec &f, uint32_t cmd, uint64_t frts)
{
    const bool sb = cmd == NV_DMEMMAPPER_CMD_SB;
    const char *tag = sb ? "FWSEC-SB" : "FRTS";
    nv_genbl bl;
    if (f.version != 2) {
        LOG("%s: FWSEC descriptor is v%u; Turing carries v2", tag, f.version);
        return false;
    }
    if (!nv_genbl_parse(nv_gen_bootloader, sizeof(nv_gen_bootloader), &bl)) {
        LOG("%s: bootloader: %s", tag, bl.err);
        return false;
    }

    const uint32_t pad    = nv_fwsec_dma_padding(&f);
    const uint32_t size   = ((pad + f.stored_size + 0xfff) & ~0xfffu) + 0x1000;  // slack for 256 B DMA chunks
    const uint32_t blSize = (bl.code_size + 0xff) & ~0xffu;

    IOBufferMemoryDescriptor *buf = nullptr;
    IODMACommand *dma = nullptr;
    uint8_t *blCode = nullptr;
    bool lead = false, leadSet = false, started = false, ok = false;
    uint64_t dmaBase = 0;
    uint8_t desc[NV_BL_DMEM_DESC_SIZE];

    buf = IOBufferMemoryDescriptor::withOptions(kIODirectionOut | kIOMemoryPhysicallyContiguous, size, 0x1000);
    blCode = (uint8_t *)IOMalloc(blSize);
    if (!buf || !blCode) {
        LOG("%s: out of memory", tag);
        goto out;
    }
    {
        uint8_t *p = (uint8_t *)buf->getBytesNoCopy();
        memset(p, 0, size);
        memcpy(p + pad, rom + f.image_rom, f.stored_size);
        if (sb ? !nv_fwsec_patch_sb(&f, p + pad + f.dmem_img)
               : !nv_fwsec_patch_frts(&f, p + pad + f.dmem_img, frts, NV_FRTS_SIZE)) {
            LOG("%s: patching the FWSEC command failed", tag);
            goto out;
        }
        memset(blCode, 0, blSize);
        memcpy(blCode, bl.code, bl.code_size);
    }

    // Map through the system IOMMU (VT-d is active); the GPU sees only this buffer.
    dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0,
                                          IODMACommand::kMapped, 0, 256);
    if (!dma || dma->setMemoryDescriptor(buf, true) != kIOReturnSuccess) {
        LOG("%s: DMA mapping failed", tag);
        goto out;
    }
    {
        UInt64 offset = 0;
        IODMACommand::Segment64 seg;
        UInt32 nseg = 1;
        if (dma->gen64IOVMSegments(&offset, &seg, &nseg) != kIOReturnSuccess || nseg != 1 ||
            seg.fLength < size || (seg.fIOVMAddr & 0xff)) {
            LOG("%s: DMA buffer is not one aligned segment", tag);
            goto out;
        }
        dmaBase = seg.fIOVMAddr;
    }
    if (!nv_fwsec_bl_desc(&f, dmaBase, desc)) {
        LOG("%s: FWSEC layout not usable with the bootloader", tag);
        goto out;
    }
    LOG("%s: DMA buffer 0x%x bytes at device address 0x%llx", tag, size, (unsigned long long)dmaBase);

    lead = pci_->setBusLeadEnable(true);
    leadSet = true;

    if (!falconReset())
        goto out;
    gwr(NV_FALCON_FBIF_CTL, grd(NV_FALCON_FBIF_CTL) | 0x80);
    gwr(NV_FALCON_DMACTL, 0);
    falconPioImem(blCode, blSize, bl.start_tag << 8, bl.start_tag);
    falconPioDmem(desc, NV_BL_DMEM_DESC_SIZE, bl.dmem_load_off);
    gwr(NV_FALCON_BOOTVEC, bl.start_tag << 8);
    gwr(NV_FALCON_FBIF_TRANSCFG(NV_FALCON_DMAIDX_PHYS_SYS_NCOH),
        (grd(NV_FALCON_FBIF_TRANSCFG(NV_FALCON_DMAIDX_PHYS_SYS_NCOH)) & ~7u) |
        NV_FBIF_TARGET_COHERENT_SYSMEM | NV_FBIF_MEM_TYPE_PHYSICAL);
    gwr(NV_FALCON_MAILBOX0, 0);
    gwr(NV_FALCON_MAILBOX1, 0);

    LOG("%s: starting GSP falcon (bootloader 0x%x bytes at IMEM 0x%x)", tag, blSize, bl.start_tag << 8);
    falconStart();
    started = true;

    if (!falconWaitHalted(2000)) {
        LOG("%s: falcon did not halt within 2 s (CPUCTL 0x%08x); resetting it", tag, grd(NV_FALCON_CPUCTL));
        falconReset();   // stops the falcon and its DMA before the buffer is unmapped
        goto out;
    }
    ok = fwsecCheck(tag, cmd, frts, grd(NV_FALCON_MAILBOX0), grd(NV_FALCON_MAILBOX1));

out:
    if (leadSet)
        pci_->setBusLeadEnable(lead);
    if (dma) {
        dma->clearMemoryDescriptor(true);
        dma->release();
    }
    OSSafeReleaseNULL(buf);
    if (blCode)
        IOFree(blCode, blSize);
    if (!started)
        LOG("%s: aborted before starting the falcon", tag);
    return ok;
}

// ---- GSP-RM ------------------------------------------------------------------------------

// Runs a booter on SEC2 (kgspExecuteHsFalcon_TU102, direct boot): its signature patched into a
// copy of the image, PIO-loaded, started with the mailboxes given; success is a halt with
// mailbox0 0. booter_load gets the WPR meta address (it copies GSP-RM into WPR2 and starts the
// RISC-V core); booter_unload gets 0xff in both (it clears WPR2).
bool NVBringup::tu1xxRunBooter(uint32_t kind, uint32_t mbox0, uint32_t mbox1)
{
    const char *name = kind == kFwBooterLoad ? "booter_load" : "booter_unload";
    GspState *g = gsp_;
    nv_booter b;
    uint8_t *img = nullptr;
    if (!g->fw[kind] || !nv_booter_parse((const uint8_t *)g->fw[kind]->getBytesNoCopy(),
                                         (uint32_t)g->fw[kind]->getLength(), &b) ||
        !(img = (uint8_t *)IOMalloc(b.img_size))) {
        LOG("GSP: %s unusable: %s", name, !g->fw[kind] ? "not loaded" : b.err ? b.err : "no memory");
        return false;
    }
    memcpy(img, b.img, b.img_size);
    // Turing has no fuse version registers (kgspReadUcodeFuseVersion returns 0), so the
    // signature is the file's last one, of which r570's tu102 and tu116 booters have one.
    uint32_t sig = 0;
    nv_booter_sig_index(&b, 0, &sig);
    nv_booter_patch(&b, img, sig);

    bool ok = false;
    if (falconReset(NV_PSEC_BASE)) {
        gwr(NV_FALCON_FBIF_CTL, grd(NV_FALCON_FBIF_CTL, NV_PSEC_BASE) | 0x80, NV_PSEC_BASE);
        gwr(NV_FALCON_DMACTL, 0, NV_PSEC_BASE);
        falconPioImem(img + b.nsec_img, b.nsec_size, b.nsec_imem, b.nsec_tag >> 8, false, NV_PSEC_BASE);
        falconPioImem(img + b.sec_img, b.sec_size, b.sec_imem, b.sec_tag >> 8, true, NV_PSEC_BASE);
        falconPioDmem(img + b.dmem_img, b.dmem_size, 0, NV_PSEC_BASE);
        gwr(NV_FALCON_BOOTVEC, b.boot_vector, NV_PSEC_BASE);
        gwr(NV_FALCON_MAILBOX0, mbox0, NV_PSEC_BASE);
        gwr(NV_FALCON_MAILBOX1, mbox1, NV_PSEC_BASE);
        LOG("GSP: starting %s on SEC2", name);
        falconStart(NV_PSEC_BASE);
        if (!falconWaitHalted(10000, NV_PSEC_BASE)) {
            LOG("GSP: %s did not halt within 10 s (SEC2 CPUCTL 0x%08x)", name, grd(NV_FALCON_CPUCTL, NV_PSEC_BASE));
        } else {
            uint32_t m0 = grd(NV_FALCON_MAILBOX0, NV_PSEC_BASE), m1 = grd(NV_FALCON_MAILBOX1, NV_PSEC_BASE);
            LOG("GSP: %s halted: mailbox0 0x%x mailbox1 0x%x", name, m0, m1);
            if (m0 != 0)
                LOG("GSP: FAILED: %s error 0x%x", name, m0);
            ok = m0 == 0;
        }
    }
    IOFree(img, b.img_size);
    return ok;
}

// kgspBootstrap_TU102 from the falcon reset on: the GSP comes out of reset ready for RISC-V,
// gets the LibOS arguments in its mailboxes; booter_load on SEC2 copies GSP-RM into WPR2 and
// starts it; then the bootloader's app version and a check that the RISC-V core runs.
bool NVBringup::tu1xxGspStart(uint32_t appVersion)
{
    GspState *g = gsp_;
    if (!falconReset(NV_PGSP_BASE))
        return false;
    gwr(NV_FALCON_MAILBOX0, (uint32_t)g->args.iova());
    gwr(NV_FALCON_MAILBOX1, (uint32_t)(g->args.iova() >> 32));

    if (!tu1xxRunBooter(kFwBooterLoad, (uint32_t)g->meta.iova(), (uint32_t)(g->meta.iova() >> 32)))
        return false;

    gwr(NV_FALCON_OS, appVersion);
    bool active = false;
    for (int i = 0; i < 100 && !active; i++) {
        active = rd32(NV_PRISCV_GSP_BASE + NV_PRISCV_CORE_SWITCH_RISCV_STATUS) & 1;
        if (!active)
            IOSleep(1);
    }
    if (!active) {
        LOG("GSP: FAILED: RISC-V core not active (GSP mailbox0 0x%x mailbox1 0x%x)",
            grd(NV_FALCON_MAILBOX0), grd(NV_FALCON_MAILBOX1));
        return false;
    }
    LOG("GSP: RISC-V active, waiting for GSP-RM");
    return true;
}

// The sequencer's CORE_RESUME. GSP-RM sends it on every Turing boot, after running a small
// falcon program on the GSP (kgspExecuteSequencerCommand_TU102, r570): reset the GSP into
// RISC-V, LibOS args -> its mailboxes, restart SEC2 (booter is still loaded), wait for its
// stage-3 handoff, then check both.
bool NVBringup::tu1xxGspResume()
{
    LOG("GSP: seq: core resume (restart via SEC2)");
    if (!falconReset(NV_PGSP_BASE))
        return false;
    gwr(NV_FALCON_MAILBOX0, (uint32_t)gsp_->args.iova());
    gwr(NV_FALCON_MAILBOX1, (uint32_t)(gsp_->args.iova() >> 32));
    falconStart(NV_PSEC_BASE);
    uint32_t ms = 0;
    while (!(rd32(NV_PGC6_BSI_SECURE_SCRATCH_14) & NV_BSI_SCRATCH_14_BOOT_STAGE_3_HANDOFF) && ms < 10000) {
        IOSleep(1);
        ms++;
    }
    uint32_t m0 = grd(NV_FALCON_MAILBOX0, NV_PSEC_BASE);
    if (ms >= 10000 || m0 != 0) {
        LOG("GSP: seq: SEC2 did not resume GSP-RM (%s, SEC2 mailbox0 0x%x)",
            ms >= 10000 ? "timeout" : "error", m0);
        return false;
    }
    if (!(rd32(NV_PRISCV_GSP_BASE + NV_PRISCV_CORE_SWITCH_RISCV_STATUS) & 1)) {
        LOG("GSP: seq: RISC-V not active after resume");
        return false;
    }
    LOG("GSP: seq: resumed after %u ms, RISC-V active", ms);
    return true;
}

// kgspTeardown_TU102 after GSP-RM suspended: FWSEC-SB on the GSP falcon (from the VBIOS read at
// boot) restores the VBIOS's pre-OS apps, then booter_unload on SEC2 clears WPR2, only while
// WPR2 is up (kgspExecuteBooterUnloadIfNeeded_TU102). Both run even if the other fails.
bool NVBringup::tu1xxGspTeardown()
{
    bool ok = true;
    falconReset(NV_PGSP_BASE);
    OSData *rom = OSDynamicCast(OSData, getProperty("NVVBIOS"));
    nv_vbios v;
    nv_fwsec f;
    uint32_t need;
    if (!rom || nv_vbios_scan((const uint8_t *)rom->getBytesNoCopy(), rom->getLength(), &v, &need) != NV_SCAN_OK ||
        !nv_vbios_find_fwsec(&v) || !nv_fwsec_parse(&v, &f)) {
        LOG("GSP: unload: VBIOS/FWSEC unavailable, skipping FWSEC-SB");
        ok = false;
    } else if (!runFwsec((const uint8_t *)rom->getBytesNoCopy(), f, NV_DMEMMAPPER_CMD_SB, 0)) {
        ok = false;
    }

    if ((rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI) >> 4) && !tu1xxRunBooter(kFwBooterUnload, 0xff, 0xff))
        ok = false;
    return ok;
}
