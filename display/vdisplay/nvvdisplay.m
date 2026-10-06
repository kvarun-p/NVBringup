// nvvdisplay: puts the HDMI monitor NVBringup lights (boot-arg nvdisp=2) into macOS as a screen.
//
// WindowServer only drives displays of a GPU that has its own Metal display pipeline, so a kernel
// IOFramebuffer on the NVIDIA GPU hangs the login (display/README.md). Instead this creates a virtual
// display (CGVirtualDisplay, the private API DeskPad and BetterDisplay use) at the monitor's mode, takes
// the frames WindowServer composes for it (CGDisplayStream), and copies each frame's dirty rectangles
// with the CPU into the surface the NVIDIA GPU scans out, which NVBringup maps here (NVMAC_DISPLAY_MAP).
//
//   nvvdisplay            run until killed
//   nvvdisplay --no-gpu   the virtual display and capture only, counting frames (no NVBringup needed)
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

static BOOL readMonitor(Monitor *mon)
{
    memset(mon, 0, sizeof(*mon));
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVBringup"));
    if (!s)
        return NO;
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

// ---- Main ---------------------------------------------------------------------------------------

static volatile sig_atomic_t gStop;
static void onSignal(int sig) { gStop = 1; }

int main(int argc, char **argv)
{
    @autoreleasepool {
        const BOOL noGpu = argc > 1 && !strcmp(argv[1], "--no-gpu");
        if (!haveVirtualDisplayAPI()) {
            fprintf(stderr, "nvvdisplay: this macOS has no CGVirtualDisplay API\n");
            return 1;
        }

        // The scanout surface.
        Monitor mon;
        nvmac_dev *dev = NULL;
        uint8_t *fb = NULL;
        uint64_t fbSize = 0;
        uint32_t fbW = 1920, fbH = 1080, fbPitch = 0;
        if (!noGpu) {
            if (!readMonitor(&mon)) {      // not an error: the LaunchAgent runs it at every login
                printf("nvvdisplay: no lit display (NVBringup with boot-arg nvdisp=2 and the monitor connected at "
                       "boot); nothing to do\n");
                return 0;
            }
            int r = nvmac_open(&dev);
            if (r) {
                fprintf(stderr, "nvvdisplay: opening NVBringup: %s (0x%x)\n", nvmac_strerror(r), r);
                return 2;
            }
            void *p = NULL;
            r = nvmac_display_map(dev, &p, &fbSize, &fbW, &fbH, &fbPitch);
            if (r) {
                fprintf(stderr, "nvvdisplay: mapping the scanout surface: %s (0x%x)\n", nvmac_strerror(r), r);
                nvmac_close(dev);
                return 2;
            }
            fb = p;
        } else {
            memset(&mon, 0, sizeof(mon));
            mon.width = fbW;
            mon.height = fbH;
        }
        double hz = (mon.pclkKhz && mon.htotal && mon.vtotal)
                        ? (double)mon.pclkKhz * 1000.0 / ((double)mon.htotal * mon.vtotal) : 60.0;

        // Capture needs Screen Recording; the first run asks (the prompt names this binary).
        if (!CGPreflightScreenCaptureAccess()) {
            CGRequestScreenCaptureAccess();
            fprintf(stderr, "nvvdisplay: allow Screen Recording for nvvdisplay (System Settings → Privacy & "
                            "Security), then run it again\n");
            if (dev)
                nvmac_close(dev);
            return 4;
        }

        // The virtual display: the monitor's identity, size and its one mode.
        CGVirtualDisplayDescriptor *desc = [[NSClassFromString(@"CGVirtualDisplayDescriptor") alloc] init];
        [desc setDispatchQueue:dispatch_get_main_queue()];
        NSString *name = edidName(&mon);
        desc.name = name.length ? name : @"NVIDIA HDMI";
        desc.maxPixelsWide = fbW;
        desc.maxPixelsHigh = fbH;
        desc.sizeInMillimeters = mon.haveEdid && mon.edid[21] && mon.edid[22]
                                     ? CGSizeMake(mon.edid[21] * 10.0, mon.edid[22] * 10.0)
                                     : CGSizeMake(fbW * 0.2652, fbH * 0.2652);   // ~96 dpi
        desc.vendorID = mon.haveEdid ? (uint32_t)(mon.edid[8] << 8 | mon.edid[9]) : 0x10de;
        desc.productID = mon.haveEdid ? (uint32_t)(mon.edid[10] | mon.edid[11] << 8) : 0x1f91;
        desc.serialNum = mon.haveEdid ? (uint32_t)(mon.edid[12] | mon.edid[13] << 8 | mon.edid[14] << 16 |
                                                   (uint32_t)mon.edid[15] << 24)
                                      : 1;
        CGVirtualDisplay *vd = [[NSClassFromString(@"CGVirtualDisplay") alloc] initWithDescriptor:desc];
        if (!vd) {
            fprintf(stderr, "nvvdisplay: creating the virtual display failed\n");
            return 3;
        }
        CGVirtualDisplaySettings *settings = [[NSClassFromString(@"CGVirtualDisplaySettings") alloc] init];
        settings.hiDPI = 0;
        settings.modes = @[ [[NSClassFromString(@"CGVirtualDisplayMode") alloc] initWithWidth:fbW height:fbH
                                                                                  refreshRate:hz] ];
        if (![vd applySettings:settings]) {
            fprintf(stderr, "nvvdisplay: applying the virtual display's mode failed\n");
            return 3;
        }
        const CGDirectDisplayID did = vd.displayID;
        printf("nvvdisplay: virtual display %u \"%s\" %ux%u @ %.2f Hz%s\n", did, desc.name.UTF8String, fbW, fbH, hz,
               noGpu ? " (no GPU)" : "");
        fflush(stdout);

        // Its frames: dirty rectangles copied into the scanout surface.
        __block uint64_t frames = 0, rects = 0, bytes = 0;
        dispatch_queue_t q = dispatch_queue_create("nvvdisplay.copy", DISPATCH_QUEUE_SERIAL);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"     // CGDisplayStream: removed in macOS 15
        NSDictionary *props = @{ (__bridge NSString *)kCGDisplayStreamShowCursor : @YES,
                                 (__bridge NSString *)kCGDisplayStreamMinimumFrameTime : @(1.0 / hz) };
        CGDisplayStreamRef stream = CGDisplayStreamCreateWithDispatchQueue(
            did, fbW, fbH, 'BGRA', (__bridge CFDictionaryRef)props, q,
            ^(CGDisplayStreamFrameStatus status, uint64_t time, IOSurfaceRef surf, CGDisplayStreamUpdateRef upd) {
                if (status != kCGDisplayStreamFrameStatusFrameComplete || !surf)
                    return;
                frames++;
                if (!fb)
                    return;
                size_t n = 0;
                const CGRect *r = upd ? CGDisplayStreamUpdateGetRects(upd, kCGDisplayStreamUpdateDirtyRects, &n) : NULL;
                const CGRect all = CGRectMake(0, 0, fbW, fbH);
                if (!r || !n) {
                    r = &all;
                    n = 1;
                }
                IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
                const uint8_t *src = IOSurfaceGetBaseAddress(surf);
                const size_t srcPitch = IOSurfaceGetBytesPerRow(surf);
                const size_t sw = IOSurfaceGetWidth(surf), sh = IOSurfaceGetHeight(surf);
                for (size_t i = 0; i < n; i++) {
                    CGRect c = CGRectIntegral(CGRectIntersection(r[i], CGRectMake(0, 0, MIN(sw, fbW), MIN(sh, fbH))));
                    if (CGRectIsEmpty(c))
                        continue;
                    const size_t x = (size_t)c.origin.x, y = (size_t)c.origin.y;
                    const size_t w = (size_t)c.size.width, h = (size_t)c.size.height;
                    for (size_t row = 0; row < h; row++)
                        memcpy(fb + (y + row) * fbPitch + x * 4, src + (y + row) * srcPitch + x * 4, w * 4);
                    rects++;
                    bytes += (uint64_t)w * h * 4;
                }
                IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
            });
        if (!stream) {
            fprintf(stderr, "nvvdisplay: no display stream: grant Screen Recording to the app that started this "
                            "(System Settings → Privacy & Security), then run it again\n");
            return 4;
        }
        if (CGDisplayStreamStart(stream) != kCGErrorSuccess) {
            fprintf(stderr, "nvvdisplay: starting the display stream failed\n");
            return 4;
        }
#pragma clang diagnostic pop

        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);
        uint64_t lastFrames = 0;
        for (int tick = 0; !gStop; tick++) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
            if (tick % 10 == 9) {
                __block uint64_t f, rc, b;
                dispatch_sync(q, ^{ f = frames; rc = rects; b = bytes; });
                printf("nvvdisplay: %llu frames (%.1f/s), %llu rects, %.1f MB copied\n", f, (f - lastFrames) / 10.0,
                       rc, b / 1e6);
                fflush(stdout);
                lastFrames = f;
            }
        }
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGDisplayStreamStop(stream);
#pragma clang diagnostic pop
        dispatch_sync(q, ^{});
        CFRelease(stream);
        vd = nil;
        if (dev)
            nvmac_close(dev);
        printf("nvvdisplay: stopped\n");
    }
    return 0;
}
