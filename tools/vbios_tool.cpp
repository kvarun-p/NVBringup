// Host-side VBIOS analyzer using the same parser as the kext.
//
//   vbios_tool <file.rom> [vram_mib]
//                           analyze a dumped VBIOS (see tools/dump_vbios.py) and
//                           prepare the FWSEC FRTS command (VRAM default 4096 MiB)
//   vbios_tool --selftest   parse a synthetic ROM with a known layout
//   vbios_tool --synthetic <file.rom> [3]   write that synthetic ROM (fuzzing seed); 3: with
//                           the v3 (GA10x+) FWSEC descriptor instead of Turing's v2
//   vbios_tool --gsp <fwdir> [vram_mib] [chip]
//                           parse the r570 GSP firmware under <fwdir> (linux-firmware
//                           nvidia/ layout) for a chip (TU117 default; tu10x = TU102, tu11x =
//                           TU117, ga10x = GA102, ad10x = AD102, or any name from nv_hal.cpp)
//                           and print the WPR2 layout

#include "../src/nv_vbios.h"
#include "../src/nv_fwsec.h"
#include "gen_bootloader.h"
#include "../src/nv_gsp.h"
#include "../src/nv_gsp_rm.h"
#include "../src/nv_vram.h"
#include "../src/nv_mmu.h"
#include "../src/nv_hal.h"
#include <map>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

static void report(const nv_vbios &v)
{
    printf("images (chain ends at 0x%x):\n", v.end);
    for (uint32_t i = 0; i < v.nimages; i++) {
        const nv_rom_image &im = v.img[i];
        printf("  %u: offset 0x%06x size 0x%06x type 0x%02x %-6s %04x:%04x%s%s\n",
               i, im.offset, im.size, im.type, nv_image_type_name(im.type),
               im.vendor, im.device, im.has_npde ? " npde" : "", im.last ? " last" : "");
    }
    if (v.pciat >= 0)
        printf("PCI-AT checksum: %s\n", v.pciat_csum_ok ? "ok" : "BAD");
}

static void hexdump(const uint8_t *p, uint32_t n, uint32_t base)
{
    for (uint32_t i = 0; i < n; i += 16) {
        printf("    %04x:", base + i);
        for (uint32_t j = i; j < i + 16 && j < n; j++)
            printf(" %02x", p[j]);
        printf("\n");
    }
}

static int fwsec_report(const nv_vbios &v, uint64_t vram)
{
    nv_fwsec f;
    if (!nv_fwsec_parse(&v, &f)) {
        printf("FWSEC parse failed: %s\n", f.err);
        return 1;
    }
    printf("FWSEC v%u ucode: ROM 0x%x, 0x%x bytes, uncompressed\n", f.version, f.image_rom, f.stored_size);
    printf("  load plan (image offset -> falcon address, size):\n");
    printf("    IMEM non-secure  0x%05x -> 0x%05x  0x%05x\n", f.nsec_img, f.nsec_imem, f.nsec_size);
    printf("    IMEM secure (HS) 0x%05x -> 0x%05x  0x%05x  (tag 0x%x)\n", f.sec_img, f.sec_imem, f.sec_size, f.sec_va);
    printf("    DMEM             0x%05x -> 0x%05x  0x%05x\n", f.dmem_img, f.dmem_addr, f.dmem_size);
    printf("    boot vector      0x%x\n", f.boot_vector);
    if (f.version == 3)
        printf("  IMEMVirtBase 0x%x, IMEMPhysBase 0x%x (nouveau boots v3 at vector 0 from image offset 0; r570 uses IMEMVirtBase)\n",
               f.imem_virt_base, f.imem_phys_base);
    if (f.version == 3) {
        printf("  PKC (GA10x+): %u signature(s) at ROM 0x%x for fuse versions 0x%x, slot at DMEM 0x%x, engine mask 0x%x, ucode id %u\n",
               f.sig_count, f.sigs_rom, f.sig_versions, f.pkc_data_off, f.engine_id_mask, f.ucode_id);
        for (uint32_t fuse = 0; fuse < 4; fuse++) {
            uint32_t idx;
            uint32_t reg = fuse ? 1u << (fuse - 1) : 0;         // fuse version `fuse` = highest bit fuse-1
            if (nv_fwsec_sig_index(&f, reg, &idx))
                printf("    fuse version %u (register 0x%x): signature %u\n", fuse, reg, idx);
            else
                printf("    fuse version %u (register 0x%x): no signature\n", fuse, reg);
        }
    }
    printf("  interface table at DMEM 0x%x, %u entries; DMEMMAPPER v%u at DMEM 0x%x\n",
           f.appif_off, f.appif_count, f.dmap_version, f.dmap_off);
    printf("  command buffer DMEM 0x%x (0x%x bytes), init_cmd 0x%x, cmd_mask0 0x%x cmd_mask1 0x%x\n",
           f.cmd_in_off, f.cmd_in_size, f.init_cmd, f.cmd_mask0, f.cmd_mask1);

    // Without the display registers, use nova-core's no-display default (top 1 MiB).
    uint64_t frts = nv_frts_addr(nv_vga_workspace(vram, false, 0));
    std::vector<uint8_t> dmem(v.rom + f.image_rom + f.dmem_img, v.rom + f.image_rom + f.dmem_img + f.dmem_size);
    if (!nv_fwsec_patch_frts(&f, dmem.data(), frts, NV_FRTS_SIZE)) {
        printf("FRTS patch failed\n");
        return 1;
    }
    printf("FRTS for %llu MiB VRAM: region 0x%llx..0x%llx (VGA workspace assumed at top 1 MiB)\n",
           (unsigned long long)(vram >> 20), (unsigned long long)frts, (unsigned long long)(frts + NV_FRTS_SIZE));
    printf("  patched DMEMMAPPER init_cmd -> 0x%x; FRTS command at DMEM 0x%x:\n", NV_DMEMMAPPER_CMD_FRTS, f.cmd_in_off);
    hexdump(dmem.data() + f.cmd_in_off, NV_FRTS_CMD_SIZE, f.cmd_in_off);
    if (f.version == 3) {
        printf("GA10x+ load the ucode with the falcon's DMA engine; the generic bootloader isn't used\n");
        return 0;
    }

    nv_genbl bl;
    if (!nv_genbl_parse(nv_gen_bootloader, sizeof(nv_gen_bootloader), &bl)) {
        printf("bootloader: %s\n", bl.err);
        return 1;
    }
    printf("bootloader: 0x%x bytes of code, start tag 0x%x (IMEM 0x%x, boot vector 0x%x), descriptor at DMEM 0x%x\n",
           bl.code_size, bl.start_tag, bl.start_tag << 8, bl.start_tag << 8, bl.dmem_load_off);
    const uint64_t example_dma = 0x80000000ull;
    uint8_t desc[NV_BL_DMEM_DESC_SIZE];
    if (!nv_fwsec_bl_desc(&f, example_dma, desc)) {
        printf("bootloader descriptor: FWSEC layout not usable with the bootloader\n");
        return 1;
    }
    printf("DMA buffer: 0x%x padding + 0x%x ucode; descriptor for an example DMA address 0x%llx:\n",
           nv_fwsec_dma_padding(&f), f.stored_size, (unsigned long long)example_dma);
    hexdump(desc, NV_BL_DMEM_DESC_SIZE, bl.dmem_load_off);
    return 0;
}

static int analyze(const uint8_t *rom, uint32_t len, uint64_t vram)
{
    nv_vbios v;
    uint32_t need;
    nv_scan_status st = nv_vbios_scan(rom, len, &v, &need);
    if (st == NV_SCAN_NEED_MORE) {
        printf("truncated: image chain needs at least 0x%x bytes, file has 0x%x\n", need, len);
        return 1;
    }
    if (st == NV_SCAN_BAD) {
        printf("not a valid ROM: %s\n", v.err);
        return 1;
    }
    report(v);

    if (!nv_vbios_find_fwsec(&v)) {
        printf("FWSEC: not found: %s\n", v.err);
        return 1;
    }
    printf("BIT: offset 0x%x version 0x%04x, %u tokens\n", v.bit_off, v.bit_version, v.bit_ntokens);
    printf("PMU lookup table: logical 0x%x version %u, %u entries\n", v.pmu_off, v.pmu_version, v.pmu_nentries);
    printf("FWSEC (%s): descriptor logical 0x%x = ROM 0x%x, v%u, header 0x%x bytes, payload 0x%x bytes\n",
           v.fwsec_appid == NV_FALCON_APPID_FWSEC_PROD ? "prod" : "debug",
           v.desc_logical, v.desc_rom, v.desc_version, v.desc_size, v.desc_stored_size);
    return fwsec_report(v, vram);
}

// ---- GSP firmware ------------------------------------------------------------

static bool read_file(const char *path, std::vector<uint8_t> &out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return false; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

static bool booter_report(const char *name, const std::vector<uint8_t> &bin)
{
    nv_booter b;
    if (!nv_booter_parse(bin.data(), (uint32_t)bin.size(), &b)) {
        printf("%s: %s\n", name, b.err);
        return false;
    }
    printf("%s: image 0x%x bytes, %u signature(s) of 0x%x bytes, fuse version %u, engine 0x%x ucode %u, patch at image 0x%x (DMEM 0x%x)\n",
           name, b.img_size, b.sig_count, b.sig_size, b.fuse_ver, b.engine_id, b.ucode_id, b.patch_loc, b.patch_loc - b.dmem_img);
    printf("  Turing, SEC2 PIO: IMEM non-secure 0x%05x -> 0x%05x 0x%05x | secure 0x%05x -> 0x%05x 0x%05x | DMEM 0x%05x -> 0 0x%05x | boot 0x%x\n",
           b.nsec_img, b.nsec_imem, b.nsec_size, b.sec_img, b.sec_imem, b.sec_size, b.dmem_img, b.dmem_size, b.boot_vector);
    printf("  GA10x+, SEC2 DMA: HS code 0x%05x 0x%05x -> IMEM 0 (tag and boot 0x%x) | DMEM 0x%05x -> 0 0x%05x | signature at DMEM 0x%x\n",
           b.hs_code_img, b.hs_code_size, b.hs_code_va, b.dmem_img, b.dmem_size, b.hs_sig_dmem);
    for (uint32_t fuse = 0; fuse < 3; fuse++) {
        uint32_t idx, reg = fuse ? 1u << (fuse - 1) : 0;
        if (nv_booter_sig_index(&b, reg, &idx))
            printf("    fuse version %u: signature %u\n", fuse, idx);
        else
            printf("    fuse version %u: no signature\n", fuse);
    }
    return true;
}

static int gsp_report(const char *dir, uint64_t vram, const nv_chip *chip)
{
    std::string d(dir), booter = d + "/" + chip->booter_dir + "/gsp/", gsp = d + "/" + chip->arch->gsp_dir + "/gsp/";
    const char *sig_name = chip->elf_sig;
    std::vector<uint8_t> elf, bl, bload, bunload;
    printf("%s (%s): booters from %s, GSP-RM from %s\n", chip->name, chip->arch->name, chip->booter_dir,
           chip->arch->gsp_dir);
    if (!read_file((gsp + "gsp-570.144.bin").c_str(), elf) ||
        !read_file((gsp + "bootloader-570.144.bin").c_str(), bl) ||
        !read_file((booter + "booter_load-570.144.bin").c_str(), bload) ||
        !read_file((booter + "booter_unload-570.144.bin").c_str(), bunload))
        return 1;

    bool ok = booter_report("booter_load", bload);
    ok = booter_report("booter_unload", bunload) && ok;

    nv_gspbl g;
    if (!nv_gspbl_parse(bl.data(), (uint32_t)bl.size(), &g)) {
        printf("GSP bootloader: %s\n", g.err);
        return 1;
    }
    printf("GSP bootloader: 0x%x bytes, code 0x%x data 0x%x manifest 0x%x, app version 0x%x\n",
           g.size, g.code_off, g.data_off, g.manifest_off, g.app_version);

    uint64_t img_off, img_size, sig_off, sig_size;
    if (!nv_elf64_section(elf.data(), elf.size(), NV_GSP_ELF_IMAGE, &img_off, &img_size) ||
        !nv_elf64_section(elf.data(), elf.size(), sig_name, &sig_off, &sig_size)) {
        printf("GSP ELF: .fwimage or %s not found\n", sig_name);
        return 1;
    }
    printf("GSP ELF: .fwimage 0x%llx bytes at 0x%llx (%llu 4K pages), %s 0x%llx bytes\n",
           (unsigned long long)img_size, (unsigned long long)img_off,
           (unsigned long long)((img_size + 0xfff) >> 12), sig_name, (unsigned long long)sig_size);

    // VGA workspace: this card reports none, so the top 1 MiB (see nv_vga_workspace).
    uint64_t vga = nv_vga_workspace(vram, false, 0);
    nv_wpr2_layout l;
    if (!chip->arch->wpr2_layout(vram, vga, img_size, g.size, 256, &l)) {
        printf("WPR2 layout: inconsistent\n");
        return 1;
    }
    auto row = [](const char *n, uint64_t a, uint64_t s) {
        printf("  %-14s 0x%09llx..0x%09llx  %8.2f MiB\n", n, (unsigned long long)a,
               (unsigned long long)(a + s), s / 1048576.0);
    };
    printf("WPR2 layout for %llu MiB VRAM (GSP heap %llu MiB):\n",
           (unsigned long long)(vram >> 20), (unsigned long long)(chip->arch->gsp_heap_size(vram) >> 20));
    row("VGA workspace", l.vga_addr, l.vga_size);
    row("FRTS", l.frts_addr, l.frts_size);
    row("GSP bootloader", l.boot_addr, l.boot_size);
    row("GSP ELF", l.elf_addr, l.elf_size);
    row("GSP heap", l.heap_addr, l.heap_size);
    row("WPR2 (all)", l.wpr2_addr, l.wpr2_size);
    row("non-WPR heap", l.nonwpr_addr, l.nonwpr_size);
    printf("  gspFwWprEnd    0x%09llx\n", (unsigned long long)l.wpr_end);
    return ok ? 0 : 1;
}

// ---- synthetic ROM -------------------------------------------------------

static void w16(std::vector<uint8_t> &b, uint32_t o, uint16_t x) { b[o] = x; b[o + 1] = x >> 8; }
static void w32(std::vector<uint8_t> &b, uint32_t o, uint32_t x) { w16(b, o, x); w16(b, o + 2, x >> 16); }

// One image: ROM header, PCIR at +0x40, NPDE at +0x60.
static void put_image(std::vector<uint8_t> &b, uint32_t off, uint32_t size, uint8_t type, bool last)
{
    w16(b, off, 0xaa55);
    w16(b, off + 0x18, 0x40);
    uint32_t p = off + 0x40;
    memcpy(&b[p], "PCIR", 4);
    w16(b, p + 0x04, 0x10de);
    w16(b, p + 0x06, 0x1f91);
    w16(b, p + 0x0a, 0x18);
    w16(b, p + 0x10, size / 512);
    b[p + 0x14] = type;
    b[p + 0x15] = 0;                // PCIR says "not last"; NPDE must win
    uint32_t n = off + 0x60;
    memcpy(&b[n], "NPDE", 4);
    w16(b, n + 0x08, size / 512);
    b[n + 0x0a] = last ? 0x80 : 0;
}

// Layout constants shared by build_synthetic() and selftest().
static const uint32_t PA = 0x0000, EFI = 0x1000, FW1 = 0x1800, FW2 = 0x2000, END = 0x3000;
static const uint32_t pmu_logical  = 0x1000 + 0x100;   // in FWSEC#1
static const uint32_t desc_logical = 0x1800 + 0x200;   // in FWSEC#2
static const uint32_t desc_rom     = FW2 + 0x200;

// v3 (GA10x+) variant: the 44-byte descriptor, two PKC signatures (versions 0 and 1), then
// the ucode: IMEM 0x200 (all secure), DMEM 0x400 with the signature slot at 0x200.
static const uint32_t v3_desc_size = NV_FWSEC_DESC_V3_SIZE + 2 * NV_FWSEC_V3_SIG_SIZE;

static std::vector<uint8_t> build_synthetic(uint8_t desc_version = 2)
{
    // Layout: PCI-AT 0x1000 | EFI 0x800 | FWSEC#1 0x800 | FWSEC#2 0x1000
    std::vector<uint8_t> b(0x4000, 0xff);   // trailing 0xff like erased flash

    put_image(b, PA,  0x1000, NV_IMAGE_PCI_AT, false);
    put_image(b, EFI, 0x0800, NV_IMAGE_EFI,    false);
    put_image(b, FW1, 0x0800, NV_IMAGE_FWSEC,  false);
    put_image(b, FW2, 0x1000, NV_IMAGE_FWSEC,  true);

    // Logical space skips EFI: PCI-AT [0,0x1000) FWSEC#1 [0x1000,0x1800) FWSEC#2 [0x1800,0x2800)

    // BIT header at 0x200 with two tokens; falcon data at 0x300
    const uint8_t bit[] = { 0xff, 0xb8, 'B', 'I', 'T', 0x00, 0x00, 0x01, 12, 6, 2, 0 };
    memcpy(&b[0x200], bit, sizeof(bit));
    b[0x20c] = 'B'; w16(b, 0x210, 0x280);                   // unrelated token
    b[0x212] = NV_BIT_TOKEN_FALCON_DATA; b[0x213] = 1; w16(b, 0x214, 4); w16(b, 0x216, 0x300);
    w32(b, 0x300, pmu_logical);

    // PMU lookup table: header {ver, hlen, elen, count}, entries {appid, target, data}
    uint32_t t = FW1 + 0x100;
    b[t] = 1; b[t + 1] = 4; b[t + 2] = 6; b[t + 3] = 2;
    b[t + 4]  = 0x05; b[t + 5]  = 0x07; w32(b, t + 6, 0xdeadbeef);
    b[t + 10] = NV_FALCON_APPID_FWSEC_PROD; b[t + 11] = 0x07; w32(b, t + 12, desc_logical);

    uint32_t dmem;
    if (desc_version == 2) {
        // v2 descriptor. Ucode: IMEM 0x200 (non-secure 0x100, secure 0x100 at 0x100),
        // then DMEM 0x200 at image offset 0x200.
        const uint32_t desc[15] = {
            1 | 2 << 8 | 0x3c << 16,    // hdr
            0x400, 0x400,               // stored, uncompressed
            0,                          // virtual entry
            0x20,                       // interface offset (in DMEM)
            0, 0x200, 0, 0x100, 0x100,  // IMEM phys, load size, virt, sec base, sec size
            0x200, 0, 0x200,            // DMEM offset, phys base, load size
            0x200, 0x200,               // alt IMEM/DMEM load size
        };
        for (uint32_t i = 0; i < 15; i++)
            w32(b, desc_rom + 4 * i, desc[i]);
        dmem = desc_rom + 0x3c + 0x200;
        for (uint32_t i = 0; i < 0x200; i++)
            b[dmem + i] = 0;
    } else {
        const uint32_t desc[9] = {
            1 | 3 << 8 | v3_desc_size << 16,    // hdr: v3, header size covers the signatures
            0x600,                      // stored size: IMEM 0x200 + DMEM 0x400
            0x200,                      // PKC data offset (in DMEM)
            0x20,                       // interface offset (in DMEM)
            0, 0x200, 0,                // IMEM phys, load size, virt
            0, 0x400,                   // DMEM phys base, load size
        };
        for (uint32_t i = 0; i < 9; i++)
            w32(b, desc_rom + 4 * i, desc[i]);
        w16(b, desc_rom + 0x24, 0x400);                 // engine id mask: GSP
        b[desc_rom + 0x26] = 3;                         // ucode id
        b[desc_rom + 0x27] = 2;                         // signature count
        w16(b, desc_rom + 0x28, 0x3);                   // signature versions 0 and 1
        w16(b, desc_rom + 0x2a, 0);
        for (uint32_t s = 0; s < 2; s++)
            for (uint32_t i = 0; i < NV_FWSEC_V3_SIG_SIZE; i++)
                b[desc_rom + NV_FWSEC_DESC_V3_SIZE + s * NV_FWSEC_V3_SIG_SIZE + i] = (uint8_t)(0x11 * (s + 1));
        const uint32_t image = desc_rom + v3_desc_size;
        for (uint32_t i = 0; i < 0x600; i++)
            b[image + i] = (uint8_t)(i < 0x200 ? 0xc0 : 0);     // code, then zeroed DMEM
        dmem = image + 0x200;
    }
    b[dmem + 0x20] = 1; b[dmem + 0x21] = 4; b[dmem + 0x22] = 8; b[dmem + 0x23] = 2;
    w32(b, dmem + 0x24, 0x05); w32(b, dmem + 0x28, 0x80);                   // other app
    w32(b, dmem + 0x2c, NV_APPIF_ID_DMEMMAPPER); w32(b, dmem + 0x30, 0x100);
    w32(b, dmem + 0x100, NV_DMEMMAPPER_SIG);
    w16(b, dmem + 0x104, 3); w16(b, dmem + 0x106, 0x40);
    w32(b, dmem + 0x108, 0x180); w32(b, dmem + 0x10c, 0x40);                // command buffer
    w32(b, dmem + 0x134, 0x44000);

    // Fix the PCI-AT checksum byte.
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 0x1000; i++) sum += b[PA + i];
    b[PA + 0xfff] -= sum;
    return b;
}

static int selftest()
{
    std::vector<uint8_t> b = build_synthetic();
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL: %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

    nv_vbios v;
    uint32_t need;

    CHECK(nv_vbios_scan(b.data(), 0x800, &v, &need) == NV_SCAN_NEED_MORE);
    CHECK(need >= 0x1000 + 0x1a);

    CHECK(nv_vbios_scan(b.data(), (uint32_t)b.size(), &v, &need) == NV_SCAN_OK);
    CHECK(v.nimages == 4);
    CHECK(v.end == END);
    CHECK(v.pciat == 0);
    CHECK(v.pciat_csum_ok);
    CHECK(v.img[3].last && v.img[3].type == NV_IMAGE_FWSEC);

    CHECK(nv_vbios_find_fwsec(&v));
    CHECK(v.bit_off == 0x200);
    CHECK(v.falcon_ptr == pmu_logical);
    CHECK(v.pmu_nentries == 2);
    CHECK(v.fwsec_appid == NV_FALCON_APPID_FWSEC_PROD);
    CHECK(v.desc_logical == desc_logical);
    CHECK(v.desc_rom == desc_rom);
    CHECK(v.desc_version == 2 && v.desc_size == 0x3c && v.desc_stored_size == 0x400);

    nv_fwsec f;
    CHECK(nv_fwsec_parse(&v, &f));
    CHECK(f.image_rom == desc_rom + 0x3c);
    CHECK(f.nsec_img == 0 && f.nsec_size == 0x100 && f.nsec_imem == 0);
    CHECK(f.sec_img == 0x100 && f.sec_size == 0x100 && f.sec_imem == 0x100);
    CHECK(f.dmem_img == 0x200 && f.dmem_size == 0x200 && f.dmem_addr == 0);
    CHECK(f.appif_count == 2 && f.dmap_off == 0x100 && f.dmap_version == 3);
    CHECK(f.cmd_in_off == 0x180 && f.cmd_in_size == 0x40 && f.cmd_mask0 == 0x44000);

    {   // The same ROM with a v3 (GA10x+) descriptor: PKC-signed, DMA-loaded, no bootloader.
        std::vector<uint8_t> b3 = build_synthetic(3);
        nv_vbios v3;
        nv_fwsec f3;
        CHECK(nv_vbios_scan(b3.data(), (uint32_t)b3.size(), &v3, &need) == NV_SCAN_OK);
        CHECK(nv_vbios_find_fwsec(&v3));
        CHECK(v3.desc_version == 3 && v3.desc_size == v3_desc_size && v3.desc_stored_size == 0x600);
        CHECK(nv_fwsec_parse(&v3, &f3));
        CHECK(f3.version == 3 && f3.image_rom == desc_rom + v3_desc_size && f3.sigs_rom == desc_rom + NV_FWSEC_DESC_V3_SIZE);
        CHECK(f3.nsec_size == 0 && f3.sec_img == 0 && f3.sec_size == 0x200 && f3.sec_imem == 0 && f3.sec_va == 0);
        CHECK(f3.dmem_img == 0x200 && f3.dmem_size == 0x400 && f3.dmem_addr == 0 && f3.boot_vector == 0);
        CHECK(f3.pkc_data_off == 0x200 && f3.engine_id_mask == 0x400 && f3.ucode_id == 3);
        CHECK(f3.sig_count == 2 && f3.sig_versions == 0x3);
        CHECK(f3.appif_count == 2 && f3.dmap_off == 0x100 && f3.cmd_in_off == 0x180);
        uint32_t idx = 99;
        CHECK(nv_fwsec_sig_index(&f3, 0, &idx) && idx == 0);                    // nothing burnt: version 0
        CHECK(nv_fwsec_sig_index(&f3, 0x1, &idx) && idx == 1);                  // bit 0 burnt: version 1
        CHECK(!nv_fwsec_sig_index(&f3, 0x2, &idx));                             // version 2: not in the VBIOS
        CHECK(!nv_fwsec_sig_index(&f, 0, &idx));                                // v2 has no signatures
        std::vector<uint8_t> d3(b3.begin() + f3.image_rom + f3.dmem_img, b3.begin() + f3.image_rom + f3.dmem_img + f3.dmem_size);
        CHECK(nv_fwsec_patch_frts(&f3, d3.data(), 0xffe00000ull, NV_FRTS_SIZE));
        nv_fwsec_patch_sig(&f3, b3.data(), d3.data(), 1);
        CHECK(d3[0x200] == 0x22 && d3[0x200 + NV_FWSEC_V3_SIG_SIZE - 1] == 0x22 && d3[0x1ff] == 0);
        CHECK(d3[0x12c] == NV_DMEMMAPPER_CMD_FRTS && d3[0x12d] == 0);             // init_cmd still patched
        uint8_t bld[NV_BL_DMEM_DESC_SIZE];
        CHECK(!nv_fwsec_bl_desc(&f3, 0x80000000ull, bld));                      // the bootloader is v2 only
        b3[desc_rom + 0x27] = 3;                                                // three signatures don't fit the header
        CHECK(nv_vbios_scan(b3.data(), (uint32_t)b3.size(), &v3, &need) == NV_SCAN_OK && nv_vbios_find_fwsec(&v3));
        CHECK(!nv_fwsec_parse(&v3, &f3));
    }

    {   // Booter signature choice (nouveau ga100_flcn_fw_signature): three signatures for
        // versions 5, 4, 3 in that order.
        nv_booter b = {};
        b.sig_count = 3;
        b.fuse_ver = 5;
        uint32_t idx = 99;
        CHECK(nv_booter_sig_index(&b, 0, &idx) && idx == 2);                    // nothing burnt: the last
        CHECK(nv_booter_sig_index(&b, 0x10, &idx) && idx == 0);                 // highest bit 5: version 5
        CHECK(nv_booter_sig_index(&b, 0x0f, &idx) && idx == 1);                 // highest bit 4
        CHECK(nv_booter_sig_index(&b, 0x04, &idx) && idx == 2);                 // highest bit 3
        CHECK(!nv_booter_sig_index(&b, 0x20, &idx));                            // version 6: newer than the file
        CHECK(!nv_booter_sig_index(&b, 0x01, &idx));                            // version 1: older than the file
        b.sig_count = 1;
        b.fuse_ver = 0;
        CHECK(nv_booter_sig_index(&b, 0, &idx) && idx == 0);                    // Turing: one signature, no fuse
    }

    // Placement, following nova-core fb.rs
    const uint64_t G4 = 4ull << 30;
    CHECK(nv_vga_workspace(G4, false, 0) == G4 - 0x100000);                  // no display
    CHECK(nv_vga_workspace(G4, true, 0) == G4 - 0x100000);                   // not valid
    CHECK(nv_vga_workspace(G4, true, (uint32_t)((G4 - 0x20000) >> 16) << 8 | 8) == G4 - 0x20000);
    CHECK(nv_vga_workspace(G4, true, (uint32_t)(0x10000000 >> 16) << 8 | 8) == G4 - 0x20000); // low
    CHECK(nv_frts_addr(G4 - 0x20000) == 0xffee0000ull);
    uint64_t frts = nv_frts_addr(nv_vga_workspace(G4, false, 0));
    CHECK(frts == 0xffe00000ull);
    std::vector<uint8_t> dm(&b[f.image_rom + f.dmem_img], &b[f.image_rom + f.dmem_img] + f.dmem_size);
    CHECK(nv_fwsec_patch_frts(&f, dm.data(), frts, NV_FRTS_SIZE));
    const uint8_t *c = dm.data() + f.cmd_in_off;
    auto r32 = [](const uint8_t *p) { return (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24); };
    CHECK(r32(dm.data() + f.dmap_off + 0x2c) == NV_DMEMMAPPER_CMD_FRTS);
    CHECK(r32(c + 0x00) == 1 && r32(c + 0x04) == 24 && r32(c + 0x14) == 2);
    CHECK(r32(c + 0x18) == 1 && r32(c + 0x1c) == 20);
    CHECK(r32(c + 0x20) == 0xffe00 && r32(c + 0x24) == 0x100 && r32(c + 0x28) == 2);
    CHECK(!nv_fwsec_patch_frts(&f, dm.data(), frts + 0x800, NV_FRTS_SIZE));   // unaligned
    {   // FWSEC-SB: command 0x19, READ_VBIOS descriptor only
        std::vector<uint8_t> sb(dm.size(), 0xee);
        CHECK(nv_fwsec_patch_sb(&f, sb.data()));
        CHECK(r32(sb.data() + f.dmap_off + 0x2c) == NV_DMEMMAPPER_CMD_SB);
        const uint8_t *q = sb.data() + f.cmd_in_off;
        CHECK(r32(q) == 1 && r32(q + 4) == 24 && r32(q + 8) == 0 && r32(q + 0x10) == 0 && r32(q + 0x14) == 2);
        CHECK(q[0x18] == 0xee);                                                  // nothing past 24 bytes
    }
    CHECK(b[f.image_rom + f.dmem_img + f.cmd_in_off] == 0);                   // ROM untouched

    // Real bootloader from linux-firmware, embedded at build time.
    nv_genbl bl;
    CHECK(nv_genbl_parse(nv_gen_bootloader, sizeof(nv_gen_bootloader), &bl));
    CHECK(bl.code_size == 0x200 && bl.start_tag == 0xfd && bl.dmem_load_off == 0);
    CHECK(bl.code == nv_gen_bootloader + 0x30);
    CHECK(!nv_genbl_parse(nv_gen_bootloader, 0x10, &bl));

    uint8_t desc[NV_BL_DMEM_DESC_SIZE];
    CHECK(nv_fwsec_dma_padding(&f) == 0);
    CHECK(nv_fwsec_bl_desc(&f, 0x123456700ull, desc));
    CHECK(r32(desc + 32) == 4);                                                 // ctx_dma
    CHECK(r32(desc + 36) == 0x23456700 && r32(desc + 40) == 0x1);               // code_dma_base
    CHECK(r32(desc + 44) == 0 && r32(desc + 48) == 0x100);                      // non-secure
    CHECK(r32(desc + 52) == 0x100 && r32(desc + 56) == 0x100);                  // secure
    CHECK(r32(desc + 64) == 0x23456900 && r32(desc + 68) == 0x1);               // data_dma_base
    CHECK(r32(desc + 72) == 0x200 && r32(desc + 76) == 0 && r32(desc + 80) == 0);
    CHECK(!nv_fwsec_bl_desc(&f, 0x123456780ull, desc));                         // misaligned

    // GSP heap and WPR2 layout for a 4 GiB TU117 (VGA workspace at top 1 MiB),
    // computed by hand from nouveau tu102_gsp_oneinit.
    CHECK(nv_gsp_heap_size_tu1xx(G4) == 105ull << 20);
    CHECK(nv_gsp_heap_size_tu1xx(1ull << 30) == 105ull << 20);
    nv_wpr2_layout l;
    CHECK(nv_wpr2_layout_tu1xx(G4, G4 - 0x100000, 28528288, 0x1000, 256, &l));
    CHECK(l.frts_addr == 0xffe00000ull && l.boot_addr == 0xffdff000ull);
    CHECK(l.elf_addr == 0xfe2c0000ull && l.heap_addr == 0xf7900000ull && l.heap_size == 0x6900000ull);
    CHECK(l.wpr2_addr == 0xf7800000ull && l.wpr2_size == 0x8700000ull);
    CHECK(l.nonwpr_addr == 0xf7700000ull && l.wpr_end == 0xfff00000ull);
    CHECK(!nv_wpr2_layout_tu1xx(G4, G4, 28528288, 0x1000, 256, &l));        // bad VGA address

    // GA10x/AD10x (LibOS3): 22 MiB OS carve-out, 88..280 MiB; the layout above it is the same.
    CHECK(nv_gsp_heap_size_ga10x(G4) == 127ull << 20);
    CHECK(nv_gsp_heap_size_ga10x(1ull << 30) == 127ull << 20);
    CHECK(nv_gsp_heap_size_ga10x(64ull << 30) == 132ull << 20);
    CHECK(nv_wpr2_layout_ga10x(G4, G4 - 0x100000, 28528288, 0x1000, 256, &l));
    CHECK(l.frts_addr == 0xffe00000ull && l.boot_addr == 0xffdff000ull && l.elf_addr == 0xfe2c0000ull);
    CHECK(l.heap_addr == 0xf6300000ull && l.heap_size == 0x7f00000ull);
    CHECK(l.wpr2_addr == 0xf6200000ull && l.wpr2_size == 0x9d00000ull && l.nonwpr_addr == 0xf6100000ull);

    // Minimal ELF64: null section, ".fwimage" (16 bytes of data), ".shstrtab".
    {
        std::vector<uint8_t> e(0x200, 0);
        memcpy(&e[0], "\x7f" "ELF", 4); e[4] = 2; e[5] = 1;
        const char names[] = "\0.fwimage\0.shstrtab\0";
        memcpy(&e[0x40], names, sizeof(names));                               // shstrtab data
        for (int i = 0; i < 16; i++) e[0x60 + i] = (uint8_t)i;                 // .fwimage data
        auto w64 = [&](uint32_t o, uint64_t x) { w32(e, o, (uint32_t)x); w32(e, o + 4, (uint32_t)(x >> 32)); };
        w64(0x28, 0x100); w16(e, 0x3a, 0x40); w16(e, 0x3c, 3); w16(e, 0x3e, 2);
        w32(e, 0x140, 1);  w64(0x158, 0x60); w64(0x160, 16);                  // [1] .fwimage
        w32(e, 0x180, 10); w64(0x198, 0x40); w64(0x1a0, sizeof(names));       // [2] .shstrtab
        uint64_t off = 0, size = 0;
        CHECK(nv_elf64_section(e.data(), e.size(), ".fwimage", &off, &size) && off == 0x60 && size == 16);
        CHECK(!nv_elf64_section(e.data(), e.size(), ".fwsignature_tu11x", &off, &size));
        CHECK(!nv_elf64_section(e.data(), 0x150, ".fwimage", &off, &size));   // truncated
    }

    // ---- r570 GSP-RM structures and helpers (nv_gsp_rm) ----
    CHECK(nv_libos_id8("LOGINIT") == 0x4c4f47494e4954ull);
    CHECK(nv_libos_id8("RMARGS") == 0x524d41524753ull);
    CHECK(nv_libos_id8("ABCDEFGHIJ") == 0x4142434445464748ull);                 // 8 chars max

    {   // Shared memory: 128 queue pages + 1 PTE page; queues start after the PTE page.
        nv_gsp_shm_layout s;
        nv_gsp_shm_plan(&s);
        CHECK(s.pte_count == 129 && s.pte_size == 0x1000);
        CHECK(s.cmdq_off == 0x1000 && s.msgq_off == 0x41000 && s.total == 0x81000);
        CHECK(s.rx_hdr_off == 0x20 && s.entry_off == 0x1000 && s.msg_count == 63);
        msgqTxHeader tx;
        nv_gsp_cmdq_init(&s, &tx);
        CHECK(tx.size == 0x40000 && tx.msgSize == 0x1000 && tx.msgCount == 63 && tx.flags == 1);

        GSP_ARGUMENTS_CACHED g;
        nv_gsp_rmargs_fill(&g, &s, 0xabc000);
        CHECK(g.messageQueueInitArguments.sharedMemPhysAddr == 0xabc000);
        CHECK(g.messageQueueInitArguments.pageTableEntryCount == 129);
        CHECK(g.messageQueueInitArguments.statQueueOffset == 0x41000 && g.bDmemStack == 1);
    }

    {   // Radix3 for the real .fwimage size: 6965 data pages -> 14 level-2 pages, 1 level-1.
        nv_radix3_sizes r;
        CHECK(nv_radix3_plan(28528288, &r));
        CHECK(r.data_pages == 6965 && r.lvl2_pages == 14 && r.lvl1_pages == 1 && r.table_pages == 16);
        std::vector<uint64_t> tables(r.table_pages * 512), tpa(r.table_pages), dpa(r.data_pages);
        for (uint64_t i = 0; i < r.table_pages; i++) tpa[i] = 0x100000 + i * 0x1000;
        for (uint64_t i = 0; i < r.data_pages; i++)  dpa[i] = 0x10000000 + i * 0x1000;
        nv_radix3_fill(&r, tables.data(), tpa.data(), dpa.data());
        CHECK(tables[0] == tpa[1] && tables[1] == 0);                             // lvl0 -> lvl1
        CHECK(tables[512] == tpa[2] && tables[512 + 13] == tpa[15] && tables[512 + 14] == 0);
        CHECK(tables[1024] == dpa[0] && tables[1024 + 6964] == dpa[6964] && tables[1024 + 6965] == 0);
        CHECK(!nv_radix3_plan(0, &r));
    }

    {   // Messages: build, verify, corrupt.
        std::vector<uint8_t> q(0x3000);
        GspSystemInfo si;
        nv_gsp_pci_info pci = { 0xa1000000, 0x6000000000, 0x6010000000, 1, 0, 0, 0x10de, 0x1f91, 0x17aa, 0x3a47, 0xa1 };
        nv_gsp_sysinfo_fill(&si, &pci);
        CHECK(si.nvDomainBusDeviceFunc == 0x100 && si.PCIDeviceID == 0x1f9110de && si.PCISubDeviceID == 0x3a4717aa);
        CHECK(nv_gsp_msg_build(q.data(), (uint32_t)q.size(), 7, NV_VGPU_MSG_FUNCTION_GSP_SET_SYSTEM_INFO,
                               &si, sizeof(si)) == 1);
        const GSP_MSG_QUEUE_ELEMENT *e = (const GSP_MSG_QUEUE_ELEMENT *)q.data();
        CHECK(e->seqNum == 7 && e->elemCount == 1 && e->rpc.length == 0x20 + 0x3a0);
        CHECK(e->rpc.function == 72 && e->rpc.signature == 0x43505256 && e->rpc.header_version == 0x03000000);
        CHECK(nv_gsp_checksum(q.data(), 0x1000) == 0);
        CHECK(nv_gsp_msg_check(q.data(), 0x1000) == nullptr);
        q[0x100] ^= 1;
        CHECK(nv_gsp_msg_check(q.data(), 0x1000) != nullptr);

        std::vector<uint8_t> big(0x1000, 0x5a);                                    // spans two entries
        CHECK(nv_gsp_msg_build(q.data(), (uint32_t)q.size(), 8, NV_VGPU_MSG_FUNCTION_SET_REGISTRY,
                               big.data(), (uint32_t)big.size()) == 2);
        CHECK(nv_gsp_msg_check(q.data(), 0x2000) == nullptr);
        CHECK(nv_gsp_msg_check(q.data(), 0x1000) != nullptr);                     // truncated
        CHECK(nv_gsp_msg_build(q.data(), 0x1000, 9, 73, big.data(), (uint32_t)big.size()) == 0);
        CHECK(nv_gsp_msg_build(q.data(), (uint32_t)q.size(), 9, 73, big.data(), 0x10000) == 0);
    }

    {   // Queues: push into the command queue, read a wrapped message from the status queue.
        nv_gsp_shm_layout s;
        nv_gsp_shm_plan(&s);
        std::vector<uint8_t> shm(s.total);
        uint8_t *cmdq = shm.data() + s.cmdq_off, *msgq = shm.data() + s.msgq_off;
        msgqTxHeader tx;
        nv_gsp_cmdq_init(&s, &tx);
        memcpy(cmdq, &tx, sizeof(tx));
        memcpy(msgq, &tx, sizeof(tx));                       // stands in for the GSP's header
        CHECK(nv_gsp_msgq_linked((msgqTxHeader *)msgq, 0x40000));
        CHECK(!nv_gsp_msgq_linked((msgqTxHeader *)(shm.data() + 0x10), 0x40000));

        std::vector<uint8_t> m(0x2000), big(0x1000, 0x5a), back(0x10000);
        CHECK(nv_gsp_msg_build(m.data(), (uint32_t)m.size(), 0, 73, big.data(), 0x100) == 1);
        CHECK(nv_gsp_cmdq_push(cmdq, m.data(), 1, 0));
        CHECK(((msgqTxHeader *)cmdq)->writePtr == 1 && cmdq[0x1000 + 0x30] == 0x00 && cmdq[0x1000 + 0x34] == 0x56);

        CHECK(nv_gsp_msg_build(m.data(), (uint32_t)m.size(), 1, 72, big.data(), 0x1000) == 2);
        ((msgqTxHeader *)cmdq)->writePtr = 62;
        CHECK(nv_gsp_cmdq_push(cmdq, m.data(), 2, 10));      // slots 62 and 0
        CHECK(((msgqTxHeader *)cmdq)->writePtr == 1);
        CHECK(memcmp(cmdq + 0x1000 + 62 * 0x1000, m.data(), 0x1000) == 0);
        CHECK(memcmp(cmdq + 0x1000, m.data() + 0x1000, 0x1000) == 0);
        ((msgqTxHeader *)cmdq)->writePtr = 9;
        CHECK(!nv_gsp_cmdq_push(cmdq, m.data(), 2, 10));     // only one free slot left
        CHECK(nv_gsp_cmdq_push(cmdq, m.data(), 1, 11));

        // Same wrapped message placed in the status queue at slot 62.
        memcpy(msgq + 0x1000 + 62 * 0x1000, m.data(), 0x1000);
        memcpy(msgq + 0x1000, m.data() + 0x1000, 0x1000);
        CHECK(nv_gsp_msgq_read(msgq, 62, back.data(), (uint32_t)back.size()) == 2);
        CHECK(memcmp(back.data(), m.data(), 0x2000) == 0 && nv_gsp_msg_check(back.data(), 0x2000) == nullptr);
        CHECK(nv_gsp_msgq_read(msgq, 62, back.data(), 0x1000) == 0);     // no room
        CHECK(nv_gsp_msgq_read(msgq, 5, back.data(), (uint32_t)back.size()) == 0);  // empty slot
        CHECK(nv_gsp_msgq_read(msgq, 63, back.data(), (uint32_t)back.size()) == 0); // out of range
    }

    {   // Registry: header 8 + 3 entries * 16, then names (22 + 20 + 19 bytes).
        uint8_t reg[256];
        uint32_t n = nv_gsp_registry_pack(reg, sizeof(reg), nv_gsp_default_registry, nv_gsp_default_registry_count);
        CHECK(n == 117);
        CHECK(r32(reg) == 117 && r32(reg + 4) == 3);
        CHECK(r32(reg + 8) == 56 && reg[12] == 1 && r32(reg + 16) == 1 && r32(reg + 20) == 4);
        CHECK(memcmp(reg + 56, "RMForcePcieConfigSave", 22) == 0);
        CHECK(r32(reg + 8 + 32) == 56 + 22 + 20 && memcmp(reg + 98, "RMDevidCheckIgnore", 19) == 0);
        CHECK(nv_gsp_registry_pack(reg, 116, nv_gsp_default_registry, nv_gsp_default_registry_count) == 0);
    }

    {   // Sequencer: WRITE, POLL, CORE_RESET, DELAY_US, then malformed buffers.
        const uint32_t s1[] = { 0, 0x110040, 5,  2, 0x110100, 0x10, 0x10, 1000, 0xdead,  5,  3, 7 };
        uint32_t pos = 0;
        nv_seq_op op;
        CHECK(nv_gsp_seq_next(s1, 12, &pos, &op) && op.opcode == 0 && op.nargs == 2 && op.args[1] == 5);
        CHECK(nv_gsp_seq_next(s1, 12, &pos, &op) && op.opcode == 2 && op.nargs == 5 && op.args[4] == 0xdead);
        CHECK(nv_gsp_seq_next(s1, 12, &pos, &op) && op.opcode == 5 && op.nargs == 0);
        CHECK(nv_gsp_seq_next(s1, 12, &pos, &op) && op.opcode == 3 && op.args[0] == 7 && pos == 12);
        CHECK(!nv_gsp_seq_next(s1, 12, &pos, &op));                               // end
        pos = 0; const uint32_t s2[] = { 1, 0x100, 0xff };                          // MODIFY missing val
        CHECK(!nv_gsp_seq_next(s2, 3, &pos, &op));
        pos = 0; const uint32_t s3[] = { 9 };                                       // unknown
        CHECK(!nv_gsp_seq_next(s3, 1, &pos, &op));
        pos = 0; const uint32_t s4[] = { 4, 0x110040, 8 };                          // save index 8
        CHECK(!nv_gsp_seq_next(s4, 3, &pos, &op));
        CHECK(strcmp(nv_gsp_seq_name(8), "CORE_RESUME") == 0);
    }

    {   // WPR meta and LibOS args from the laptop layout above.
        nv_gsp_sysmem sm = { 0x1000, 28528288, 0x2000, 0x1000, 0x10, 0x20, 0x30, 0x3000, 0x1000 };
        CHECK(nv_wpr2_layout_tu1xx(G4, G4 - 0x100000, 28528288, 0x1000, 256, &l));
        GspFwWprMeta m;
        nv_gsp_wpr_meta_fill(&m, &l, &sm);
        CHECK(m.magic == GSP_FW_WPR_META_MAGIC && m.revision == 1 && m.verified == 0);
        CHECK(m.gspFwRsvdStart == 0xf7700000ull && m.nonWprHeapOffset == 0xf7700000ull);
        CHECK(m.gspFwWprStart == 0xf7800000ull && m.gspFwHeapOffset == 0xf7900000ull);
        CHECK(m.gspFwOffset == 0xfe2c0000ull && m.bootBinOffset == 0xffdff000ull);
        CHECK(m.frtsOffset == 0xffe00000ull && m.frtsSize == 0x100000 && m.gspFwWprEnd == 0xfff00000ull);
        CHECK(m.vgaWorkspaceOffset == 0xfff00000ull && m.vgaWorkspaceSize == 0x100000 && m.fbSize == G4);
        CHECK(m.sysmemAddrOfSignature == 0x3000 && m.bootloaderManifestOffset == 0x30);

        std::vector<uint8_t> page(LIBOS_INIT_ARGUMENTS_SIZE);
        LibosMemoryRegionInitArgument *a = (LibosMemoryRegionInitArgument *)page.data();
        const uint64_t logs[NV_GSP_LOG_COUNT] = { 0x10000, 0x20000, 0x30000, 0x40000 };
        CHECK(nv_gsp_libos_args_fill(a, logs, 0x50000, 0x1000) == 5);
        CHECK(a[0].id8 == nv_libos_id8("LOGINIT") && a[0].pa == 0x10000 && a[0].size == 0x10000);
        CHECK(a[0].kind == LIBOS_MEMORY_REGION_CONTIGUOUS && a[0].loc == LIBOS_MEMORY_REGION_LOC_SYSMEM);
        CHECK(a[4].id8 == nv_libos_id8("RMARGS") && a[4].pa == 0x50000 && a[5].id8 == 0);
    }

    {   // VRAM heap over the usable region GSP-RM reported: 0xf80000..0xf406ffff
        static nv_vram_heap h;
        CHECK(nv_vram_init(&h, 0xf80000, 0xf406ffff));
        CHECK(!nv_vram_init(&h, 0, 0xfff) && !nv_vram_init(&h, 0x1800, 0xffff));
        CHECK(nv_vram_init(&h, 0xf80000, 0xf406ffff));
        uint64_t a = nv_vram_alloc(&h, 0x1000, 0x1000);
        uint64_t b = nv_vram_alloc(&h, 0x10, 0x10000);           // rounded to 4 KiB, 64 KiB aligned
        uint64_t c = nv_vram_alloc(&h, 0x200000, 0x200000);      // 2 MiB aligned
        CHECK(a == 0xf80000 && b == 0xf90000 && c == 0x1000000);
        CHECK(nv_vram_used(&h) == 0x202000 && h.count == 3);
        CHECK(nv_vram_alloc(&h, 0x1000, 0x1000) == 0xf81000);    // fills the first gap
        CHECK(nv_vram_free(&h, b) && !nv_vram_free(&h, b));
        CHECK(nv_vram_alloc(&h, 0x2000, 0x1000) == 0xf82000);
        CHECK(nv_vram_alloc(&h, 0x100000000ull, 0x1000) == 0);  // larger than the heap
        CHECK(nv_vram_alloc(&h, 0x1000, 0x1800) == 0);           // align not a power of two
        uint64_t big = nv_vram_alloc(&h, 0xf406ffffull + 1 - 0x1200000, 0x1000);
        CHECK(big == 0x1200000 && nv_vram_alloc(&h, 0x1000, 0x1000) == 0xf84000);
        CHECK(nv_vram_alloc(&h, 0x100000, 0x1000) == 0);          // full except small gaps

        // Fixed ranges (nv_vram_reserve): kept in order, no overlaps, freed like allocations
        CHECK(nv_vram_init(&h, 0x1000000, 0xfffffff));
        CHECK(nv_vram_reserve(&h, 0x2000000, 0x10));              // rounded to 4 KiB
        CHECK(nv_vram_reserve(&h, 0x1800000, 0x100000) && h.count == 2 && h.a[0].addr == 0x1800000);
        CHECK(!nv_vram_reserve(&h, 0x18ff000, 0x2000));            // overlaps the end of one
        CHECK(!nv_vram_reserve(&h, 0x1fff000, 0x2000));            // overlaps the start of another
        CHECK(!nv_vram_reserve(&h, 0x2000800, 0x1000));            // not aligned
        CHECK(!nv_vram_reserve(&h, 0xfff000, 0x1000) && !nv_vram_reserve(&h, 0xfff000, 0x2000));
        CHECK(!nv_vram_reserve(&h, 0xffff000, 0x2000) && !nv_vram_reserve(&h, 0x2000000, 0));
        CHECK(nv_vram_reserve(&h, 0x1900000, 0x700000) && h.count == 3);   // fills the gap exactly
        CHECK(nv_vram_reserve(&h, 0xffff000, 0x1000));             // last page
        CHECK(nv_vram_alloc(&h, 0x800000, 0x1000) == 0x1000000);   // allocations go around them
        CHECK(nv_vram_alloc(&h, 0x1000, 0x1000) == 0x2001000);
        CHECK(nv_vram_free(&h, 0x1900000) && nv_vram_alloc(&h, 0x100000, 0x100000) == 0x1900000);
    }

    {   // Turing page tables (GMMU ver2) over fake VRAM
        CHECK(nv_mmu_index(0x20000000, NV_MMU_PD1) == 1 && nv_mmu_index(0x100000000ull, NV_MMU_PD1) == 8);
        CHECK(nv_mmu_index(0x1ffffffffffffull, NV_MMU_PD3) == 3 && nv_mmu_index(0x1fe00000, NV_MMU_PD0) == 0xff);
        CHECK(nv_mmu_index(0x20005000, NV_MMU_PT) == 5);
        CHECK(nv_mmu_level_bytes(NV_MMU_PD3) == 32 && nv_mmu_level_bytes(NV_MMU_PD0) == 4096 &&
              nv_mmu_level_bytes(NV_MMU_PT) == 4096);
        CHECK(nv_mmu_level_shift(NV_MMU_PD2) == 38 && nv_mmu_level_shift(NV_MMU_PD1) == 29);
        CHECK(nv_mmu_pde_vram(0x1000000) == 0x100002 && nv_mmu_pde_addr(0x100002) == 0x1000000);
        CHECK(nv_mmu_pde_addr(0x100004) == 0);                     // sysmem aperture
        CHECK(nv_mmu_pte_vram(0xf80000, 0) == 0xf8001 && nv_mmu_pte_addr(0xf8001) == 0xf80000);
        CHECK(nv_mmu_pte_vram(0xf406f000, NV_MMU_PTE_READ_ONLY) == 0xf406f41);
        CHECK(nv_mmu_pte_addr(0xf8000) == 0);                      // not valid

        struct Fake {
            std::map<uint64_t, uint64_t> mem;
            uint64_t next = 0x2000000;
            uint32_t allocs = 0;
        } fk;
        nv_mmu_ops ops = {
            &fk,
            [](void *c, uint32_t bytes) -> uint64_t {
                Fake *f = (Fake *)c;
                uint64_t a = f->next;
                f->next += (bytes + 0xfff) & ~0xfffu;
                f->allocs++;
                return a;
            },
            [](void *c, uint64_t a) -> uint64_t {
                auto &m = ((Fake *)c)->mem;
                auto it = m.find(a);
                return it == m.end() ? 0 : it->second;
            },
            [](void *c, uint64_t a, uint64_t v) { ((Fake *)c)->mem[a] = v; },
            nullptr,
            false,
        };
        const uint64_t root = 0x1000000, pd2 = 0x1001000, pd1 = 0x1002000;
        fk.mem[root] = nv_mmu_pde_vram(pd2);                      // pre-linked as in the kext
        fk.mem[pd2] = nv_mmu_pde_vram(pd1);
        CHECK(nv_mmu_map_4k(&ops, root, 0x20000000, 0xf90000, 0x10000, 0));
        CHECK(fk.allocs == 2);                                    // one PD0, one PT
        CHECK(fk.mem[pd1 + 8] == nv_mmu_pde_vram(0x2000000));    // PD1[1] -> PD0
        CHECK(fk.mem[0x2000000 + 8] == nv_mmu_pde_vram(0x2001000) && !fk.mem[0x2000000]);  // small half
        CHECK(fk.mem[0x2001000 + 8 * 15] == nv_mmu_pte_vram(0xf9f000, 0));
        CHECK(nv_mmu_pte_addr(nv_mmu_lookup(&ops, root, 0x20003000)) == 0xf93000);
        CHECK(nv_mmu_lookup(&ops, root, 0x20010000) == 0 && nv_mmu_lookup(&ops, root, 0x100000000ull) == 0);
        CHECK(!nv_mmu_map_4k(&ops, root, 0x2000f000, 0xf90000, 0x1000, 0));     // already mapped
        CHECK(!nv_mmu_map_4k(&ops, root, 0x20010800, 0xf90000, 0x1000, 0));     // unaligned
        CHECK(nv_mmu_map_4k(&ops, root, 0x3fe00000, 0xf90000, 0x2000, 0) && fk.allocs == 3);  // same PD0, new PT
        uint64_t pd0b = 0x3000000;
        fk.mem[pd1 + 8 * 2] = nv_mmu_pde_vram(pd0b);
        fk.mem[pd0b] = nv_mmu_pde_vram(0x3001000);                // big-page PT only
        CHECK(!nv_mmu_map_4k(&ops, root, 0x40000000, 0xf90000, 0x1000, 0));

        // Mapping into existing tables (BAR1): link point, then undo.
        CHECK(nv_mmu_link_entry(&ops, root, 0x20000000) == 0);                 // all levels exist
        CHECK(nv_mmu_link_entry(&ops, root, 0x60000000) == pd1 + 8 * 3);       // PD1[3] empty
        CHECK(nv_mmu_link_entry(&ops, root, 0x8000000000ull) == pd2 + 8 * 2);  // PD2[2] empty
        CHECK(nv_mmu_unmap_4k(&ops, root, 0x2000e000, 0x3000) == 2);            // 0x2000e/f mapped, 0x20010 not
        CHECK(nv_mmu_lookup(&ops, root, 0x2000f000) == 0 && nv_mmu_lookup(&ops, root, 0x2000d000) != 0);
        CHECK(nv_mmu_unmap_4k(&ops, root, 0x60000000, 0x1000) == 0);            // no tables there

        // Phase 5: system memory, kind, walk across 2 MiB, rollback, link callback.
        CHECK(nv_mmu_pte_sys(0x12345000, 0) == 0x123450d);                    // addr, aperture 2, vol, valid
        CHECK(nv_mmu_pte_sys(0xff00000000ull, NV_MMU_PTE_READ_ONLY) == 0xff000004dull);
        CHECK(nv_mmu_pte_addr(nv_mmu_pte_sys(0x1000, 0)) == 0);                // not a VRAM PTE
        std::vector<uint64_t> pages;
        for (uint64_t i = 0; i < 4; i++)
            pages.push_back(0xabc0000000ull + 0x7000 * i);                     // scattered IOVAs
        nv_mmu_target sys = { true, 0, pages.data(), 1, 0, 0xfe };
        uint32_t before = fk.allocs;
        CHECK(nv_mmu_map(&ops, root, 0x201fe000, 0x3000, &sys));             // crosses 0x20200000
        CHECK(fk.allocs == before + 1);                                        // one new PT (same PD0)
        CHECK(nv_mmu_lookup(&ops, root, 0x201fe000) == (nv_mmu_pte_sys(pages[1], 0) | (0xfeull << 56)));
        CHECK(nv_mmu_lookup(&ops, root, 0x20200000) == (nv_mmu_pte_sys(pages[3], 0) | (0xfeull << 56)));
        // Rollback: the last page collides with 0x201fe000, so nothing of 0x201fc000.. stays.
        nv_mmu_target v2 = { false, 0xf90000, nullptr, 0, 0, 0 };
        CHECK(!nv_mmu_map(&ops, root, 0x201fc000, 0x3000, &v2));
        CHECK(nv_mmu_lookup(&ops, root, 0x201fc000) == 0 && nv_mmu_lookup(&ops, root, 0x201fd000) == 0);
        CHECK(nv_mmu_lookup(&ops, root, 0x201fe000) != 0);                   // the old mapping is untouched
        // Unmapping a large, mostly unmapped range skips whole 2 MiB spans.
        CHECK(nv_mmu_unmap_4k(&ops, root, 0x100000000000ull, 1ull << 36) == 0);
        CHECK(nv_mmu_unmap_4k(&ops, root, 0x20000000, 0x40000000) == 14 + 2 + 3);
        // Link callback: new PD1, PD0 and PT; the first link is the one into the existing PD2.
        static std::vector<uint64_t> linked;
        linked.clear();
        ops.linked = [](void *, uint64_t e) { linked.push_back(e); };
        CHECK(nv_mmu_map_4k(&ops, root, 0x8000000000ull, 0xf90000, 0x1000, 0));
        CHECK(linked.size() == 3 && linked[0] == pd2 + 8 * 2);

        // 64 KiB pages in our own VA space (ownPd0).
        ops.linked = nullptr;
        ops.ownPd0 = true;
        const uint64_t V = 0x7000000000ull;
        nv_mmu_target big = { false, 0x5000000, nullptr, 0, 0, 0x7 };
        CHECK(nv_mmu_map(&ops, root, V + 0x10000, 0x28000, &big));          // 2 big pages + 8 small
        const uint64_t K7 = 0x7ull << 56;
        CHECK(nv_mmu_lookup_big(&ops, root, V + 0x10000) == (nv_mmu_pte_vram(0x5000000, 0) | K7));
        CHECK(nv_mmu_lookup_big(&ops, root, V + 0x2f000) == (nv_mmu_pte_vram(0x5010000, 0) | K7));
        CHECK(nv_mmu_lookup_big(&ops, root, V + 0x30000) == 0);
        CHECK(nv_mmu_lookup(&ops, root, V + 0x15000) == (nv_mmu_pte_vram(0x5000000, 0) | K7));
        CHECK(nv_mmu_lookup(&ops, root, V + 0x37000) == (nv_mmu_pte_vram(0x5027000, 0) | K7));
        // VRAM not 64 KiB aligned: small pages only.
        nv_mmu_target odd = { false, 0x5041000, nullptr, 0, 0, 0 };
        CHECK(nv_mmu_map(&ops, root, V + 0x100000, 0x20000, &odd));
        CHECK(nv_mmu_lookup_big(&ops, root, V + 0x100000) == 0);
        CHECK(nv_mmu_pte_addr(nv_mmu_lookup(&ops, root, V + 0x101000)) == 0x5042000);
        // A small page under a big one, or a big page over small ones, is refused and rolled back.
        CHECK(!nv_mmu_map_4k(&ops, root, V + 0x14000, 0x9000000, 0x1000, 0));
        nv_mmu_target over = { false, 0x6000000, nullptr, 0, 0, 0 };
        CHECK(!nv_mmu_map(&ops, root, V + 0x30000, 0x10000, &over));
        CHECK(nv_mmu_pte_addr(nv_mmu_lookup(&ops, root, V + 0x30000)) == 0x5020000);
        // Rollback after a big page was written: V..V+64K is free, V+64K.. clashes.
        nv_mmu_target over2 = { false, 0x6000000, nullptr, 0, 0, 0 };
        CHECK(!nv_mmu_map(&ops, root, V, 0x30000, &over2));
        CHECK(nv_mmu_lookup(&ops, root, V) == 0);
        CHECK(nv_mmu_lookup_big(&ops, root, V + 0x10000) == (nv_mmu_pte_vram(0x5000000, 0) | K7));
        // Partial unmap splits a big page into small ones.
        CHECK(nv_mmu_unmap_4k(&ops, root, V + 0x12000, 0x2000) == 2);
        CHECK(nv_mmu_lookup_big(&ops, root, V + 0x10000) == 0);
        CHECK(nv_mmu_lookup(&ops, root, V + 0x11000) == (nv_mmu_pte_vram(0x5001000, 0) | K7));
        CHECK(nv_mmu_lookup(&ops, root, V + 0x12000) == 0 && nv_mmu_lookup(&ops, root, V + 0x13000) == 0);
        CHECK(nv_mmu_lookup(&ops, root, V + 0x1f000) == (nv_mmu_pte_vram(0x500f000, 0) | K7));
        // Full unmap: 14 split + 16 (big) + 8 small.
        CHECK(nv_mmu_unmap_4k(&ops, root, V + 0x10000, 0x28000) == 38);
        CHECK(nv_mmu_lookup(&ops, root, V + 0x10000) == 0 && nv_mmu_lookup(&ops, root, V + 0x20000) == 0 &&
              nv_mmu_lookup(&ops, root, V + 0x37000) == 0);
        // The slot is free for either page size again.
        CHECK(nv_mmu_map(&ops, root, V + 0x10000, 0x10000, &big) && nv_mmu_lookup_big(&ops, root, V + 0x10000));
        // System memory never uses big pages.
        nv_mmu_target sys0 = { true, 0, pages.data(), 0, 0, 0 };
        CHECK(nv_mmu_map(&ops, root, V + 0x200000, 0x3000, &sys0) && !nv_mmu_lookup_big(&ops, root, V + 0x200000));
    }

    std::vector<uint8_t> junk(0x2000, 0xff);
    CHECK(nv_vbios_scan(junk.data(), (uint32_t)junk.size(), &v, &need) == NV_SCAN_BAD);

    if (!fails) {
        printf("selftest: synthetic ROM\n");
        nv_vbios_scan(b.data(), (uint32_t)b.size(), &v, &need);
        report(v);
        printf("selftest passed\n");
    }
    return fails ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if ((argc == 3 || argc == 4) && strcmp(argv[1], "--synthetic") == 0) {
        std::vector<uint8_t> b = build_synthetic(argc == 4 && strcmp(argv[3], "3") == 0 ? 3 : 2);
        FILE *f = fopen(argv[2], "wb");
        if (!f || fwrite(b.data(), 1, b.size(), f) != b.size()) { perror(argv[2]); return 2; }
        fclose(f);
        return 0;
    }
    if (argc >= 3 && argc <= 5 && strcmp(argv[1], "--gsp") == 0) {
        const char *name = argc < 5 ? "TU117" : !strcmp(argv[4], "tu10x") ? "TU102" : !strcmp(argv[4], "tu11x") ? "TU117"
                         : !strcmp(argv[4], "ga10x") ? "GA102" : !strcmp(argv[4], "ad10x") ? "AD102" : argv[4];
        const nv_chip *chip = nv_chip_find_name(name);
        if (!chip) {
            fprintf(stderr, "unknown chip %s\n", name);
            return 2;
        }
        return gsp_report(argv[2], (argc >= 4 ? strtoull(argv[3], nullptr, 0) : 4096) << 20, chip);
    }
    if (argc != 2 && argc != 3) {
        fprintf(stderr, "usage: %s <vbios.rom> [vram_mib] | --selftest | --synthetic <out.rom>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    // Pad with 0xff like erased flash: a dump can end exactly where the image
    // chain does, and the scanner needs to see the (missing) next header.
    std::vector<uint8_t> rom(NV_ROM_MAX, 0xff);
    size_t len = fread(rom.data(), 1, rom.size(), f);
    fclose(f);
    printf("%s: 0x%zx bytes\n", argv[1], len);
    uint64_t vram = (argc == 3 ? strtoull(argv[2], nullptr, 0) : 4096) << 20;
    return analyze(rom.data(), (uint32_t)rom.size(), vram);
}
