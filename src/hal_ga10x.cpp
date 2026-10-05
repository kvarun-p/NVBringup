// Ampere GA10x and Ada HAL (NVBringup::Hal): the boot sequences r570 runs on GA102..GA107 and
// AD102..AD107, following NVIDIA's _GA102 HAL functions (kflcnReset_TU102 with
// kflcnPreResetWait_GA102 / kflcnWaitForResetToFinish_GA102 / kflcnSwitchToFalcon_GA102,
// kflcnResetIntoRiscv_GA102, kgspExecuteHsFalcon_GA102, kgspBootstrap_TU102,
// kgspExecuteSequencerCommand_GA102, kgspTeardown_TU102), cross-checked against nouveau
// (nvkm/subdev/gsp/ga102.c, nvkm/falcon/ga102.c, nvkm/subdev/gsp/fwsec.c).
//
// What differs from Turing (hal_tu1xx.cpp):
// - Falcons have a second register space (NV_FALCON2_*) with the RISC-V core's control: the
//   GSP must be switched to RISC-V explicitly (BCR_CTRL) before the booter starts it, and a
//   falcon that runs RISC-V is switched back to falcon mode after a reset.
// - FWSEC (v3 descriptor) and the booters are PKC-signed HS ucodes: no generic bootloader,
//   no PIO. The falcon's own DMA engine copies code and data from system memory and its boot
//   ROM verifies the signature (RSA-3K) the driver patched into DMEM, chosen by the chip's
//   fuse version of that ucode id.
// - RISC-V is active when NV_PRISCV_RISCV_CPUCTL (0x388) says so, not CORE_SWITCH_RISCV_STATUS.
// Ada (kgspExecuteScrubberIfNeeded_AD102) also runs a scrubber ucode from NVIDIA's driver
// package before the booter, which lets GSP-RM's heap leave the 256 MiB the VBIOS pre-scrubs.
// linux-firmware doesn't ship it, so the heap stays inside that region (nv_wpr2_layout_ga10x),
// as nouveau does; the scrubber's handoff state is logged.
//
// None of this has run on hardware: GA10x and AD10x chips are marked experimental in
// nv_hal.cpp and boot only with boot-arg nvexperimental=1.
#include "nv_gsp_state.h"
#include "nv_regs.h"
#include "nv_vbios.h"
#include "nv_fwsec.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <string.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

const NVBringup::Hal NVBringup::halGa10x = {
    &NVBringup::ga10xFalconReset,
    &NVBringup::ga10xRunFwsec,
    &NVBringup::ga10xGspStart,
    &NVBringup::ga10xGspResume,
    &NVBringup::ga10xGspTeardown,
    &NVBringup::ga10xRiscvActive,
};

// ---- Falcons -----------------------------------------------------------------------------

// kflcnPreResetWait_GA102 (RESET_READY, which may never come: NVIDIA bug 3419321, so a 150 us
// cap), kflcnResetHw (kgspResetHw_TU102 / ksec2ResetHw_TU102: the ENGINE reset bit on and
// off), kflcnWaitForResetToFinish_GA102 (HWCFG2 memory scrubbing done). Leaves the core
// selection alone: the callers switch it.
// Before the reset, nouveau (gm200_flcn_disable) also masks the falcon's interrupts and
// disables its context and method interfaces; r570's kflcnReset doesn't, but both are
// harmless on a falcon about to be reset and keep a stale interrupt from reaching the host,
// so they're done here. nouveau additionally toggles SEC2's PMC enable bit (ga102_sec2_flcn
// reset_pmc), whose bit comes from the PTOP table (tools/ptop_dump.py); r570 only pulses
// ENGINE, which is what's followed here.
bool NVBringup::ga10xResetCore(uint32_t base)
{
    gwr(NV_FALCON_ITFEN, grd(NV_FALCON_ITFEN, base) & ~3u, base);
    gwr(NV_FALCON_IRQMCLR, 0xffffffff, base);
    for (uint32_t us = 0; us < 150 && !(grd(NV_FALCON_HWCFG2, base) & NV_HWCFG2_RESET_READY); us += 10)
        IODelay(10);
    uint32_t e = grd(NV_FALCON_ENGINE, base);
    gwr(NV_FALCON_ENGINE, e | 1, base);
    IODelay(10);
    gwr(NV_FALCON_ENGINE, e & ~1u, base);
    for (uint32_t us = 0; us < 20000; us += 100) {
        if (!(grd(NV_FALCON_HWCFG2, base) & NV_HWCFG2_MEM_SCRUBBING))
            return true;
        IODelay(100);
    }
    LOG("falcon 0x%x: memory scrubbing did not finish (HWCFG2 0x%08x)", base, grd(NV_FALCON_HWCFG2, base));
    return false;
}

// kflcnReset_TU102 as built for GA102: reset, then kflcnSwitchToFalcon_GA102 (a falcon that
// has a RISC-V core and had it selected is switched back; nouveau ga102_flcn_select), then
// FALCON_RM = BOOT_0. Leaves the falcon ready for an HS ucode.
bool NVBringup::ga10xFalconReset(uint32_t base)
{
    if (!ga10xResetCore(base))
        return false;
    const uint32_t bcrReg = base + NV_FALCON2_OFFSET + NV_PRISCV_RISCV_BCR_CTRL;
    if ((grd(NV_FALCON_HWCFG2, base) & NV_HWCFG2_RISCV) && (rd32(bcrReg) & NV_PRISCV_BCR_CORE_RISCV)) {
        wr32(bcrReg, 0);                                    // CORE_SELECT_FALCON
        bool valid = false;
        for (uint32_t ms = 0; ms < 10 && !(valid = rd32(bcrReg) & NV_PRISCV_BCR_VALID); ms++)
            IOSleep(1);
        if (!valid) {
            LOG("falcon 0x%x: core switch to falcon mode did not complete (BCR_CTRL 0x%08x)", base, rd32(bcrReg));
            return false;
        }
    }
    gwr(NV_FALCON_RM, rd32(NV_PMC_BOOT_0), base);
    return true;
}

// kflcnResetIntoRiscv_GA102: reset the GSP and select its RISC-V core, with the boot ROM
// fetching the bootloader (BCR_CTRL: CORE_SELECT_RISCV, VALID, BRFETCH; nouveau ga102_gsp_reset
// sets the same 0x111).
bool NVBringup::ga10xResetIntoRiscv()
{
    if (!ga10xResetCore(NV_PGSP_BASE))
        return false;
    wr32(NV_PGSP_BASE + NV_FALCON2_OFFSET + NV_PRISCV_RISCV_BCR_CTRL,
         NV_PRISCV_BCR_CORE_RISCV | NV_PRISCV_BCR_VALID | NV_PRISCV_BCR_BRFETCH);
    return true;
}

bool NVBringup::ga10xRiscvActive()
{
    return rd32(NV_PGSP_BASE + NV_FALCON2_OFFSET + NV_PRISCV_RISCV_CPUCTL) & NV_PRISCV_RISCV_CPUCTL_ACTIVE;
}

// ---- HS ucodes -----------------------------------------------------------------------------

// kgspExecuteHsFalcon_GA102 (nouveau ga102_flcn_fw_load + ga102_flcn_fw_boot): the image goes
// into one DMA buffer mapped through the IOMMU; the falcon DMAs its code into IMEM (secure,
// tagged with code_va, which is also the boot vector) and its data into DMEM, 256 bytes at
// a time; the boot ROM is told where the signature is and which ucode this is; then the
// falcon starts and runs until it halts. The caller reset the falcon. mboxOut: the
// mailboxes after the halt.
bool NVBringup::ga10xRunHs(const char *tag, uint32_t base, const HsImage &hs, uint32_t mbox0, uint32_t mbox1,
                           uint32_t waitMs, uint32_t *mboxOut)
{
    const uint32_t riscv = base + NV_FALCON2_OFFSET;
    // The code's device address is its tag base taken back from the image's, so that the
    // falcon, adding the tag again, reads the image (srcPhysAddr = base + codeOffset - imemVa,
    // kgspExecuteHsFalcon_GA102). The image is placed `pad` bytes into the buffer so that
    // subtraction never goes below the buffer's address (both offsets are 256-byte aligned).
    const uint32_t pad   = hs.code_va > hs.code_img ? hs.code_va - hs.code_img : 0;
    const uint32_t size  = pad + ((hs.size + 0xff) & ~0xffu) + 0x100;    // whole blocks, plus one of slack
    IOBufferMemoryDescriptor *buf = nullptr;
    IODMACommand *dma = nullptr;
    bool lead = false, leadSet = false, started = false, ok = false;
    uint64_t dmaBase = 0, codeBase = 0, dataBase = 0;

    // One DMA transfer (s_dmaTransfer_GA102): src is the device address the falcon adds `off`
    // to; dst the IMEM/DMEM address; the request queue must have room before each block.
    // The first millisecond is spun (a block takes microseconds), the rest slept.
    auto dmaPoll = [&](uint32_t mask, uint32_t value) -> bool {
        for (uint32_t us = 0; us < 2000000; us += us < 1000 ? 10 : 1000) {
            if ((grd(NV_FALCON_DMATRFCMD, base) & mask) == value)
                return true;
            if (us < 1000)
                IODelay(10);
            else
                IOSleep(1);
        }
        LOG("%s: falcon DMA did not make progress (DMATRFCMD 0x%08x)", tag, grd(NV_FALCON_DMATRFCMD, base));
        return false;
    };
    auto dmaLoad = [&](uint64_t src, uint32_t dst, uint32_t off, uint32_t len, uint32_t cmd) -> bool {
        if (!dmaPoll(NV_DMATRFCMD_FULL, 0))
            return false;
        gwr(NV_FALCON_DMATRFBASE, (uint32_t)(src >> 8), base);
        gwr(NV_FALCON_DMATRFBASE1, (uint32_t)(src >> 40) & 0x1ff, base);
        for (uint32_t n = 0; n < len; n += 0x100) {
            if (!dmaPoll(NV_DMATRFCMD_FULL, 0))
                return false;
            gwr(NV_FALCON_DMATRFMOFFS, (dst + n) & 0xffffff, base);
            gwr(NV_FALCON_DMATRFFBOFFS, off + n, base);
            gwr(NV_FALCON_DMATRFCMD, cmd, base);
        }
        return dmaPoll(NV_DMATRFCMD_IDLE, NV_DMATRFCMD_IDLE);    // GA10x+ has no TCM tagging: wait before the next use
    };

    if (hs.code_img + hs.code_size > hs.size || hs.data_img + hs.data_size > hs.size ||
        ((hs.code_img | hs.code_va | hs.code_pa | hs.data_img | hs.data_pa) & 0xff) || !hs.code_size || !hs.data_size ||
        pad > (16u << 20)) {
        LOG("%s: HS image layout unusable (code 0x%x+0x%x -> 0x%x tag 0x%x, data 0x%x+0x%x -> 0x%x)", tag,
            hs.code_img, hs.code_size, hs.code_pa, hs.code_va, hs.data_img, hs.data_size, hs.data_pa);
        return false;
    }
    buf = IOBufferMemoryDescriptor::withOptions(kIODirectionOut | kIOMemoryPhysicallyContiguous, size, 0x1000);
    if (!buf) {
        LOG("%s: out of memory", tag);
        goto out;
    }
    {
        uint8_t *p = (uint8_t *)buf->getBytesNoCopy();
        memset(p, 0, size);
        memcpy(p + pad, hs.data, hs.size);
    }
    // Map through the system IOMMU (VT-d is active); the GPU sees only this buffer.
    dma = IODMACommand::withSpecification(kIODMACommandOutputHost64, 40, 0, IODMACommand::kMapped, 0, 256);
    if (!dma || dma->setMemoryDescriptor(buf, true) != kIOReturnSuccess) {
        LOG("%s: DMA mapping failed", tag);
        goto out;
    }
    {
        UInt64 offset = 0;
        IODMACommand::Segment64 seg;
        UInt32 nseg = 1;
        if (dma->gen64IOVMSegments(&offset, &seg, &nseg) != kIOReturnSuccess || nseg != 1 ||
            seg.fLength < size || (seg.fIOVMAddr & 0xfff)) {
            LOG("%s: DMA buffer is not one aligned segment", tag);
            goto out;
        }
        dmaBase = seg.fIOVMAddr;
    }
    codeBase = dmaBase + pad + hs.code_img - hs.code_va;     // >= dmaBase by the choice of pad
    dataBase = dmaBase + pad + hs.data_img;
    LOG("%s: DMA buffer 0x%x bytes at device address 0x%llx; code 0x%x bytes -> IMEM 0x%x (tag 0x%x), data 0x%x bytes -> DMEM 0x%x, signature at DMEM 0x%x, engine 0x%x ucode %u",
        tag, size, (unsigned long long)dmaBase, hs.code_size, hs.code_pa, hs.code_va, hs.data_size, hs.data_pa,
        hs.sig_dmem, hs.engine_id, hs.ucode_id);

    lead = pci_->setBusLeadEnable(true);
    leadSet = true;

    // kflcnDisableCtxReq, then context DMA 0 fetches from coherent physical system memory.
    gwr(NV_FALCON_FBIF_CTL, grd(NV_FALCON_FBIF_CTL, base) | 0x80, base);
    gwr(NV_FALCON_DMACTL, 0, base);
    gwr(NV_FALCON_FBIF_TRANSCFG(0), (grd(NV_FALCON_FBIF_TRANSCFG(0), base) & ~7u) |
        NV_FBIF_TARGET_COHERENT_SYSMEM | NV_FBIF_MEM_TYPE_PHYSICAL, base);
    if (!dmaLoad(codeBase, hs.code_pa, hs.code_va, hs.code_size, NV_DMATRFCMD_SIZE_256B | NV_DMATRFCMD_IMEM | NV_DMATRFCMD_SEC) ||
        !dmaLoad(dataBase, hs.data_pa, 0, hs.data_size, NV_DMATRFCMD_SIZE_256B)) {
        ga10xFalconReset(base);     // stops a transfer still in flight before the buffer is unmapped
        goto out;
    }

    // The boot ROM verifies the PKC signature in DMEM against this engine and ucode id.
    wr32(riscv + NV_PFALCON2_FALCON_BROM_PARAADDR(0), hs.sig_dmem);
    wr32(riscv + NV_PFALCON2_FALCON_BROM_ENGIDMASK, hs.engine_id);
    wr32(riscv + NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID, hs.ucode_id & 0xff);
    wr32(riscv + NV_PFALCON2_FALCON_MOD_SEL, NV_PFALCON2_MOD_SEL_RSA3K);
    gwr(NV_FALCON_BOOTVEC, hs.code_va, base);
    gwr(NV_FALCON_MAILBOX0, mbox0, base);
    gwr(NV_FALCON_MAILBOX1, mbox1, base);

    LOG("%s: starting falcon 0x%x (mailboxes 0x%x 0x%x)", tag, base, mbox0, mbox1);
    falconStart(base);
    started = true;
    if (!falconWaitHalted(waitMs, base)) {
        LOG("%s: falcon did not halt within %u ms (CPUCTL 0x%08x); resetting it", tag, waitMs, grd(NV_FALCON_CPUCTL, base));
        ga10xFalconReset(base);     // stops the falcon and its DMA before the buffer is unmapped
        goto out;
    }
    mboxOut[0] = grd(NV_FALCON_MAILBOX0, base);
    mboxOut[1] = grd(NV_FALCON_MAILBOX1, base);
    LOG("%s: halted; mailbox0 0x%x mailbox1 0x%x", tag, mboxOut[0], mboxOut[1]);
    ok = true;

out:
    if (leadSet)
        pci_->setBusLeadEnable(lead);
    if (dma) {
        dma->clearMemoryDescriptor(true);
        dma->release();
    }
    OSSafeReleaseNULL(buf);
    if (!started)
        LOG("%s: aborted before starting the falcon", tag);
    return ok;
}

// kgspExecuteFwsec_TU102 with the BOOT_FROM_HS ucode of s_vbiosFillFlcnUcodeFromDescV3: the
// v3 FWSEC image (all of IMEM, then DMEM) with the command and the signature for this chip's
// fuse version of the FWSEC ucode id patched into DMEM, run on the GSP falcon.
bool NVBringup::ga10xRunFwsec(const uint8_t *rom, const nv_fwsec &f, uint32_t cmd, uint64_t frts)
{
    const bool sb = cmd == NV_DMEMMAPPER_CMD_SB;
    const char *tag = sb ? "FWSEC-SB" : "FRTS";
    if (f.version != 3) {
        LOG("%s: FWSEC descriptor is v%u; GA10x and later carry v3", tag, f.version);
        return false;
    }
    uint32_t fuse = 0;
    if (f.ucode_id >= 1 && f.ucode_id <= NV_FUSE_OPT_FPF_UCODE_VERSION_COUNT)
        fuse = rd32(NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION + 4 * (f.ucode_id - 1));
    uint32_t sig = 0;
    if (!nv_fwsec_sig_index(&f, fuse, &sig)) {
        LOG("%s: no signature for GSP ucode id %u at fuse version 0x%x (VBIOS has versions 0x%x, %u signatures)",
            tag, f.ucode_id, fuse, f.sig_versions, f.sig_count);
        return false;
    }
    const uint32_t imgSize = f.dmem_img + f.dmem_size;
    uint8_t *img = (uint8_t *)IOMalloc(imgSize);
    if (!img) {
        LOG("%s: out of memory", tag);
        return false;
    }
    memcpy(img, rom + f.image_rom, imgSize);
    bool ok = sb ? nv_fwsec_patch_sb(&f, img + f.dmem_img)
                 : nv_fwsec_patch_frts(&f, img + f.dmem_img, frts, NV_FRTS_SIZE);
    if (!ok) {
        LOG("%s: patching the FWSEC command failed", tag);
    } else {
        nv_fwsec_patch_sig(&f, rom, img + f.dmem_img, sig);
        LOG("%s: FWSEC v3: signature %u of %u (fuse 0x%x, versions 0x%x)", tag, sig, f.sig_count, fuse, f.sig_versions);
        HsImage hs = { img, imgSize, f.sec_img, f.sec_size, f.sec_imem, f.sec_va,
                       f.dmem_img, f.dmem_size, f.dmem_addr, f.pkc_data_off, f.engine_id_mask, f.ucode_id };
        uint32_t mbox[2] = { 0, 0 };
        ok = ga10xFalconReset(NV_PGSP_BASE) && ga10xRunHs(tag, NV_PGSP_BASE, hs, 0, 0, 2000, mbox) &&
             fwsecCheck(tag, cmd, frts, mbox[0], mbox[1]);
    }
    IOFree(img, imgSize);
    return ok;
}

// A booter on SEC2 (kgspExecuteBooterLoad_TU102 with kgspExecuteHsFalcon_GA102): the HS app
// goes to IMEM, the OS data to DMEM, with the signature for this chip's fuse version of the
// booter's ucode id (nouveau ga100_flcn_fw_signature). booter_load gets the WPR meta address
// (it copies GSP-RM into WPR2 and starts the RISC-V core); booter_unload 0xff in both (it
// clears WPR2). Success is a halt with mailbox0 0.
bool NVBringup::ga10xRunBooter(uint32_t kind, uint32_t mbox0, uint32_t mbox1)
{
    const char *name = kind == kFwBooterLoad ? "booter_load" : "booter_unload";
    GspState *g = gsp_;
    nv_booter b;
    uint8_t *img = nullptr;
    if (!g->fw[kind] || !nv_booter_parse((const uint8_t *)g->fw[kind]->getBytesNoCopy(),
                                         (uint32_t)g->fw[kind]->getLength(), &b)) {
        LOG("GSP: %s unusable: %s", name, !g->fw[kind] ? "not loaded" : b.err);
        return false;
    }
    // The fuse version register of the engine the booter is signed for (its meta data).
    uint32_t fuseBase = 0, fuse = 0, sig = 0;
    if (b.engine_id & 0x001)
        fuseBase = NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION;
    else if (b.engine_id & 0x004)
        fuseBase = NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION;
    else if (b.engine_id & 0x400)
        fuseBase = NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION;
    if (fuseBase && b.ucode_id >= 1 && b.ucode_id <= NV_FUSE_OPT_FPF_UCODE_VERSION_COUNT)
        fuse = rd32(fuseBase + 4 * (b.ucode_id - 1));
    if (!nv_booter_sig_index(&b, fuse, &sig)) {
        LOG("GSP: %s: no signature for fuse version 0x%x (file: version %u, %u signatures, engine 0x%x ucode %u)",
            name, fuse, b.fuse_ver, b.sig_count, b.engine_id, b.ucode_id);
        return false;
    }
    if (!(img = (uint8_t *)IOMalloc(b.img_size))) {
        LOG("GSP: %s: no memory", name);
        return false;
    }
    memcpy(img, b.img, b.img_size);
    nv_booter_patch(&b, img, sig);
    LOG("GSP: %s: signature %u of %u (fuse 0x%x, file version %u)", name, sig, b.sig_count, fuse, b.fuse_ver);

    HsImage hs = { img, b.img_size, b.hs_code_img, b.hs_code_size, 0, b.hs_code_va,
                   b.dmem_img, b.dmem_size, 0, b.hs_sig_dmem, b.engine_id, b.ucode_id };
    uint32_t mbox[2] = { 0, 0 };
    bool ok = ga10xFalconReset(NV_PSEC_BASE) && ga10xRunHs(name, NV_PSEC_BASE, hs, mbox0, mbox1, 10000, mbox);
    if (ok && mbox[0] != 0) {
        LOG("GSP: FAILED: %s error 0x%x", name, mbox[0]);
        ok = false;
    }
    IOFree(img, b.img_size);
    return ok;
}

// ---- GSP-RM ------------------------------------------------------------------------------

// kgspBootstrap_TU102 from the GSP reset on, as built for GA102: the GSP is reset with its
// RISC-V core selected, gets the LibOS arguments in its mailboxes; (Ada: NVIDIA would run the
// scrubber here); booter_load on SEC2 copies GSP-RM into WPR2 and starts the core; then the
// bootloader's app version and a check that the core runs.
bool NVBringup::ga10xGspStart(uint32_t appVersion)
{
    GspState *g = gsp_;
    if (chip_->arch->id == NV_ARCH_AD10X) {
        uint32_t s15 = rd32(NV_PGC6_BSI_SECURE_SCRATCH_15);
        LOG("GSP: Ada: scrubber handoff %s (BSI_SECURE_SCRATCH_15 0x%08x); the heap stays in the pre-scrubbed region",
            (s15 >> 29) >= NV_BSI_SCRATCH_15_SCRUBBER_HANDOFF_DONE ? "done" : "not done", s15);
    }
    if (!ga10xResetIntoRiscv())
        return false;
    gwr(NV_FALCON_MAILBOX0, (uint32_t)g->args.iova());
    gwr(NV_FALCON_MAILBOX1, (uint32_t)(g->args.iova() >> 32));

    if (!ga10xRunBooter(kFwBooterLoad, (uint32_t)g->meta.iova(), (uint32_t)(g->meta.iova() >> 32)))
        return false;

    gwr(NV_FALCON_OS, appVersion);
    bool active = false;
    for (int i = 0; i < 100 && !(active = ga10xRiscvActive()); i++)
        IOSleep(1);
    if (!active) {
        LOG("GSP: FAILED: RISC-V core not active (GSP mailbox0 0x%x mailbox1 0x%x, RISCV_CPUCTL 0x%08x)",
            grd(NV_FALCON_MAILBOX0), grd(NV_FALCON_MAILBOX1),
            rd32(NV_PGSP_BASE + NV_FALCON2_OFFSET + NV_PRISCV_RISCV_CPUCTL));
        return false;
    }
    LOG("GSP: RISC-V active, waiting for GSP-RM");
    return true;
}

// The sequencer's CORE_RESUME (kgspExecuteSequencerCommand_GA102): reset the GSP into RISC-V,
// LibOS args -> its mailboxes, restart SEC2 (booter is still loaded), wait for its stage-3
// handoff, then the app version and a check that the core runs.
bool NVBringup::ga10xGspResume()
{
    LOG("GSP: seq: core resume (restart via SEC2)");
    if (!ga10xResetIntoRiscv())
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
    gwr(NV_FALCON_OS, gsp_->appVersion);
    if (!ga10xRiscvActive()) {
        LOG("GSP: seq: RISC-V not active after resume");
        return false;
    }
    LOG("GSP: seq: resumed after %u ms, RISC-V active", ms);
    return true;
}

// kgspTeardown_TU102 after GSP-RM suspended: FWSEC-SB on the GSP falcon (from the VBIOS read
// at boot) restores the VBIOS's pre-OS apps, then booter_unload on SEC2 clears WPR2, only
// while WPR2 is up (kgspExecuteBooterUnloadIfNeeded_TU102). Both run even if the other fails;
// each resets its falcon first.
bool NVBringup::ga10xGspTeardown()
{
    bool ok = true;
    OSData *rom = OSDynamicCast(OSData, getProperty("NVVBIOS"));
    nv_vbios v;
    nv_fwsec f;
    uint32_t need;
    if (!rom || nv_vbios_scan((const uint8_t *)rom->getBytesNoCopy(), rom->getLength(), &v, &need) != NV_SCAN_OK ||
        !nv_vbios_find_fwsec(&v) || !nv_fwsec_parse(&v, &f)) {
        LOG("GSP: unload: VBIOS/FWSEC unavailable, skipping FWSEC-SB");
        ok = false;
    } else if (!ga10xRunFwsec((const uint8_t *)rom->getBytesNoCopy(), f, NV_DMEMMAPPER_CMD_SB, 0)) {
        ok = false;
    }

    if ((rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI) >> 4) && !ga10xRunBooter(kFwBooterUnload, 0xff, 0xff))
        ok = false;
    return ok;
}
