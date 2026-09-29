// NVIDIA VBIOS parsing: PCI expansion-ROM image chain, BIT table, and the
// FWSEC falcon ucode descriptor. Pure C++ with no IOKit dependency, so the same
// code builds into the kext and into the host tool (tools/vbios_tool.cpp).
//
// Layouts follow nouveau (nvkm/subdev/bios) and nova-core (vbios.rs).
#pragma once

#include <stdint.h>

#define NV_ROM_MAX          0x100000u   // scan limit (1 MiB)
#define NV_ROM_MAX_IMAGES   16u

// PCIR code types seen on NVIDIA boards
#define NV_IMAGE_PCI_AT     0x00
#define NV_IMAGE_EFI        0x03
#define NV_IMAGE_NBSI       0x70
#define NV_IMAGE_FWSEC      0xe0

// BIT token and PMU lookup-table application IDs
#define NV_BIT_TOKEN_FALCON_DATA    0x70
#define NV_FALCON_APPID_FWSEC_DBG   0x45
#define NV_FALCON_APPID_FWSEC_PROD  0x85

enum nv_scan_status {
    NV_SCAN_OK,         // image chain complete within the bytes given
    NV_SCAN_NEED_MORE,  // provide at least *need bytes and scan again
    NV_SCAN_BAD,        // not a valid ROM (see nv_vbios::err)
};

struct nv_rom_image {
    uint32_t offset;    // from start of ROM
    uint32_t size;
    uint16_t vendor, device;
    uint8_t  type;      // PCIR code type
    bool     last;
    bool     has_npde;
};

struct nv_vbios {
    const uint8_t *rom;
    uint32_t len;

    // image chain (nv_vbios_scan)
    uint32_t     nimages;
    nv_rom_image img[NV_ROM_MAX_IMAGES];
    uint32_t     end;           // end of the last image
    int          pciat;         // index of the PCI-AT image, -1 if none
    bool         pciat_csum_ok; // PCI-AT image bytes sum to 0

    // FWSEC lookup (nv_vbios_find_fwsec)
    uint32_t bit_off;           // ROM offset of the BIT header
    uint16_t bit_version;
    uint8_t  bit_ntokens;
    uint32_t falcon_ptr;        // logical offset, see nv_vbios_find_fwsec
    uint32_t pmu_off;           // logical offset of PMU lookup table
    uint8_t  pmu_version, pmu_nentries;
    uint8_t  fwsec_appid;       // PROD, or DBG if only that exists
    uint32_t desc_logical;      // FWSEC ucode descriptor, logical offset
    uint32_t desc_rom;          //   ... and its ROM offset
    uint32_t desc_hdr;
    uint8_t  desc_version;      // 2 on Turing, 3 on Ampere+
    uint32_t desc_size;         // descriptor header size in bytes
    uint32_t desc_stored_size;  // ucode payload size

    const char *err;
};

// Walks the ROM image chain. Call again with more bytes on NV_SCAN_NEED_MORE.
nv_scan_status nv_vbios_scan(const uint8_t *rom, uint32_t len, nv_vbios *v, uint32_t *need);

// After a successful scan: finds BIT -> falcon data -> PMU table -> FWSEC descriptor.
bool nv_vbios_find_fwsec(nv_vbios *v);

const char *nv_image_type_name(uint8_t type);
