// NVHDAPatch: lets AppleHDAController drive the HDA function of NVIDIA GPUs it doesn't list (a Lilu plugin).
//
// AppleHDAController::probe reads the controller's PCI vendor/device from config space and accepts it only if it is
// in a 57-entry table of 48-byte records (vendor, device, codec mask, per-chip flags; macOS 14.8.9, AppleHDAController
// 540.3, table at 0x13f3b2f0 in SystemKernelExtensions.kc). The NVIDIA records are GT21x/GF1xx/GK10x parts; 10de:10fa
// (TU117) isn't there, and probe returns NULL without a log line (scratchpad/hda-attach-2026-10-16/REPORT.md). The
// record for 10de:0e1b (GK107, index 38, inside the 26-40 range probe treats as NVIDIA) is rewritten in memory to the
// GPU's own device ID when AppleHDAController loads. Its other fields (codec mask 1, flags) are what every other
// NVIDIA record carries. The find pattern (vendor, device and the next 16 bytes) occurs once in the system KC.
//
// The device ID is read from the boot-arg nvhdadid (hex, default 0x10fa). Probe still turns the device down unless it
// is named HDEF/HDAU or the boot-arg hdax=1 is set (Apple's), so hdax=1 goes with this. Disabled by -nvhdaoff.

#include <Headers/kern_api.hpp>
#include <Headers/kern_util.hpp>
#include <Headers/plugin_start.hpp>

static const char *kPath[] {
    "/System/Library/Extensions/AppleHDA.kext/Contents/PlugIns/AppleHDAController.kext/Contents/MacOS/AppleHDAController"
};
static KernelPatcher::KextInfo kController {"com.apple.driver.AppleHDAController", kPath, arrsize(kPath), {true}, {},
                                            KernelPatcher::KextInfo::Unloaded};

// 10de:0e1b, then codec mask/flags words as in every NVIDIA record (dumped from the KC, 2026-10-16).
static uint8_t kFind[] {0xde, 0x10, 0x1b, 0x0e, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00, 0x01, 0x00,
                        0x28, 0x00, 0x28, 0x00};
static uint8_t kReplace[sizeof(kFind)];

static void controllerLoaded(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t, size_t)
{
    if (index != kController.loadIndex)
        return;
    KernelPatcher::LookupPatch p {&kController, kFind, kReplace, sizeof(kFind), 1};
    patcher.applyLookupPatch(&p);
    if (patcher.getError() != KernelPatcher::Error::NoError) {
        SYSLOG("nvhda", "AppleHDAController: table record not found (%d); nothing patched", patcher.getError());
        patcher.clearError();
        return;
    }
    SYSLOG("nvhda", "AppleHDAController: table record 10de:0e1b now 10de:%02x%02x", kReplace[3], kReplace[2]);
}

static void pluginStart()
{
    uint32_t did = 0x10fa;
    char buf[16] {};
    if (PE_parse_boot_argn("nvhdadid", buf, sizeof(buf))) {
        uint32_t v = 0;
        for (const char *s = buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X') ? buf + 2 : buf; *s; s++) {
            const char c = *s;
            const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
                        : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d > 15)
                break;
            v = v << 4 | (uint32_t)d;
        }
        if (v && v <= 0xffff)
            did = v;
    }
    lilu_os_memcpy(kReplace, kFind, sizeof(kFind));
    kReplace[2] = (uint8_t)(did & 0xff);
    kReplace[3] = (uint8_t)(did >> 8);
    SYSLOG("nvhda", "patching AppleHDAController's NVIDIA table for 10de:%04x", did);
    lilu.onKextLoadForce(&kController, 1, controllerLoaded);
}

static const char *kDisable[] {"-nvhdaoff"};
static const char *kDebug[] {"-nvhdadbg"};
static const char *kBeta[] {"-nvhdabeta"};

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal,
    kDisable, arrsize(kDisable),
    kDebug, arrsize(kDebug),
    kBeta, arrsize(kBeta),
    KernelVersion::Sonoma,
    KernelVersion::Tahoe,
    pluginStart
};
