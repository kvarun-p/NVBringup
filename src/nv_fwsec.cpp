#include "nv_fwsec.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t x) { p[0] = (uint8_t)x; p[1] = (uint8_t)(x >> 8); p[2] = (uint8_t)(x >> 16); p[3] = (uint8_t)(x >> 24); }
static void wr64(uint8_t *p, uint64_t x) { wr32(p, (uint32_t)x); wr32(p + 4, (uint32_t)(x >> 32)); }

// DMEMMAPPER v3 field offsets
#define DMAP_VERSION        0x04
#define DMAP_SIZE           0x06
#define DMAP_CMD_IN_OFF     0x08
#define DMAP_CMD_IN_SIZE    0x0c
#define DMAP_INIT_CMD       0x2c
#define DMAP_CMD_MASK0      0x34
#define DMAP_CMD_MASK1      0x38
#define DMAP_V3_SIZE        0x40

static bool fail(nv_fwsec *f, const char *why)
{
    f->err = why;
    return false;
}

bool nv_fwsec_parse(const nv_vbios *v, nv_fwsec *f)
{
    memset(f, 0, sizeof(*f));
    if (v->desc_version != 2)
        return fail(f, "FWSEC descriptor is not v2 (Turing)");
    if (v->desc_size != NV_FWSEC_DESC_V2_SIZE)
        return fail(f, "FWSEC v2 descriptor has unexpected size");

    // The descriptor and ucode must lie within the FWSEC image that holds them.
    uint32_t img_end = 0;
    for (uint32_t i = 0; i < v->nimages; i++) {
        const nv_rom_image &im = v->img[i];
        if (im.type == NV_IMAGE_FWSEC && v->desc_rom >= im.offset && v->desc_rom < im.offset + im.size)
            img_end = im.offset + im.size;
    }
    if (!img_end)
        return fail(f, "FWSEC descriptor is not inside a FWSEC image");
    if (v->desc_rom + NV_FWSEC_DESC_V2_SIZE > img_end)
        return fail(f, "FWSEC descriptor runs past its image");

    const uint8_t *d = v->rom + v->desc_rom;
    f->stored_size       = rd32(d + 0x04);
    f->uncompressed_size = rd32(d + 0x08);
    f->virtual_entry     = rd32(d + 0x0c);
    f->interface_offset  = rd32(d + 0x10);
    f->imem_phys_base    = rd32(d + 0x14);
    f->imem_load_size    = rd32(d + 0x18);
    f->imem_virt_base    = rd32(d + 0x1c);
    f->imem_sec_base     = rd32(d + 0x20);
    f->imem_sec_size     = rd32(d + 0x24);
    f->dmem_offset       = rd32(d + 0x28);
    f->dmem_phys_base    = rd32(d + 0x2c);
    f->dmem_load_size    = rd32(d + 0x30);

    f->image_rom = v->desc_rom + NV_FWSEC_DESC_V2_SIZE;
    if (f->stored_size != f->uncompressed_size)
        return fail(f, "FWSEC ucode is compressed (unsupported)");
    if (f->stored_size > img_end - f->image_rom)
        return fail(f, "FWSEC ucode runs past its image");

    // IMEM: non-secure code first, then the secure (HS) part at imem_sec_base.
    if (f->imem_sec_size > f->imem_load_size || f->imem_sec_base < f->imem_virt_base)
        return fail(f, "FWSEC IMEM layout inconsistent");
    f->nsec_img  = 0;
    f->nsec_size = f->imem_load_size - f->imem_sec_size;
    f->nsec_imem = f->imem_phys_base;
    f->sec_img   = f->imem_sec_base - f->imem_virt_base;
    f->sec_size  = f->imem_sec_size;
    f->sec_imem  = f->imem_phys_base + f->sec_img;     // as nova-core: phys base + offset
    if (f->sec_img + f->sec_size > f->imem_load_size)
        return fail(f, "FWSEC secure IMEM runs past the IMEM load size");
    if ((f->nsec_size | f->sec_size | f->sec_imem) & 0xff)
        return fail(f, "FWSEC IMEM sections are not 256-byte aligned");

    f->dmem_img  = f->dmem_offset;
    f->dmem_size = f->dmem_load_size;
    f->dmem_addr = f->dmem_phys_base;
    if (f->dmem_img < f->imem_load_size || f->dmem_img + f->dmem_size > f->stored_size)
        return fail(f, "FWSEC DMEM outside the ucode image");
    if ((f->dmem_size | f->dmem_addr) & 3)
        return fail(f, "FWSEC DMEM not 4-byte aligned");
    f->boot_vector = f->virtual_entry;

    // Application interface table in DMEM.
    const uint8_t *dmem = v->rom + f->image_rom + f->dmem_img;
    f->appif_off = f->interface_offset;
    if (f->appif_off + 4 > f->dmem_size)
        return fail(f, "interface table outside DMEM");
    const uint8_t *hdr = dmem + f->appif_off;
    uint8_t hsize = hdr[1], esize = hdr[2];
    f->appif_count = hdr[3];
    if (hdr[0] != 1 || hsize != 4 || esize != 8 || f->appif_count == 0 || f->appif_count > 16)
        return fail(f, "interface table header not v1/4/8");
    if (f->appif_off + hsize + (uint32_t)f->appif_count * esize > f->dmem_size)
        return fail(f, "interface table entries outside DMEM");

    bool found = false;
    for (uint32_t i = 0; i < f->appif_count; i++) {
        const uint8_t *e = hdr + hsize + i * esize;
        if (rd32(e) == NV_APPIF_ID_DMEMMAPPER) {
            f->dmap_off = rd32(e + 4);
            found = true;
            break;
        }
    }
    if (!found)
        return fail(f, "no DMEMMAPPER entry in interface table");
    if (f->dmap_off + DMAP_V3_SIZE > f->dmem_size)
        return fail(f, "DMEMMAPPER outside DMEM");

    const uint8_t *m = dmem + f->dmap_off;
    f->dmap_version = rd16(m + DMAP_VERSION);
    f->dmap_size    = rd16(m + DMAP_SIZE);
    f->cmd_in_off   = rd32(m + DMAP_CMD_IN_OFF);
    f->cmd_in_size  = rd32(m + DMAP_CMD_IN_SIZE);
    f->init_cmd     = rd32(m + DMAP_INIT_CMD);
    f->cmd_mask0    = rd32(m + DMAP_CMD_MASK0);
    f->cmd_mask1    = rd32(m + DMAP_CMD_MASK1);
    if (rd32(m) != NV_DMEMMAPPER_SIG || f->dmap_version != 3 || f->dmap_size < DMAP_V3_SIZE)
        return fail(f, "DMEMMAPPER is not a DMAP v3 structure");
    if (f->cmd_in_size < NV_FRTS_CMD_SIZE || f->cmd_in_off + NV_FRTS_CMD_SIZE > f->dmem_size)
        return fail(f, "DMEMMAPPER command buffer too small or outside DMEM");
    return true;
}

uint64_t nv_vga_workspace(uint64_t vram_size, bool display_enabled, uint32_t vga_reg)
{
    uint64_t base = vram_size - 0x100000;
    if (!display_enabled || !(vga_reg & 0x8))
        return base;
    uint64_t addr = (uint64_t)(vga_reg >> 8) << 16;
    return addr < base ? vram_size - 0x20000 : addr;
}

uint64_t nv_frts_addr(uint64_t vga_workspace)
{
    return (vga_workspace & ~(uint64_t)0x1ffff) - NV_FRTS_SIZE;
}

bool nv_fwsec_patch_frts(const nv_fwsec *f, uint8_t *dmem, uint64_t frts_addr, uint64_t frts_size)
{
    if ((frts_addr | frts_size) & 0xfff)
        return false;

    wr32(dmem + f->dmap_off + DMAP_INIT_CMD, NV_DMEMMAPPER_CMD_FRTS);

    uint8_t *c = dmem + f->cmd_in_off;
    memset(c, 0, NV_FRTS_CMD_SIZE);
    // FWSECLIC_READ_VBIOS_DESC: no separate VBIOS image, flags = 2
    wr32(c + 0x00, 1);
    wr32(c + 0x04, 24);
    wr64(c + 0x08, 0);
    wr32(c + 0x10, 0);
    wr32(c + 0x14, 2);
    // FWSECLIC_FRTS_REGION_DESC: offset and size in 4 KiB units, in framebuffer
    wr32(c + 0x18, 1);
    wr32(c + 0x1c, 20);
    wr32(c + 0x20, (uint32_t)(frts_addr >> 12));
    wr32(c + 0x24, (uint32_t)(frts_size >> 12));
    wr32(c + 0x28, NV_FRTS_REGION_MEDIA_FB);
    return true;
}

bool nv_fwsec_patch_sb(const nv_fwsec *f, uint8_t *dmem)
{
    if (f->cmd_in_size < 24)
        return false;
    wr32(dmem + f->dmap_off + DMAP_INIT_CMD, NV_DMEMMAPPER_CMD_SB);
    uint8_t *c = dmem + f->cmd_in_off;
    memset(c, 0, 24);
    wr32(c + 0x00, 1);      // FWSECLIC_READ_VBIOS_DESC, as for FRTS
    wr32(c + 0x04, 24);
    wr32(c + 0x14, 2);
    return true;
}

// ---- Generic bootloader ----------------------------------------------------

bool nv_genbl_parse(const uint8_t *bin, uint32_t len, nv_genbl *bl)
{
    memset(bl, 0, sizeof(*bl));
    if (len < 0x18) {
        bl->err = "bootloader file too small";
        return false;
    }
    uint32_t magic = rd32(bin), hoff = rd32(bin + 0x0c), doff = rd32(bin + 0x10), dsize = rd32(bin + 0x14);
    if (magic != 0x10de) {
        bl->err = "bootloader file has no 0x10de header";
        return false;
    }
    if (hoff + 0x18 > len || doff > len || dsize > len - doff) {
        bl->err = "bootloader header points outside the file";
        return false;
    }
    const uint8_t *d = bin + hoff;
    bl->start_tag     = rd32(d + 0x00);
    bl->dmem_load_off = rd32(d + 0x04);
    uint32_t code_off = rd32(d + 0x08);
    bl->code_size     = rd32(d + 0x0c);
    if (code_off > dsize || bl->code_size > dsize - code_off || bl->code_size == 0 || (bl->code_size & 3)) {
        bl->err = "bootloader code outside its data section";
        return false;
    }
    // Loaded at start_tag << 8 and must fit below 64 KiB of IMEM.
    if (bl->start_tag == 0 || bl->start_tag > 0xff ||
        (bl->start_tag << 8) + ((bl->code_size + 0xff) & ~0xffu) > 0x10000) {
        bl->err = "bootloader start tag out of range";
        return false;
    }
    if (bl->dmem_load_off & 3) {
        bl->err = "bootloader DMEM offset misaligned";
        return false;
    }
    bl->code = bin + doff + code_off;
    return true;
}

uint32_t nv_fwsec_dma_padding(const nv_fwsec *f)
{
    return f->sec_imem - f->sec_img;
}

bool nv_fwsec_bl_desc(const nv_fwsec *f, uint64_t dma_base, uint8_t out[NV_BL_DMEM_DESC_SIZE])
{
    // The descriptor uses one offset as both DMA source and IMEM destination, so
    // the buffer must mirror IMEM; the bootloader always copies data to DMEM 0.
    uint32_t pad = nv_fwsec_dma_padding(f);
    if (f->sec_imem < f->sec_img || f->nsec_imem != f->nsec_img + pad || f->dmem_addr != 0)
        return false;
    if (dma_base & 0xff)
        return false;

    memset(out, 0, NV_BL_DMEM_DESC_SIZE);
    wr32(out + 32, NV_FALCON_DMAIDX_PHYS_SYS_NCOH);          // ctx_dma
    wr64(out + 36, dma_base);                                 // code_dma_base
    wr32(out + 44, f->nsec_imem);                             // non_sec_code_off
    wr32(out + 48, f->nsec_size);                             // non_sec_code_size
    wr32(out + 52, f->sec_imem);                              // sec_code_off
    wr32(out + 56, f->sec_size);                              // sec_code_size
    wr32(out + 60, f->boot_vector);                           // code_entry_point
    wr64(out + 64, dma_base + pad + f->dmem_img);             // data_dma_base
    wr32(out + 72, f->dmem_size);                             // data_size
    return true;                                              // argc, argv = 0
}
