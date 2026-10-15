// HDMI audio, the GPU's half (boot-arg nvhdmiaudio, default 0: none of this runs).
//
//   bit 0 (1)  RM side. After an HDMI modeset GSP-RM gets the sink's audio state as NVKMS gives it
//              (nvHdmiDpEnableDisableAudio, nvkms-hdmi.c; nouveau r535_sor_hdmi_audio / r535_sor_hda_eld): the
//              unmuting general control packet through SET_OD_PACKET, SET_HDMI_AUDIO_MUTESTREAM(false), then the
//              ELD built from the EDID (nv_eld_build) through DFP_SET_ELD_AUDIO_CAPS with PD=1 ELDV=1. GSP-RM
//              writes it into the GPU's HDA codec (PCI function 1), whose pin then reports presence and a valid
//              ELD to whatever HDA driver runs there. Before a modeset of a display with audio on, the ELD goes
//              invalid (PD=1 ELDV=0); at an unplug (also the kept-lit one) or a detach it is cleared (PD=0 ELDV=0)
//              and the stream muted. The audio InfoFrame, the ACR packets (N/CTS) and the stream are the HDA side's:
//              the codec's DIP buffer and converter format are set by the audio driver, and Turing's SF registers
//              have no audio InfoFrame slot (nvhdmipkt_9171.c translatePacketType). NVKMS's
//              OS_UNIX_AUDIO_DYNAMIC_POWER around the ELD write wakes the HDA function on Linux (runtime PM) in the
//              kernel RM, not in GSP-RM: here function 1 stays in D0 (IOPCIFamily), so it is not sent.
//   bit 1 (2)  HDA codec probe: after the ELD, reads it back through function 1's own registers (immediate command
//              interface), so the GPU half can be checked with no macOS audio driver. Only while nothing drives
//              function 1; its command register and GCTL.CRST are put back afterwards. Runs outside gspLock_.
//   bit 2 (4)  The Linux quirk (drivers/pci/quirks.c quirk_nvidia_hda, not in our reference tree): many laptop
//              firmwares hide function 1 by clearing bit 25 of function 0's config register 0x488. Set here if
//              clear; macOS enumerated the bus long before, so it only helps whatever enumerates next (logged).
//   bit 3 (8)  Snoop on function 1, as Linux does for every NVIDIA HDA controller before using it (hda_intel.c
//              azx_init_pci, AZX_SNOOP_TYPE_NVIDIA, v6.12): config 0x4e low nibble = 0xf (TRANSREG ENABLE_COHBITS),
//              0x4d bit 0 (ISTRM_COH), 0x4c bit 0 (OSTRM_COH). Without it the controller's CORB/RIRB and stream DMA
//              may miss the CPU caches (inferred: AppleHDA's codec setup asserted, IOHDACodecDevice line 146,
//              2026-10-16, while the probe's immediate commands, which use no DMA, worked). Done at start, before
//              AppleHDAController attaches. The three bytes are logged before and after whenever nvhdmiaudio is set.
//
// Turing only (C57x display, deviceEntry 0 / head 0 as everywhere in this display code).

#include "NVBringup.hpp"
#include "nv_gsp_state.h"

#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

uint32_t NVBringup::hdmiAudioArg()
{
    uint32_t arg = 0;
    PE_parse_boot_argn("nvhdmiaudio", &arg, sizeof(arg));
    return arg;
}

// NV0073_CTRL_CMD_DFP_SET_ELD_AUDIO_CAPS (0x731144; ctrl0073dfp.h): u32 subDeviceInstance, displayId, numELDSize,
// u8 bufferELD[96] at 12, u32 maxFreqSupported at 108, ctrl at 112 (bit 0 PD, bit 1 ELDV), deviceEntry at 116.
bool NVBringup::dispEldSend(uint32_t displayId, const uint8_t *eld, uint32_t maxFreq, bool pd, bool eldv)
{
    GspState *g = gsp_;
    uint8_t p[120] = {};
    uint32_t st = 0;
    put32(p, 4, displayId);
    if (eld && eldv) {
        put32(p, 8, 96);                                // NVKMS sends the whole buffer
        memcpy(p + 12, eld, 96);
        put32(p, 108, maxFreq);
    }
    put32(p, 112, (pd ? 1u : 0) | (eld && eldv ? 2u : 0));
    put32(p, 116, 0);                                   // device entry 0 (SST HDMI, GetAudioDeviceEntry)
    const bool ok = gspRmControl(g->hClient, g->hDisp, 0x731144, p, sizeof(p), &st);
    LOG("GSP: display: HDMI audio: SET_ELD_AUDIO_CAPS 0x%x PD %u ELDV %u maxFreq %u: %s (status 0x%x)", displayId,
        pd, eld && eldv, eld && eldv ? maxFreq : 0, ok ? "ok" : "FAILED", st);
    return ok;
}

// Audio on (sink given) or off (nullptr) for the lit HDMI display: GCP mute state through SET_OD_PACKET, the stream
// mute, the ELD. gspLock_ held.
void NVBringup::dispHdmiAudio(uint32_t displayId, const nv_sink_caps *sink)
{
    if (!(hdmiAudioArg() & 1))
        return;
    GspState *g = gsp_;
    GspState::DispHw &d = g->disp;
    uint32_t st = 0;
    uint8_t eld[96];
    uint32_t maxFreq = 0, eldLen = 0;
    if (sink) {
        eldLen = nv_eld_build(sink, displayId, eld, &maxFreq);
        if (!eldLen) {
            LOG("GSP: display: HDMI audio: the sink has no audio (no SAD, no basic audio): ELD cleared");
            dispEldSend(displayId, nullptr, 0, false, false);
            d.audioOn = false;
            setProperty("NVDisplayAudio", "none (the sink has no audio)");
            return;
        }
    } else if (!d.audioOn) {
        return;
    }
    const bool on = sink != nullptr;
    // NV0073_CTRL_SPECIFIC_SET_OD_PACKET_PARAMS (0x730288; ctrl0073specific.h): u32 sub, displayId,
    // transmitControl (ENABLE_YES, the rest 0: every frame, SW video format, no legacy mode), packetSize,
    // targetHead; NvBool bUsePsrHeadforSdp at 20; u8 aPacket[36] at 21. The packet is the general control
    // packet: HB0 3, SB0 0x10 clear AVMUTE / 0x01 set AVMUTE (NVKMS EnableHdmiAudio, nouveau r535_sor_hdmi_ctrl_audio).
    uint8_t od[60] = {};
    put32(od, 4, displayId);
    put32(od, 8, 1);
    put32(od, 12, 10);
    od[21] = 0x03;
    od[24] = on ? 0x10 : 0x01;
    if (!gspRmControl(g->hClient, g->hDisp, 0x730288, od, sizeof(od), &st))
        LOG("GSP: display: HDMI audio: SET_OD_PACKET (GCP %s) failed (status 0x%x)", on ? "unmute" : "mute", st);
    // NV0073_CTRL_CMD_SPECIFIC_SET_HDMI_AUDIO_MUTESTREAM_PARAMS (0x730275): u8 sub, u32 displayId, u8 mute.
    uint8_t ms[12] = {};
    put32(ms, 4, displayId);
    ms[8] = on ? 0 : 1;
    if (!gspRmControl(g->hClient, g->hDisp, 0x730275, ms, sizeof(ms), &st))
        LOG("GSP: display: HDMI audio: SET_HDMI_AUDIO_MUTESTREAM(%u) failed (status 0x%x)", ms[8], st);
    const bool ok = dispEldSend(displayId, on ? eld : nullptr, maxFreq, on, on);
    d.audioOn = on && ok;
    if (on) {
        char hex[3 * 40 + 1] = {};
        for (uint32_t i = 0; i < eldLen && i < 40; i++)
            snprintf(hex + i * 3, 4, "%02x ", eld[i]);
        LOG("GSP: display: HDMI audio: ELD %u bytes (%u SADs, speakers 0x%02x): %s", eldLen, sink->nSad,
            sink->speaker, hex);
        char s[96];
        snprintf(s, sizeof(s), "%s: ELD %u bytes, %u SADs, max rate %u, speakers 0x%02x", ok ? "on" : "ELD refused",
                 eldLen, sink->nSad, maxFreq, sink->speaker);
        setProperty("NVDisplayAudio", s);
        if (hdmiAudioArg() & 2)
            hdaProbeWant_ = true;
    } else {
        setProperty("NVDisplayAudio", "off");
    }
}

// Before a modeset of a display whose audio is on: the ELD invalid, the presence kept (NV_ELD_PRE_MODESET).
void NVBringup::dispHdmiAudioPreModeset(uint32_t displayId)
{
    if ((hdmiAudioArg() & 1) && gsp_->disp.audioOn) {
        dispEldSend(displayId, nullptr, 0, true, false);
        gsp_->disp.audioOn = false;
    }
}

// ---- function 1 -----------------------------------------------------------------------------------------------

// The GPU's HDA function: the IOPCIDevice next to pci_ with function number 1 (retained), or nullptr.
IOPCIDevice *NVBringup::hdaFunction()
{
    IORegistryEntry *parent = pci_ ? pci_->getParentEntry(gIOServicePlane) : nullptr;
    OSIterator *it = parent ? parent->getChildIterator(gIOServicePlane) : nullptr;
    IOPCIDevice *found = nullptr;
    if (!it)
        return nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOPCIDevice *p = OSDynamicCast(IOPCIDevice, o);
        if (p && p != pci_ && p->getBusNumber() == pci_->getBusNumber() &&
            p->getDeviceNumber() == pci_->getDeviceNumber() && p->getFunctionNumber() == 1) {
            found = p;
            found->retain();
            break;
        }
    }
    it->release();
    return found;
}

// At start (nvhdmiaudio set): is function 1 there, and what does function 0's 0x488 say. Bit 2 sets bit 25.
void NVBringup::hdaCheckFunction()
{
    const uint32_t arg = hdmiAudioArg();
    if (!arg || !pci_)
        return;
    const uint32_t r488 = pci_->configRead32(0x488);
    const uint8_t hdr = pci_->configRead8(kIOPCIConfigHeaderType);
    IOPCIDevice *f1 = hdaFunction();
    char s[160];
    snprintf(s, sizeof(s), "function 1 %s%s; 0x488 = 0x%08x (bit 25 %s), header type 0x%02x (%s)",
             f1 ? "present" : "ABSENT", f1 && f1->getClient() ? ", driven" : f1 ? ", no driver" : "", r488,
             r488 & (1u << 25) ? "set" : "clear", hdr, hdr & 0x80 ? "multi-function" : "single function");
    LOG("HDMI audio: %s", s);
    setProperty("NVHdaFunction", s);
    if (!f1 && !(r488 & (1u << 25)) && (arg & 4)) {
        pci_->configWrite32(0x488, r488 | 1u << 25);
        LOG("HDMI audio: 0x488 bit 25 set (now 0x%08x, header type 0x%02x): function 1 shows up only when the bus "
            "is enumerated again (not by macOS while running)", pci_->configRead32(0x488),
            pci_->configRead8(kIOPCIConfigHeaderType));
    }
    if (f1) {
        const uint8_t t0 = f1->configRead8(0x4e), i0 = f1->configRead8(0x4d), o0 = f1->configRead8(0x4c);
        if (arg & 8) {
            f1->configWrite8(0x4e, (uint8_t)((t0 & ~0x0f) | 0x0f));
            f1->configWrite8(0x4d, (uint8_t)(i0 | 1));
            f1->configWrite8(0x4c, (uint8_t)(o0 | 1));
        }
        LOG("HDMI audio: function 1 snoop: 0x4e 0x%02x 0x4d 0x%02x 0x4c 0x%02x%s (now 0x%02x 0x%02x 0x%02x)", t0, i0,
            o0, arg & 8 ? ", set as Linux does" : ", left (bit 3 sets it)", f1->configRead8(0x4e),
            f1->configRead8(0x4d), f1->configRead8(0x4c));
    }
    OSSafeReleaseNULL(f1);
}

// HDA controller registers (HDA spec 3.3) and the codec verbs used (4.4, 7.3.3; the HDMI ones from 015a).
enum : uint32_t {
    kHdaGcap = 0x00, kHdaGctl = 0x08, kHdaStatests = 0x0e, kHdaIcoi = 0x60, kHdaIcii = 0x64, kHdaIcis = 0x68,
};

static inline uint32_t hdaRd32(volatile uint8_t *b, uint32_t o) { return *(volatile uint32_t *)(b + o); }
static inline uint16_t hdaRd16(volatile uint8_t *b, uint32_t o) { return *(volatile uint16_t *)(b + o); }
static inline void hdaWr32(volatile uint8_t *b, uint32_t o, uint32_t v) { *(volatile uint32_t *)(b + o) = v; }
static inline void hdaWr16(volatile uint8_t *b, uint32_t o, uint16_t v) { *(volatile uint16_t *)(b + o) = v; }

// One verb through the immediate command interface: ICOI, ICIS.ICB, wait for ICIS.IRV, ICII. False on a timeout.
static bool hdaVerb(volatile uint8_t *b, uint32_t cad, uint32_t nid, uint32_t verb, uint32_t payload, uint32_t *resp)
{
    for (int i = 0; i < 100 && (hdaRd16(b, kHdaIcis) & 1); i++)
        IODelay(10);
    if (hdaRd16(b, kHdaIcis) & 1)
        return false;
    hdaWr16(b, kHdaIcis, 2);                            // IRV is write-1-to-clear
    hdaWr32(b, kHdaIcoi, cad << 28 | nid << 20 | verb << 8 | (payload & 0xff));
    hdaWr16(b, kHdaIcis, 1);
    for (int i = 0; i < 100; i++) {
        const uint16_t s = hdaRd16(b, kHdaIcis);
        if (!(s & 1) && (s & 2)) {
            *resp = hdaRd32(b, kHdaIcii);
            return true;
        }
        IODelay(10);
    }
    return false;
}

// nvhdmiaudio bit 1: the ELD as function 1's codec reports it. gspLock_ not held (dispNubSync).
void NVBringup::hdaProbe()
{
    IOPCIDevice *f1 = hdaFunction();
    if (!f1) {
        LOG("HDMI audio: probe: no function 1");
        return;
    }
    if (f1->getClient()) {
        LOG("HDMI audio: probe: function 1 has a driver (%s): not touched", f1->getClient()->getMetaClass()->getClassName());
        f1->release();
        return;
    }
    const uint16_t cmd = f1->configRead16(kIOPCIConfigCommand);
    if (!(cmd & 2))
        f1->configWrite16(kIOPCIConfigCommand, cmd | 2);
    IOMemoryMap *map = f1->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
    volatile uint8_t *b = map ? (volatile uint8_t *)map->getVirtualAddress() : nullptr;
    uint32_t gctl0 = 0;
    bool reset = false;
    if (!b || map->getLength() < 0x70) {
        LOG("HDMI audio: probe: BAR0 of function 1 can't be mapped");
        goto out;
    }
    gctl0 = hdaRd32(b, kHdaGctl);
    LOG("HDMI audio: probe: GCAP 0x%04x, version %u.%u, GCTL 0x%08x, STATESTS 0x%04x", hdaRd16(b, kHdaGcap),
        b[3], b[2], gctl0, hdaRd16(b, kHdaStatests));
    if (gctl0 == 0xffffffff) {
        LOG("HDMI audio: probe: function 1 reads all ones (powered down?)");
        goto out;
    }
    if (!(gctl0 & 1)) {                                 // in reset: out of it, then the codecs report (>= 521 us)
        hdaWr32(b, kHdaGctl, gctl0 | 1);
        for (int i = 0; i < 100 && !(hdaRd32(b, kHdaGctl) & 1); i++)
            IODelay(1000);
        reset = true;
        for (int i = 0; i < 50 && !hdaRd16(b, kHdaStatests); i++)
            IODelay(1000);
    }
    {
        const uint16_t codecs = hdaRd16(b, kHdaStatests);
        if (!codecs) {
            LOG("HDMI audio: probe: no codec (STATESTS 0, GCTL 0x%08x)", hdaRd32(b, kHdaGctl));
            goto out;
        }
        const uint32_t cad = (uint32_t)__builtin_ctz(codecs);
        uint32_t vid = 0, sub = 0, fg = 0;
        if (!hdaVerb(b, cad, 0, 0xf00, 0x00, &vid) || !hdaVerb(b, cad, 0, 0xf00, 0x04, &sub)) {
            LOG("HDMI audio: probe: codec %u doesn't answer immediate commands (ICIS 0x%04x)", cad,
                hdaRd16(b, kHdaIcis));
            goto out;
        }
        LOG("HDMI audio: probe: codec %u vendor 0x%08x, function groups %u from node %u", cad, vid, sub & 0xff,
            (sub >> 16) & 0xff);
        const uint32_t afg = (sub >> 16) & 0xff;
        uint32_t wsub = 0;
        if (!hdaVerb(b, cad, afg, 0xf00, 0x05, &fg) || !hdaVerb(b, cad, afg, 0xf00, 0x04, &wsub))
            goto out;
        const uint32_t first = (wsub >> 16) & 0xff, count = wsub & 0xff;
        for (uint32_t nid = first; nid < first + count && nid < 0x80; nid++) {
            uint32_t caps = 0, sense = 0, size = 0;
            if (!hdaVerb(b, cad, nid, 0xf00, 0x09, &caps) || ((caps >> 20) & 0xf) != 4)
                continue;                               // pins only
            hdaVerb(b, cad, nid, 0xf09, 0, &sense);     // bit 31 presence, bit 30 ELD valid
            hdaVerb(b, cad, nid, 0xf2e, 0x08, &size);   // the ELD buffer's size (DIP size, ELD)
            char hex[3 * 36 + 1] = {};
            uint32_t n = 0;
            if (sense & (1u << 30)) {
                for (; n < 36 && n < (size & 0xff); n++) {
                    uint32_t v = 0;
                    if (!hdaVerb(b, cad, nid, 0xf2f, n, &v) || !(v & 0x80000000))
                        break;
                    snprintf(hex + n * 3, 4, "%02x ", v & 0xff);
                }
            }
            LOG("HDMI audio: probe: pin %u: presence %u, ELD valid %u, ELD size %u%s%s", nid, sense >> 31,
                (sense >> 30) & 1, size & 0xff, n ? ": " : "", hex);
        }
        LOG("HDMI audio: probe: done (AFG %u type 0x%x, %u widgets)", afg, fg & 0xff, count);
    }
out:
    if (b && reset)
        hdaWr32(b, kHdaGctl, gctl0);                    // back into reset, as found
    OSSafeReleaseNULL(map);
    if (!(cmd & 2))
        f1->configWrite16(kIOPCIConfigCommand, cmd);
    f1->release();
}
