#include "NVBringup.hpp"
#include "nv_regs.h"
#include "nv_vbios.h"
#include "nv_fwsec.h"
#include "gen_bootloader.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <string.h>

#define super IOService
OSDefineMetaClassAndStructors(NVBringup, IOService)

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

#define NV_PROM_BASE 0x300000   // VBIOS flash window in BAR0

// Display-class functions only (reads the real class from config space; the "class-code"
// property may be rewritten by WhateverGreen's -wegnoegpu to hide the GPU from macOS graphics).
IOService *NVBringup::probe(IOService *provider, SInt32 *score)
{
    IOPCIDevice *p = OSDynamicCast(IOPCIDevice, provider);
    if (!p || (p->configRead32(kIOPCIConfigRevisionID) >> 24) != 0x03)
        return nullptr;
    return super::probe(provider, score);
}

bool NVBringup::start(IOService *provider)
{
    if (!super::start(provider))
        return false;

    pci_ = OSDynamicCast(IOPCIDevice, provider);
    if (!pci_) {
        LOG("provider is not an IOPCIDevice");
        return false;
    }

    if (!pci_->open(this)) {
        LOG("could not open PCI device (another driver owns it?)");
        return false;
    }
    opened_ = true;

    gspLock_ = IOLockAlloc();
    waitLock_ = IOLockAlloc();
    powerInit();
    if (!gspLock_ || !waitLock_ || !powerLock_) {
        stop(provider);
        return false;
    }

    // Non-stall interrupts (item 4) come on after GSP-RM boots; boot-arg nvintr=0 keeps SYNC_WAIT polling.
    uint32_t intr = 1;
    PE_parse_boot_argn("nvintr", &intr, sizeof(intr));
    intrWanted_ = intr != 0;
    if (!intrWanted_)
        LOG("boot-arg nvintr=0: non-stall interrupts stay off (SYNC_WAIT polls)");

    findAcpiNode();
    powerProbe();
    logLastTeardown();
    if (!powerOn()) {
        stop(provider);
        return false;
    }

    logConfigSpace();
    logBars();

    // Memory decode is required to read BAR0. Bus mastering stays off: no DMA yet.
    memWasEnabled_ = pci_->setMemoryEnable(true);

    bar0_ = pci_->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    if (!bar0_) {
        LOG("failed to map BAR0");
        stop(provider);
        return false;
    }
    LOG("BAR0 mapped: phys=0x%llx len=0x%llx",
        (unsigned long long)bar0_->getPhysicalAddress(),
        (unsigned long long)bar0_->getLength());

    if (!identifyChip()) {
        stop(provider);
        return false;
    }

    // A missing or unparsable VBIOS is logged but doesn't unload the driver.
    readVbios();

    registerService();
    return true;
}

// Stops GSP-RM, releases BAR0/ACPI and closes the PCI device. Idempotent.
void NVBringup::shutdownHw()
{
    powerFree();                                    // no idle power-off from here on
    if (gspLock_) {                                 // before BAR0 and the device go away
        IOLockLock(gspLock_);
        intrHwOff();
        IOLockUnlock(gspLock_);
    }
    intrRelease();
    if (resumeCall_) {
        thread_call_cancel_wait(resumeCall_);       // a wake-time GSP boot must not race us
        thread_call_free(resumeCall_);
        resumeCall_ = nullptr;
    }
    if (sleepNotifier_) {
        sleepNotifier_->remove();
        sleepNotifier_ = nullptr;
    }
    stopGspPoller();
    if (gspLock_) {
        IOLockLock(gspLock_);
        freeGsp(true);      // stops a running GSP before its buffers go away
        IOLockUnlock(gspLock_);
        IOLockFree(gspLock_);
        gspLock_ = nullptr;
    }
    OSSafeReleaseNULL(bar0_);
    OSSafeReleaseNULL(acpi_);

    if (pci_ && opened_) {
        pci_->setMemoryEnable(memWasEnabled_);
        pci_->close(this);
        opened_ = false;
    }
}

// The PCI device is being terminated (e.g. WhateverGreen's -wegnoegpu removes it after we
// started). Its termination waits for us to close it, and stop() only comes after that, so
// tear down and close here; otherwise both hang and boot waits for them.
bool NVBringup::didTerminate(IOService *provider, IOOptionBits options, bool *defer)
{
    LOG("provider terminating: shutting down");
    shutdownHw();
    return super::didTerminate(provider, options, defer);
}

void NVBringup::stop(IOService *provider)
{
    shutdownHw();
    pci_ = nullptr;
    super::stop(provider);
}

// SYNC_WAIT callers may still be leaving when stop() runs; the wait lock goes with the object.
void NVBringup::free()
{
    if (waitLock_) {
        IOLockFree(waitLock_);
        waitLock_ = nullptr;
    }
    if (powerLock_) {
        IOLockFree(powerLock_);
        powerLock_ = nullptr;
    }
    if (bar1Held_)
        IOFree(bar1Held_, bar1HeldCap_ * sizeof(Bar1Held));
    super::free();
}

void NVBringup::log(const char *fmt, ...)
{
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    IOLog("NVBringup: %s", line);

    // When full, drop the oldest half (at a line boundary) so new lines keep coming.
    uint32_t n = (uint32_t)strlen(line);
    if (logLen_ + n >= sizeof(log_)) {
        const char *cut = strchr(log_ + logLen_ / 2, '\n');
        uint32_t keep = cut ? logLen_ - (uint32_t)(cut + 1 - log_) : 0;
        memmove(log_, log_ + logLen_ - keep, keep);
        logLen_ = keep;
        log_[logLen_] = 0;
    }
    if (logLen_ + n < sizeof(log_)) {
        memcpy(log_ + logLen_, line, n + 1);
        logLen_ += n;
        setProperty("NVLog", log_);
    }
}

uint32_t NVBringup::rd32(uint32_t offset) const
{
    volatile uint32_t *base = (volatile uint32_t *)bar0_->getVirtualAddress();
    return base[offset / 4];
}

// ---- Phase 2: identification ---------------------------------------------

void NVBringup::logConfigSpace()
{
    uint16_t vendor = pci_->configRead16(kIOPCIConfigVendorID);
    uint16_t device = pci_->configRead16(kIOPCIConfigDeviceID);
    uint16_t subven = pci_->configRead16(kIOPCIConfigSubSystemVendorID);
    uint16_t subdev = pci_->configRead16(kIOPCIConfigSubSystemID);
    uint32_t classc = pci_->configRead32(kIOPCIConfigRevisionID) >> 8;
    uint16_t cmd    = pci_->configRead16(kIOPCIConfigCommand);

    LOG("PCI %04x:%04x subsys %04x:%04x class 0x%06x command 0x%04x (%u:%u.%u)",
        vendor, device, subven, subdev, classc, cmd,
        pci_->getBusNumber(), pci_->getDeviceNumber(), pci_->getFunctionNumber());

    setProperty("NVPCIDeviceID", device, 16);
    setProperty("NVPCISubsystem", ((uint32_t)subven << 16) | subdev, 32);
}

void NVBringup::logBars()
{
    for (uint32_t i = 0; i < pci_->getDeviceMemoryCount(); i++) {
        IODeviceMemory *mem = pci_->getDeviceMemoryWithIndex(i);
        if (!mem)
            continue;
        LOG("memory range %u: phys=0x%llx len=0x%llx", i,
            (unsigned long long)mem->getPhysicalAddress(),
            (unsigned long long)mem->getLength());
    }
}

bool NVBringup::identifyChip()
{
    uint32_t boot0 = rd32(NV_PMC_BOOT_0);
    if (boot0 == 0xffffffff || boot0 == 0) {
        LOG("NV_PMC_BOOT_0 = 0x%08x: GPU not responding on BAR0", boot0);
        return false;
    }

    uint32_t chipset = nv_boot0_chipset(boot0);
    uint32_t arch    = nv_boot0_arch(boot0);
    uint32_t boot42  = rd32(NV_PMC_BOOT_42);
    chipset_ = chipset;

    LOG("NV_PMC_BOOT_0 = 0x%08x -> chipset 0x%03x (%s), arch %s, rev 0x%02x; BOOT_42 = 0x%08x",
        boot0, chipset, nv_chip_name(chipset), nv_arch_name(arch), nv_boot0_rev(boot0), boot42);

    // Visible with: ioreg -l -c NVBringup
    setProperty("NVBoot0", boot0, 32);
    setProperty("NVBoot42", boot42, 32);
    setProperty("NVChipset", chipset, 32);
    setProperty("NVChipName", nv_chip_name(chipset));
    setProperty("NVArch", nv_arch_name(arch));
    return true;
}

// ---- Phase 3a: power -----------------------------------------------------

void NVBringup::findAcpiNode()
{
    acpi_ = OSDynamicCast(IOACPIPlatformDevice, pci_->getProperty("acpi-device"));
    if (!acpi_) {
        LOG("ACPI: no companion node for this PCI device");
        return;
    }
    acpi_->retain();

    OSString *path = OSDynamicCast(OSString, pci_->getProperty("acpi-path"));
    static const char *const methods[] = { "_STA", "_PS0", "_PS3", "_PR0", "_PR3", "_ON", "_OFF", "_ROM", "_DSM" };
    char present[80] = "";
    for (const char *m : methods) {
        if (acpiHas(m)) {
            strlcat(present, m, sizeof(present));
            strlcat(present, " ", sizeof(present));
        }
    }
    LOG("ACPI: %s, methods: %s", path ? path->getCStringNoCopy() : "(no path)",
        present[0] ? present : "(none of interest)");
    setProperty("NVAcpiMethods", present);
}

bool NVBringup::acpiHas(const char *method)
{
    return acpi_ && acpi_->validateObject(method) == kIOReturnSuccess;
}

// PCI PM D-state (0..3), or -1 without a PM capability.
int NVBringup::pciPowerState()
{
    UInt8 cap = 0;
    if (!pci_->findPCICapability(kIOPCIPowerManagementCapability, &cap) || !cap)
        return -1;
    return pci_->configRead16(cap + 4) & 3;
}

bool NVBringup::powerOn()
{
    bool dead = pci_->configRead16(kIOPCIConfigVendorID) == 0xffff;
    int  d    = dead ? -1 : pciPowerState();
    if (dead)
        LOG("power: config space reads 0xffff (GPU off)");
    else
        LOG("power: PCI PM state D%d", d);

    if (!dead && d <= 0)
        return true;

    // The test laptop's switchable-graphics SSDT provides _ON/_OFF on PEGP; _PS0 may not exist.
    const char *method = acpiHas("_PS0") ? "_PS0" : acpiHas("_ON") ? "_ON" : nullptr;
    if (method) {
        IOReturn r = acpi_->evaluateObject(method);
        LOG("power: ACPI %s -> 0x%x", method, r);
        IOSleep(100);
    } else {
        LOG("power: no ACPI _PS0 or _ON on the GPU node");
    }

    if (pci_->configRead16(kIOPCIConfigVendorID) == 0xffff) {
        LOG("power: GPU still off. It may be powered via the root port's "
            "power resources (_PR0 on PEG0) or kept off by an SSDT; see README");
        return false;
    }

    d = pciPowerState();
    if (d > 0) {
        // Standard PCI PM transition to D0 via PMCSR.
        UInt8 cap = 0;
        pci_->findPCICapability(kIOPCIPowerManagementCapability, &cap);
        pci_->configWrite16(cap + 4, pci_->configRead16(cap + 4) & ~3);
        IOSleep(10);
        d = pciPowerState();
        LOG("power: PMCSR set to D0, now D%d", d);
    }
    return d <= 0;
}

// ---- Phase 3b: VBIOS -----------------------------------------------------

// ACPI _ROM(offset, length) returns at most 4 KiB per call.
bool NVBringup::fetchAcpiRom(uint32_t offset, uint32_t length, uint8_t *out)
{
    OSObject *params[2] = {
        OSNumber::withNumber(offset, 32),
        OSNumber::withNumber(length, 32),
    };
    OSObject *result = nullptr;
    IOReturn r = acpi_->evaluateObject("_ROM", &result, params, 2);
    params[0]->release();
    params[1]->release();

    OSData *data = OSDynamicCast(OSData, result);
    bool ok = r == kIOReturnSuccess && data && data->getLength() > 0;
    if (ok) {
        uint32_t n = data->getLength() < length ? data->getLength() : length;
        memcpy(out, data->getBytesNoCopy(), n);
    }
    OSSafeReleaseNULL(result);
    return ok;
}

// Read-only access to the flash window. Optimus laptops often have no flash
// behind it, in which case the scan below rejects what comes back.
bool NVBringup::fetchProm(uint32_t offset, uint32_t length, uint8_t *out)
{
    if (bar0_->getLength() < NV_PROM_BASE + NV_ROM_MAX)
        return false;
    for (uint32_t i = 0; i < length; i += 4) {
        uint32_t w = rd32(NV_PROM_BASE + offset + i);
        memcpy(out + i, &w, 4);
    }
    return true;
}

// Fetches ROM bytes in 4 KiB steps until the image chain is complete.
bool NVBringup::readRom(const char *name, RomFetch fetch, uint8_t *buf, uint32_t *romLen)
{
    uint32_t have = 0, need = 0x1000;
    nv_vbios v;

    for (;;) {
        if (need > NV_ROM_MAX) {
            LOG("VBIOS %s: image chain larger than 1 MiB", name);
            return false;
        }
        uint32_t ahead  = have + 0x10000 < NV_ROM_MAX ? have + 0x10000 : NV_ROM_MAX;
        uint32_t target = need > ahead ? need : ahead;
        while (have < target) {
            uint32_t n = target - have < 0x1000 ? target - have : 0x1000;
            if (!(this->*fetch)(have, n, buf + have)) {
                LOG("VBIOS %s: read failed at 0x%x", name, have);
                return false;
            }
            have += n;
        }

        nv_scan_status st = nv_vbios_scan(buf, have, &v, &need);
        if (st == NV_SCAN_OK) {
            *romLen = v.end;
            return true;
        }
        if (st == NV_SCAN_BAD) {
            LOG("VBIOS %s: %s", name, v.err);
            return false;
        }
    }
}

// Logs the image chain and FWSEC lookup; returns whether FWSEC was found.
bool NVBringup::analyzeRom(const char *source, const uint8_t *buf, uint32_t len)
{
    nv_vbios v;
    uint32_t need;
    nv_vbios_scan(buf, len, &v, &need);
    LOG("VBIOS via %s: 0x%x bytes, %u images, PCI-AT checksum %s",
        source, len, v.nimages, v.pciat_csum_ok ? "ok" : "BAD");
    for (uint32_t i = 0; i < v.nimages; i++) {
        const nv_rom_image &im = v.img[i];
        LOG("  image %u: offset 0x%06x size 0x%06x type 0x%02x (%s)%s",
            i, im.offset, im.size, im.type, nv_image_type_name(im.type), im.last ? " last" : "");
    }

    if (!nv_vbios_find_fwsec(&v)) {
        LOG("VBIOS via %s: FWSEC not found: %s", source, v.err);
        return false;
    }
    LOG("VBIOS: BIT at 0x%x (version 0x%04x, %u tokens), PMU table %u entries",
        v.bit_off, v.bit_version, v.bit_ntokens, v.pmu_nentries);
    LOG("VBIOS: FWSEC %s descriptor at ROM 0x%x: v%u, header 0x%x bytes, payload 0x%x bytes",
        v.fwsec_appid == NV_FALCON_APPID_FWSEC_PROD ? "prod" : "debug",
        v.desc_rom, v.desc_version, v.desc_size, v.desc_stored_size);
    return true;
}

// Publishes the ROM for offline analysis (tools/dump_vbios.py) plus FWSEC facts.
void NVBringup::publishRom(const char *source, const uint8_t *buf, uint32_t len)
{
    if (OSData *rom = OSData::withBytes(buf, len)) {
        setProperty("NVVBIOS", rom);
        rom->release();
    }
    setProperty("NVVBIOSSource", source);

    nv_vbios v;
    uint32_t need;
    if (nv_vbios_scan(buf, len, &v, &need) == NV_SCAN_OK && nv_vbios_find_fwsec(&v)) {
        setProperty("NVFwsecDescOffset", v.desc_rom, 32);
        setProperty("NVFwsecVersion", v.desc_version, 32);
        setProperty("NVFwsecPayloadSize", v.desc_stored_size, 32);
        prepareFwsec(buf, len);
    }
}

void NVBringup::prepareFwsec(const uint8_t *buf, uint32_t len)
{
    nv_vbios v;
    uint32_t need;
    nv_fwsec f;
    if (nv_vbios_scan(buf, len, &v, &need) != NV_SCAN_OK || !nv_vbios_find_fwsec(&v))
        return;
    if (!nv_fwsec_parse(&v, &f)) {
        LOG("FWSEC: parse failed: %s", f.err);
        return;
    }
    LOG("FWSEC: IMEM nsec 0x%x@0x%x, sec 0x%x@0x%x; DMEM 0x%x@0x%x; boot 0x%x",
        f.nsec_size, f.nsec_imem, f.sec_size, f.sec_imem, f.dmem_size, f.dmem_addr, f.boot_vector);
    LOG("FWSEC: DMEMMAPPER v%u at DMEM 0x%x, command buffer 0x%x (0x%x bytes), mask0 0x%x",
        f.dmap_version, f.dmap_off, f.cmd_in_off, f.cmd_in_size, f.cmd_mask0);

    uint32_t range = rd32(NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE);
    uint64_t vram  = nv_vram_size(range);
    uint32_t lo    = rd32(NV_PFB_PRI_MMU_WPR2_ADDR_LO);
    uint32_t hi    = rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
    uint32_t scr   = rd32(NV_PBUS_SW_SCRATCH_0E);
    LOG("FB: LOCAL_MEMORY_RANGE 0x%08x -> VRAM %llu MiB", range, (unsigned long long)(vram >> 20));
    LOG("WPR2: lo 0x%08x hi 0x%08x (%s); SW_SCRATCH_0E 0x%08x",
        lo, hi, (hi >> 4) ? "ALREADY SET UP" : "not set up", scr);

    setProperty("NVVramSize", vram, 64);
    setProperty("NVWpr2Lo", lo, 32);
    setProperty("NVWpr2Hi", hi, 32);
    if (vram < (256ull << 20) || vram > (64ull << 30)) {
        LOG("FRTS: VRAM size implausible, not planning FRTS");
        return;
    }
    uint32_t fuse   = rd32(NV_FUSE_STATUS_OPT_DISPLAY);
    bool display    = !(fuse & 1);
    uint32_t vgaReg = display ? rd32(NV_PDISP_VGA_WORKSPACE_BASE) : 0;
    uint64_t vga    = nv_vga_workspace(vram, display, vgaReg);
    uint64_t frts   = nv_frts_addr(vga);
    LOG("FB: display %s (fuse 0x%08x), VGA_WORKSPACE_BASE 0x%08x -> workspace at 0x%llx",
        display ? "enabled" : "disabled", fuse, vgaReg, (unsigned long long)vga);
    LOG("FRTS plan: region 0x%llx..0x%llx (init_cmd 0x%x)",
        (unsigned long long)frts, (unsigned long long)(frts + NV_FRTS_SIZE), NV_DMEMMAPPER_CMD_FRTS);
    setProperty("NVFrtsAddr", frts, 64);

    // Step 2 gate: explicit opt-in, and only in the state verified above.
    uint32_t arm = 0;
    if (!PE_parse_boot_argn("nvfwsec", &arm, sizeof(arm)) || arm != 1) {
        LOG("FRTS: not run (add boot-arg nvfwsec=1 to run it)");
        setProperty("NVFrtsResult", "not run");
        return;
    }
    const char *why = nullptr;
    if (!nv_is_turing(chipset_))
        why = "chip is not Turing (TU102/104/106/116/117)";
    else if (hi >> 4)
        why = "WPR2 already set up; shut down fully to reset the GPU";
    else if (!vram)
        why = "VRAM size unknown";
    else if ((frts & 0xfffff) || frts + NV_FRTS_SIZE > vga)
        why = "FRTS region misplaced";
    if (why) {
        LOG("FRTS: not run: %s", why);
        setProperty("NVFrtsResult", "refused");
        return;
    }
    bool ok = runFwsec(buf, f, NV_DMEMMAPPER_CMD_FRTS, frts);
    setProperty("NVFrtsResult", ok ? "success" : "failed");
    frtsOk_   = ok;
    frtsAddr_ = frts;
    vgaAddr_  = vga;
    vramSize_ = vram;
}

void NVBringup::wr32(uint32_t offset, uint32_t value)
{
    volatile uint32_t *base = (volatile uint32_t *)bar0_->getVirtualAddress();
    base[offset / 4] = value;
}

// Engine reset, then wait for IMEM/DMEM scrubbing (nova-core falcon/hal/tu102.rs;
// r570 kgspResetHw_TU102 / ksec2ResetHw_TU102 use the same ENGINE register).
bool NVBringup::falconReset(uint32_t base)
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

bool NVBringup::falconWaitHalted(uint32_t ms, uint32_t base)
{
    for (uint32_t t = 0; t <= ms; t++) {
        if (grd(NV_FALCON_CPUCTL, base) & NV_CPUCTL_HALTED)
            return true;
        IOSleep(1);
    }
    return false;
}

void NVBringup::falconStart(uint32_t base)
{
    if (grd(NV_FALCON_CPUCTL, base) & NV_CPUCTL_ALIAS_EN)
        gwr(NV_FALCON_CPUCTL_ALIAS, NV_CPUCTL_STARTCPU, base);
    else
        gwr(NV_FALCON_CPUCTL, NV_CPUCTL_STARTCPU, base);
}

// PIO into IMEM port 0: 256-byte blocks, each tagged with its virtual page.
void NVBringup::falconPioImem(const uint8_t *data, uint32_t len, uint32_t dst, uint32_t tag,
                              bool secure, uint32_t base)
{
    gwr(NV_FALCON_IMEMC0, NV_MEMC_AINCW | (secure ? NV_IMEMC_SECURE : 0) | (dst & 0xffff), base);
    for (uint32_t off = 0; off < len; off += 4) {
        if ((off & 0xff) == 0)
            gwr(NV_FALCON_IMEMT0, (tag + off / 256) & 0xffff, base);
        uint32_t w;
        memcpy(&w, data + off, 4);
        gwr(NV_FALCON_IMEMD0, w, base);
    }
}

void NVBringup::falconPioDmem(const uint8_t *data, uint32_t len, uint32_t dst, uint32_t base)
{
    gwr(NV_FALCON_DMEMC0, NV_MEMC_AINCW | (dst & 0xffff), base);
    for (uint32_t off = 0; off < len; off += 4) {
        uint32_t w;
        memcpy(&w, data + off, 4);
        gwr(NV_FALCON_DMEMD0, w, base);
    }
}

// The bootloader is PIO-loaded; it DMA-reads FWSEC from a buffer mapped through
// the IOMMU, then runs it. The GPU only reads that buffer; FRTS writes go to VRAM.
bool NVBringup::runFwsec(const uint8_t *rom, const nv_fwsec &f, uint32_t cmd, uint64_t frts)
{
    const bool sb = cmd == NV_DMEMMAPPER_CMD_SB;
    const char *tag = sb ? "FWSEC-SB" : "FRTS";
    nv_genbl bl;
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
    if (sb) {
        // kgspExecuteFwsec_TU102, SB branch: GFW privilege mask lowered, GFW boot
        // completed, and no SB error in VBIOS scratch 0x15.
        uint32_t mbox0 = grd(NV_FALCON_MAILBOX0);
        uint32_t plm   = rd32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK);
        uint32_t gfw   = rd32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT);
        uint32_t err   = rd32(NV_PBUS_VBIOS_SCRATCH_15) & 0xffff;
        LOG("%s: halted; mailbox0 0x%x, PLM 0x%x, GFW boot 0x%x, SB error 0x%x", tag, mbox0, plm, gfw, err);
        ok = mbox0 == 0 && (plm & 1) && (gfw & 0xff) == 0xff && err == 0;
        LOG("%s: %s", tag, ok ? "SUCCESS" : "FAILED");
    } else {
        {
            uint32_t mbox0 = grd(NV_FALCON_MAILBOX0), mbox1 = grd(NV_FALCON_MAILBOX1);
            uint32_t err   = rd32(NV_PBUS_SW_SCRATCH_0E) >> 16;
            uint32_t wlo_r = rd32(NV_PFB_PRI_MMU_WPR2_ADDR_LO), whi_r = rd32(NV_PFB_PRI_MMU_WPR2_ADDR_HI);
            uint64_t wlo   = nv_wpr2_addr(wlo_r), whi = nv_wpr2_addr(whi_r);
            LOG("%s: halted; mailbox0 0x%x mailbox1 0x%x, FRTS error 0x%x, WPR2 0x%llx..0x%llx", tag,
                mbox0, mbox1, err, (unsigned long long)wlo, (unsigned long long)whi);
            setProperty("NVWpr2Lo", wlo_r, 32);
            setProperty("NVWpr2Hi", whi_r, 32);
            if (mbox0 != 0)
                LOG("%s: FAILED: FWSEC returned error 0x%x", tag, mbox0);
            else if (err != 0)
                LOG("%s: FAILED: FRTS error code 0x%x", tag, err);
            else if (!(whi_r >> 4))
                LOG("%s: FAILED: WPR2 was not created", tag);
            else if (wlo != frts)
                LOG("%s: FAILED: WPR2 starts at 0x%llx, expected 0x%llx", tag,
                    (unsigned long long)wlo, (unsigned long long)frts);
            else {
                LOG("%s: SUCCESS: WPR2 created at 0x%llx..0x%llx", tag,
                    (unsigned long long)wlo, (unsigned long long)whi);
                ok = true;
            }
        }
    }

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

// On the test laptop the ACPI _ROM copy holds only the PCI-AT and EFI images; the
// FWSEC images exist only in the GPU's own flash, read through PROM.
void NVBringup::readVbios()
{
    uint8_t *acpiBuf = (uint8_t *)IOMalloc(NV_ROM_MAX);
    uint8_t *promBuf = (uint8_t *)IOMalloc(NV_ROM_MAX);
    if (!acpiBuf || !promBuf) {
        LOG("VBIOS: out of memory");
        goto out;
    }
    memset(acpiBuf, 0xff, NV_ROM_MAX);
    memset(promBuf, 0xff, NV_ROM_MAX);

    {
        uint32_t acpiLen = 0, promLen = 0;
        bool acpiOk = acpiHas("_ROM") && readRom("ACPI _ROM", &NVBringup::fetchAcpiRom, acpiBuf, &acpiLen);
        if (acpiOk && analyzeRom("ACPI _ROM", acpiBuf, acpiLen)) {
            publishRom("ACPI _ROM", acpiBuf, acpiLen);
            goto out;
        }

        if (acpiOk)
            LOG("VBIOS: ACPI copy lacks FWSEC, reading the full ROM via PROM");
        bool promOk = readRom("PROM", &NVBringup::fetchProm, promBuf, &promLen);
        if (promOk && analyzeRom("PROM", promBuf, promLen))
            publishRom("PROM", promBuf, promLen);
        else if (acpiOk)
            publishRom("ACPI _ROM", acpiBuf, acpiLen);
        else if (promOk)
            publishRom("PROM", promBuf, promLen);
        else
            LOG("VBIOS: no valid ROM from ACPI _ROM or PROM");
    }

out:
    if (acpiBuf) IOFree(acpiBuf, NV_ROM_MAX);
    if (promBuf) IOFree(promBuf, NV_ROM_MAX);
}
