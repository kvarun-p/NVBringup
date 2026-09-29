#include "nv_vbios.h"

#include <string.h>

static uint8_t  rd8(const uint8_t *p)  { return p[0]; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

#define SIG_PCIR 0x52494350u  // "PCIR"
#define SIG_NPDS 0x5344504eu  // "NPDS" (NVIDIA variant of PCIR)
#define SIG_NPDE 0x4544504eu  // "NPDE" (NVIDIA PCI data extension)

static bool rom_signature_ok(uint16_t sig)
{
    return sig == 0xaa55 || sig == 0xbb77 || sig == 0x4e56;
}

const char *nv_image_type_name(uint8_t type)
{
    switch (type) {
    case NV_IMAGE_PCI_AT: return "PCI-AT";
    case NV_IMAGE_EFI:    return "EFI";
    case NV_IMAGE_NBSI:   return "NBSI";
    case NV_IMAGE_FWSEC:  return "FWSEC";
    default:              return "other";
    }
}

// Asks for at least `bytes`, rounded up so callers fetch in useful chunks.
static nv_scan_status need_more(uint32_t bytes, uint32_t *need)
{
    *need = (bytes + 0xfff) & ~0xfffu;
    return NV_SCAN_NEED_MORE;
}

nv_scan_status nv_vbios_scan(const uint8_t *rom, uint32_t len, nv_vbios *v, uint32_t *need)
{
    memset(v, 0, sizeof(*v));
    v->rom = rom;
    v->len = len;
    v->pciat = -1;
    *need = 0;

    uint32_t off = 0;
    for (;;) {
        if (off >= NV_ROM_MAX)
            break;
        if (off + 0x1a > len)
            return need_more(off + 0x1a, need);

        uint16_t sig = rd16(rom + off);
        if (!rom_signature_ok(sig)) {
            if (v->nimages == 0) {
                v->err = "no expansion-ROM signature at offset 0";
                return NV_SCAN_BAD;
            }
            break;  // chain ended without a last-image flag; keep what we have
        }

        uint32_t pcir = off + rd16(rom + off + 0x18);
        if (pcir + 0x18 > len)
            return need_more(pcir + 0x18, need);
        uint32_t psig = rd32(rom + pcir);
        if (psig != SIG_PCIR && psig != SIG_NPDS) {
            v->err = "image without PCIR/NPDS structure";
            return NV_SCAN_BAD;
        }

        nv_rom_image im = {};
        im.offset = off;
        im.vendor = rd16(rom + pcir + 0x04);
        im.device = rd16(rom + pcir + 0x06);
        im.size   = (uint32_t)rd16(rom + pcir + 0x10) * 512;
        im.type   = rd8(rom + pcir + 0x14);
        im.last   = rd8(rom + pcir + 0x15) & 0x80;

        // NVIDIA's NPDE extension, 16-byte aligned after PCIR, overrides size/last.
        uint32_t npde = (pcir + rd16(rom + pcir + 0x0a) + 0x0f) & ~0x0fu;
        if (npde + 0x0b > len)
            return need_more(npde + 0x0b, need);
        if (rd32(rom + npde) == SIG_NPDE) {
            im.has_npde = true;
            im.size = (uint32_t)rd16(rom + npde + 0x08) * 512;
            im.last = rd8(rom + npde + 0x0a) & 0x80;
        }

        if (im.size == 0) {
            v->err = "image with zero length";
            return NV_SCAN_BAD;
        }
        if (v->nimages == NV_ROM_MAX_IMAGES) {
            v->err = "too many images";
            return NV_SCAN_BAD;
        }
        if (off + im.size > NV_ROM_MAX) {
            v->err = "image chain exceeds scan limit";
            return NV_SCAN_BAD;
        }

        if (im.type == NV_IMAGE_PCI_AT && v->pciat < 0)
            v->pciat = (int)v->nimages;
        v->img[v->nimages++] = im;
        off += im.size;

        if (im.last)
            break;
    }

    if (off > len)
        return need_more(off, need);
    v->end = off;

    if (v->pciat >= 0) {
        const nv_rom_image &pa = v->img[v->pciat];
        uint8_t sum = 0;
        for (uint32_t i = 0; i < pa.size; i++)
            sum += rom[pa.offset + i];
        v->pciat_csum_ok = (sum == 0);
    }
    return NV_SCAN_OK;
}

// Pointers inside the BIT/falcon structures are "logical" offsets: they assume
// the PCI-AT image is directly followed by the FWSEC images, skipping any EFI
// or NBSI images in between (see nova-core FwSecBiosBuilder::setup_falcon_data).
static bool read_logical(const nv_vbios *v, uint32_t loff, void *out, uint32_t n, uint32_t *rom_off)
{
    uint8_t *dst = (uint8_t *)out;
    uint32_t base = 0;
    bool first = true;

    for (uint32_t i = 0; i < v->nimages && n; i++) {
        const nv_rom_image &im = v->img[i];
        if ((int)i != v->pciat && im.type != NV_IMAGE_FWSEC)
            continue;
        if (loff < base + im.size) {
            uint32_t in  = loff - base;
            uint32_t cnt = im.size - in < n ? im.size - in : n;
            if (first && rom_off)
                *rom_off = im.offset + in;
            first = false;
            memcpy(dst, v->rom + im.offset + in, cnt);
            dst += cnt; loff += cnt; n -= cnt;
        }
        base += im.size;
    }
    return n == 0;
}

bool nv_vbios_find_fwsec(nv_vbios *v)
{
    if (v->pciat < 0) {
        v->err = "no PCI-AT image";
        return false;
    }
    const nv_rom_image &pa = v->img[v->pciat];
    const uint8_t *img = v->rom + pa.offset;

    static const uint8_t bit_sig[] = { 0xff, 0xb8, 'B', 'I', 'T', 0x00 };
    uint32_t bit = 0;
    bool found = false;
    for (uint32_t i = 0; i + 12 <= pa.size; i++) {
        if (memcmp(img + i, bit_sig, sizeof(bit_sig)) == 0) {
            bit = i;
            found = true;
            break;
        }
    }
    if (!found) {
        v->err = "BIT header not found in PCI-AT image";
        return false;
    }
    v->bit_off     = pa.offset + bit;
    v->bit_version = rd16(img + bit + 6);
    uint8_t hsize  = rd8(img + bit + 8);
    uint8_t tsize  = rd8(img + bit + 9);
    v->bit_ntokens = rd8(img + bit + 10);
    if (tsize < 6) {
        v->err = "BIT token size too small";
        return false;
    }

    found = false;
    for (uint32_t t = 0; t < v->bit_ntokens; t++) {
        uint32_t tok = bit + hsize + t * tsize;
        if (tok + 6 > pa.size)
            break;
        if (rd8(img + tok) != NV_BIT_TOKEN_FALCON_DATA)
            continue;
        uint32_t data = rd16(img + tok + 4);
        if (data + 4 > pa.size) {
            v->err = "falcon data token points outside PCI-AT image";
            return false;
        }
        v->falcon_ptr = rd32(img + data);
        found = true;
        break;
    }
    if (!found) {
        v->err = "no falcon data token (0x70) in BIT";
        return false;
    }

    uint8_t hdr[4];
    if (!read_logical(v, v->falcon_ptr, hdr, 4, nullptr)) {
        v->err = "falcon data pointer outside PCI-AT + FWSEC images";
        return false;
    }
    v->pmu_off      = v->falcon_ptr;
    v->pmu_version  = hdr[0];
    uint8_t lhsize  = hdr[1], esize = hdr[2];
    v->pmu_nentries = hdr[3];
    if (lhsize < 4 || esize < 6 || v->pmu_nentries == 0 || v->pmu_nentries > 64) {
        v->err = "PMU lookup table header looks invalid";
        return false;
    }

    uint32_t dbg = 0, prod = 0;
    for (uint32_t e = 0; e < v->pmu_nentries; e++) {
        uint8_t ent[6];
        if (!read_logical(v, v->pmu_off + lhsize + e * esize, ent, 6, nullptr))
            break;
        if (ent[0] == NV_FALCON_APPID_FWSEC_PROD && !prod)
            prod = rd32(ent + 2);
        if (ent[0] == NV_FALCON_APPID_FWSEC_DBG && !dbg)
            dbg = rd32(ent + 2);
    }
    if (!prod && !dbg) {
        v->err = "no FWSEC entry in PMU lookup table";
        return false;
    }
    v->fwsec_appid  = prod ? NV_FALCON_APPID_FWSEC_PROD : NV_FALCON_APPID_FWSEC_DBG;
    v->desc_logical = prod ? prod : dbg;

    uint8_t desc[8];
    if (!read_logical(v, v->desc_logical, desc, 8, &v->desc_rom)) {
        v->err = "FWSEC descriptor outside PCI-AT + FWSEC images";
        return false;
    }
    v->desc_hdr         = rd32(desc);
    v->desc_version     = (uint8_t)(v->desc_hdr >> 8);
    v->desc_size        = v->desc_hdr >> 16;
    v->desc_stored_size = rd32(desc + 4);
    if (!(v->desc_hdr & 1) || (v->desc_version != 2 && v->desc_version != 3)) {
        v->err = "FWSEC descriptor header not valid (expected v2 or v3)";
        return false;
    }
    return true;
}
