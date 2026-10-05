// The chips the driver supports and what differs between GPU architectures: the data here
// (firmware, classes, GSP-RM heap and WPR2 layout); the boot sequences are NVBringup's HAL
// (NVBringup::Hal, hal_tu1xx.cpp). Pure C++, shared by the kext and the host tools.
//
// Adding an architecture: an nv_arch here, its chips in nv_chips (nv_hal.cpp), and a Hal with
// its boot sequences (NVIDIA's _GA102/_AD102 variants of kgspBootstrap, kflcnReset, ...).
// Chips marked experimental only run with boot-arg nvexperimental=1.
#pragma once

#include <stdint.h>

struct nv_wpr2_layout;

enum nv_arch_id {
    NV_ARCH_TU1XX,      // Turing: FWSEC v2 through the generic bootloader, booters on SEC2
};

// Engine classes a channel binds (SET_OBJECT) and GSP-RM allocates.
struct nv_classes {
    uint16_t gpfifo, copy, eng3d, compute, vdec;
};

struct nv_arch {
    nv_arch_id id;
    const char *name;
    uint8_t sm;                 // shader ISA level (7.5 -> 75), as NVK/NAK want it
    nv_classes cls;
    const char *gsp_dir;        // linux-firmware nvidia/<dir>/gsp: gsp- and bootloader-570.144.bin
    // GSP-RM heap size and the WPR2 layout below the VGA workspace (kgspGetFwHeapSize,
    // kgspCalculateFbLayout); false when the layout doesn't fit
    uint64_t (*gsp_heap_size)(uint64_t fb_size);
    bool (*wpr2_layout)(uint64_t fb_size, uint64_t vga_addr, uint64_t elf_size,
                        uint64_t boot_size, uint32_t meta_size, nv_wpr2_layout *l);
};

struct nv_chip {
    uint16_t chipset;           // NV_PMC_BOOT_0 bits 28:20
    const char *name;
    const nv_arch *arch;
    const char *booter_dir;     // linux-firmware nvidia/<dir>/gsp: booter_load/unload (signed per group)
    const char *elf_sig;        // section of the GSP-RM ELF with this chip's boot-ROM signature
    bool experimental;          // never run on hardware: needs boot-arg nvexperimental=1
};

// The chip for a chipset, or nullptr when the driver doesn't support it.
const nv_chip *nv_chip_find(uint32_t chipset);
// The chip by name ("TU117", any case), for the host tools.
const nv_chip *nv_chip_find_name(const char *name);
