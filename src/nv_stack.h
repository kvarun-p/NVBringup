// The contract between NVBringup and its auxiliary kexts (NVMetalAccel in accel/, NVFramebuffer and NVAGDCControl in
// display/), and the switches all three honour. They are built from this repository but installed by different routes
// (NVBringup on the OpenCore EFI, the others into the auxiliary kext collection with kmutil), so a mixed pair is easy to
// boot, and it used to show up only as "no signal" or a login loop.
//
// NVBringup publishes on itself:
//   NVStackABI   (number)   NV_STACK_ABI, bumped on every change to a cross-kext contract: the callPlatformFunction
//                           names and argument structs (nv_pipe_copy.h), the NVDisplayFB keys, NVDisplayNub and the
//                           NVDisplayPipeArmed gate, the NVDisplayMsgState message
//   NVStackFeatures (number) NV_STACK_F_* bits for what this NVBringup offers inside that ABI
//   NVUapiVersion, NVUapiFeatures (numbers)  NVMAC_ABI_VERSION and nvmac_info.features (nv_uapi.h), for tools that
//                           compare a Mesa build with the kext without opening a connection
//   NVSafeMode   (boolean)  true when the boot-attempt guard (boot-arg nvguard=1) found too many unfinished boots: the
//                           experimental gates are off this boot and the auxiliary kexts stay out
// An auxiliary kext calls nv_stack_admit() on the NVBringup it is about to use, in probe(): it refuses (and says why in
// the log) unless NVStackABI equals the NV_STACK_ABI it was built with. An NVBringup that predates NVStackABI reads as 0.
//
// Boot-arg -nvoff: NVBringup and every auxiliary kext decline to start (the GPU is left as the firmware left it), for a
// boot that must not touch the GPU. nv_stack_off() reads it.
#pragma once

#include <stdint.h>

#define NV_STACK_ABI            1

#define NV_STACK_ABI_KEY        "NVStackABI"
#define NV_STACK_FEATURES_KEY   "NVStackFeatures"
#define NV_SAFE_MODE_KEY        "NVSafeMode"
#define NV_UAPI_VERSION_KEY     "NVUapiVersion"
#define NV_UAPI_FEATURES_KEY    "NVUapiFeatures"

// NVStackFeatures
enum {
    NV_STACK_F_PIPE_COPY    = 1u << 0,      // NV_PIPE_COPY_FN (copy-engine frame copy for NVDisplayPipe)
    NV_STACK_F_VBLANK_LINES = 1u << 1,      // NV_VBLANK_LINES_FN
    NV_STACK_F_CURSOR       = 1u << 2,      // NV_CURSOR_IMAGE_FN, NV_CURSOR_MOVE_FN
    NV_STACK_F_DISPLAY_NUB  = 1u << 3,      // NVDisplayNub with NVDisplayFB (NVFramebuffer's provider)
    NV_STACK_F_PIPE_FLIP    = 1u << 4,      // pipe frames flipped at vblank (NVPipeFlip)
    NV_STACK_F_PIPE_SCANOUT = 1u << 5,      // NV_PIPE_SCANOUT_FN (zero-copy scanout); published only with boot-arg nvzerocopy
};
#define NV_STACK_FEATURES   (NV_STACK_F_PIPE_COPY | NV_STACK_F_VBLANK_LINES | NV_STACK_F_CURSOR | \
                             NV_STACK_F_DISPLAY_NUB | NV_STACK_F_PIPE_FLIP)

#if defined(KERNEL) && defined(__cplusplus)
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSNumber.h>
#include <pexpert/pexpert.h>

// Boot-arg -nvoff (a flag; nvoff=1 works too).
static inline bool nv_stack_off()
{
    uint32_t v = 0;
    return PE_parse_boot_argn("-nvoff", &v, sizeof(v)) || (PE_parse_boot_argn("nvoff", &v, sizeof(v)) && v);
}

// May the kext `who` use this NVBringup? Logs the reason once per kext load when it may not. nullptr (no NVBringup)
// is "no" without a log line: the callers' own checks report a missing GPU.
static inline bool nv_stack_admit(IOService *gpu, const char *who)
{
    static bool told;       // one instance per kext binary (the header is compiled into each)
    if (nv_stack_off()) {
        if (!told)
            IOLog("%s: boot-arg -nvoff: not starting\n", who);
        told = true;
        return false;
    }
    if (!gpu)
        return false;
    OSNumber *abi = OSDynamicCast(OSNumber, gpu->getProperty(NV_STACK_ABI_KEY));
    const uint32_t have = abi ? abi->unsigned32BitValue() : 0;
    if (have != NV_STACK_ABI) {
        if (!told)
            IOLog("%s: NVBringup has stack ABI %u, this kext was built for %u: not starting. Install the NVBringup.kext "
                  "and the auxiliary kexts from the same checkout (tools/nvctl status shows both)\n",
                  who, have, NV_STACK_ABI);
        told = true;
        return false;
    }
    OSBoolean *safe = OSDynamicCast(OSBoolean, gpu->getProperty(NV_SAFE_MODE_KEY));
    if (safe && safe->isTrue()) {
        if (!told)
            IOLog("%s: NVBringup is in safe mode (boot-attempt guard): not starting\n", who);
        told = true;
        return false;
    }
    return true;
}
#endif
