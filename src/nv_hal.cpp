#include "nv_hal.h"
#include "nv_gsp.h"

// Classes: r570 class headers (clc46f.h, clc5b5.h, clc597.h, clc5c0.h, clc4b0.h).
static const nv_arch nv_arch_tu1xx = {
    NV_ARCH_TU1XX, "Turing", 75,
    { 0xc46f /* TURING_CHANNEL_GPFIFO_A */, 0xc5b5 /* TURING_DMA_COPY_A */, 0xc597 /* TURING_A */,
      0xc5c0 /* TURING_COMPUTE_A */, 0xc4b0 /* NVC4B0_VIDEO_DECODER */ },
    "tu102",
    nv_gsp_heap_size_tu1xx,
    nv_wpr2_layout_tu1xx,
};

// The Turing chips GSP-RM r570 supports: TU102/TU104/TU106 (TU10x: RTX 20, Quadro RTX, Titan RTX)
// and TU116/TU117 (TU11x: GTX 16). Signed firmware differs between the two groups. TU117 is the
// one tested; the others share its code paths and were supported before the HAL existed.
static const nv_chip nv_chips[] = {
    { 0x162, "TU102", &nv_arch_tu1xx, "tu102", NV_GSP_ELF_SIG_TU10X, false },
    { 0x164, "TU104", &nv_arch_tu1xx, "tu102", NV_GSP_ELF_SIG_TU10X, false },
    { 0x166, "TU106", &nv_arch_tu1xx, "tu102", NV_GSP_ELF_SIG_TU10X, false },
    { 0x167, "TU117", &nv_arch_tu1xx, "tu116", NV_GSP_ELF_SIG_TU11X, false },
    { 0x168, "TU116", &nv_arch_tu1xx, "tu116", NV_GSP_ELF_SIG_TU11X, false },
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
