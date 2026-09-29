// GSP-RM boot preparation for Turing (r570.144 firmware): firmware container
// parsing and the WPR2 layout. Pure C++, shared by the kext and the host tool.
//
// Follows nouveau (nvkm/subdev/gsp/tu102.c, rm/r535/gsp.c) and nova-core
// (fb.rs, gsp/fw.rs); constants from NVIDIA open-gpu-kernel-modules r570.144.
#pragma once

#include <stdint.h>

// ---- ELF64 (gsp-570.144.bin) ----------------------------------------------

// Finds a section by name. Returns false if the file is malformed or lacks it.
bool nv_elf64_section(const uint8_t *elf, uint64_t len, const char *name,
                      uint64_t *off, uint64_t *size);

#define NV_GSP_ELF_IMAGE      ".fwimage"
#define NV_GSP_ELF_SIG_TU10X  ".fwsignature_tu10x"   // TU102/TU104/TU106 (nouveau tu102.c)
#define NV_GSP_ELF_SIG_TU11X  ".fwsignature_tu11x"   // TU116/TU117 (nouveau tu116.c)

// ---- Booter (booter_load/unload-570.144.bin, HS v2) -----------------------

struct nv_booter {
    const uint8_t *img;         // ucode image (bin data section)
    uint32_t img_size;
    uint32_t os_code_off, os_code_size, os_data_off, os_data_size;
    uint32_t num_apps, app0_off, app0_size;
    const uint8_t *sig;         // production signature 0 (Turing files carry one)
    uint32_t sig_count, sig_size;
    uint32_t patch_loc;         // image offset where the signature goes (inside DMEM)
    uint32_t fuse_ver;

    // PIO load plan into SEC2 (r570 s_prepareHsFalconDirect): image offset -> falcon
    // address, IMEM tag (byte address; the tag register takes it >> 8)
    uint32_t nsec_img, nsec_imem, nsec_tag, nsec_size;
    uint32_t sec_img, sec_imem, sec_tag, sec_size;
    uint32_t dmem_img, dmem_size;    // to DMEM 0
    uint32_t boot_vector;

    const char *err;
};

bool nv_booter_parse(const uint8_t *bin, uint32_t len, nv_booter *b);

// Copies the signature into `img` (a writable copy of b->img, img_size bytes).
void nv_booter_patch(const nv_booter *b, uint8_t *img);

// ---- RISC-V GSP bootloader (bootloader-570.144.bin) -----------------------

struct nv_gspbl {
    const uint8_t *img;         // loaded as-is into system memory
    uint32_t size;
    uint32_t code_off, data_off, manifest_off;   // monitor code/data, manifest
    uint32_t app_version;       // written to the GSP falcon's OS register (0x080)
    const char *err;
};

bool nv_gspbl_parse(const uint8_t *bin, uint32_t len, nv_gspbl *bl);

// ---- WPR2 layout ------------------------------------------------------------

// GSP-RM heap for Turing (LibOS2): base 8 MiB + 96 KiB per GiB of VRAM
// + 96 MiB client allocations, each 1 MiB-aligned, clamped to 64..256 MiB.
uint64_t nv_gsp_heap_size_tu1xx(uint64_t fb_size);

struct nv_wpr2_layout {
    uint64_t fb_size;
    uint64_t vga_addr, vga_size;          // VBIOS VGA workspace (top of VRAM)
    uint64_t frts_addr, frts_size;
    uint64_t boot_addr, boot_size;        // RISC-V GSP bootloader
    uint64_t elf_addr, elf_size;          // .fwimage
    uint64_t heap_addr, heap_size;        // GSP-RM heap
    uint64_t wpr2_addr, wpr2_size;        // WPR2 start (GspFwWprMeta sits here)
    uint64_t nonwpr_addr, nonwpr_size;    // 1 MiB non-WPR heap below WPR2
    uint64_t wpr_end;                     // gspFwWprEnd
};

// Lays out WPR2 top-down below the VGA workspace (nouveau tu102_gsp_oneinit).
bool nv_wpr2_layout_tu1xx(uint64_t fb_size, uint64_t vga_addr, uint64_t elf_size,
                          uint64_t boot_size, uint32_t meta_size, nv_wpr2_layout *l);
