// nvgsp: boots GSP-RM through the NVBringup kext (Phase 3d step 3).
//
//   sudo nvgsp boot [fwdir]    hand the r570 firmware to the kext and boot GSP-RM
//                              (fwdir defaults to firmware/nvidia, linux-firmware layout)
//   sudo nvgsp logs [outdir]   save the four GSP log buffers (raw) to outdir
//   sudo nvgsp unload          tear GSP-RM down and clear WPR2 (also done at shutdown/sleep)
//   nvgsp status               print the kext's GSP log lines (no root needed)
//   nvgsp decode <dir> [elf]   decode saved logs (full text needs NVIDIA's logging ELF)
//
// The kext refuses to boot unless boot-arg nvgsp=1 is set and FRTS succeeded
// this boot; it allows one attempt per cold boot.

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include "../src/nv_hal.h"

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <string>
#include <vector>

enum { kSetFirmware = 0, kBootGsp = 1, kReadLog = 2, kUnloadGsp = 3, kIntr = 4 };              // NVBringupUserClient.hpp
enum { kFwGspElf, kFwGspBootloader, kFwBooterLoad, kFwBooterUnload };                   // NVBringup.hpp
static const char *const kLogNames[] = { "LOGINIT", "LOGINTR", "LOGRM", "LOGMNOC" };

static io_service_t find_driver()
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"));
    if (!s)
        fprintf(stderr, "NVBringup is not loaded (no matching service)\n");
    return s;
}

static bool read_file(const std::string &path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) { perror(path.c_str()); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok && !out.empty();
}

// Prints the NVLog lines that start with "GSP:" (all of them after `from`).
static void print_gsp_log(io_service_t s)
{
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("NVLog"), kCFAllocatorDefault, 0);
    if (!p || CFGetTypeID(p) != CFStringGetTypeID()) {
        fprintf(stderr, "no NVLog property\n");
        if (p) CFRelease(p);
        return;
    }
    CFIndex n = CFStringGetMaximumSizeForEncoding(CFStringGetLength((CFStringRef)p), kCFStringEncodingUTF8) + 1;
    std::vector<char> buf((size_t)n);
    CFStringGetCString((CFStringRef)p, buf.data(), n, kCFStringEncodingUTF8);
    CFRelease(p);
    for (char *line = strtok(buf.data(), "\n"); line; line = strtok(nullptr, "\n"))
        if (!strncmp(line, "GSP:", 4) || !strncmp(line, "FRTS:", 5) || !strncmp(line, "FWSEC-SB:", 9))
            printf("%s\n", line);
}

static int open_conn(io_service_t s, io_connect_t *c)
{
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0, c);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen: 0x%x%s\n", kr, kr == kIOReturnNotPrivileged ? " (run with sudo)" : "");
        return 1;
    }
    return 0;
}

// NV_PMC_BOOT_0 chipset the kext read (NVChipset), 0 if unknown.
static uint32_t driver_chipset(io_service_t s)
{
    uint32_t v = 0;
    CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("NVChipset"), kCFAllocatorDefault, 0);
    if (p && CFGetTypeID(p) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef)p, kCFNumberSInt32Type, &v);
    if (p)
        CFRelease(p);
    return v;
}

static int cmd_boot(const std::string &dir)
{
    io_service_t s = find_driver();
    if (!s)
        return 1;
    // GSP-RM and its bootloader are shared per architecture (Turing: linux-firmware nvidia/tu102);
    // the booters are signed per chip group (TU102/TU104/TU106: tu102, TU116/TU117: tu116). nv_hal.cpp.
    uint32_t chipset = driver_chipset(s);
    const nv_chip *chip = nv_chip_find(chipset);
    if (!chip) {
        fprintf(stderr, "chipset 0x%x is not a chip GSP-RM r570 supports here\n", chipset);
        IOObjectRelease(s);
        return 1;
    }
    std::string booter = std::string("/") + chip->booter_dir + "/gsp/", gsp = std::string("/") + chip->arch->gsp_dir + "/gsp/";
    std::string elf = gsp + "gsp-570.144.bin", bl = gsp + "bootloader-570.144.bin";
    std::string bload = booter + "booter_load-570.144.bin", bunload = booter + "booter_unload-570.144.bin";
    const struct { uint64_t kind; const char *path; } files[] = {
        { kFwGspElf,        elf.c_str() },
        { kFwGspBootloader, bl.c_str() },
        { kFwBooterLoad,    bload.c_str() },
        { kFwBooterUnload,  bunload.c_str() },
    };
    io_connect_t c;
    if (open_conn(s, &c)) {
        IOObjectRelease(s);
        return 1;
    }

    int rc = 0;
    for (auto &f : files) {
        std::vector<uint8_t> data;
        if (!read_file(dir + f.path, data)) { rc = 1; break; }
        kern_return_t kr = IOConnectCallMethod(c, kSetFirmware, &f.kind, 1, data.data(), data.size(),
                                               nullptr, nullptr, nullptr, nullptr);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "sending %s: 0x%x\n", f.path, kr);
            rc = 1;
            break;
        }
        printf("sent %s (%zu bytes)\n", f.path + 1, data.size());
    }
    if (!rc) {
        printf("booting GSP-RM (up to ~45 s)...\n");
        uint64_t result = 0;
        uint32_t n = 1;
        kern_return_t kr = IOConnectCallMethod(c, kBootGsp, nullptr, 0, nullptr, 0, &result, &n, nullptr, nullptr);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "boot call: 0x%x\n", kr);
            rc = 1;
        } else {
            printf("result: 0x%llx (%s)\n", (unsigned long long)result, result == 0 ? "GSP-RM running" : "failed");
            rc = result == 0 ? 0 : 1;
        }
        print_gsp_log(s);
    }
    IOServiceClose(c);
    IOObjectRelease(s);
    return rc;
}

static int cmd_unload()
{
    io_service_t s = find_driver();
    io_connect_t c;
    if (!s || open_conn(s, &c))
        return 1;
    printf("unloading GSP-RM (UNLOADING_GUEST_DRIVER, FWSEC-SB, booter_unload)...\n");
    uint64_t result = 0;
    uint32_t n = 1;
    kern_return_t kr = IOConnectCallMethod(c, kUnloadGsp, nullptr, 0, nullptr, 0, &result, &n, nullptr, nullptr);
    int rc = 1;
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "unload call: 0x%x\n", kr);
    else {
        printf("result: 0x%llx (%s)\n", (unsigned long long)result, result == 0 ? "unloaded, WPR2 cleared" : "failed");
        rc = result == 0 ? 0 : 1;
    }
    print_gsp_log(s);
    IOServiceClose(c);
    IOObjectRelease(s);
    return rc;
}

// GPU power through the control client (IOServiceOpen type 2, src/nv_uapi.h): no sudo, and asking
// doesn't wake the GPU.
static int cmd_power(const char *arg)
{
    static const char *const states[] = { "on", "off", "switching", "failed" };
    static const char *const modes[] = { "off", "on", "auto" };
    int set = !strcmp(arg, "off") ? 0 : !strcmp(arg, "on") ? 1 : !strcmp(arg, "auto") ? 2 : -1;
    if (set < 0 && strcmp(arg, "status")) {
        fprintf(stderr, "power: on | off | auto | status\n");
        return 1;
    }
    io_service_t s = find_driver();
    io_connect_t c;
    if (!s)
        return 1;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 2, &c);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen (control): 0x%x (kext without runtime power, or not the console user?)\n", kr);
        IOObjectRelease(s);
        return 1;
    }
    if (set == 1)
        printf("powering on (FRTS + GSP-RM boot)...\n");
    uint64_t in = (uint64_t)set, out[7] = {};
    uint32_t n = 7;
    kr = IOConnectCallScalarMethod(c, set < 0 ? 0 : 1, &in, set < 0 ? 0 : 1, out, &n);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "power call: 0x%x%s\n", kr, kr == kIOReturnBusy ? " (a program has the GPU open)" :
                kr == kIOReturnUnsupported ? " (no ACPI method to cut this GPU's power: it stays on)" : "");
    if (n == 7)
        printf("GPU %s, mode %s (idle power-off after %llu s); %llu power-offs, %llu power-ons, last power-on %llu ms; "
               "%llu connections\n", states[out[0] & 3], modes[out[1] % 3], (unsigned long long)out[2],
               (unsigned long long)out[3], (unsigned long long)out[4], (unsigned long long)out[5],
               (unsigned long long)out[6]);
    IOServiceClose(c);
    IOObjectRelease(s);
    return kr == KERN_SUCCESS ? 0 : 1;
}

// P-state and boost through the control client (no sudo):
//   nvgsp perf                          P-state, boost held, policy
//   nvgsp perf max|1level|clear [s|inf] boost by hand (set the policy off first: the automatic
//                                       boost shares the RM client and would clear it)
//   nvgsp perf policy off|fixed|adaptive [seconds=2] [burst=1level|max|none] [busy=50] [idle=150]
//   nvgsp perf watch [ms] [count]       P-state, level and busy EWMA every ms (default 10 ms, 300)
static const char *const kBoostNames[] = { "none", "1level", "max" };
static const char *const kPolicyNames[] = { "off", "fixed", "adaptive" };

static int perf_open(io_connect_t *c)
{
    io_service_t s = find_driver();
    if (!s)
        return 1;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 2, c);
    IOObjectRelease(s);
    if (kr != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen (control): 0x%x\n", kr);
        return 1;
    }
    return 0;
}

static kern_return_t perf_policy(io_connect_t c, const uint64_t *in, uint32_t nin, uint64_t out[10])
{
    uint32_t n = 10;
    return IOConnectCallScalarMethod(c, 4, in, nin, out, &n);
}

static void print_policy(const uint64_t *o)
{
    printf("policy %s (fixed: %llu s; adaptive: burst %s, max at %llu%% busy, clear after %llu ms idle); "
           "holding %s, busy %.1f%%; sent %llu max, %llu 1level, %llu clear\n",
           kPolicyNames[o[0] % 3], (unsigned long long)o[1], kBoostNames[o[2] % 3], (unsigned long long)o[3],
           (unsigned long long)o[4], kBoostNames[o[5] % 3], o[6] / 10.0, (unsigned long long)o[7],
           (unsigned long long)o[8], (unsigned long long)o[9]);
}

static int cmd_perf(int argc, char **argv)
{
    io_connect_t c;
    uint64_t out[10] = {}, pin[2] = {};
    kern_return_t kr;
    if (argc >= 1 && !strcmp(argv[0], "policy")) {
        if (perf_open(&c))
            return 1;
        if ((kr = perf_policy(c, NULL, 0, out)) != KERN_SUCCESS) {
            fprintf(stderr, "policy call: 0x%x\n", kr);
            IOServiceClose(c);
            return 1;
        }
        if (argc >= 2) {
            uint64_t in[5] = { out[0], out[1], out[2], out[3], out[4] };
            for (int i = 1; i < argc; i++) {
                const char *a = argv[i];
                if (!strcmp(a, "off") || !strcmp(a, "fixed") || !strcmp(a, "adaptive"))
                    in[0] = !strcmp(a, "off") ? 0 : !strcmp(a, "fixed") ? 1 : 2;
                else if (!strncmp(a, "seconds=", 8))
                    in[1] = strtoull(a + 8, NULL, 0);
                else if (!strncmp(a, "burst=", 6))
                    in[2] = !strcmp(a + 6, "max") ? 2 : !strcmp(a + 6, "1level") ? 1 : 0;
                else if (!strncmp(a, "busy=", 5))
                    in[3] = strtoull(a + 5, NULL, 0);
                else if (!strncmp(a, "idle=", 5))
                    in[4] = strtoull(a + 5, NULL, 0);
                else {
                    fprintf(stderr, "perf policy: off|fixed|adaptive [seconds=N] [burst=1level|max|none] [busy=PCT] [idle=MS]\n");
                    IOServiceClose(c);
                    return 1;
                }
            }
            if ((kr = perf_policy(c, in, 5, out)) != KERN_SUCCESS) {
                fprintf(stderr, "policy call: 0x%x%s\n", kr, kr == kIOReturnBadArgument ?
                        " (seconds 1-3600, busy 1-100, idle 20-10000)" : "");
                IOServiceClose(c);
                return 1;
            }
        }
        print_policy(out);
        IOServiceClose(c);
        return 0;
    }
    if (argc >= 1 && !strcmp(argv[0], "watch")) {
        unsigned ms = argc >= 2 ? (unsigned)strtoul(argv[1], NULL, 0) : 10;
        unsigned count = argc >= 3 ? (unsigned)strtoul(argv[2], NULL, 0) : 300;
        if (perf_open(&c))
            return 1;
        struct timespec t0;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int last = -2;
        for (unsigned i = 0; i < count; i++) {
            uint64_t o[4] = {};
            uint32_t n = 4;
            kr = IOConnectCallScalarMethod(c, 3, NULL, 0, o, &n);
            perf_policy(c, NULL, 0, out);
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            double at = (t.tv_sec - t0.tv_sec) * 1e3 + (t.tv_nsec - t0.tv_nsec) / 1e6;
            int p = kr != KERN_SUCCESS ? -1 : o[0] == 0xffffffffull ? 99 : (int)o[0];
            int key = p * 16 + (int)(out[5] % 3);
            if (key != last || i == count - 1) {     // print changes only
                printf("%9.1f ms  P%-2d  holding %-6s  busy %5.1f%%\n", at, p, kBoostNames[out[5] % 3], out[6] / 10.0);
                last = key;
            }
            usleep(ms * 1000);
        }
        IOServiceClose(c);
        return 0;
    }
    uint32_t nin = 0;
    if (argc >= 1) {
        pin[0] = !strcmp(argv[0], "clear") ? 0 : !strcmp(argv[0], "1level") ? 1 : !strcmp(argv[0], "max") ? 2 : 99;
        pin[1] = argc >= 2 ? (!strcmp(argv[1], "inf") ? 0xffffffffull : strtoull(argv[1], NULL, 0)) : 2;
        if (pin[0] == 99) {
            fprintf(stderr, "perf: [max|1level|clear [seconds|inf]] | policy ... | watch [ms] [count]\n");
            return 1;
        }
        nin = 2;
    }
    if (perf_open(&c))
        return 1;
    uint64_t o[4] = {};
    uint32_t n = 4;
    kr = IOConnectCallScalarMethod(c, 3, pin, nin, o, &n);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "perf call: 0x%x%s\n", kr, kr == kIOReturnNoDevice ? " (GPU off or GSP-RM not running)" : "");
    else {
        if (o[0] == 0xffffffffull)
            printf("P-state unknown");
        else
            printf("P%llu", (unsigned long long)o[0]);
        if (nin)
            printf(", boost status 0x%llx", (unsigned long long)o[1]);
        printf("; %llu boost requests accepted\n", (unsigned long long)o[3]);
        if (perf_policy(c, NULL, 0, out) == KERN_SUCCESS)
            print_policy(out);
    }
    IOServiceClose(c);
    return kr == KERN_SUCCESS ? 0 : 1;
}

static int cmd_intr(const char *arg)
{
    uint64_t mode = !strcmp(arg, "off") ? 0 : !strcmp(arg, "on") ? 1 : 2;
    io_service_t s = find_driver();
    io_connect_t c;
    if (!s || open_conn(s, &c))
        return 1;
    uint64_t out[4] = {};
    uint32_t n = 4;
    kern_return_t kr = IOConnectCallMethod(c, kIntr, &mode, 1, nullptr, 0, out, &n, nullptr, nullptr);
    if (kr != KERN_SUCCESS)
        fprintf(stderr, "intr call: 0x%x%s\n", kr, kr == kIOReturnUnsupported ? " (no MSI or no vectors: see the log)" : "");
    else
        printf("non-stall interrupts %s: %llu interrupts, %llu spurious, %llu storms\n",
               out[0] ? "on" : mode == 1 ? "wanted (on after the next GSP-RM boot)" : "off",
               (unsigned long long)out[1], (unsigned long long)out[2], (unsigned long long)out[3]);
    IOServiceClose(c);
    IOObjectRelease(s);
    return kr == KERN_SUCCESS ? 0 : 1;
}

static int cmd_logs(const std::string &dir)
{
    io_service_t s = find_driver();
    io_connect_t c;
    if (!s || open_conn(s, &c))
        return 1;
    int rc = 0;
    std::vector<uint8_t> buf(0x10000);
    for (uint64_t i = 0; i < 4; i++) {
        size_t n = buf.size();
        kern_return_t kr = IOConnectCallMethod(c, kReadLog, &i, 1, nullptr, 0, nullptr, nullptr, buf.data(), &n);
        if (kr != KERN_SUCCESS) {
            fprintf(stderr, "%s: 0x%x%s\n", kLogNames[i], kr, kr == kIOReturnNotReady ? " (no GSP boot attempt yet)" : "");
            rc = 1;
            continue;
        }
        std::string path = dir + "/" + kLogNames[i] + ".bin";
        FILE *f = fopen(path.c_str(), "wb");
        if (!f || fwrite(buf.data(), 1, n, f) != n) { perror(path.c_str()); rc = 1; }
        if (f) fclose(f);
        uint64_t put;
        memcpy(&put, buf.data(), 8);
        printf("%s: %zu bytes -> %s (put pointer %llu)\n", kLogNames[i], n, path.c_str(), (unsigned long long)put);
    }
    IOServiceClose(c);
    IOObjectRelease(s);
    return rc;
}

// ---- LibOS log decoding (liblogdecode.c, r570) ------------------------------------
//
// Buffer: word 0 = number of words ever written ("put"), then a ring of
// len/8 - 1 words. A record is  args..., metadata address, timestamp  and is read
// backwards from put. Metadata (filename, format, line, argument count) lives in
// the firmware's .logging data, which linux-firmware strips; NVIDIA ships it
// separately. Without it, records are split by their timestamps.

struct Elf {
    const std::vector<uint8_t> *f = nullptr;
    uint64_t base = 0, size = 0;    // an ELF inside f (container sections hold whole ELFs)

    template <typename T> T rd(uint64_t off) const
    {
        T v = 0;
        if (off + sizeof(T) <= size) memcpy(&v, f->data() + base + off, sizeof(T));
        return v;
    }
    bool valid() const { return size >= 0x40 && !memcmp(f->data() + base, "\x7f" "ELF", 4) && (*f)[base + 4] == 2; }

    // Section by name -> (offset, size) relative to this ELF.
    bool section(const char *name, uint64_t *off, uint64_t *sz) const
    {
        uint64_t shoff = rd<uint64_t>(0x28);
        uint16_t shent = rd<uint16_t>(0x3a), shnum = rd<uint16_t>(0x3c), shstr = rd<uint16_t>(0x3e);
        uint64_t stro = rd<uint64_t>(shoff + (uint64_t)shstr * shent + 0x18);
        for (uint16_t i = 0; i < shnum; i++) {
            uint64_t sh = shoff + (uint64_t)i * shent;
            uint64_t n = stro + rd<uint32_t>(sh);
            std::string s;
            for (char c; n < size && (c = (char)(*f)[base + n]); n++) s += c;
            if (s == name) {
                *off = rd<uint64_t>(sh + 0x18);
                *sz  = rd<uint64_t>(sh + 0x20);
                return *off + *sz <= size;
            }
        }
        return false;
    }

    // Virtual address -> offset in this ELF (program headers, then sections), or ~0.
    uint64_t map(uint64_t va, uint64_t len) const
    {
        uint64_t phoff = rd<uint64_t>(0x20);
        uint16_t phent = rd<uint16_t>(0x36), phnum = rd<uint16_t>(0x38);
        for (uint16_t i = 0; i < phnum; i++) {
            uint64_t p = phoff + (uint64_t)i * phent;
            uint64_t off = rd<uint64_t>(p + 8), vaddr = rd<uint64_t>(p + 16), fsz = rd<uint64_t>(p + 32);
            if (va >= vaddr && va + len <= vaddr + fsz && off + (va - vaddr) + len <= size)
                return off + (va - vaddr);
        }
        uint64_t shoff = rd<uint64_t>(0x28);
        uint16_t shent = rd<uint16_t>(0x3a), shnum = rd<uint16_t>(0x3c);
        for (uint16_t i = 1; i < shnum; i++) {
            uint64_t sh = shoff + (uint64_t)i * shent;
            uint64_t addr = rd<uint64_t>(sh + 0x10), off = rd<uint64_t>(sh + 0x18), sz = rd<uint64_t>(sh + 0x20);
            if (addr && va >= addr && va + len <= addr + sz && off + (va - addr) + len <= size)
                return off + (va - addr);
        }
        return ~0ull;
    }

    std::string str(uint64_t va) const
    {
        uint64_t o = map(va, 1);
        if (o == ~0ull) return "(bad-pointer)";
        std::string s;
        for (; o < size && (*f)[base + o] && s.size() < 512; o++) s += (char)(*f)[base + o];
        return s;
    }
};

struct LogMeta { uint64_t filename, format; uint32_t line; uint8_t nargs, level; bool ok; };

static LogMeta read_meta(const Elf &e, uint64_t va)
{
    LogMeta m = {};
    uint64_t o = e.map(va, 24);
    if (o == ~0ull) return m;
    m.filename = e.rd<uint64_t>(o);
    if (m.filename == 0x8000000000000000ull) {          // libosLogMetadata_extended
        o = e.map(va + 16, 24);
        if (o == ~0ull) return m;
        m.filename = e.rd<uint64_t>(o);
    }
    m.format = e.rd<uint64_t>(o + 8);
    m.line   = e.rd<uint32_t>(o + 16);
    m.nargs  = e.rd<uint8_t>(o + 20);
    m.level  = e.rd<uint8_t>(o + 21);
    m.ok     = m.nargs <= 20;
    return m;
}

// printf over 64-bit args; %s arguments are addresses of strings in the ELF.
static std::string log_format(const Elf &e, const std::string &fmt, const uint64_t *args, unsigned nargs)
{
    std::string out;
    unsigned ai = 0;
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        size_t j = i + 1;
        std::string spec = "%";
        while (j < fmt.size() && strchr("-+ #0", fmt[j])) spec += fmt[j++];
        while (j < fmt.size() && (isdigit((unsigned char)fmt[j]) || fmt[j] == '.')) spec += fmt[j++];
        while (j < fmt.size() && strchr("hlLqjzt", fmt[j])) j++;             // all args are 64-bit
        if (j >= fmt.size()) break;
        char c = fmt[j];
        i = j;
        if (c == '%') { out += '%'; continue; }
        uint64_t a = ai < nargs ? args[ai++] : 0;
        char buf[600];
        switch (c) {
        case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)a); break;
        case 'u': case 'x': case 'X': case 'o':
            snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), (unsigned long long)a); break;
        case 'c': snprintf(buf, sizeof buf, "%c", (char)a); break;
        case 's': snprintf(buf, sizeof buf, (spec + "s").c_str(), e.str(a).c_str()); break;
        case 'p': case 'a': snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)a); break;
        default:  snprintf(buf, sizeof buf, "%%%c", c); break;
        }
        out += buf;
    }
    return out;
}

static void decode_log(const char *name, const std::vector<uint8_t> &buf, const Elf *elf)
{
    size_t nw = buf.size() / 8;
    if (nw < 2) return;
    std::vector<uint64_t> w(nw);
    memcpy(w.data(), buf.data(), nw * 8);
    uint64_t put = w[0], ring = nw - 1;
    uint64_t oldest = put > ring ? put - ring : 0;
    auto at = [&](uint64_t k) { return w[1 + k % ring]; };
    printf("== %s: %llu words written%s\n", name, (unsigned long long)put,
           put > ring ? " (wrapped; oldest records lost)" : "");

    std::vector<std::string> lines;
    uint64_t i = put;
    if (elf) {
        while (i >= oldest + 2) {
            uint64_t ts = at(--i), mva = at(--i);
            LogMeta m = read_meta(*elf, mva);
            if (!m.ok || i < oldest + m.nargs) { lines.push_back("  (undecodable record; stopping)"); break; }
            uint64_t args[20];
            for (unsigned k = m.nargs; k > 0; k--) args[k - 1] = at(--i);
            std::string file = elf->str(m.filename);
            size_t slash = file.rfind('/');
            if (slash != std::string::npos) file = file.substr(slash + 1);
            std::string text = log_format(*elf, elf->str(m.format), args, m.nargs);
            while (!text.empty() && text.back() == '\n') text.pop_back();
            char head[160];
            snprintf(head, sizeof head, "  [%12.6f] %s:%u: ", ts / 1e9, file.c_str(), m.line);
            lines.push_back(head + text);
        }
    } else {
        // Timestamps are nanoseconds, above 2^32 and below 2^48, and grow; the word
        // before each is the metadata address, the words before that its args.
        std::vector<uint64_t> tsIdx;
        for (uint64_t k = oldest; k < put; k++) {
            uint64_t v = at(k);
            if (v > (1ull << 32) && v < (1ull << 48) && (tsIdx.empty() || v >= at(tsIdx.back())))
                tsIdx.push_back(k);
        }
        uint64_t prev = oldest;
        for (uint64_t k : tsIdx) {
            if (k == prev) { prev = k + 1; continue; }
            char head[200];
            int n = snprintf(head, sizeof head, "  [%12.6f] meta 0x%08llx args", at(k) / 1e9,
                             (unsigned long long)at(k - 1));
            std::string line(head, (size_t)n);
            for (uint64_t a = prev; a + 1 < k; a++) {
                snprintf(head, sizeof head, " 0x%llx", (unsigned long long)at(a));
                line += head;
            }
            lines.push_back(line);
            prev = k + 1;
        }
    }
    if (elf)
        for (auto it = lines.rbegin(); it != lines.rend(); ++it) printf("%s\n", it->c_str());
    else
        for (auto &l : lines) printf("%s\n", l.c_str());
}

static int cmd_decode(const std::string &dir, const char *logElfPath)
{
    static const char *const sections[] = { ".fwlogging_init", ".fwlogging_rm", ".fwlogging_rm", ".fwlogging_mnoc" };
    std::vector<uint8_t> lf;
    Elf whole;
    if (logElfPath) {
        if (!read_file(logElfPath, lf)) return 1;
        whole.f = &lf;
        whole.size = lf.size();
        if (!whole.valid()) { fprintf(stderr, "%s is not an ELF64 file\n", logElfPath); return 1; }
    } else {
        printf("(no logging ELF given: raw records only; see README)\n");
    }
    int rc = 1;
    for (int i = 0; i < 4; i++) {
        std::vector<uint8_t> buf;
        FILE *f = fopen((dir + "/" + kLogNames[i] + ".bin").c_str(), "rb");
        if (!f) continue;
        fclose(f);
        if (!read_file(dir + "/" + kLogNames[i] + ".bin", buf)) continue;
        rc = 0;
        Elf e = whole, *pe = nullptr;
        if (logElfPath) {
            uint64_t off, sz;
            if (whole.section(sections[i], &off, &sz)) {    // container: one logging ELF per task
                e.base = off;
                e.size = sz;
            }
            pe = e.valid() ? &e : nullptr;
        }
        decode_log(kLogNames[i], buf, pe);
    }
    if (rc) fprintf(stderr, "no LOG*.bin files in %s\n", dir.c_str());
    return rc;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "decode") && argc <= 4)
        return cmd_decode(argv[2], argc == 4 ? argv[3] : nullptr);
    if (argc >= 2 && !strcmp(argv[1], "boot") && argc <= 3)
        return cmd_boot(argc == 3 ? argv[2] : "firmware/nvidia");
    if (argc == 2 && !strcmp(argv[1], "unload"))
        return cmd_unload();
    if (argc >= 2 && !strcmp(argv[1], "perf"))
        return cmd_perf(argc - 2, argv + 2);
    if (argc >= 2 && !strcmp(argv[1], "power") && argc <= 3)
        return cmd_power(argc == 3 ? argv[2] : "status");
    if (argc >= 2 && !strcmp(argv[1], "intr") && argc <= 3)
        return cmd_intr(argc == 3 ? argv[2] : "status");
    if (argc >= 2 && !strcmp(argv[1], "logs") && argc <= 3)
        return cmd_logs(argc == 3 ? argv[2] : ".");
    if (argc == 2 && !strcmp(argv[1], "status")) {
        io_service_t s = find_driver();
        if (!s)
            return 1;
        print_gsp_log(s);
        IOObjectRelease(s);
        return 0;
    }
    fprintf(stderr, "usage: sudo %s boot [fwdir] | sudo %s unload | %s power [on|off|auto|status] | %s perf [max|1level|clear [s|inf] | policy ... | watch [ms] [n]] | sudo %s intr [on|off|status] | sudo %s logs [outdir] | %s status | %s decode <logdir> [logging-elf]\n", argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0], argv[0]);
    return 2;
}
