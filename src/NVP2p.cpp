// Whether another PCIe device may DMA to or from the GPU's BAR1 (peer-to-peer).
//
// Direct I/O (F_NOCACHE) on a CPU mapping of VRAM makes the disk controller DMA straight to the BAR1 address
// (docs/bar1-cpu-mappings.md). That is a peer-to-peer transfer, and not every root complex forwards one: on
// this Mac (Coffee Lake-H, host bridge 8086:3e10, the GPU and the NVMe SSD on different root ports) one panicked
// the machine on 2026-11-17 ("nvme: Controller fatal bit set. Write"). The policy is Linux's
// (drivers/pci/p2pdma.c, calc_map_type_and_dist): a pair may do peer-to-peer when
// 1. they share an upstream bridge (in practice a PCIe switch; a root port counts too, as for two functions of
//    one device) and no port on the path between them redirects peer requests upstream (ACS RR, CR or EC set:
//    the transfer then goes through the root complex after all, so 2 or 3 must hold), or
// 2. the CPU is AMD family 17h (Zen) or newer, whose root complexes forward peer-to-peer, or
// 3. the root complex is one known to forward it: the first device on the host bridge's root bus, if it is
//    00.0 or a root port, is in the list below. Some entries only hold for two devices under the same host bridge.
// Only the decision follows Linux; this code and the table layout are this driver's own. The device IDs are
// facts about the hardware, taken from Linux's list (and, for the named constants, its pci_ids.h).
//
// The peer of a direct I/O is the disk's controller, which the driver doesn't know when it maps VRAM: any disk
// could be the target. So p2pAllowed() asks it for every PCI device with a disk (IOMedia) under it, the GPU's
// own functions aside, and allows peer-to-peer only if every one passes. nvmac_info.features has
// NVMAC_FEATURE_P2P only then. The driver can't stop a process doing direct I/O on its own mapping; the bit is
// what tools (bar1_alias_test release-dio) check before they make a disk DMA to BAR1.

#include "NVBringup.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>

#define LOG(fmt, ...) log(fmt "\n", ##__VA_ARGS__)

namespace {

enum : uint16_t { kVendorIntel = 0x8086, kVendorNvidia = 0x10de, kVendorGoogle = 0x1ae0, kVendorZhaoxin = 0x1d17,
                  kVendorAlibaba = 0x1ded };
enum : int32_t { kAnyDevice = -1 };

struct HostBridgeEntry {
    uint16_t vendor;
    int32_t  device;            // kAnyDevice: every device of the vendor
    bool     sameHostBridge;    // only for two devices under this one host bridge
};

const HostBridgeEntry kHostBridges[] = {
    { kVendorIntel, 0x3c00, true },   { kVendorIntel, 0x3c01, true },   // Xeon E5 / Core i7 (Sandy Bridge-E)
    { kVendorIntel, 0x6f00, true },                                     // Xeon E7 v4 / E5 v4 / E3 v4 / D
    { kVendorIntel, 0x2f00, true },   { kVendorIntel, 0x2f01, true },   // Xeon E7 v3 / E5 v3 / Core i7
    { kVendorIntel, 0x0c08, true },                                     // Haswell client
    { kVendorIntel, 0x2030, false },  { kVendorIntel, 0x2031, false },  // Skylake-E root ports
    { kVendorIntel, 0x2032, false },  { kVendorIntel, 0x2033, false },
    { kVendorIntel, 0x2020, false },
    { kVendorIntel, 0x09a2, false },
    { kVendorIntel, 0x0b25, false },                                    // DSA (Sapphire Rapids)
    { kVendorIntel, 0x0cfe, false },                                    // IAX (Sapphire Rapids)
    { kVendorIntel, 0x11fb, false },                                    // DSA (Granite Rapids-D)
    { kVendorIntel, 0x1212, false },  { kVendorIntel, 0x1216, false },  // DSA, IAA (Diamond Rapids)
    { kVendorIntel, 0x4940, false },  { kVendorIntel, 0x4942, false },  // QAT 4xxx, 401xx
    { kVendorIntel, 0x4944, false },  { kVendorIntel, 0x4946, false },  // QAT 402xx, 420xx
    { kVendorIntel, 0x4948, false },                                    // QAT 6xxx
    { kVendorGoogle, kAnyDevice, false },                               // Google SoCs
    { kVendorNvidia, 0x2f95, false }, { kVendorNvidia, 0x2f96, false }, // NVIDIA CPUs
    { kVendorNvidia, 0x2f97, false }, { kVendorNvidia, 0x2f98, false },
    { kVendorZhaoxin, 0x1003, true }, { kVendorZhaoxin, 0x1005, true }, // KX-6000 / KH-40000 / KX-6000G / KX-7000
    { kVendorZhaoxin, 0x1006, true }, { kVendorZhaoxin, 0x1007, true },
    { kVendorZhaoxin, 0x1008, false },                                  // KH-50000
    { kVendorAlibaba, 0x8000, false },                                  // T-HEAD Yitian 710
};

// The bridge device above d (switch downstream port, switch upstream port, root port), or null at the root bus.
// In IOKit a device's provider is the bridge driver (IOPCI2PCIBridge), whose provider is the bridge's own
// IOPCIDevice; on the root bus that driver is the host bridge's (AppleACPIPCI), whose provider isn't one.
IOPCIDevice *upstream(IOPCIDevice *d)
{
    IOService *br = d->getProvider();
    return br ? OSDynamicCast(IOPCIDevice, br->getProvider()) : nullptr;
}

// The host bridge driver d's hierarchy hangs from.
IOService *hostBridge(IOPCIDevice *d)
{
    for (IOPCIDevice *u; (u = upstream(d));)
        d = u;
    return d->getProvider();
}

int pcieType(IOPCIDevice *d)
{
    IOByteCount off = 0;
    if (!d->extendedFindPCICapability(kIOPCIPCIExpressCapability, &off) || !off)
        return -1;
    uint16_t cap = d->extendedConfigRead16(off + 2);
    return cap == 0xffff ? -1 : (cap >> 4) & 0xf;
}

// ACS P2P Request Redirect, Completion Redirect or Egress Control: peer requests through this port go up to the
// root complex. A port that doesn't answer (0xffff) counts as redirecting.
bool acsRedirects(IOPCIDevice *d)
{
    IOByteCount off = 0;
    if (!d->extendedFindPCICapability(kIOPCIExpressAccessControlServicesCapability, &off) || !off)
        return false;
    uint16_t ctrl = d->extendedConfigRead16(off + 6);
    return ctrl == 0xffff || (ctrl & (0x04 | 0x08 | 0x20));
}

// The device the list is matched against: the first device on the host bridge's root bus (lowest device and
// function), if it is 00.0 or a root port (Skylake and later have no 00.0 host bridge device).
IOPCIDevice *hostBridgeDevice(IOService *host)
{
    if (!host)
        return nullptr;
    IOPCIDevice *first = nullptr;
    unsigned firstFn = ~0u;
    OSIterator *it = host->getClientIterator();
    if (!it)
        return nullptr;
    while (OSObject *o = it->getNextObject()) {
        IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
        if (!d)
            continue;
        unsigned fn = (unsigned)d->getDeviceNumber() << 3 | d->getFunctionNumber();
        if (fn < firstFn) {
            first = d;
            firstFn = fn;
        }
    }
    it->release();
    if (!first || (firstFn != 0 && pcieType(first) != 4 /* root port */))
        return nullptr;
    return first;
}

bool hostBridgeListed(IOService *host, bool sameHostBridge, uint32_t *id)
{
    IOPCIDevice *root = hostBridgeDevice(host);
    *id = 0;
    if (!root)
        return false;
    uint16_t vendor = root->configRead16(kIOPCIConfigVendorID), device = root->configRead16(kIOPCIConfigDeviceID);
    *id = (uint32_t)vendor << 16 | device;
    for (const HostBridgeEntry &e : kHostBridges)
        if (e.vendor == vendor && (e.device == kAnyDevice || e.device == device))
            return !e.sameHostBridge || sameHostBridge;
    return false;
}

void cpuid(uint32_t leaf, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

bool cpuAmdZenOrNewer()
{
    uint32_t r[4];
    cpuid(0, r);
    if (r[1] != 0x68747541 || r[3] != 0x69746e65 || r[2] != 0x444d4163)    // "AuthenticAMD"
        return false;
    cpuid(1, r);
    uint32_t family = (r[0] >> 8) & 0xf;
    if (family == 0xf)
        family += (r[0] >> 20) & 0xff;
    return family >= 0x17;
}

// d is a function of the same device as gpu (its HDA function): not a peer.
bool sameDevice(IOPCIDevice *d, IOPCIDevice *gpu)
{
    return d == gpu || (upstream(d) == upstream(gpu) && hostBridge(d) == hostBridge(gpu) &&
                        d->getBusNumber() == gpu->getBusNumber() && d->getDeviceNumber() == gpu->getDeviceNumber());
}

void bdf(IOPCIDevice *d, char *s, size_t n)
{
    snprintf(s, n, "%02x:%02x.%x", d->getBusNumber(), d->getDeviceNumber(), d->getFunctionNumber());
}

} // namespace

// The pair a, b (either way round) per the policy at the top. why: what decided it, for the log.
bool NVBringup::p2pPair(IOPCIDevice *a, IOPCIDevice *b, char *why, size_t n)
{
    // 1. A common upstream bridge, with no port on the path redirecting. Linux's walk: every bridge above a,
    // and a itself, up to and including the first one b is under; then b's side up to it.
    int acs = 0;
    IOPCIDevice *common = nullptr;
    for (IOPCIDevice *x = a; x && !common; x = upstream(x)) {
        acs += acsRedirects(x);
        for (IOPCIDevice *y = b; y; y = upstream(y))
            if (y == x) {
                common = x;
                break;
            }
    }
    if (common) {
        for (IOPCIDevice *y = b; y && y != common; y = upstream(y))
            acs += acsRedirects(y);
        if (!acs) {
            char s[16];
            bdf(common, s, sizeof s);
            snprintf(why, n, "under the same bridge (%s)", s);
            return true;
        }
    }
    // Through the root complex: 2 or 3.
    const char *path = common ? "ACS redirects it through the root complex" : "no common upstream bridge";
    if (cpuAmdZenOrNewer()) {
        snprintf(why, n, "%s, AMD Zen or newer CPU", path);
        return true;
    }
    IOService *ha = hostBridge(a), *hb = hostBridge(b);
    uint32_t ida = 0, idb = 0;
    bool listed = ha == hb ? hostBridgeListed(ha, true, &ida)
                           : hostBridgeListed(ha, false, &ida) && hostBridgeListed(hb, false, &idb);
    if (listed) {
        snprintf(why, n, "%s, host bridge %04x:%04x listed", path, ida >> 16, ida & 0xffff);
        return true;
    }
    if (!ida)
        snprintf(why, n, "%s, host bridge not identified, CPU not AMD Zen", path);
    else if (ha == hb)
        snprintf(why, n, "%s, host bridge %04x:%04x not listed, CPU not AMD Zen", path, ida >> 16, ida & 0xffff);
    else
        snprintf(why, n, "%s, different host bridges (%04x:%04x, %04x:%04x) not both listed, CPU not AMD Zen", path,
                 ida >> 16, ida & 0xffff, idb >> 16, idb & 0xffff);
    return false;
}

// Every PCI device with a disk under it, against the GPU. Rechecked at most once a second (fillInfo calls
// this; disks come and go with Thunderbolt and USB). Logs when the answer changes. Not with a spinlock held.
bool NVBringup::p2pAllowed()
{
    uint64_t now = mach_absolute_time(), ns = 0;
    absolutetime_to_nanoseconds(now - p2pCheckedAt_, &ns);
    if (p2pCheckedAt_ && ns < 1000000000ull)
        return p2pOk_;
    p2pCheckedAt_ = now;

    IOPCIDevice *peers[16];
    unsigned nPeers = 0;
    bool tooMany = false;
    if (OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOMedia"))) {
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *d = nullptr;
            for (IOService *s = OSDynamicCast(IOService, o); s && !d; s = s->getProvider())
                d = OSDynamicCast(IOPCIDevice, s);
            if (!d || sameDevice(d, pci_))
                continue;               // not on PCI (a disk image), or one of the GPU's own functions
            bool seen = false;
            for (unsigned i = 0; i < nPeers && !seen; i++)
                seen = peers[i] == d;
            if (seen)
                continue;
            if (nPeers == 16) {
                tooMany = true;
                break;
            }
            d->retain();
            peers[nPeers++] = d;
        }
        it->release();
    }

    bool ok = nPeers && !tooMany;
    char why[160] = "", peer[16] = "";
    if (!nPeers)
        snprintf(why, sizeof why, "no disk controller found to check");
    else if (tooMany)
        snprintf(why, sizeof why, "more than 16 disk controllers");
    for (unsigned i = 0; i < nPeers; i++) {
        if (ok) {
            bdf(peers[i], peer, sizeof peer);
            ok = p2pPair(pci_, peers[i], why, sizeof why);
        }
        peers[i]->release();
    }
    if (ok && nPeers > 1)
        snprintf(why, sizeof why, "every disk controller (%u) passes", nPeers);

    if (ok != p2pOk_ || !p2pLogged_ || strncmp(why, p2pWhy_, sizeof p2pWhy_)) {
        if (ok)
            LOG("P2P: disk DMA to BAR1 allowed: %s%s%s", nPeers == 1 ? peer : "", nPeers == 1 ? ": " : "", why);
        else
            LOG("P2P: disk DMA to BAR1 NOT allowed%s%s: %s", peer[0] ? " with " : "", peer, why);
        strlcpy(p2pWhy_, why, sizeof p2pWhy_);
        p2pLogged_ = true;
        char prop[200];
        snprintf(prop, sizeof prop, "%s%s%s: %s", ok ? "allowed" : "not allowed", !ok && peer[0] ? " with " : "",
                 ok ? "" : peer, why);
        setProperty("NVP2P", prop);
    }
    p2pOk_ = ok;
    return ok;
}
