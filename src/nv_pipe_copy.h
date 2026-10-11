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
#define NV_CURSOR_IMAGE_FN    "NVCursorImage"
#define NV_CURSOR_MOVE_FN     "NVCursorMove"

struct nv_pipe_copy {
    uint32_t version;                   // NV_PIPE_COPY_VERSION
    uint32_t srcPitch;                  // bytes per row of the source; 4 bytes per pixel on both sides
    uint32_t x0, y0, x1, y1;            // the rectangle copied, in pixels, the same in source and destination
    uint32_t dstWidth, dstHeight, dstPitch;   // the framebuffer the caller copies for: refused unless it is the lit one
    uint32_t gpuUs;                     // out: submit to completion, in microseconds
};
