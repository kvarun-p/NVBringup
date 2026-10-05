// FWSEC (VBIOS falcon ucode): the v2 (Turing) and v3 (GA10x+) descriptors, the falcon load
// plan, and the DMEMMAPPER interface used to hand FWSEC the FRTS command. Pure C++, shared
// by the kext and the host tool.
//
// Layouts follow nouveau (nvkm/subdev/gsp/fwsec.c, include/nvfw/fw.h) and the
// FALCON_UCODE_DESC_V2/V3 and FALCON_APPLICATION_INTERFACE_* definitions in NVIDIA's
// open-gpu-kernel-modules (kernel_gsp_fwsec.c).
#pragma once

#include <stdint.h>
#include "nv_vbios.h"

#define NV_FWSEC_DESC_V2_SIZE       0x3c
#define NV_FWSEC_DESC_V3_SIZE       0x2c         // FALCON_UCODE_DESC_V3_SIZE_44; PKC signatures follow
#define NV_FWSEC_V3_SIG_SIZE        384          // BCRT30_RSA3K_SIG_SIZE
#define NV_APPIF_ID_DMEMMAPPER      0x04
#define NV_DMEMMAPPER_SIG           0x50414d44u  // "DMAP"
#define NV_DMEMMAPPER_CMD_FRTS      0x15
#define NV_DMEMMAPPER_CMD_SB        0x19         // restore pre-OS apps (driver unload)
#define NV_FRTS_CMD_SIZE            44           // READ_VBIOS (24) + FRTS_REGION (20)
#define NV_FRTS_SIZE                0x100000     // 1 MiB, as nouveau/nova use
#define NV_FRTS_REGION_MEDIA_FB     2

struct nv_fwsec {
    uint8_t  version;                            // descriptor version: 2 (Turing) or 3 (GA10x+)
    // descriptor fields (v2; v3 shares the ones it has)
    uint32_t stored_size, uncompressed_size, virtual_entry, interface_offset;
    uint32_t imem_phys_base, imem_load_size, imem_virt_base, imem_sec_base, imem_sec_size;
    uint32_t dmem_offset, dmem_phys_base, dmem_load_size;
    // v3 (PKC-signed HS ucode, booted by the falcon's boot ROM): the signature's DMEM offset,
    // the engine id mask and ucode id the boot ROM checks, and the signatures after the
    // descriptor: sig_count of NV_FWSEC_V3_SIG_SIZE bytes, one per version bit in sig_versions
    // (lowest version first), to be chosen by the chip's fuse (nv_fwsec_sig_index).
    uint32_t pkc_data_off;
    uint16_t engine_id_mask, sig_versions;
    uint8_t  ucode_id, sig_count;
    uint32_t sigs_rom;                           // ROM offset of the signatures

    // Load plan. *_img offsets are relative to image_rom (start of the ucode image).
    uint32_t image_rom;                          // ROM offset of the ucode image
    uint32_t nsec_img, nsec_size, nsec_imem;     // non-secure IMEM (v2 only; v3: empty)
    uint32_t sec_img,  sec_size,  sec_imem;      // secure IMEM (HS code); v3: all of IMEM
    uint32_t sec_va;                             // v3: IMEM virtual base, tag and boot vector
    uint32_t dmem_img, dmem_size, dmem_addr;     // DMEM
    uint32_t boot_vector;

    // DMEM interface (offsets within DMEM)
    uint32_t appif_off;
    uint8_t  appif_count;
    uint32_t dmap_off;
    uint16_t dmap_version, dmap_size;
    uint32_t init_cmd, cmd_in_off, cmd_in_size, cmd_mask0, cmd_mask1;

    const char *err;
};

// Parses the FWSEC descriptor found by nv_vbios_find_fwsec(): v2 (Turing) or v3 (GA10x+).
bool nv_fwsec_parse(const nv_vbios *v, nv_fwsec *f);

// v3: which signature the chip's fuse selects. `fuse_reg` is NV_FUSE_OPT_FPF_GSP_UCODE<n>_VERSION
// for the ucode id (kgspReadUcodeFuseVersion_GA100: the highest set bit + 1 is the version,
// 0 when nothing is burnt). False: the VBIOS carries no signature for that version.
bool nv_fwsec_sig_index(const nv_fwsec *f, uint32_t fuse_reg, uint32_t *index);

// v3: copies signature `index` (from the ROM) into `dmem` (a copy of the FWSEC DMEM).
void nv_fwsec_patch_sig(const nv_fwsec *f, const uint8_t *rom, uint8_t *dmem, uint32_t index);

// VGA workspace start, as nova-core computes it (fb.rs): the top 1 MiB of VRAM
// unless display is enabled and NV_PDISP_VGA_WORKSPACE_BASE holds a valid
// address; one below that 1 MiB boundary means "top 128 KiB" instead.
uint64_t nv_vga_workspace(uint64_t vram_size, bool display_enabled, uint32_t vga_reg);

// FRTS placement: 1 MiB directly below the VGA workspace aligned down to 128 KiB.
uint64_t nv_frts_addr(uint64_t vga_workspace);

// Writes the FRTS command into `dmem` (a copy of the FWSEC DMEM, f->dmem_size
// bytes) and sets the DMEMMAPPER init command. The ROM itself is never modified.
bool nv_fwsec_patch_frts(const nv_fwsec *f, uint8_t *dmem, uint64_t frts_addr, uint64_t frts_size);

// FWSEC-SB (r570 kgspPrepareForFwsecSb_TU102): command 0x19, whose input is just
// the READ_VBIOS descriptor. Run at driver unload, before booter_unload.
bool nv_fwsec_patch_sb(const nv_fwsec *f, uint8_t *dmem);

// ---- Generic bootloader (linux-firmware nvidia/tu102/gsp/gen_bootloader-*.bin) ----
//
// On Turing, FWSEC is not loaded directly: this small falcon program is loaded
// by PIO, then DMA-copies FWSEC from system memory and jumps to it (nouveau
// fwsec.c nvkm_gsp_fwsec_v2, nova-core firmware/fwsec/bootloader.rs). v2 only:
// GA10x+ load the v3 ucode with the falcon's own DMA engine (hal_ga10x.cpp).

#define NV_BL_DMEM_DESC_SIZE            84   // flcn_bl_dmem_desc_v2
#define NV_FALCON_DMAIDX_PHYS_SYS_NCOH  4

struct nv_genbl {
    const uint8_t *code;
    uint32_t code_size;       // padded to 256 by the caller when loading
    uint32_t start_tag;       // IMEM tag; code is loaded and booted at start_tag << 8
    uint32_t dmem_load_off;   // where the descriptor goes in DMEM
    const char *err;
};

// Parses the nvfw_bin_hdr + nvfw_bl_desc container.
bool nv_genbl_parse(const uint8_t *bin, uint32_t len, nv_genbl *bl);

// FWSEC as the bootloader's DMA source: the ucode placed at `padding` bytes into
// the buffer so every IMEM section sits at its destination offset ("mirror image").
uint32_t nv_fwsec_dma_padding(const nv_fwsec *f);

// Builds the bootloader's DMEM descriptor for a DMA buffer at device address
// `dma_base` holding [padding zeros][patched FWSEC ucode].
bool nv_fwsec_bl_desc(const nv_fwsec *f, uint64_t dma_base, uint8_t out[NV_BL_DMEM_DESC_SIZE]);
