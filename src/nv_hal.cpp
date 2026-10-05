#include "nv_hal.h"
#include "nv_gsp.h"
#include "nv_regs.h"

// Classes: r570 class headers (clc46f.h, clc5b5.h, clc597.h, clc5c0.h, clc4b0.h).
static const nv_arch nv_arch_tu1xx = {
    NV_ARCH_TU1XX, "Turing", 75,
    { 0xc46f /* TURING_CHANNEL_GPFIFO_A */, 0xc5b5 /* TURING_DMA_COPY_A */, 0xc597 /* TURING_A */,
      0xc5c0 /* TURING_COMPUTE_A */, 0xc4b0 /* NVC4B0_VIDEO_DECODER */ },
    "tu102",
    nv_gsp_heap_size_tu1xx,
    nv_wpr2_layout_tu1xx,
    2, NV_FUSE_STATUS_OPT_DISPLAY, 0,
};

// Ampere GA10x (clc56f.h, clc7b5.h, clc797.h, clc7c0.h, clc7b0.h; nouveau rm/ga1xx.c). GSP-RM
// is LibOS3 here: a larger heap (nv_gsp_heap_size_ga10x). The display fuse moved
// (NV_FUSE_STATUS_OPT_DISPLAY_GA100) and the VBIOS reports the usable FB size in a scratch
// register. The same r570 GSP-RM ELF serves every GA10x chip (linux-firmware nvidia/ga102).
static const nv_arch nv_arch_ga10x = {
    NV_ARCH_GA10X, "Ampere", 86,
    { 0xc56f /* AMPERE_CHANNEL_GPFIFO_A */, 0xc7b5 /* AMPERE_DMA_COPY_B */, 0xc797 /* AMPERE_B */,
      0xc7c0 /* AMPERE_COMPUTE_B */, 0xc7b0 /* NVC7B0_VIDEO_DECODER */ },
    "ga102",
    nv_gsp_heap_size_ga10x,
    nv_wpr2_layout_ga10x,
    3, NV_FUSE_STATUS_OPT_DISPLAY_GA100, NV_USABLE_FB_SIZE_IN_MB,
};

// Ada AD10x (clc997.h, clc9c0.h, clc9b0.h; nouveau rm/ad10x.c): GA10x's boot path with Ada's
// graphics and video classes; the ELF is nvidia/ad102's.
static const nv_arch nv_arch_ad10x = {
    NV_ARCH_AD10X, "Ada", 89,
    { 0xc56f /* AMPERE_CHANNEL_GPFIFO_A */, 0xc7b5 /* AMPERE_DMA_COPY_B */, 0xc997 /* ADA_A */,
      0xc9c0 /* ADA_COMPUTE_A */, 0xc9b0 /* NVC9B0_VIDEO_DECODER */ },
    "ad102",
    nv_gsp_heap_size_ga10x,
    nv_wpr2_layout_ga10x,
    3, NV_FUSE_STATUS_OPT_DISPLAY_GA100, NV_USABLE_FB_SIZE_IN_MB,
};

// The chips GSP-RM r570 supports here. Turing: TU102/TU104/TU106 (TU10x: RTX 20, Quadro RTX,
// Titan RTX) and TU116/TU117 (TU11x: GTX 16); signed firmware differs between the two groups.
// TU117 is the one tested; the others share its code paths and were supported before the HAL
// existed. Ampere GA10x (RTX 30) and Ada AD10x (RTX 40) have booters signed per chip
// (linux-firmware nvidia/<chip>/gsp) and are experimental: the HAL follows NVIDIA's r570 and
// nouveau's GA102 code but has not run on hardware (boot-arg nvexperimental=1 lets it).
// GA100 (A100) is left out: no FRTS, LibOS2, Turing-style falcons; nothing to test it on.
static const nv_chip nv_chips[] = {
    { 0x162, "TU102", &nv_arch_tu1xx, "tu102", NV_GSP_ELF_SIG_TU10X, false },
    { 0x164, "TU104", &nv_arch_tu1xx, "tu102", NV_GSP_ELF_SIG_TU10X, false },
    { 0x166, "TU106", &nv_arch_tu1xx, "tu102", NV_GSP_ELF_SIG_TU10X, false },
    { 0x167, "TU117", &nv_arch_tu1xx, "tu116", NV_GSP_ELF_SIG_TU11X, false },
    { 0x168, "TU116", &nv_arch_tu1xx, "tu116", NV_GSP_ELF_SIG_TU11X, false },
    { 0x172, "GA102", &nv_arch_ga10x, "ga102", NV_GSP_ELF_SIG_GA10X, true },
    { 0x173, "GA103", &nv_arch_ga10x, "ga103", NV_GSP_ELF_SIG_GA10X, true },
    { 0x174, "GA104", &nv_arch_ga10x, "ga104", NV_GSP_ELF_SIG_GA10X, true },
    { 0x176, "GA106", &nv_arch_ga10x, "ga106", NV_GSP_ELF_SIG_GA10X, true },
    { 0x177, "GA107", &nv_arch_ga10x, "ga107", NV_GSP_ELF_SIG_GA10X, true },
    { 0x192, "AD102", &nv_arch_ad10x, "ad102", NV_GSP_ELF_SIG_AD10X, true },
    { 0x193, "AD103", &nv_arch_ad10x, "ad103", NV_GSP_ELF_SIG_AD10X, true },
    { 0x194, "AD104", &nv_arch_ad10x, "ad104", NV_GSP_ELF_SIG_AD10X, true },
    { 0x196, "AD106", &nv_arch_ad10x, "ad106", NV_GSP_ELF_SIG_AD10X, true },
    { 0x197, "AD107", &nv_arch_ad10x, "ad107", NV_GSP_ELF_SIG_AD10X, true },
};

const nv_chip *nv_chip_find(uint32_t chipset)
{
    for (const nv_chip &c : nv_chips)
        if (c.chipset == chipset)
            return &c;
    return nullptr;
}

const nv_chip *nv_chip_find_name(const char *name)
{
    for (const nv_chip &c : nv_chips) {
        const char *a = c.name, *b = name;
        while (*a && (*a == *b || (*b >= 'a' && *b <= 'z' && *a == *b - 32))) {
            a++;
            b++;
        }
        if (!*a && !*b)
            return &c;
    }
    return nullptr;
}
