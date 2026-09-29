#include "nv_gsp.h"

#include <string.h>

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint64_t align_down(uint64_t x, uint64_t a) { return x & ~(a - 1); }
static uint64_t align_up(uint64_t x, uint64_t a)   { return (x + a - 1) & ~(a - 1); }

// ---- ELF64 ----------------------------------------------------------------

bool nv_elf64_section(const uint8_t *elf, uint64_t len, const char *name,
                      uint64_t *off, uint64_t *size)
{
    if (len < 0x40 || memcmp(elf, "\x7f" "ELF", 4) != 0 || elf[4] != 2 || elf[5] != 1)
        return false;   // not a little-endian ELF64

    uint64_t shoff = rd64(elf + 0x28);
    uint16_t shentsize = rd16(elf + 0x3a), shnum = rd16(elf + 0x3c), shstrndx = rd16(elf + 0x3e);
    if (shentsize < 0x40 || shstrndx >= shnum || shoff > len || (uint64_t)shnum * shentsize > len - shoff)
        return false;

    const uint8_t *strsh = elf + shoff + (uint64_t)shstrndx * shentsize;
    uint64_t stroff = rd64(strsh + 0x18), strsize = rd64(strsh + 0x20);
    if (stroff > len || strsize > len - stroff)
        return false;

    size_t nlen = strlen(name);
    for (uint16_t i = 0; i < shnum; i++) {
        const uint8_t *sh = elf + shoff + (uint64_t)i * shentsize;
        uint32_t noff = rd32(sh);
        if (noff >= strsize || nlen + 1 > strsize - noff)
            continue;
        if (memcmp(elf + stroff + noff, name, nlen + 1) != 0)
            continue;
        uint64_t o = rd64(sh + 0x18), s = rd64(sh + 0x20);
        if (o > len || s > len - o)
            return false;
        *off = o;
        *size = s;
        return true;
    }
    return false;
}

// ---- nvfw_bin_hdr ------------------------------------------------------------

struct bin_hdr { uint32_t header_off, data_off, data_size; };

static bool parse_bin_hdr(const uint8_t *bin, uint32_t len, bin_hdr *h)
{
    if (len < 0x18 || rd32(bin) != 0x10de)
        return false;
    h->header_off = rd32(bin + 0x0c);
    h->data_off   = rd32(bin + 0x10);
    h->data_size  = rd32(bin + 0x14);
    return h->header_off < len && h->data_off <= len && h->data_size <= len - h->data_off;
}

// ---- Booter ---------------------------------------------------------------

static bool booter_fail(nv_booter *b, const char *why)
{
    b->err = why;
    return false;
}

bool nv_booter_parse(const uint8_t *bin, uint32_t len, nv_booter *b)
{
    memset(b, 0, sizeof(*b));
    bin_hdr h;
    if (!parse_bin_hdr(bin, len, &h))
        return booter_fail(b, "booter: no valid 0x10de header");
    b->img = bin + h.data_off;
    b->img_size = h.data_size;

    // nvfw_hs_header_v2: sig_prod_offset, sig_prod_size, patch_loc, patch_sig,
    // meta_data_offset, meta_data_size, num_sig, header_offset, header_size.
    if (h.header_off + 36 > len)
        return booter_fail(b, "booter: HS header outside file");
    const uint8_t *hs = bin + h.header_off;
    uint32_t sig_off = rd32(hs), sig_total = rd32(hs + 4);
    uint32_t p_loc = rd32(hs + 8), p_sig = rd32(hs + 12);
    uint32_t meta_off = rd32(hs + 16), meta_size = rd32(hs + 20);
    uint32_t p_num = rd32(hs + 24), lh_off = rd32(hs + 28);

    // patch_loc, patch_sig and num_sig are file offsets of the actual u32 values.
    if (p_loc + 4 > len || p_sig + 4 > len || p_num + 4 > len)
        return booter_fail(b, "booter: patch fields outside file");
    b->patch_loc = rd32(bin + p_loc);
    uint32_t sig_idx0 = rd32(bin + p_sig);
    b->sig_count = rd32(bin + p_num);
    if (b->sig_count == 0 || sig_total % b->sig_count)
        return booter_fail(b, "booter: bad signature count");
    // r570's booters (tu102 and tu116 alike) carry one signature. Several would need the one
    // matching the chip's fuse version (nouveau/r570 pick it by fuse_ver): not implemented.
    if (b->sig_count != 1)
        return booter_fail(b, "booter: several signatures; choosing by fuse version is not implemented");
    b->sig_size = sig_total / b->sig_count;
    if (sig_off > len || (uint64_t)sig_idx0 + sig_total > len - sig_off || b->sig_size == 0 || (b->sig_size & 3))
        return booter_fail(b, "booter: signatures outside file");
    b->sig = bin + sig_off + sig_idx0;
    b->fuse_ver = (meta_size >= 4 && meta_off + 4 <= len) ? rd32(bin + meta_off) : 0;

    // nvfw_hs_load_header_v2: os_code_offset/size, os_data_offset/size, num_apps, apps[] {offset, size}
    if (lh_off + 28 > len)
        return booter_fail(b, "booter: load header outside file");
    const uint8_t *lh = bin + lh_off;
    b->os_code_off  = rd32(lh);
    b->os_code_size = rd32(lh + 4);
    b->os_data_off  = rd32(lh + 8);
    b->os_data_size = rd32(lh + 12);
    b->num_apps     = rd32(lh + 16);
    b->app0_off     = rd32(lh + 20);
    b->app0_size    = rd32(lh + 24);
    if (b->num_apps != 1)
        return booter_fail(b, "booter: expected exactly one app");

    // Image: [os code][app0 code] ... [os data]; os code is non-secure, app0 is HS.
    // s_prepareHsFalconDirect (r570): non-secure code to IMEM 0, secure code right
    // after it (256-byte aligned); each tagged with its image offset.
    b->nsec_img  = b->os_code_off;
    b->nsec_imem = 0;
    b->nsec_tag  = b->os_code_off;
    b->nsec_size = b->os_code_size;
    b->sec_img   = b->app0_off;
    b->sec_imem  = (b->os_code_size + 0xff) & ~0xffu;
    b->sec_tag   = b->app0_off;
    b->sec_size  = b->app0_size;
    b->dmem_img  = b->os_data_off;
    b->dmem_size = b->os_data_size;
    b->boot_vector = 0;

    if ((uint64_t)b->nsec_img + b->nsec_size > b->img_size ||
        (uint64_t)b->sec_img + b->sec_size > b->img_size ||
        (uint64_t)b->dmem_img + b->dmem_size > b->img_size)
        return booter_fail(b, "booter: sections outside the image");
    if ((b->nsec_size | b->sec_size | b->nsec_imem | b->sec_imem) & 0xff)
        return booter_fail(b, "booter: IMEM sections not 256-byte aligned");
    if (b->dmem_size & 3)
        return booter_fail(b, "booter: DMEM size not 4-byte aligned");
    if (b->patch_loc < b->dmem_img || (uint64_t)b->patch_loc + b->sig_size > (uint64_t)b->dmem_img + b->dmem_size)
        return booter_fail(b, "booter: signature patch location not inside DMEM");
    return true;
}

void nv_booter_patch(const nv_booter *b, uint8_t *img)
{
    memcpy(img + b->patch_loc, b->sig, b->sig_size);
}

// ---- RISC-V GSP bootloader ---------------------------------------------------

bool nv_gspbl_parse(const uint8_t *bin, uint32_t len, nv_gspbl *bl)
{
    memset(bl, 0, sizeof(*bl));
    bin_hdr h;
    if (!parse_bin_hdr(bin, len, &h)) {
        bl->err = "GSP bootloader: no valid 0x10de header";
        return false;
    }
    // RM_RISCV_UCODE_DESC is 19 u32 (0x4c bytes).
    if (h.header_off + 0x4c > len) {
        bl->err = "GSP bootloader: descriptor outside file";
        return false;
    }
    const uint8_t *d = bin + h.header_off;
    bl->app_version  = rd32(d + 0x1c);
    bl->manifest_off = rd32(d + 0x20);
    bl->data_off     = rd32(d + 0x28);   // monitorDataOffset
    bl->code_off     = rd32(d + 0x30);   // monitorCodeOffset
    bl->img  = bin + h.data_off;
    bl->size = h.data_size;
    if (bl->code_off >= bl->size || bl->data_off >= bl->size || bl->manifest_off >= bl->size) {
        bl->err = "GSP bootloader: offsets outside the image";
        return false;
    }
    return true;
}

// ---- WPR2 layout ------------------------------------------------------------

uint64_t nv_gsp_heap_size_tu1xx(uint64_t fb_size)
{
    const uint64_t MiB = 1ull << 20;
    uint64_t fb_gb = (fb_size + (1ull << 30) - 1) >> 30;
    uint64_t heap = 0                                   // GSP_FW_HEAP_PARAM_OS_SIZE_LIBOS2
                  + 8 * MiB                             // GSP_FW_HEAP_PARAM_BASE_RM_SIZE_TU10X
                  + align_up(98304 * fb_gb, MiB)        // GSP_FW_HEAP_PARAM_SIZE_PER_GB_FB
                  + align_up(100663296, MiB);           // GSP_FW_HEAP_PARAM_CLIENT_ALLOC_SIZE
    if (heap < 64 * MiB)                                // GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MIN_MB
        heap = 64 * MiB;
    if (heap > 256 * MiB)                               // GSP_FW_HEAP_SIZE_OVERRIDE_LIBOS2_MAX_MB
        heap = 256 * MiB;
    return heap;
}

bool nv_wpr2_layout_tu1xx(uint64_t fb_size, uint64_t vga_addr, uint64_t elf_size,
                          uint64_t boot_size, uint32_t meta_size, nv_wpr2_layout *l)
{
    memset(l, 0, sizeof(*l));
    if (!fb_size || vga_addr >= fb_size || !elf_size || !boot_size || !meta_size)
        return false;
    l->fb_size  = fb_size;
    l->vga_addr = vga_addr;
    l->vga_size = fb_size - vga_addr;

    l->frts_size = 0x100000;
    l->frts_addr = align_down(vga_addr, 0x20000) - l->frts_size;

    l->boot_size = boot_size;
    l->boot_addr = align_down(l->frts_addr - boot_size, 0x1000);

    l->elf_size = elf_size;
    l->elf_addr = align_down(l->boot_addr - elf_size, 0x10000);

    // Turing has no scrubber ucode, so the heap must also fit in the 256 MiB the
    // VBIOS pre-scrubs at the top of VRAM, next to the 1 MiB meta + 1 MiB non-WPR
    // heap below it and everything above it (kgspGetFwHeapSize, r570).
    uint64_t heap = nv_gsp_heap_size_tu1xx(fb_size);
    uint64_t post = align_up(fb_size - l->elf_addr, 0x100000);
    if (post + 2 * 0x100000 >= 256ull << 20)
        return false;
    uint64_t max_heap = align_down((256ull << 20) - post - 2 * 0x100000, 0x100000);
    if (heap > max_heap)
        heap = max_heap;
    l->heap_addr = align_down(l->elf_addr - heap, 0x100000);
    l->heap_size = align_down(l->elf_addr - l->heap_addr, 0x100000);

    l->wpr2_addr = align_down(l->heap_addr - meta_size, 0x100000);
    l->wpr2_size = l->frts_addr + l->frts_size - l->wpr2_addr;

    l->nonwpr_size = 0x100000;
    l->nonwpr_addr = l->wpr2_addr - l->nonwpr_size;

    l->wpr_end = align_down(vga_addr, 0x20000);

    // Sanity: everything inside VRAM, ordered, and non-overlapping.
    return l->nonwpr_addr < l->wpr2_addr && l->wpr2_addr < l->heap_addr &&
           l->heap_addr + l->heap_size <= l->elf_addr && l->elf_addr + l->elf_size <= l->boot_addr &&
           l->boot_addr + l->boot_size <= l->frts_addr && l->frts_addr + l->frts_size <= l->wpr_end &&
           l->wpr_end <= fb_size && l->nonwpr_addr < fb_size;
}
