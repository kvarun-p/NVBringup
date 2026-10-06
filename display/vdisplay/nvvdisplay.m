// nvvdisplay: puts the HDMI monitor NVBringup lights (boot-arg nvdisp=2) into macOS as a screen.
//
// WindowServer only drives displays of a GPU that has its own Metal display pipeline, so a kernel
// IOFramebuffer on the NVIDIA GPU hangs the login (display/README.md). Instead this creates a virtual
// display (CGVirtualDisplay, the private API DeskPad and BetterDisplay use) at the monitor's mode, takes
// the frames WindowServer composes for it (CGDisplayStream), and copies each frame's dirty rectangles
// into the surface the NVIDIA GPU scans out. The GPU's copy engine does the copies: each frame's IOSurface is
// imported once (NVMAC_MEM_IMPORT) and the scanout surface bound (NVMAC_DISPLAY_MEM), so only the dirty
// rectangles cross PCIe, as DMA. The CPU copies through a write-combined mapping (NVMAC_DISPLAY_MAP) instead if
// any of that fails.
// Hot-plug: NVBringup lights a monitor plugged in later and bumps NVDisplayGen on every connect and
// disconnect; this follows it once a second, removing the virtual display while nothing is lit (so macOS
// moves the windows back) and making it again, at the new monitor's mode, when one is.
//
//   nvvdisplay            run until killed (the LaunchAgent from install.sh)
//
// Needs the Screen Recording permission (System Settings → Privacy & Security) for whatever starts it.

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#include <signal.h>
#include "libnvmac.h"

// ---- CGVirtualDisplay (private, CoreGraphics) --------------------------------------------------------

@class CGVirtualDisplay;
@interface CGVirtualDisplayDescriptor : NSObject
@property (retain, nonatomic) NSString *name;
@property (nonatomic) unsigned int maxPixelsWide;
@property (nonatomic) unsigned int maxPixelsHigh;
@property (nonatomic) CGSize sizeInMillimeters;
@property (nonatomic) unsigned int productID;
@property (nonatomic) unsigned int vendorID;
@property (nonatomic) unsigned int serialNum;
- (void)setDispatchQueue:(dispatch_queue_t)queue;
@end

@interface CGVirtualDisplayMode : NSObject
- (instancetype)initWithWidth:(unsigned int)width height:(unsigned int)height refreshRate:(double)hz;
@end

@interface CGVirtualDisplaySettings : NSObject
@property (retain, nonatomic) NSArray *modes;
@property (nonatomic) unsigned int hiDPI;
@end

@interface CGVirtualDisplay : NSObject
- (instancetype)initWithDescriptor:(CGVirtualDisplayDescriptor *)descriptor;
- (BOOL)applySettings:(CGVirtualDisplaySettings *)settings;
@property (readonly, nonatomic) unsigned int displayID;
@end

static BOOL haveVirtualDisplayAPI(void)
{
    Class d = NSClassFromString(@"CGVirtualDisplayDescriptor"), m = NSClassFromString(@"CGVirtualDisplayMode"),
          s = NSClassFromString(@"CGVirtualDisplaySettings"), v = NSClassFromString(@"CGVirtualDisplay");
    return d && m && s && v && [d instancesRespondToSelector:@selector(setDispatchQueue:)] &&
           [d instancesRespondToSelector:@selector(setMaxPixelsWide:)] &&
           [m instancesRespondToSelector:@selector(initWithWidth:height:refreshRate:)] &&
           [s instancesRespondToSelector:@selector(setModes:)] &&
           [v instancesRespondToSelector:@selector(initWithDescriptor:)] &&
           [v instancesRespondToSelector:@selector(applySettings:)] && [v instancesRespondToSelector:@selector(displayID)];
}

// ---- The monitor, from NVBringup's NVDisplayFB property ---------------------------------------------

typedef struct {
    uint32_t gen;                   // NVDisplayGen
    uint32_t width, height, pclkKhz, htotal, vtotal;
    uint8_t edid[128];
    BOOL haveEdid;
} Monitor;

static uint64_t dictNum(CFDictionaryRef d, CFStringRef k)
{
    CFNumberRef n = CFDictionaryGetValue(d, k);
    uint64_t v = 0;
    if (n && CFGetTypeID(n) == CFNumberGetTypeID())
        CFNumberGetValue(n, kCFNumberSInt64Type, &v);
    return v;
}

// NO when no display is lit (or no NVBringup); mon->gen is filled in either way.
static BOOL readMonitor(Monitor *mon)
{
    memset(mon, 0, sizeof(*mon));
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"));
    if (!s)
        return NO;
    CFNumberRef gen = IORegistryEntryCreateCFProperty(s, CFSTR("NVDisplayGen"), kCFAllocatorDefault, 0);
    if (gen) {
        if (CFGetTypeID(gen) == CFNumberGetTypeID())
            CFNumberGetValue(gen, kCFNumberSInt32Type, &mon->gen);
        CFRelease(gen);
    }
    CFDictionaryRef d = IORegistryEntryCreateCFProperty(s, CFSTR("NVDisplayFB"), kCFAllocatorDefault, 0);
    IOObjectRelease(s);
    if (!d)
        return NO;
    BOOL ok = CFGetTypeID(d) == CFDictionaryGetTypeID();
    if (ok) {
        mon->width = (uint32_t)dictNum(d, CFSTR("Width"));
        mon->height = (uint32_t)dictNum(d, CFSTR("Height"));
        mon->pclkKhz = (uint32_t)dictNum(d, CFSTR("PixelClockKHz"));
        mon->htotal = (uint32_t)dictNum(d, CFSTR("HTotal"));
        mon->vtotal = (uint32_t)dictNum(d, CFSTR("VTotal"));
        CFDataRef e = CFDictionaryGetValue(d, CFSTR("EDID"));
        if (e && CFGetTypeID(e) == CFDataGetTypeID() && CFDataGetLength(e) >= 128) {
            memcpy(mon->edid, CFDataGetBytePtr(e), 128);
            mon->haveEdid = YES;
        }
        ok = mon->width && mon->height;
    }
    CFRelease(d);
    return ok;
}

// The monitor name descriptor (tag 0xfc) of the EDID, if any.
static NSString *edidName(const Monitor *mon)
{
    if (!mon->haveEdid)
        return nil;
    for (int i = 0; i < 4; i++) {
        const uint8_t *b = mon->edid + 54 + i * 18;
        if (b[0] == 0 && b[1] == 0 && b[3] == 0xfc) {
            char name[14] = {0};
            for (int j = 0; j < 13 && b[5 + j] != 0x0a; j++)
                name[j] = (char)b[5 + j];
            return [[NSString stringWithUTF8String:name] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        }
    }
    return nil;
}

// ---- A session: the virtual display for one lit monitor ---------------------------------------------

@interface NVSession : NSObject
@property (nonatomic) uint32_t gen;
- (BOOL)startWithMonitor:(const Monitor *)mon;
- (void)stop;
- (void)report;
@end

enum { kSurfCache = 8 };
static const uint64_t kPushSize = 64 << 10;
static const uint64_t kSurfVaStride = 64ull << 20;       // per cached surface: up to 64 MiB (4K at 4 bytes/pixel)
#define CLS_COPY 0xc5b5                                   // TURING_DMA_COPY_A
#define SUBC     4                                        // the copy object's subchannel

// Volta+ incrementing method header, then the method's data words.
static uint32_t *ceMthd(uint32_t *p, uint32_t method, uint32_t count, const uint32_t *data)
{
    *p++ = (1u << 29) | (count << 16) | (SUBC << 13) | (method >> 2);
    memcpy(p, data, count * 4);
    return p + count;
}

@implementation NVSession {
    nvmac_dev *_dev;
    uint8_t *_fb;
    uint64_t _fbSize;
    uint32_t _w, _h, _pitch;
    CGVirtualDisplay *_vd;
    CGDisplayStreamRef _stream;
    dispatch_queue_t _q;
    BOOL _active;                   // on _q: copies allowed (cleared before the surface is unmapped)
    uint64_t _frames, _rects, _bytes, _lastFrames, _gpuFrames;
    // Copy engine path (all on _q once started).
    BOOL _gpu;
    uint32_t _ctx, _seqSync, _fbMem, _pushMem;
    uint64_t _fbVa, _pushVa, _surfVaBase;
    uint32_t *_push;
    struct { IOSurfaceRef surf; uint32_t mem; uint64_t va, size, lastUse; } _cache[kSurfCache];
    uint64_t _useClock;
}

- (BOOL)startWithMonitor:(const Monitor *)mon
{
    int r = nvmac_open(&_dev);
    if (r) {
        fprintf(stderr, "nvvdisplay: opening NVBringup: %s (0x%x)\n", nvmac_strerror(r), r);
        _dev = NULL;
        return NO;
    }
    void *p = NULL;
    r = nvmac_display_map(_dev, &p, &_fbSize, &_w, &_h, &_pitch);
    if (r) {
        fprintf(stderr, "nvvdisplay: mapping the scanout surface: %s (0x%x)\n", nvmac_strerror(r), r);
        [self stop];
        return NO;
    }
    _fb = p;
    _gpu = [self startGpu];
    const double hz = (mon->pclkKhz && mon->htotal && mon->vtotal)
                          ? (double)mon->pclkKhz * 1000.0 / ((double)mon->htotal * mon->vtotal) : 60.0;

    // The virtual display: the monitor's identity, size and its one mode.
    CGVirtualDisplayDescriptor *desc = [[NSClassFromString(@"CGVirtualDisplayDescriptor") alloc] init];
    [desc setDispatchQueue:dispatch_get_main_queue()];
    NSString *name = edidName(mon);
    desc.name = name.length ? name : @"NVIDIA HDMI";
    desc.maxPixelsWide = _w;
    desc.maxPixelsHigh = _h;
    desc.sizeInMillimeters = mon->haveEdid && mon->edid[21] && mon->edid[22]
                                 ? CGSizeMake(mon->edid[21] * 10.0, mon->edid[22] * 10.0)
                                 : CGSizeMake(_w * 0.2652, _h * 0.2652);   // ~96 dpi
    desc.vendorID = mon->haveEdid ? (uint32_t)(mon->edid[8] << 8 | mon->edid[9]) : 0x10de;
    desc.productID = mon->haveEdid ? (uint32_t)(mon->edid[10] | mon->edid[11] << 8) : 0x1f91;
    desc.serialNum = mon->haveEdid ? (uint32_t)(mon->edid[12] | mon->edid[13] << 8 | mon->edid[14] << 16 |
                                                (uint32_t)mon->edid[15] << 24)
                                   : 1;
    _vd = [[NSClassFromString(@"CGVirtualDisplay") alloc] initWithDescriptor:desc];
    if (!_vd) {
        fprintf(stderr, "nvvdisplay: creating the virtual display failed\n");
        [self stop];
        return NO;
    }
    CGVirtualDisplaySettings *settings = [[NSClassFromString(@"CGVirtualDisplaySettings") alloc] init];
    settings.hiDPI = 0;
    settings.modes = @[ [[NSClassFromString(@"CGVirtualDisplayMode") alloc] initWithWidth:_w height:_h refreshRate:hz] ];
    if (![_vd applySettings:settings]) {
        fprintf(stderr, "nvvdisplay: applying the virtual display's mode failed\n");
        [self stop];
        return NO;
    }
    const CGDirectDisplayID did = _vd.displayID;

    // Its frames: dirty rectangles copied into the scanout surface.
    _q = dispatch_queue_create("nvvdisplay.copy", DISPATCH_QUEUE_SERIAL);
    _active = YES;
    uint8_t *fb = _fb;
    const uint32_t fbW = _w, fbH = _h, fbPitch = _pitch;
    __unsafe_unretained NVSession *weakSelf = self;     // the stream is stopped and drained before self goes
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"     // CGDisplayStream: removed in macOS 15
    NSDictionary *props = @{ (__bridge NSString *)kCGDisplayStreamShowCursor : @YES,
                             (__bridge NSString *)kCGDisplayStreamMinimumFrameTime : @(1.0 / hz) };
    _stream = CGDisplayStreamCreateWithDispatchQueue(
        did, fbW, fbH, 'BGRA', (__bridge CFDictionaryRef)props, _q,
        ^(CGDisplayStreamFrameStatus status, uint64_t time, IOSurfaceRef surf, CGDisplayStreamUpdateRef upd) {
            NVSession *me = weakSelf;
            if (status != kCGDisplayStreamFrameStatusFrameComplete || !surf || !me->_active)
                return;
            me->_frames++;
            size_t n = 0;
            const CGRect *r = upd ? CGDisplayStreamUpdateGetRects(upd, kCGDisplayStreamUpdateDirtyRects, &n) : NULL;
            const CGRect all = CGRectMake(0, 0, fbW, fbH);
            if (!r || !n) {
                r = &all;
                n = 1;
            }
            IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
            const size_t sw = IOSurfaceGetWidth(surf), sh = IOSurfaceGetHeight(surf);
            const CGRect bounds = CGRectMake(0, 0, MIN(sw, fbW), MIN(sh, fbH));
            if (!(me->_gpu && [me gpuCopy:surf rects:r count:n bounds:bounds])) {
                const uint8_t *src = IOSurfaceGetBaseAddress(surf);
                const size_t srcPitch = IOSurfaceGetBytesPerRow(surf);
                for (size_t i = 0; i < n; i++) {
                    CGRect c = CGRectIntegral(CGRectIntersection(r[i], bounds));
                    if (CGRectIsEmpty(c))
                        continue;
                    const size_t x = (size_t)c.origin.x, y = (size_t)c.origin.y;
                    const size_t w = (size_t)c.size.width, h = (size_t)c.size.height;
                    for (size_t row = 0; row < h; row++)
                        memcpy(fb + (y + row) * fbPitch + x * 4, src + (y + row) * srcPitch + x * 4, w * 4);
                    me->_rects++;
                    me->_bytes += (uint64_t)w * h * 4;
                }
            }
            IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
        });
    if (!_stream || CGDisplayStreamStart(_stream) != kCGErrorSuccess) {
#pragma clang diagnostic pop
        fprintf(stderr, "nvvdisplay: no display stream (Screen Recording permission?)\n");
        [self stop];
        return NO;
    }
    printf("nvvdisplay: display generation %u: virtual display %u \"%s\" %ux%u @ %.2f Hz\n", self.gen, did,
           desc.name.UTF8String, _w, _h, hz);
    fflush(stdout);
    return YES;
}

- (void)stop
{
    if (_q) {
        dispatch_sync(_q, ^{ self->_active = NO; });     // no copy into the surface from here on
    }
    if (_stream) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGDisplayStreamStop(_stream);
#pragma clang diagnostic pop
        dispatch_sync(_q, ^{});
        CFRelease(_stream);
        _stream = NULL;
    }
    _vd = nil;                      // the virtual display goes away with its last reference
    for (int i = 0; i < kSurfCache; i++)
        if (_cache[i].surf) {
            CFRelease(_cache[i].surf);  // its import goes with the connection
            _cache[i].surf = NULL;
        }
    _gpu = NO;
    if (_dev) {
        nvmac_close(_dev);          // unmaps the surface
        _dev = NULL;
    }
    _fb = NULL;
    _q = nil;
}

// The copy engine path: the scanout surface bound at _fbVa, a push buffer, a copy context.
- (BOOL)startGpu
{
    const struct nvmac_info *info = nvmac_info(_dev);
    uint64_t fbSize = 0;
    uint32_t w = 0, h = 0, pitch = 0;
    int r;
    const uint64_t base = (info->va_start + 0x1fffff) & ~0x1fffffull;
    _fbVa = base;
    _pushVa = base + 0x40000000ull;             // 1 GiB up: the surface is far smaller
    _surfVaBase = base + 0x80000000ull;
    void *push = NULL;
    if ((r = nvmac_display_mem(_dev, &_fbMem, &fbSize, &w, &h, &pitch)) ||
        (r = nvmac_bind(_dev, _fbVa, _fbMem, 0, fbSize)) ||
        (r = nvmac_mem_alloc(_dev, kPushSize, 0, NVMAC_MEM_GART, 0, &_pushMem, NULL)) ||
        (r = nvmac_mem_map(_dev, _pushMem, &push, NULL)) ||
        (r = nvmac_bind(_dev, _pushVa, _pushMem, 0, kPushSize)) ||
        (r = nvmac_ctx_create(_dev, NVMAC_ENGINE_COPY, &_ctx, &_seqSync))) {
        fprintf(stderr, "nvvdisplay: copy engine unavailable (%s, 0x%x): the CPU copies\n", nvmac_strerror(r), r);
        return NO;
    }
    if (_surfVaBase + kSurfCache * kSurfVaStride > info->va_end || pitch != _pitch || w != _w || h != _h) {
        fprintf(stderr, "nvvdisplay: copy engine setup doesn't fit: the CPU copies\n");
        return NO;
    }
    _push = push;
    return YES;
}

// The cache slot holding surf, imported on first sight (the stream reuses a small pool of surfaces).
// -1 if it can't be imported (not page-aligned).
- (int)slotFor:(IOSurfaceRef)surf
{
    int lru = 0;
    for (int i = 0; i < kSurfCache; i++) {
        if (_cache[i].surf == surf) {
            _cache[i].lastUse = ++_useClock;
            return i;
        }
        if (_cache[i].lastUse < _cache[lru].lastUse)
            lru = i;
    }
    const uintptr_t addr = (uintptr_t)IOSurfaceGetBaseAddress(surf);
    const uint64_t size = IOSurfaceGetAllocSize(surf);
    if ((addr | size) & 0xfff || !size || size > kSurfVaStride)
        return -1;
    if (_cache[lru].surf) {                     // evict: unbinds and unwires it
        nvmac_mem_free(_dev, _cache[lru].mem);
        CFRelease(_cache[lru].surf);
        _cache[lru].surf = NULL;
    }
    uint32_t mem = 0;
    const uint64_t va = _surfVaBase + (uint64_t)lru * kSurfVaStride;
    int r = nvmac_mem_import(_dev, (void *)addr, size, &mem, NULL);
    if (!r && (r = nvmac_bind(_dev, va, mem, 0, size)))
        nvmac_mem_free(_dev, mem);
    if (r) {
        fprintf(stderr, "nvvdisplay: importing a frame surface failed (%s, 0x%x)\n", nvmac_strerror(r), r);
        return -1;
    }
    CFRetain(surf);
    _cache[lru].surf = surf;
    _cache[lru].mem = mem;
    _cache[lru].va = va;
    _cache[lru].size = size;
    _cache[lru].lastUse = ++_useClock;
    return lru;
}

// One 2D pitch copy per dirty rectangle, surf -> scanout surface, waited for. NO: the CPU copies this frame
// (and, after a GPU failure, every frame).
- (BOOL)gpuCopy:(IOSurfaceRef)surf rects:(const CGRect *)rects count:(size_t)n bounds:(CGRect)bounds
{
    const int slot = [self slotFor:surf];
    if (slot < 0)
        return NO;
    const uint64_t src = _cache[slot].va, srcPitch = IOSurfaceGetBytesPerRow(surf);
    const CGRect all = bounds;
    if (n > 512) {                              // a push buffer's worth: copy everything instead
        rects = &all;
        n = 1;
    }
    uint32_t *p = _push;
    p = ceMthd(p, 0x000, 1, (uint32_t[]){ CLS_COPY });                        // SET_OBJECT
    uint64_t bytes = 0;
    size_t done = 0;
    for (size_t i = 0; i < n; i++) {
        CGRect c = CGRectIntegral(CGRectIntersection(rects[i], bounds));
        if (CGRectIsEmpty(c))
            continue;
        const uint64_t x = (uint64_t)c.origin.x, y = (uint64_t)c.origin.y;
        const uint32_t w = (uint32_t)c.size.width, h = (uint32_t)c.size.height;
        const uint64_t in = src + y * srcPitch + x * 4, out = _fbVa + y * _pitch + x * 4;
        p = ceMthd(p, 0x400, 4, (uint32_t[]){ (uint32_t)(in >> 32), (uint32_t)in, (uint32_t)(out >> 32), (uint32_t)out });
        p = ceMthd(p, 0x410, 4, (uint32_t[]){ (uint32_t)srcPitch, _pitch, w * 4, h });   // PITCH_IN/OUT, LINE_LENGTH, COUNT
        // LAUNCH_DMA: non-pipelined, flush, pitch -> pitch, multi-line, virtual addresses
        p = ceMthd(p, 0x300, 1, (uint32_t[]){ 2u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 9 });
        bytes += (uint64_t)w * h * 4;
        done++;
    }
    if (!done)
        return YES;
    struct nvmac_push push = { _pushVa, (uint32_t)((uint8_t *)p - (uint8_t *)_push), 0 };
    uint64_t seqno = 0;
    int r = nvmac_exec(_dev, _ctx, NULL, 0, &push, 1, NULL, 0, &seqno);
    if (!r) {
        struct nvmac_sync_point pt = { _seqSync, 0, seqno };
        r = nvmac_sync_wait(_dev, &pt, 1, 1, 1000000000ull, NULL);
    }
    if (r) {
        fprintf(stderr, "nvvdisplay: copy engine failed (%s, 0x%x): the CPU copies from now on\n", nvmac_strerror(r), r);
        _gpu = NO;
        return NO;
    }
    _gpuFrames++;
    _rects += done;
    _bytes += bytes;
    return YES;
}

- (void)report
{
    if (!_q)
        return;
    __block uint64_t f = 0, rc = 0, b = 0, gf = 0;
    __block BOOL gpu = NO;
    dispatch_sync(_q, ^{
        f = self->_frames;
        rc = self->_rects;
        b = self->_bytes;
        gf = self->_gpuFrames;
        gpu = self->_gpu;
    });
    printf("nvvdisplay: %llu frames (%.1f/s), %llu rects, %.1f MB copied; %llu by the copy engine%s\n", f,
           (f - _lastFrames) / 10.0, rc, b / 1e6, gf, gpu ? "" : " (CPU copies now)");
    fflush(stdout);
    _lastFrames = f;
}
@end

// ---- Main ---------------------------------------------------------------------------------------

static volatile sig_atomic_t gStop;
static void onSignal(int sig) { gStop = 1; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (!haveVirtualDisplayAPI()) {
            fprintf(stderr, "nvvdisplay: this macOS has no CGVirtualDisplay API\n");
            return 1;
        }
        // Capture needs Screen Recording; the first run asks (the prompt names this binary).
        if (!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess();
            fprintf(stderr, "nvvdisplay: allow Screen Recording for nvvdisplay (System Settings → Privacy & "
                            "Security), then run it again\n");
            return 4;
        }
        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);

        // Follows NVBringup's lit display: a session per display generation, none while nothing is lit.
        NVSession *session = nil;
        uint32_t failedGen = UINT32_MAX;        // a generation whose session couldn't start: not retried
        BOOL saidIdle = NO;
        for (int tick = 0; !gStop; tick++) {
            Monitor mon;
            const BOOL lit = readMonitor(&mon);
            if (session && (!lit || mon.gen != session.gen)) {
                printf("nvvdisplay: display generation %u ended (%s)\n", session.gen, lit ? "changed" : "unplugged");
                fflush(stdout);
                [session stop];
                session = nil;
            }
            if (lit && !session && mon.gen != failedGen) {
                session = [NVSession new];
                session.gen = mon.gen;
                if (![session startWithMonitor:&mon]) {
                    session = nil;
                    failedGen = mon.gen;
                }
                saidIdle = NO;
            }
            if (!lit && !session && !saidIdle) {
                printf("nvvdisplay: no lit display (NVBringup with boot-arg nvdisp=2); waiting for one\n");
                fflush(stdout);
                saidIdle = YES;
            }
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
            if (session && tick % 10 == 9)
                [session report];
        }
        [session stop];
        printf("nvvdisplay: stopped\n");
    }
    return 0;
}
