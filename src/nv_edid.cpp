// EDID / CTA-861 parsing and HDMI InfoFrames for the HDMI output (nv_edid.h).

#include "nv_edid.h"

#include <string.h>

// CTA-861 progressive video formats with their timings (r570 nvt_edidext_861.c EIA861B; refresh in Hz, the
// 1000/1001 variant of each matches too). aspect: 1 4:3, 2 16:9, 0 other (256:135).
struct CtaFormat {
    uint8_t  vic, aspect;
    uint16_t hact, hfp, hsw, htotal;
    uint16_t vact, vfp, vsw, vtotal;
    uint16_t hz;
    bool     nh, nv;
};

static const CtaFormat kCta[] = {
    {   1, 1,  640,   16,  96,  800,  480, 10,  2,  525,  60, true,  true  },
    {   2, 1,  720,   16,  62,  858,  480,  9,  6,  525,  60, true,  true  },
    {   3, 2,  720,   16,  62,  858,  480,  9,  6,  525,  60, true,  true  },
    {   4, 2, 1280,  110,  40, 1650,  720,  5,  5,  750,  60, false, false },
    {  16, 2, 1920,   88,  44, 2200, 1080,  4,  5, 1125,  60, false, false },
    {  17, 1,  720,   12,  64,  864,  576,  5,  5,  625,  50, true,  true  },
    {  18, 2,  720,   12,  64,  864,  576,  5,  5,  625,  50, true,  true  },
    {  19, 2, 1280,  440,  40, 1980,  720,  5,  5,  750,  50, false, false },
    {  31, 2, 1920,  528,  44, 2640, 1080,  4,  5, 1125,  50, false, false },
    {  32, 2, 1920,  638,  44, 2750, 1080,  4,  5, 1125,  24, false, false },
    {  33, 2, 1920,  528,  44, 2640, 1080,  4,  5, 1125,  25, false, false },
    {  34, 2, 1920,   88,  44, 2200, 1080,  4,  5, 1125,  30, false, false },
    {  60, 2, 1280, 1760,  40, 3300,  720,  5,  5,  750,  24, false, false },
    {  61, 2, 1280, 2420,  40, 3960,  720,  5,  5,  750,  25, false, false },
    {  62, 2, 1280, 1760,  40, 3300,  720,  5,  5,  750,  30, false, false },
    {  63, 2, 1920,   88,  44, 2200, 1080,  4,  5, 1125, 120, false, false },
    {  64, 2, 1920,  528,  44, 2640, 1080,  4,  5, 1125, 100, false, false },
    {  93, 2, 3840, 1276,  88, 5500, 2160,  8, 10, 2250,  24, false, false },
    {  94, 2, 3840, 1056,  88, 5280, 2160,  8, 10, 2250,  25, false, false },
    {  95, 2, 3840,  176,  88, 4400, 2160,  8, 10, 2250,  30, false, false },
    {  96, 2, 3840, 1056,  88, 5280, 2160,  8, 10, 2250,  50, false, false },
    {  97, 2, 3840,  176,  88, 4400, 2160,  8, 10, 2250,  60, false, false },
    {  98, 0, 4096, 1020,  88, 5500, 2160,  8, 10, 2250,  24, false, false },
    {  99, 0, 4096,  968,  88, 5280, 2160,  8, 10, 2250,  25, false, false },
    { 100, 0, 4096,   88,  88, 4400, 2160,  8, 10, 2250,  30, false, false },
    { 101, 0, 4096,  968,  88, 5280, 2160,  8, 10, 2250,  50, false, false },
    { 102, 0, 4096,   88,  88, 4400, 2160,  8, 10, 2250,  60, false, false },
};

static const CtaFormat *ctaFind(uint8_t vic)
{
    for (const CtaFormat &f : kCta)
        if (f.vic == vic)
            return &f;
    return nullptr;
}

// An 18-byte detailed timing descriptor; false for a display descriptor (pixel clock 0).
static bool parseDtd(const uint8_t *t, nv_edid_timing *o)
{
    const uint32_t pclk = (uint32_t)(t[0] | t[1] << 8) * 10;
    if (!pclk)
        return false;
    memset(o, 0, sizeof(*o));
    o->pclkKhz = pclk;
    o->hact = t[2] | (uint32_t)(t[4] & 0xf0) << 4;
    o->htotal = o->hact + (t[3] | (uint32_t)(t[4] & 0x0f) << 8);
    o->vact = t[5] | (uint32_t)(t[7] & 0xf0) << 4;
    o->vtotal = o->vact + (t[6] | (uint32_t)(t[7] & 0x0f) << 8);
    o->hfp = t[8] | (uint32_t)(t[11] & 0xc0) << 2;
    o->hsw = t[9] | (uint32_t)(t[11] & 0x30) << 4;
    o->vfp = (t[10] >> 4) | (uint32_t)(t[11] & 0x0c) << 2;
    o->vsw = (t[10] & 0x0f) | (uint32_t)(t[11] & 0x03) << 4;
    o->hmm = (uint16_t)(t[12] | (t[14] & 0xf0) << 4);
    o->vmm = (uint16_t)(t[13] | (t[14] & 0x0f) << 8);
    o->interlaced = (t[17] & 0x80) != 0;
    // Digital separate sync (bits 4:3 = 3): bit 2 vsync +, bit 1 hsync +. Anything else: negative, as before.
    o->nhsync = !(t[17] & 0x02);
    o->nvsync = !(t[17] & 0x04);
    return true;
}

static bool ctaBlock(const uint8_t *e, uint32_t len, uint32_t b)
{
    return (b + 1) * 128 <= len && e[b * 128] == 0x02;
}

bool nv_edid_parse(const uint8_t *e, uint32_t len, nv_sink_caps *c)
{
    memset(c, 0, sizeof(*c));
    static const uint8_t hdr[8] = { 0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00 };
    if (!e || len < 128)
        return false;
    c->valid = true;                                // a bad header is reported, not refused: GSP-RM read it as an EDID
    c->headerOk = !memcmp(e, hdr, 8);
    c->nExt = e[126];
    uint32_t want = 128u * (1u + c->nExt);
    if (want > kNvEdidMax)
        want = kNvEdidMax;
    c->len = want < len ? want : len - len % 128;
    for (uint32_t d = 54; d + 18 <= 126; d += 18) {
        const uint8_t *t = e + d;
        if (t[0] || t[1] || t[2])
            continue;
        if (t[3] == 0xfc) {                         // monitor name, newline-terminated
            uint32_t i = 0;
            for (; i < 13 && t[5 + i] != 0x0a; i++)
                c->name[i] = (char)t[5 + i];
            c->name[i] = 0;
        } else if (t[3] == 0xfd) {                  // range limits: byte 9 is the max pixel clock / 10 MHz
            c->rangeMaxPclkKhz = (uint32_t)t[9] * 10000;
        }
    }
    for (uint32_t b = 1; b * 128 < c->len; b++) {
        if (!ctaBlock(e, c->len, b))
            continue;
        const uint8_t *x = e + b * 128;
        if (!c->cta) {
            c->cta = true;
            c->ctaRev = x[1];
        }
        if (x[1] >= 2) {
            c->underscan |= (x[3] & 0x80) != 0;
            c->basicAudio |= (x[3] & 0x40) != 0;
            c->ycc444 |= (x[3] & 0x20) != 0;
            c->ycc422 |= (x[3] & 0x10) != 0;
        }
        // Data blocks from byte 4 up to the first detailed timing (byte 2); rev 1 has none.
        const uint32_t end = x[2] >= 4 && x[2] <= 127 ? x[2] : 4;
        for (uint32_t o = 4; o < end;) {
            const uint32_t tag = x[o] >> 5, n = x[o] & 31;
            const uint8_t *p = x + o + 1;
            if (o + 1 + n > end)
                break;                              // a block running past the end: stop, keep what was read
            switch (tag) {
            case 1:                                 // audio: 3-byte SADs
                c->nSad = (uint8_t)(c->nSad + n / 3);
                break;
            case 2:                                 // video: SVDs
                for (uint32_t i = 0; i < n && c->nVic < sizeof(c->vics); i++) {
                    uint8_t v = p[i];
                    // VICs 1..64 carry the native flag in bit 7; 129..192 are native 1..64; 193.. are plain.
                    const bool native = v >= 129 && v <= 192;
                    if (native)
                        v = (uint8_t)(v & 0x7f);
                    if (!v || v == 128)
                        continue;
                    if (native && !c->nativeVic)
                        c->nativeVic = v;
                    c->vics[c->nVic++] = v;
                }
                break;
            case 3:                                 // vendor-specific
                if (n >= 5 && p[0] == 0x03 && p[1] == 0x0c && p[2] == 0x00) {
                    c->hdmi = true;
                    c->physAddr = (uint16_t)(p[3] << 8 | p[4]);
                    if (n >= 7)
                        c->hdmiMaxTmdsKhz = (uint32_t)p[6] * 5000;
                } else if (n >= 7 && p[0] == 0xd8 && p[1] == 0x5d && p[2] == 0xc4) {
                    c->hf = true;
                    c->hfMaxTmdsKhz = (uint32_t)p[4] * 5000;
                    c->scdc = (p[5] & 0x80) != 0;
                    c->lte340 = (p[5] & 0x08) != 0;     // NVT_HDMI_FORUM_VSDB_PAYLOAD: Lte_340mcsc_Scramble
                }
                break;
            case 7:                                 // extended tag
                if (n >= 2 && p[0] == 0) {          // video capability
                    c->vcdb = true;
                    c->qy = (p[1] & 0x80) != 0;
                    c->qs = (p[1] & 0x40) != 0;
                    c->sPT = (p[1] >> 4) & 3;
                    c->sIT = (p[1] >> 2) & 3;
                    c->sCE = p[1] & 3;
                }
                break;
            }
            o += 1 + n;
        }
    }
    return true;
}

uint32_t nv_sink_max_tmds_khz(const nv_sink_caps *c)
{
    if (c->hf && c->hfMaxTmdsKhz > 340000)
        return c->hfMaxTmdsKhz;
    if (c->hdmiMaxTmdsKhz)
        return c->hdmiMaxTmdsKhz;
    if (c->rangeMaxPclkKhz)
        return c->rangeMaxPclkKhz;
    return 165000;
}

static void fromCta(const CtaFormat &f, nv_edid_timing *o)
{
    memset(o, 0, sizeof(*o));
    o->hact = f.hact;
    o->hfp = f.hfp;
    o->hsw = f.hsw;
    o->htotal = f.htotal;
    o->vact = f.vact;
    o->vfp = f.vfp;
    o->vsw = f.vsw;
    o->vtotal = f.vtotal;
    o->pclkKhz = (uint32_t)((uint64_t)f.htotal * f.vtotal * f.hz / 1000);
    o->nhsync = f.nh;
    o->nvsync = f.nv;
    o->vic = f.vic;
}

uint32_t nv_edid_timings(const uint8_t *e, uint32_t len, const nv_sink_caps *c, nv_edid_timing *out, uint32_t max)
{
    uint32_t n = 0;
    auto add = [&](const nv_edid_timing &t) {
        for (uint32_t i = 0; i < n; i++)
            if (out[i].hact == t.hact && out[i].vact == t.vact && out[i].htotal == t.htotal &&
                out[i].vtotal == t.vtotal && out[i].pclkKhz == t.pclkKhz && out[i].interlaced == t.interlaced)
                return;
        if (n < max)
            out[n++] = t;
    };
    if (!c->valid || len < c->len)
        return 0;
    nv_edid_timing t;
    for (uint32_t d = 54; d + 18 <= 126; d += 18)
        if (parseDtd(e + d, &t)) {
            t.source = NV_EDID_SRC_DTD;
            add(t);
        }
    for (uint32_t b = 1; b * 128 < c->len; b++) {
        if (!ctaBlock(e, c->len, b))
            continue;
        const uint8_t *x = e + b * 128;
        if (x[2] < 4 || x[2] > 127)
            continue;
        for (uint32_t o = x[2]; o + 18 <= 127; o += 18)
            if (parseDtd(x + o, &t)) {
                t.source = NV_EDID_SRC_EXT_DTD;
                add(t);
            } else {
                break;
            }
    }
    for (int pass = 0; pass < 2; pass++)            // the native SVD first, then the rest in order
        for (uint32_t i = 0; i < c->nVic; i++) {
            if ((pass == 0) != (c->vics[i] == c->nativeVic))
                continue;
            if (const CtaFormat *f = ctaFind(c->vics[i])) {
                fromCta(*f, &t);
                t.source = NV_EDID_SRC_SVD;
                add(t);
            }
        }
    fromCta(kCta[0], &t);
    t.source = NV_EDID_SRC_SAFE;
    add(t);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t aspect;
        out[i].vic = out[i].interlaced ? 0 : nv_cta_vic(&out[i], c, &aspect);
    }
    return n;
}

uint8_t nv_cta_vic(const nv_edid_timing *t, const nv_sink_caps *c, uint8_t *aspect)
{
    *aspect = 0;
    if (!t->htotal || !t->vtotal || t->interlaced)
        return 0;
    const uint64_t mhz = (uint64_t)t->pclkKhz * 1000000 / ((uint64_t)t->htotal * t->vtotal);   // refresh, mHz
    const CtaFormat *hit[2] = {};
    uint32_t nHit = 0;
    for (const CtaFormat &f : kCta) {
        if (f.hact != t->hact || f.htotal != t->htotal || f.vact != t->vact || f.vtotal != t->vtotal ||
            f.hfp != t->hfp || f.hsw != t->hsw || f.vfp != t->vfp || f.vsw != t->vsw)
            continue;
        const uint64_t a = (uint64_t)f.hz * 1000, b = a * 1000 / 1001, tol = a / 200;   // 0.5 %
        const bool ok = (mhz + tol >= a && mhz <= a + tol) || (mhz + tol >= b && mhz <= b + tol);
        if (ok && nHit < 2)
            hit[nHit++] = &f;
    }
    if (!nHit)
        return 0;
    const CtaFormat *f = hit[0];
    if (nHit == 2) {                                // a 4:3 / 16:9 pair (VIC 2/3, 17/18)
        bool in[2] = {};
        for (uint32_t i = 0; c && i < c->nVic; i++)
            for (uint32_t k = 0; k < 2; k++)
                in[k] |= c->vics[i] == hit[k]->vic;
        if (in[0] != in[1])
            f = in[0] ? hit[0] : hit[1];
        else if (t->hmm && t->vmm)                  // the image size: wider than 14:9 is 16:9
            f = (uint32_t)t->hmm * 9 > (uint32_t)t->vmm * 14 ? (hit[0]->aspect == 2 ? hit[0] : hit[1])
                                                              : (hit[0]->aspect == 1 ? hit[0] : hit[1]);
        else
            f = hit[0]->aspect == 2 ? hit[0] : hit[1];
    }
    *aspect = f->aspect;
    return f->vic;
}

uint8_t nv_hdmi_vic(uint8_t vic)
{
    switch (vic) {
    case 95: return 1;      // 3840x2160p30
    case 94: return 2;      // 3840x2160p25
    case 93: return 3;      // 3840x2160p24
    case 98: return 4;      // 4096x2160p24
    default: return 0;
    }
}

static void checksum(uint8_t *pkt, uint32_t bytes)
{
    uint8_t sum = 0;
    pkt[3] = 0;
    for (uint32_t i = 0; i < bytes; i++)
        sum = (uint8_t)(sum + pkt[i]);
    pkt[3] = (uint8_t)(0x100 - sum);
}

uint32_t nv_avi_infoframe(uint8_t out[17], uint8_t vic, uint8_t aspect, uint8_t q)
{
    memset(out, 0, 17);
    out[0] = 0x82;                                  // AVI
    out[1] = vic >= 128 ? 3 : 2;                    // version
    out[2] = 13;                                    // length
    out[4] = 0u << 5 | 1u << 4 | 0u << 2 | 2u;      // PB1: RGB, active format present, no bars, underscanned
    out[5] = (uint8_t)(0u << 6 | (aspect & 3u) << 4 | 8u);   // PB2: no colorimetry, picture aspect, AFD = picture
    out[6] = (uint8_t)(1u << 7 | (q & 3u) << 2);    // PB3: IT content, quantization range, no scaling
    out[7] = vic >= 128 ? vic : (uint8_t)(vic & 0x7f);       // PB4: VIC
    out[8] = 0;                                     // PB5: content type graphics, no pixel repetition
    checksum(out, 17);
    return 17;
}

uint32_t nv_spd_infoframe(uint8_t out[31], const char *vendor, const char *product)
{
    memset(out, 0, 31);
    out[0] = 0x83;                                  // source product description
    out[1] = 1;
    out[2] = 25;
    for (uint32_t i = 0; i < 8 && vendor && vendor[i]; i++)
        out[4 + i] = (uint8_t)(vendor[i] & 0x7f);
    for (uint32_t i = 0; i < 16 && product && product[i]; i++)
        out[12 + i] = (uint8_t)(product[i] & 0x7f);
    out[28] = 0x09;                                 // PB25: PC general
    checksum(out, 29);
    return 31;
}

uint32_t nv_hdmi_vsi_infoframe(uint8_t out[31], uint8_t hdmiVic)
{
    memset(out, 0, 31);
    out[0] = 0x81;                                  // vendor-specific
    out[1] = 1;
    out[2] = 5;
    out[4] = 0x03;                                  // PB1..3: IEEE OUI 00-0C-03, LSB first
    out[5] = 0x0c;
    out[6] = 0x00;
    out[7] = 1u << 5;                               // PB4: HDMI_Video_Format = extended resolution
    out[8] = hdmiVic;                               // PB5: HDMI_VIC
    checksum(out, 9);
    return 31;
}
