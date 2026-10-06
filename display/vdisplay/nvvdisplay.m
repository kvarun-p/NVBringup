// nvvdisplay: puts the HDMI monitor NVBringup lights (boot-arg nvdisp=2) into macOS as a screen.
//
// WindowServer only drives displays of a GPU that has its own Metal display pipeline, so a kernel
// IOFramebuffer on the NVIDIA GPU hangs the login (display/README.md). Instead this creates a virtual
// display (CGVirtualDisplay, the private API DeskPad and BetterDisplay use) at the monitor's mode, takes
// the frames WindowServer composes for it (ScreenCaptureKit, macOS 12.3 and later; CGDisplayStream as a
// fallback on macOS 14), and copies each frame's dirty rectangles
// into the surface the NVIDIA GPU scans out. The GPU's copy engine does the copies: each frame's IOSurface is
// imported once (NVMAC_MEM_IMPORT) and the scanout surface bound (NVMAC_DISPLAY_MEM), so only the dirty
// rectangles cross PCIe, as DMA. The display has two buffers: the copies go to the one not scanned out, which
// then becomes visible at the next vblank (NVMAC_DISPLAY_FLIP), so no frame tears. That buffer missed the previous
// frame, so each frame copies the previous frame's dirty rectangles too. The CPU copies through a write-combined
// mapping (NVMAC_DISPLAY_MAP) instead if any of that fails, into the scanned-out buffer.
// Hot-plug: NVBringup lights a monitor plugged in later and bumps NVDisplayGen on every connect and
// disconnect; this follows it once a second, removing the virtual display while nothing is lit (so macOS
// moves the windows back) and making it again, at the new monitor's mode, when one is.
//
//   nvvdisplay                     run until killed (the LaunchAgent from install.sh)
//   nvvdisplay --cgdisplaystream   capture with CGDisplayStream instead (macOS 14 only; for comparison)
//
// Needs the Screen Recording permission (System Settings → Privacy & Security) for whatever starts it.

#import <Foundation/Foundation.h>
#import <CoreGraphics/CoreGraphics.h>
#import <IOKit/IOKitLib.h>
#import <IOSurface/IOSurface.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#include <signal.h>
#include <os/lock.h>
#include <pthread.h>
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

static BOOL gUseCG;                 // --cgdisplaystream

// Runs the main run loop (and so the main queue) until *done is set there, or timeout seconds pass.
static BOOL spinUntil(const BOOL *done, double timeout)
{
    const CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + timeout;
    while (!*done && CFAbsoluteTimeGetCurrent() < end)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    return *done;
}

@interface NVSession : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic) uint32_t gen;
@property (nonatomic, readonly) BOOL failed;    // the capture stopped on its own (main queue)
- (BOOL)startWithMonitor:(const Monitor *)mon;
- (void)stop;
- (void)report;
@end

enum { kSurfCache = 8 };
static const uint64_t kPushSize = 64 << 10;
static const uint64_t kSurfVaStride = 64ull << 20;       // per cached surface: up to 64 MiB (4K at 4 bytes/pixel)
enum { kMaxRects = 512 };
enum { kMaxBuf = 3, kHist = 8 };
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
    CGDisplayStreamRef _stream;     // capture: one of these
    SCStream *_scStream;
    dispatch_queue_t _q;
    // ScreenCaptureKit frames arrive on _cq and wait here for _q; a newer frame replaces a waiting one, its
    // dirty rectangles merged, so the copies always take the newest frame instead of working down a backlog.
    dispatch_queue_t _cq;
    os_unfair_lock _pendLock;
    CMSampleBufferRef _pendSb;
    CGRect _pend[512];
    size_t _nPend;
    BOOL _pendAll, _draining;
    uint64_t _skipped;
    BOOL _active;                   // on _q: copies allowed (cleared before the surface is unmapped)
    uint64_t _frames, _rects, _bytes, _lastFrames, _gpuFrames;
    uint64_t _lastGpuFrames;
    uint64_t _copyNs;               // since the last report
    // Copy engine path (on _q once started).
    BOOL _gpu;
    uint32_t _ctx, _seqSync, _fbMem[kMaxBuf], _pushMem;
    uint64_t _fbVa[kMaxBuf], _pushVa, _surfVaBase;
    // The display's buffers (1: no flips, copies go to the scanned-out one; 2 or 3: flipped). Each holds some
    // frame (_bufSeq, 0: not a captured one); bringing it up to date copies the damage of every frame since,
    // from the history of the last kHist frames.
    int _nbuf;
    uint64_t _seq, _bufSeq[kMaxBuf];
    struct { CGRect r[512]; size_t n; BOOL all; } _hist[kHist];
    // Flips run on _fq so copies go on while one waits for its vblank. Under _fm: the buffer shown, the one
    // being flipped to, the one copied and waiting for a flip (a newer frame takes it over); -1: none.
    dispatch_queue_t _fq;
    pthread_mutex_t _fm;
    pthread_cond_t _fc;
    BOOL _fmInit;
    int _shown, _inflight, _ready;
    BOOL _flipping, _flipsBroken;
    int _flipStuck;                 // flips in a row that timed out
    uint64_t _flips, _flipTimeouts, _flipNs;   // since the last report
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
    const char *how = NULL;
    if (!gUseCG && [self startScreenCaptureKit:did hz:hz])
        how = "ScreenCaptureKit";
    else if ([self startDisplayStream:did hz:hz])
        how = "CGDisplayStream";
    if (!how) {
        fprintf(stderr, "nvvdisplay: no capture of the virtual display (Screen Recording permission?)\n");
        [self stop];
        return NO;
    }
    printf("nvvdisplay: display generation %u: virtual display %u \"%s\" %ux%u @ %.2f Hz, captured by %s\n", self.gen,
           did, desc.name.UTF8String, _w, _h, hz, how);
    fflush(stdout);
    return YES;
}

// ScreenCaptureKit: the virtual display as an SCDisplay (it shows up shortly after it's made), streamed at
// its own size and rate. Frames arrive on _cq (stream:didOutputSampleBuffer:ofType:) and are copied on _q.
- (BOOL)startScreenCaptureKit:(CGDirectDisplayID)did hz:(double)hz
{
    SCDisplay *display = nil;
    for (int attempt = 0; attempt < 20 && !display; attempt++) {
        __block BOOL done = NO;
        __block SCShareableContent *content = nil;
        __block NSError *error = nil;
        [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
            completionHandler:^(SCShareableContent *c, NSError *e) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    content = c;
                    error = e;
                    done = YES;
                });
            }];
        if (!spinUntil(&done, 5.0) || error) {
            fprintf(stderr, "nvvdisplay: ScreenCaptureKit: listing displays failed (%s)\n",
                    error ? error.localizedDescription.UTF8String : "timed out");
            return NO;
        }
        for (SCDisplay *d in content.displays)
            if (d.displayID == did)
                display = d;
        if (!display) {
            BOOL never = NO;
            spinUntil(&never, 0.25);
        }
    }
    if (!display) {
        fprintf(stderr, "nvvdisplay: ScreenCaptureKit doesn't list virtual display %u\n", did);
        return NO;
    }
    SCContentFilter *filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
    SCStreamConfiguration *cfg = [SCStreamConfiguration new];
    cfg.width = _w;
    cfg.height = _h;
    cfg.pixelFormat = kCVPixelFormatType_32BGRA;
    cfg.minimumFrameInterval = CMTimeMake(1000, (int32_t)lround(hz * 1000.0));
    cfg.showsCursor = YES;
    cfg.scalesToFit = NO;
    cfg.queueDepth = 5;                         // within the surface cache (kSurfCache)
    cfg.captureResolution = SCCaptureResolutionNominal;
    _scStream = [[SCStream alloc] initWithFilter:filter configuration:cfg delegate:self];
    NSError *error = nil;
    _cq = dispatch_queue_create("nvvdisplay.capture", DISPATCH_QUEUE_SERIAL);
    _pendLock = OS_UNFAIR_LOCK_INIT;
    if (![_scStream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:_cq error:&error]) {
        fprintf(stderr, "nvvdisplay: ScreenCaptureKit: %s\n", error.localizedDescription.UTF8String);
        _scStream = nil;
        return NO;
    }
    __block BOOL done = NO;
    __block NSError *startError = nil;
    [_scStream startCaptureWithCompletionHandler:^(NSError *e) {
        dispatch_async(dispatch_get_main_queue(), ^{
            startError = e;
            done = YES;
        });
    }];
    if (!spinUntil(&done, 10.0) || startError) {
        fprintf(stderr, "nvvdisplay: ScreenCaptureKit: starting the stream failed (%s)\n",
                startError ? startError.localizedDescription.UTF8String : "timed out");
        [self stopScreenCaptureKit];
        return NO;
    }
    return YES;
}

- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sb ofType:(SCStreamOutputType)type
{
    if (type != SCStreamOutputTypeScreen || !_active)
        return;
    CFArrayRef atts = CMSampleBufferGetSampleAttachmentsArray(sb, false);
    NSDictionary *info = atts && CFArrayGetCount(atts) ? (__bridge NSDictionary *)CFArrayGetValueAtIndex(atts, 0) : nil;
    NSNumber *status = info[SCStreamFrameInfoStatus];
    if (!status || status.integerValue != SCFrameStatusComplete)    // idle, blank, ...: no new image
        return;
    CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
    IOSurfaceRef surf = img ? CVPixelBufferGetIOSurface(img) : NULL;
    if (!surf)
        return;
    // Documented as NSValues; some releases hand out CGRect dictionaries. Either is taken.
    NSArray *dirty = info[SCStreamFrameInfoDirtyRects];
    CGRect rects[kMaxRects];
    size_t n = 0;
    BOOL all = dirty == nil || dirty.count > kMaxRects;
    for (id v in all ? nil : dirty) {
        CGRect r = CGRectNull;
        if ([v isKindOfClass:NSValue.class])
            [v getValue:&r size:sizeof(r)];
        else if ([v isKindOfClass:NSDictionary.class] && !CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)v, &r))
            r = CGRectNull;
        if (!CGRectIsNull(r))
            rects[n++] = r;
    }
    if (!n)
        all = YES;                          // the whole frame
    os_unfair_lock_lock(&_pendLock);
    if (_pendSb) {                          // not copied yet: this one replaces it, damage and all
        CFRelease(_pendSb);
        _skipped++;
    }
    _pendSb = (CMSampleBufferRef)CFRetain(sb);
    if (all || _nPend + n > kMaxRects)
        _pendAll = YES;
    else {
        memcpy(_pend + _nPend, rects, n * sizeof(CGRect));
        _nPend += n;
    }
    const BOOL kick = !_draining;
    _draining = YES;
    os_unfair_lock_unlock(&_pendLock);
    if (kick)
        dispatch_async(_q, ^{ [self drain]; });
}

// On _q: copies the waiting frame until none is left.
- (void)drain
{
    CGRect rects[kMaxRects];
    for (;;) {
        os_unfair_lock_lock(&_pendLock);
        CMSampleBufferRef sb = _pendSb;
        const size_t n = _pendAll ? 0 : _nPend;
        memcpy(rects, _pend, n * sizeof(CGRect));
        _pendSb = NULL;
        _nPend = 0;
        _pendAll = NO;
        if (!sb)
            _draining = NO;
        os_unfair_lock_unlock(&_pendLock);
        if (!sb)
            return;
        CVImageBufferRef img = CMSampleBufferGetImageBuffer(sb);
        IOSurfaceRef surf = img ? CVPixelBufferGetIOSurface(img) : NULL;
        if (surf && _active)
            [self frame:surf rects:rects count:n];
        CFRelease(sb);
    }
}

- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
    fprintf(stderr, "nvvdisplay: ScreenCaptureKit stopped the stream: %s\n", error.localizedDescription.UTF8String);
    dispatch_async(dispatch_get_main_queue(), ^{ self->_failed = YES; });
}

- (void)stopScreenCaptureKit
{
    if (!_scStream)
        return;
    __block BOOL done = NO;
    [_scStream stopCaptureWithCompletionHandler:^(NSError *e) {
        dispatch_async(dispatch_get_main_queue(), ^{ done = YES; });
    }];
    spinUntil(&done, 5.0);
    [_scStream removeStreamOutput:self type:SCStreamOutputTypeScreen error:nil];
    _scStream = nil;
}

// CGDisplayStream: macOS 14 only (removed in 15), for when ScreenCaptureKit fails or --cgdisplaystream.
- (BOOL)startDisplayStream:(CGDirectDisplayID)did hz:(double)hz
{
    __unsafe_unretained NVSession *weakSelf = self;     // the stream is stopped and drained before self goes
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    NSDictionary *props = @{ (__bridge NSString *)kCGDisplayStreamShowCursor : @YES,
                             (__bridge NSString *)kCGDisplayStreamMinimumFrameTime : @(1.0 / hz) };
    _stream = CGDisplayStreamCreateWithDispatchQueue(
        did, _w, _h, 'BGRA', (__bridge CFDictionaryRef)props, _q,
        ^(CGDisplayStreamFrameStatus status, uint64_t time, IOSurfaceRef surf, CGDisplayStreamUpdateRef upd) {
            NVSession *me = weakSelf;
            if (status != kCGDisplayStreamFrameStatusFrameComplete || !surf || !me->_active)
                return;
            size_t n = 0;
            const CGRect *r = upd ? CGDisplayStreamUpdateGetRects(upd, kCGDisplayStreamUpdateDirtyRects, &n) : NULL;
            [me frame:surf rects:r count:r ? n : 0];
        });
    if (_stream && CGDisplayStreamStart(_stream) == kCGErrorSuccess)
        return YES;
    if (_stream)
        CFRelease(_stream);
#pragma clang diagnostic pop
    _stream = NULL;
    return NO;
}

// One captured frame (on _q): its dirty rectangles (none: all of it) into the scanout surface.
- (void)frame:(IOSurfaceRef)surf rects:(const CGRect *)r count:(size_t)n
{
    _frames++;
    const CGRect all = CGRectMake(0, 0, _w, _h);
    if (!r || !n) {
        r = &all;
        n = 1;
    }
    IOSurfaceLock(surf, kIOSurfaceLockReadOnly, NULL);
    const size_t sw = IOSurfaceGetWidth(surf), sh = IOSurfaceGetHeight(surf);
    const CGRect bounds = CGRectMake(0, 0, MIN(sw, _w), MIN(sh, _h));
    if (!(_gpu && [self gpuCopy:surf rects:r count:n bounds:bounds])) {
        if ([self cpuCopyNeedsFullFrame]) {
            r = &bounds;
            n = 1;
        }
        const uint8_t *src = IOSurfaceGetBaseAddress(surf);
        const size_t srcPitch = IOSurfaceGetBytesPerRow(surf);
        for (size_t i = 0; i < n; i++) {
            CGRect c = CGRectIntegral(CGRectIntersection(r[i], bounds));
            if (CGRectIsEmpty(c))
                continue;
            const size_t x = (size_t)c.origin.x, y = (size_t)c.origin.y;
            const size_t w = (size_t)c.size.width, h = (size_t)c.size.height;
            for (size_t row = 0; row < h; row++)
                memcpy(_fb + (y + row) * _pitch + x * 4, src + (y + row) * srcPitch + x * 4, w * 4);
            _rects++;
            _bytes += (uint64_t)w * h * 4;
        }
    }
    IOSurfaceUnlock(surf, kIOSurfaceLockReadOnly, NULL);
}

- (void)stop
{
    if (_q) {
        dispatch_sync(_q, ^{ self->_active = NO; });     // no copy into the surface from here on
    }
    [self stopScreenCaptureKit];
    if (_cq)
        dispatch_sync(_cq, ^{});        // no capture handler running, so no more drains get queued
    if (_q)
        dispatch_sync(_q, ^{});         // no frame handler running
    if (_pendSb) {
        CFRelease(_pendSb);
        _pendSb = NULL;
    }
    _cq = nil;
    if (_stream) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        CGDisplayStreamStop(_stream);
#pragma clang diagnostic pop
        dispatch_sync(_q, ^{});
        CFRelease(_stream);
        _stream = NULL;
    }
    if (_fq)
        dispatch_sync(_fq, ^{});        // no flip running
    _fq = nil;
    if (_fmInit) {
        pthread_cond_destroy(&_fc);
        pthread_mutex_destroy(&_fm);
        _fmInit = NO;
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
    for (int b = 0; b < kMaxBuf; b++)
        _fbVa[b] = base + (uint64_t)b * 0x10000000ull;      // 256 MiB apart: a buffer is far smaller
    _pushVa = base + 0x40000000ull;
    _surfVaBase = base + 0x80000000ull;
    void *push = NULL;
    if ((r = nvmac_display_mem(_dev, 0, &_fbMem[0], &fbSize, &w, &h, &pitch)) ||
        (r = nvmac_bind(_dev, _fbVa[0], _fbMem[0], 0, fbSize)) ||
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
    // More buffers: 1 with NVMAC_DISPLAY_FLIP, 2 since the third buffer (older kernels reject the index).
    _nbuf = 1;
    for (int b = 1; b < kMaxBuf; b++) {
        uint64_t sz = 0;
        uint32_t wb = 0, hb = 0, pb = 0;
        if (nvmac_display_mem(_dev, (uint32_t)b, &_fbMem[b], &sz, &wb, &hb, &pb) || wb != w || hb != h ||
            pb != pitch || nvmac_bind(_dev, _fbVa[b], _fbMem[b], 0, sz))
            break;
        _nbuf = b + 1;
    }
    // A kernel without flips ignores the buffer index above (and hands out buffer 0 again): a flip to the buffer
    // already shown tells, and changes nothing on screen.
    if (_nbuf > 1) {
        const int fr = nvmac_display_flip(_dev, 0);
        if (fr && fr != kIOReturnTimeout)
            _nbuf = 1;
    }
    _shown = 0;
    _inflight = _ready = -1;
    _seq = 0;
    memset(_bufSeq, 0, sizeof(_bufSeq));
    if (_nbuf > 1) {
        _fq = dispatch_queue_create("nvvdisplay.flip", DISPATCH_QUEUE_SERIAL);
        pthread_mutex_init(&_fm, NULL);
        pthread_cond_init(&_fc, NULL);
        _fmInit = YES;
    }
    if (_nbuf == 1)
        fprintf(stderr, "nvvdisplay: one display buffer only: frames may tear\n");
    else
        printf("nvvdisplay: %d display buffers, flipped at vblank\n", _nbuf);
    return YES;
}

// On _fq: flips to the waiting buffer until none waits. Each flip returns once the display shows it.
- (void)flipLoop
{
    for (;;) {
        pthread_mutex_lock(&_fm);
        if (_ready < 0 || _flipsBroken) {
            _flipping = NO;
            pthread_cond_broadcast(&_fc);
            pthread_mutex_unlock(&_fm);
            return;
        }
        const int b = _ready;
        _ready = -1;
        _inflight = b;
        pthread_mutex_unlock(&_fm);
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        const int fr = nvmac_display_flip(_dev, (uint32_t)b);
        const uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        pthread_mutex_lock(&_fm);
        _flips++;
        _flipNs += ns;
        _flipTimeouts += fr == kIOReturnTimeout;
        _flipStuck = fr == kIOReturnTimeout ? _flipStuck + 1 : 0;
        if (!fr || fr == kIOReturnTimeout)
            _shown = b;
        _inflight = -1;
        if ((fr && fr != kIOReturnTimeout) || _flipStuck >= 3) {
            fprintf(stderr, "nvvdisplay: %s: one display buffer from now on (frames may tear)\n",
                    fr == kIOReturnTimeout ? "flips never report completion" : "a flip failed");
            _flipsBroken = YES;
        }
        pthread_cond_broadcast(&_fc);
        pthread_mutex_unlock(&_fm);
    }
}

// Back to one buffer, buffer 0 (the one NVMAC_DISPLAY_MAP maps), shown: after flips broke, and before CPU copies.
- (void)singleBuffer
{
    if (_nbuf == 1)
        return;
    pthread_mutex_lock(&_fm);
    _flipsBroken = YES;
    while (_flipping)
        pthread_cond_wait(&_fc, &_fm);
    pthread_mutex_unlock(&_fm);
    if (_shown != 0)
        nvmac_display_flip(_dev, 0);
    _shown = 0;
    _ready = -1;
    _nbuf = 1;
}

// Which buffer the next frame goes into: one neither shown nor being flipped to, or the one waiting for a flip
// (its frame is replaced before anyone saw it). With two buffers, that may mean waiting for a flip to finish.
- (int)acquireBuffer
{
    if (_nbuf == 1)
        return 0;
    pthread_mutex_lock(&_fm);
    int b = -1;
    while (b < 0 && !_flipsBroken) {
        for (int i = 0; i < _nbuf && b < 0; i++)
            if (i != _shown && i != _inflight && i != _ready)
                b = i;
        if (b < 0 && _ready >= 0) {
            b = _ready;
            _ready = -1;
        }
        if (b < 0 && pthread_cond_timedwait_relative_np(&_fc, &_fm, &(struct timespec){ 0, 300000000 }) == ETIMEDOUT)
            _flipsBroken = YES;
    }
    pthread_mutex_unlock(&_fm);
    return b;
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
    // This frame's damage, kept for the buffers that don't have it yet.
    const uint64_t seq = ++_seq;
    typeof(_hist[0]) *h = &_hist[seq % kHist];
    h->all = n > kMaxRects;
    h->n = h->all ? 0 : n;
    memcpy(h->r, rects, h->n * sizeof(CGRect));
    int target = [self acquireBuffer];
    if (target < 0) {
        [self singleBuffer];
        target = 0;
    }
    // What the target lacks: the damage of every frame after the one it holds, or all of it.
    static CGRect list[kHist * kMaxRects];
    size_t nl = 0;
    BOOL whole = !_bufSeq[target] || seq - _bufSeq[target] > kHist;
    for (uint64_t k = _bufSeq[target] + 1; !whole && k <= seq; k++) {
        const typeof(_hist[0]) *hk = &_hist[k % kHist];
        if (hk->all || nl + hk->n > kMaxRects)
            whole = YES;
        else {
            memcpy(list + nl, hk->r, hk->n * sizeof(CGRect));
            nl += hk->n;
        }
    }
    if (whole) {
        list[0] = bounds;
        nl = 1;
    }
    const uint64_t dstVa = _fbVa[target];
    uint32_t *p = _push;
    p = ceMthd(p, 0x000, 1, (uint32_t[]){ CLS_COPY });                        // SET_OBJECT
    uint64_t bytes = 0;
    size_t done = 0;
    for (size_t i = 0; i < nl; i++) {
        CGRect c = CGRectIntegral(CGRectIntersection(list[i], bounds));
        if (CGRectIsEmpty(c))
            continue;
        const uint64_t x = (uint64_t)c.origin.x, y = (uint64_t)c.origin.y;
        const uint32_t w = (uint32_t)c.size.width, hh = (uint32_t)c.size.height;
        const uint64_t in = src + y * srcPitch + x * 4, out = dstVa + y * _pitch + x * 4;
        p = ceMthd(p, 0x400, 4, (uint32_t[]){ (uint32_t)(in >> 32), (uint32_t)in, (uint32_t)(out >> 32), (uint32_t)out });
        p = ceMthd(p, 0x410, 4, (uint32_t[]){ (uint32_t)srcPitch, _pitch, w * 4, hh });  // PITCH_IN/OUT, LINE_LENGTH, COUNT
        // LAUNCH_DMA: non-pipelined, flush, pitch -> pitch, multi-line, virtual addresses
        p = ceMthd(p, 0x300, 1, (uint32_t[]){ 2u | 1u << 2 | 1u << 7 | 1u << 8 | 1u << 9 });
        bytes += (uint64_t)w * hh * 4;
        done++;
    }
    int r = 0;
    if (done) {
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        struct nvmac_push push = { _pushVa, (uint32_t)((uint8_t *)p - (uint8_t *)_push), 0 };
        uint64_t seqno = 0;
        r = nvmac_exec(_dev, _ctx, NULL, 0, &push, 1, NULL, 0, &seqno);
        if (!r) {
            struct nvmac_sync_point pt = { _seqSync, 0, seqno };
            r = nvmac_sync_wait(_dev, &pt, 1, 1, 1000000000ull, NULL);
        }
        _copyNs += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    }
    if (r) {
        _bufSeq[target] = 0;            // partly written
        fprintf(stderr, "nvvdisplay: copy engine failed (%s, 0x%x): the CPU copies from now on\n", nvmac_strerror(r), r);
        _gpu = NO;
        return NO;
    }
    _bufSeq[target] = seq;
    if (_nbuf > 1) {                    // flip to it, on _fq
        pthread_mutex_lock(&_fm);
        _ready = target;
        const BOOL kick = !_flipping && !_flipsBroken;
        if (kick)
            _flipping = YES;
        pthread_mutex_unlock(&_fm);
        if (kick)
            dispatch_async(_fq, ^{ [self flipLoop]; });
    }
    _gpuFrames++;
    _rects += done;
    _bytes += bytes;
    return YES;
}

// Before a CPU copy, which goes to buffer 0 (the one NVMAC_DISPLAY_MAP maps) on screen: one buffer from now on,
// and a whole frame if buffer 0 doesn't hold the previous one.
- (BOOL)cpuCopyNeedsFullFrame
{
    [self singleBuffer];
    const BOOL full = _bufSeq[0] != _seq;
    _bufSeq[0] = ++_seq;
    return full;
}

- (void)report
{
    if (!_q)
        return;
    __block uint64_t f = 0, rc = 0, b = 0, gf = 0, fl = 0, fto = 0, fns = 0, cns = 0, sk = 0;
    __block int nbuf = 1;
    __block BOOL gpu = NO;
    dispatch_sync(_q, ^{
        f = self->_frames;
        rc = self->_rects;
        b = self->_bytes;
        gf = self->_gpuFrames;
        gpu = self->_gpu;
        nbuf = self->_nbuf;
        cns = self->_copyNs;
        self->_copyNs = 0;
        if (self->_fmInit) {
            pthread_mutex_lock(&self->_fm);
            fl = self->_flips;
            fto = self->_flipTimeouts;
            fns = self->_flipNs;
            self->_flips = self->_flipTimeouts = self->_flipNs = 0;
            pthread_mutex_unlock(&self->_fm);
        }
        os_unfair_lock_lock(&self->_pendLock);
        sk = self->_skipped;
        self->_skipped = 0;
        os_unfair_lock_unlock(&self->_pendLock);
    });
    printf("nvvdisplay: %llu frames (%.1f/s), %llu rects, %.1f MB copied; %llu by the copy engine%s\n", f,
           (f - _lastFrames) / 10.0, rc, b / 1e6, gf, gpu ? "" : " (CPU copies now)");
    const uint64_t gpuNow = gf - _lastGpuFrames;
    if (fl || gpuNow)
        printf("nvvdisplay:   copies %.2f ms each; %d buffers, %llu flips, %.2f ms each, %llu timed out; %llu "
               "frames replaced by newer ones\n", gpuNow ? cns / 1e6 / gpuNow : 0.0, nbuf, fl, fl ? fns / 1e6 / fl : 0.0,
               fto, sk);
    fflush(stdout);
    _lastFrames = f;
    _lastGpuFrames = gf;
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
        for (int i = 1; i < argc; i++)
            if (!strcmp(argv[i], "--cgdisplaystream"))
                gUseCG = YES;
        signal(SIGINT, onSignal);
        signal(SIGTERM, onSignal);

        // Follows NVBringup's lit display: a session per display generation, none while nothing is lit.
        NVSession *session = nil;
        uint32_t failedGen = UINT32_MAX;        // a generation whose session couldn't start: not retried
        int restarts = 0;                       // of this generation's session, after its capture stopped
        uint32_t lastGen = UINT32_MAX;
        BOOL saidIdle = NO;
        for (int tick = 0; !gStop; tick++) {
            Monitor mon;
            const BOOL lit = readMonitor(&mon);
            if (session && session.failed && lit && mon.gen == session.gen) {
                [session stop];
                session = nil;
                if (++restarts > 3)
                    failedGen = mon.gen;
                else
                    printf("nvvdisplay: restarting the capture\n");
            }
            if (session && (!lit || mon.gen != session.gen)) {
                printf("nvvdisplay: display generation %u ended (%s)\n", session.gen, lit ? "changed" : "unplugged");
                fflush(stdout);
                [session stop];
                session = nil;
            }
            if (lit && !session && mon.gen != failedGen) {
                if (mon.gen != lastGen)
                    restarts = 0;
                lastGen = mon.gen;
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
