// The display pipe's frame copy, between NVMetalAccel (accel/) and NVBringup: NVDisplayPipe hands each
// transaction's plane 0 to NVBringup, whose copy engine copies it into the lit scanout surface.
//
//   gpu->callPlatformFunction(sym("NVPipeCopy"), false, key, md, &args, nullptr)
//
// key: an OSObject identifying the source (the IOSurface); NVBringup retains it while the source stays mapped.
// md:  the source's memory (the IOSurface's descriptor); NVBringup prepares (wires) it while mapped.
// The source is mapped into the GPU once and reused while the same key and descriptor come back (WindowServer
// cycles through a few surfaces). Anything but kIOReturnSuccess: nothing was copied; the caller copies itself.
#pragma once

#include <stdint.h>

#define NV_PIPE_COPY_FN       "NVPipeCopy"
#define NV_PIPE_COPY_VERSION  1

// The hardware cursor, from NVFramebuffer (display/):
//   gpu->callPlatformFunction(sym(NV_CURSOR_IMAGE_FN), false, argb, (void *)width, (void *)height, nullptr)
//       straight-alpha A8R8G8B8 pixels, packed rows, at most 256x256
//   gpu->callPlatformFunction(sym(NV_CURSOR_MOVE_FN), false, (void *)(intptr_t)x, (void *)(intptr_t)y,
//                             (void *)(uintptr_t)visible, nullptr)
//       the image's top-left in framebuffer pixels; doesn't block (callable from IOFramebuffer's cursor paths)
// NVDisplayFB's CursorSize (256) says the cursor is available.
// Head 0's vblank, for NVFramebuffer's vertical blank timer:
//   gpu->callPlatformFunction(sym(NV_VBLANK_LINES_FN), false, &lines, nullptr, nullptr, nullptr)
//       lines (uint32_t): raster lines until the next vblank starts, 0 when unknown
// Display power (DPMS), from NVFramebuffer when macOS sleeps or wakes the display:
//   gpu->callPlatformFunction(sym(NV_DISPLAY_POWER_FN), false, (void *)(uintptr_t)on, nullptr, nullptr, nullptr)
//       on 0: the monitor's signal off (it may go to standby), 1: back on; the mode and surfaces stay
#define NV_VBLANK_LINES_FN    "NVVblankLines"
#define NV_DISPLAY_POWER_FN   "NVDisplayPower"
#define NV_CURSOR_IMAGE_FN    "NVCursorImage"
#define NV_CURSOR_MOVE_FN     "NVCursorMove"

struct nv_pipe_copy {
    uint32_t version;                   // NV_PIPE_COPY_VERSION
    uint32_t srcPitch;                  // bytes per row of the source; 4 bytes per pixel on both sides
    uint32_t x0, y0, x1, y1;            // the rectangle copied, in pixels, the same in source and destination
    uint32_t dstWidth, dstHeight, dstPitch;   // the framebuffer the caller copies for: refused unless it is the lit one
    uint32_t gpuUs;                     // out: submit to completion, in microseconds
};

// Zero-copy scanout (boot-arg nvzerocopy; NVStackFeatures has NV_STACK_F_PIPE_SCANOUT only then). Instead of
// NVPipeCopy, the display pipe asks NVBringup to scan out the image NVMetal rendered the IOSurface from
// (NVMAC_SURFACE_SCANOUT in nv_uapi.h):
//
//   gpu->callPlatformFunction(sym(NV_PIPE_SCANOUT_FN), false, &args, nullptr, nullptr, nullptr)
//
// kIOReturnSuccess: window 0 now scans the image out (from args.showAt on). Anything else, with args.result saying
// why: nothing was changed, and the caller copies the surface (NVPipeCopy) as before. The surface and its image must
// stay as they are until the transaction completes, which the caller holds back until args.showAt (the flip before
// it is out of scanout from then on); NVBringup keeps the image's VRAM allocated until a later flip has replaced it.
#define NV_PIPE_SCANOUT_FN        "NVPipeScanout"
#define NV_PIPE_SCANOUT_VERSION   1

enum {
    NV_PIPE_SCANOUT_OK           = 0,
    NV_PIPE_SCANOUT_NOT_READY    = 1,   // display not lit, GSP-RM down, flips off, feature off
    NV_PIPE_SCANOUT_NOT_REGISTERED = 2, // NVMetal registered no image for this surface
    NV_PIPE_SCANOUT_STALE        = 3,   // the surface's seed is not the one registered: someone wrote it since
    NV_PIPE_SCANOUT_GEOMETRY     = 4,   // not the lit mode's size, or a layout the window can't scan
    NV_PIPE_SCANOUT_BUSY         = 5,   // too many scanned-out images still held
};

struct nv_pipe_scanout {
    uint32_t version;                   // NV_PIPE_SCANOUT_VERSION
    uint32_t surfaceId;                 // IOSurface::getSurfaceID
    uint32_t seed;                      // IOSurface::getUnlockSeed now
    uint32_t width, height;             // plane 0 of the surface
    uint32_t dstWidth, dstHeight, dstPitch;   // the framebuffer the caller presents for: refused unless it is the lit one
    uint32_t result;                    // out: NV_PIPE_SCANOUT_*
    uint32_t regSeed;                   // out: the seed registered (0: none), for the log
    uint64_t showAt;                    // out: mach_absolute_time at which the flip is shown
};
