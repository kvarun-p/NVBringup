// Runtime GPU power: the GPU is cut off while nothing uses it. Needs ACPI methods on the GPU's node
// that cut its power: _OFF plus _ON or _PS0, as laptops with switchable graphics (Optimus) have.
// Without them (desktop cards, most) the GPU simply stays on.
//
// Auto mode (default, boot-arg nvidle=<seconds>, 30 by default; nvidle=0 keeps it on): once no GPU
// connection has been open for the idle time, GSP-RM is unloaded the way sleep does it (contexts
// gone, WPR2 cleared, interrupts off), the PCI state saved and the GPU cut off with ACPI _OFF
// (on the test laptop, PEGP._OFF, the method its stock EFI's SSDT already uses to disable the GPU).
// The next GPU open powers it on with _ON (_PS0 as a fallback), restores the PCI state and runs the wake path:
// FWSEC-FRTS, then GSP-RM boot. The open waits for that. Linux does the same for Optimus GPUs
// (pci_save_state, D3cold through ACPI; D0, pci_restore_state).
//
// Around system sleep, restart and power off an idle GPU is powered on again without GSP-RM, so
// the platform, IOPCIFamily and the next OS find it as the firmware left it; after wake it goes
// back off. The control client (NVMAC_CONTROL_TYPE) reads and sets the mode without opening the GPU.

#include "NVBringup.hpp"
#include "nv_gsp_state.h"
#include "nv_uapi.h"
#include "nv_regs.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

static const char *const kStateNames[] = { "on", "off", "switching", "failed" };
static const char *const kModeNames[] = { "off", "on", "auto" };

void NVBringup::powerInit()
{
    powerLock_ = IOLockAlloc();
    idleCall_ = thread_call_allocate(&NVBringup::idleThunk, this);
    rawOffCall_ = thread_call_allocate(&NVBringup::rawOffThunk, this);
    uint32_t idle = 30;
    PE_parse_boot_argn("nvidle", &idle, sizeof(idle));
    idleSec_ = idle;
    pwrMode_ = idle ? kModeAuto : kModeOn;
    pwrState_ = kPwrOn;                     // the firmware leaves the GPU powered
    publishPower();
}

void NVBringup::powerProbe()
{
    pwrCapable_ = acpi_ && acpiHas("_OFF") && (acpiHas("_ON") || acpiHas("_PS0"));
    if (!pwrCapable_) {
        pwrMode_ = kModeOn;                 // nothing to cut the power with: always on
        idleSec_ = 0;
        LOG("power: no ACPI _OFF/_ON on the GPU's node; runtime power-off unavailable, the GPU stays on");
    }
    publishPower();
}

void NVBringup::powerFree()
{
    if (idleCall_) {
        thread_call_cancel_wait(idleCall_);
        thread_call_free(idleCall_);
        idleCall_ = nullptr;
    }
    if (rawOffCall_) {
        thread_call_cancel_wait(rawOffCall_);
        thread_call_free(rawOffCall_);
        rawOffCall_ = nullptr;
    }
}

void NVBringup::publishPower()
{
    OSDictionary *d = OSDictionary::withCapacity(6);
    if (!d)
        return;
    auto num = [&](const char *k, uint64_t v) {
        OSNumber *n = OSNumber::withNumber(v, 32);
        if (n) {
            d->setObject(k, n);
            n->release();
        }
    };
    auto str = [&](const char *k, const char *v) {
        OSString *s = OSString::withCString(v);
        if (s) {
            d->setObject(k, s);
            s->release();
        }
    };
    str("State", kStateNames[pwrState_ & 3]);
    str("Mode", kModeNames[pwrMode_ % 3]);
    num("IdleSeconds", idleSec_);
    num("PowerOffs", pwrOffCount_);
    num("PowerOns", pwrOnCount_);
    num("LastPowerOnMs", lastOnMs_);
    OSBoolean *cap = pwrCapable_ ? kOSBooleanTrue : kOSBooleanFalse;
    d->setObject("Capable", cap);
    setProperty("NVPower", d);
    d->release();
}

// From gspPoll every 100 ms while GSP-RM runs.
void NVBringup::idleTick()
{
    GspState *g = gsp_;
    if (!g || pwrMode_ != kModeAuto || !idleSec_ || pwrState_ != kPwrOn || !idleCall_)
        return;
    bool used = g->dispLit;         // a lit display counts as in use
    for (uint32_t i = 0; i < kMaxConns && !used; i++)
        used = g->conns[i] != nullptr;
    uint64_t now = mach_absolute_time();
    if (used) {
        idleSince_ = 0;
        return;
    }
    if (!idleSince_) {
        idleSince_ = now;
        return;
    }
    uint64_t limit = 0;
    nanoseconds_to_absolutetime((uint64_t)idleSec_ * 1000000000ull, &limit);
    if (now - idleSince_ >= limit && !idleQueued_) {
        idleQueued_ = true;
        thread_call_enter(idleCall_);       // unloading stops this poller: not from here
    }
}

void NVBringup::idleThunk(thread_call_param_t self, thread_call_param_t)
{
    NVBringup *d = (NVBringup *)self;
    IOLockLock(d->powerLock_);
    bool idle = d->pwrMode_ == kModeAuto && d->pwrState_ == kPwrOn;
    if (idle) {                             // still unused? (an open holds powerLock_)
        IOLockLock(d->gspLock_);
        GspState *g = d->gsp_;
        idle = g && g->booted;
        for (uint32_t i = 0; idle && i < kMaxConns; i++)
            idle = g->conns[i] == nullptr;
        IOLockUnlock(d->gspLock_);
    }
    if (idle)
        d->gpuPowerOff("idle");
    d->idleSince_ = 0;
    d->idleQueued_ = false;
    IOLockUnlock(d->powerLock_);
}

bool NVBringup::rawPowerOff(const char *why)
{
    if (!acpi_ || !pci_)
        return false;
    pci_->saveDeviceState();
    // MSI address/data are ours to keep across D3cold (the interrupt source stays registered).
    msiCap_ = 0;
    uint32_t ctl = pci_->findPCICapability(kIOPCICapabilityIDMSI, &msiCap_) ? pci_->configRead16(msiCap_ + 2u) : 0;
    if (msiCap_) {
        bool wide = ctl & 0x80;
        msiSave_[0] = ctl;
        msiSave_[1] = pci_->configRead32(msiCap_ + 4u);
        msiSave_[2] = wide ? pci_->configRead32(msiCap_ + 8u) : 0;
        msiSave_[3] = pci_->configRead16(msiCap_ + (wide ? 12u : 8u));
    }
    IOReturn r = acpi_->evaluateObject("_OFF");
    bool gone = false;
    for (int i = 0; i < 20 && !gone; i++) {
        gone = pci_->configRead16(kIOPCIConfigVendorID) == 0xffff;
        if (!gone)
            IOSleep(10);
    }
    LOG("power: GPU off (%s): ACPI _OFF 0x%x, config space %s", why, r, gone ? "gone (D3cold)" : "still readable");
    return r == kIOReturnSuccess;
}

bool NVBringup::rawPowerOn(const char *why)
{
    if (!acpi_ || !pci_)
        return false;
    const char *m = "_ON";
    IOReturn r = acpi_->evaluateObject(m);
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        up = pci_->configRead16(kIOPCIConfigVendorID) == 0x10de;
        if (!up)
            IOSleep(10);
    }
    if (!up && acpiHas("_PS0")) {
        m = "_PS0";
        r = acpi_->evaluateObject(m);
        for (int i = 0; i < 100 && !up; i++) {
            up = pci_->configRead16(kIOPCIConfigVendorID) == 0x10de;
            if (!up)
                IOSleep(10);
        }
    }
    if (!up) {
        LOG("power: GPU on (%s): ACPI %s 0x%x but config space still reads 0x%04x", why, m, r,
            pci_->configRead16(kIOPCIConfigVendorID));
        return false;
    }
    pci_->restoreDeviceState();
    if (msiCap_) {
        bool wide = msiSave_[0] & 0x80;
        pci_->configWrite32(msiCap_ + 4u, msiSave_[1]);
        if (wide)
            pci_->configWrite32(msiCap_ + 8u, msiSave_[2]);
        pci_->configWrite16(msiCap_ + (wide ? 12u : 8u), (uint16_t)msiSave_[3]);
        pci_->configWrite16(msiCap_ + 2u, (uint16_t)msiSave_[0]);
    }
    uint16_t cmd = pci_->configRead16(kIOPCIConfigCommand);
    LOG("power: GPU on (%s): ACPI %s 0x%x; command 0x%04x, BAR0 0x%08x, BOOT_0 0x%08x", why, m, r, cmd,
        pci_->configRead32(kIOPCIConfigBaseAddress0), rd32(0));

    // From cold, the GPU's own boot firmware (GFW: devinit, FWSEC from IFR) runs first and leaves
    // WPR2 and the falcons in between states until it finishes. Wait for it as r570 does
    // (kgspWaitForGfwBootOk_TU102: read protection lowered, then GFW_BOOT progress 0xff; 4 s).
    uint32_t plm = 0, gfw = 0, ms = 0;
    for (; ms < 4000; ms += 5) {
        plm = rd32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK);
        gfw = (plm & 1) ? rd32(NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT) : 0;
        if ((plm & 1) && (gfw & 0xff) == 0xff)
            break;
        IOSleep(5);
    }
    bool done = (plm & 1) && (gfw & 0xff) == 0xff;
    LOG("power: GFW boot %s after %u ms (PLM 0x%x, GFW_BOOT 0x%x)", done ? "complete" : "NOT complete", ms, plm, gfw);
    return done;
}

// powerLock_ held. Leaves the GPU off (kPwrOff), or on if the unload fails.
bool NVBringup::gpuPowerOff(const char *why)
{
    if (pwrState_ == kPwrOff)
        return true;
    if (pwrState_ != kPwrOn)
        return false;
    pwrState_ = kPwrSwitching;
    publishPower();
    if (gsp_ && gsp_->booted) {
        IOReturn r = unloadGsp(why);
        if (r != kIOReturnSuccess) {
            // Don't cut power under a GSP-RM in an unknown state; leave it for the next boot.
            LOG("power: not powering off (%s): GSP-RM unload failed (0x%x)", why, r);
            pwrState_ = kPwrFailed;
            publishPower();
            return false;
        }
    }
    bool ok = rawPowerOff(why);
    pwrState_ = kPwrOff;
    pwrOffCount_++;
    publishPower();
    setProperty("NVGspResult", "powered off (idle)");
    return ok;
}

// powerLock_ held. On success GSP-RM runs again.
bool NVBringup::gpuPowerOn(const char *why)
{
    if (pwrState_ == kPwrOn)
        return true;
    if (pwrState_ != kPwrOff)
        return false;
    pwrState_ = kPwrSwitching;
    publishPower();
    rawOn_ = false;                         // a pending after-wake power-off no longer applies
    uint64_t t0 = mach_absolute_time(), ns = 0;
    bool ok = rawPowerOn(why);
    if (ok) {
        resumeGsp(why);                     // FRTS + GSP-RM boot, as on wake
        ok = gsp_ && gsp_->booted;
    }
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
    lastOnMs_ = (uint32_t)(ns / 1000000);
    pwrState_ = ok ? kPwrOn : kPwrFailed;
    if (ok)
        pwrOnCount_++;
    idleSince_ = 0;
    LOG("power: GPU %s (%s) after %u ms", ok ? "ready" : "NOT ready", why, lastOnMs_);
    publishPower();
    return ok;
}

// ---- system sleep, restart, power off ----------------------------------------------------

void NVBringup::powerSleep(UInt32 messageType)
{
    if (!powerLock_)
        return;
    IOLockLock(powerLock_);
    if (pwrState_ == kPwrOff && !rawOn_) {
        // The platform and IOPCIFamily expect a powered device around sleep; the next OS too.
        const char *why = messageType == kIOMessageSystemWillSleep ? "sleep" :
                          messageType == kIOMessageSystemWillRestart ? "restart" : "power off";
        rawOn_ = rawPowerOn(why);
        if (rawOn_)
            setProperty("NVGspResult", "powered on without GSP-RM (sleep/restart)");
    }
    IOLockUnlock(powerLock_);
}

void NVBringup::powerWake()
{
    if (rawOn_ && rawOffCall_)
        thread_call_enter(rawOffCall_);     // don't hold up the wake
}

void NVBringup::rawOffThunk(thread_call_param_t self, thread_call_param_t)
{
    NVBringup *d = (NVBringup *)self;
    IOLockLock(d->powerLock_);
    if (d->rawOn_ && d->pwrState_ == kPwrOff) {
        d->rawOn_ = false;
        d->rawPowerOff("after wake");
        d->setProperty("NVGspResult", "powered off (idle)");
    }
    IOLockUnlock(d->powerLock_);
}

// ---- control client ----------------------------------------------------------------------

IOReturn NVBringup::powerCall(uint32_t selector, IOExternalMethodArguments *a)
{
    if (!powerLock_ || !gspLock_)
        return kIOReturnNotReady;
    IOReturn r = kIOReturnSuccess;
    if (selector == NVMAC_POWER_INFO) {
        // Device info without powering on: NVK lists the GPU with this and only opens it (which
        // powers it on) when a Vulkan device is created.
        if (a->structureOutputSize < sizeof(nvmac_info) || !a->structureOutput)
            return kIOReturnBadArgument;
        if (pwrMode_ == kModeOff)
            return kIOReturnNoDevice;       // switched off: not listed (opens would fail)
        nvmac_info i;
        IOLockLock(gspLock_);
        bool live = pwrState_ == kPwrOn && gsp_ && gsp_->booted && gsp_->vram && infoCached_;
        if (live)
            fillInfo(&i);
        else if (infoCached_) {
            i = infoCache_;
            i.vram_used = 0;                // nothing allocated while it's off
            i.bar1_used = 0;
        }
        IOLockUnlock(gspLock_);
        if (!live && !infoCached_)
            return kIOReturnNotReady;
        memcpy(a->structureOutput, &i, sizeof(i));
        a->structureOutputSize = sizeof(i);
        return kIOReturnSuccess;
    }
    if (selector == NVMAC_PERF) {
        if ((a->scalarInputCount != 0 && a->scalarInputCount != 2) || a->scalarOutputCount != NVMAC_PERF_COUNT ||
            (a->scalarInputCount == 2 && (a->scalarInput[0] > NVMAC_BOOST_TO_MAX ||
                                          (a->scalarInput[1] > 3600 && a->scalarInput[1] != 0xffffffffull))))
            return kIOReturnBadArgument;
        IOLockLock(gspLock_);
        if (pwrState_ != kPwrOn || !gsp_ || !gsp_->booted) {
            IOLockUnlock(gspLock_);
            return kIOReturnNoDevice;       // asking doesn't power the GPU on
        }
        uint32_t st = 0;
        if (a->scalarInputCount == 2) {
            perfBoostLocked((uint32_t)a->scalarInput[0], (uint32_t)a->scalarInput[1], &st);
            LOG("perf: boost %llu for %llu s by user: status 0x%x", a->scalarInput[0], a->scalarInput[1], st);
        }
        a->scalarOutput[0] = pstateLocked();
        a->scalarOutput[1] = st;
        a->scalarOutput[2] = boostHeldLocked();
        a->scalarOutput[3] = boostCount_;
        IOLockUnlock(gspLock_);
        return kIOReturnSuccess;
    }
    if (selector == NVMAC_PERF_POLICY) {
        const uint64_t *in = a->scalarInput;
        if ((a->scalarInputCount != 0 && a->scalarInputCount != NVMAC_PERF_POLICY_COUNT) ||
            a->scalarOutputCount != NVMAC_PERF_POLICY_OUT ||
            (a->scalarInputCount && (in[0] > NVMAC_PERF_POLICY_ADAPTIVE || !in[1] || in[1] > 3600 ||
                                     in[2] > NVMAC_BOOST_TO_MAX || !in[3] || in[3] > 100 ||
                                     in[4] < 20 || in[4] > 10000)))
            return kIOReturnBadArgument;
        IOLockLock(gspLock_);
        if (a->scalarInputCount) {
            uint32_t st = 0;
            if (in[0] != boostPolicy_ && boostLevel_ && gsp_ && gsp_->booted)
                perfBoostLocked(NVMAC_BOOST_CLEAR, 0, &st);     // a new policy starts unboosted
            if (in[0] != boostPolicy_)
                boostLevel_ = NVMAC_BOOST_CLEAR;
            boostPolicy_ = (uint32_t)in[0];
            boostSec_ = (uint32_t)in[1];
            boostBurst_ = (uint32_t)in[2];
            boostBusyPct_ = (uint32_t)in[3];
            boostIdleMs_ = (uint32_t)in[4];
            LOG("perf: boost policy %llu (seconds %llu, burst %llu, busy %llu%%, idle %llu ms) by user",
                in[0], in[1], in[2], in[3], in[4]);
        }
        uint64_t *out = a->scalarOutput;
        out[0] = boostPolicy_;
        out[1] = boostSec_;
        out[2] = boostBurst_;
        out[3] = boostBusyPct_;
        out[4] = boostIdleMs_;
        out[5] = boostHeldLocked();
        out[6] = boostEwma_;
        out[7] = boostSent_[NVMAC_BOOST_TO_MAX];
        out[8] = boostSent_[NVMAC_BOOST_1LEVEL];
        out[9] = boostSent_[NVMAC_BOOST_CLEAR];
        IOLockUnlock(gspLock_);
        return kIOReturnSuccess;
    }
    if (selector == NVMAC_POWER_SET) {
        if (a->scalarInputCount != 1 || a->scalarInput[0] > NVMAC_POWER_MODE_AUTO)
            return kIOReturnBadArgument;
        if (!pwrCapable_ && a->scalarInput[0] != NVMAC_POWER_MODE_ON)
            return kIOReturnUnsupported;    // no ACPI method to cut the power with
        int mode = (int)a->scalarInput[0];
        IOLockLock(powerLock_);
        if (mode == kModeOn) {
            pwrMode_ = kModeOn;
            if (pwrState_ == kPwrOff && !gpuPowerOn("switched on"))
                r = kIOReturnNotResponding;
        } else if (mode == kModeOff) {
            bool used = false;
            IOLockLock(gspLock_);
            for (uint32_t i = 0; gsp_ && i < kMaxConns && !used; i++)
                used = gsp_->conns[i] != nullptr;
            IOLockUnlock(gspLock_);
            if (used) {
                r = kIOReturnBusy;          // a program has the GPU open
            } else {
                pwrMode_ = kModeOff;
                if (pwrState_ == kPwrOn && !gpuPowerOff("switched off"))
                    r = kIOReturnError;
            }
        } else {
            pwrMode_ = kModeAuto;
            if (!idleSec_)
                idleSec_ = 30;
            idleSince_ = 0;
        }
        LOG("power: mode %s by user (%s)", kModeNames[pwrMode_], r == kIOReturnSuccess ? "ok" : "failed");
        publishPower();
        IOLockUnlock(powerLock_);
    } else if (selector != NVMAC_POWER_STATUS) {
        return kIOReturnBadArgument;
    }
    if (a->scalarOutputCount != NVMAC_POWER_STATUS_COUNT)
        return r == kIOReturnSuccess ? kIOReturnBadArgument : r;
    uint32_t conns = 0;
    IOLockLock(gspLock_);
    for (uint32_t i = 0; gsp_ && i < kMaxConns; i++)
        conns += gsp_->conns[i] != nullptr;
    IOLockUnlock(gspLock_);
    a->scalarOutput[0] = (uint64_t)pwrState_;
    a->scalarOutput[1] = (uint64_t)pwrMode_;
    a->scalarOutput[2] = idleSec_;
    a->scalarOutput[3] = pwrOffCount_;
    a->scalarOutput[4] = pwrOnCount_;
    a->scalarOutput[5] = lastOnMs_;
    a->scalarOutput[6] = conns;
    return r;
}
