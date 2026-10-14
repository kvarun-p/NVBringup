// EDID and CTA-861 for the HDMI output: the sink's capabilities from the base block and its CTA extension blocks,
// the timings the sink offers (in preference order), the CTA-861 video format (VIC) of a timing, and the HDMI
// InfoFrames (AVI, SPD, HDMI vendor-specific) the display engine's SF registers send.
//
// References (MIT): r570 src/common/modeset/timing/nvt_edidext_861.c (the EIA861B VIC table,
// NvTiming_ConstructVideoInfoframe), src/nvidia-modeset/src/nvkms-hdmi.c (GetHDMISupportCap,
// CalculateVideoInfoFrameColorFormat). Plain C++, no kernel headers, so it builds in user space for tests too.
#pragma once

#include <stdint.h>

static const uint32_t kNvEdidMax = 1024;            // 8 blocks: the base block and 7 extensions

// One timing. Porches and sync widths in pixels / lines; pclk in kHz.
struct nv_edid_timing {
    uint32_t pclkKhz;
    uint32_t hact, hfp, hsw, htotal;
    uint32_t vact, vfp, vsw, vtotal;
    bool     nhsync, nvsync, interlaced;
    uint16_t hmm, vmm;                              // image size in mm (detailed timings), 0 unknown
    uint8_t  vic;                                   // CTA-861 video format, 0 none (IT/VESA timing)
    uint8_t  source;                                // nv_edid_src
};

enum nv_edid_src : uint8_t {
    NV_EDID_SRC_DTD = 0,                            // a detailed timing descriptor of the base block (the first is preferred)
    NV_EDID_SRC_EXT_DTD = 1,                        // a detailed timing descriptor of a CTA extension
    NV_EDID_SRC_SVD = 2,                            // a short video descriptor (VIC) of a CTA extension
    NV_EDID_SRC_SAFE = 3,                           // VIC 1, 640x480p60, which every HDMI/DVI sink must take
};

struct nv_sink_caps {
    bool     valid;                                 // a base block with a good header
    uint32_t len;                                   // bytes that belong to the EDID: 128 * (1 + extensions), capped
    uint8_t  nExt;
    char     name[14];                              // the monitor name descriptor
    uint32_t rangeMaxPclkKhz;                       // display range limits descriptor: max pixel clock, 0 none
    // CTA-861 extension
    bool     cta;
    uint8_t  ctaRev;
    bool     underscan, basicAudio, ycc444, ycc422; // byte 3 of the CTA block
    bool     hdmi;                                  // HDMI 1.x VSDB (IEEE 00-0C-03): the sink takes HDMI
    uint32_t hdmiMaxTmdsKhz;                        // its Max_TMDS_Clock, 0 not given
    uint16_t physAddr;                              // its CEC physical address
    bool     hf;                                    // HDMI Forum VSDB (C4-5D-D8): an HDMI 2.x sink
    uint32_t hfMaxTmdsKhz;                          // its Max_TMDS_Character_Rate, 0 not above 340 MHz
    bool     scdc;
    bool     vcdb;                                  // video capability data block
    bool     qs, qy;                                // RGB / YCC quantization range selectable through the AVI InfoFrame
    uint8_t  sPT, sIT, sCE;                         // overscan behaviour (0 no data, 1 overscans, 2 underscans, 3 both)
    uint8_t  nVic;
    uint8_t  vics[48];                              // the SVDs in EDID order, without the native bit
    uint8_t  nativeVic;                             // the first SVD marked native, 0 none
    uint8_t  nSad;                                  // short audio descriptors
};

bool nv_edid_parse(const uint8_t *e, uint32_t len, nv_sink_caps *c);

// The highest TMDS clock the sink says it takes (kHz): the HDMI Forum VSDB, then the HDMI VSDB, then the range
// limits; with none of them 165 MHz (HDMI 1.x and single-link DVI without a Max_TMDS_Clock).
uint32_t nv_sink_max_tmds_khz(const nv_sink_caps *c);

// The sink's timings in preference order: base-block detailed timings (the first is the preferred one), CTA
// detailed timings, CTA short video descriptors with a known timing (the native one first), then 640x480p60.
// Interlaced ones are returned too (marked); the caller filters. Returns the count written.
uint32_t nv_edid_timings(const uint8_t *e, uint32_t len, const nv_sink_caps *c, nv_edid_timing *out, uint32_t max);

// The CTA-861 video format of a timing, 0 if it is none (an IT/VESA timing). aspect: 1 4:3, 2 16:9, 0 other.
uint8_t nv_cta_vic(const nv_edid_timing *t, const nv_sink_caps *c, uint8_t *aspect);
// The HDMI 1.4 HDMI_VIC (vendor-specific InfoFrame) for the 4K VICs 93/94/95/98, 0 for others.
uint8_t nv_hdmi_vic(uint8_t vic);

// InfoFrames as the SF registers take them: HB0..HB2, PB0 (checksum) .. PBn. Return the bytes used.
//   AVI (17 bytes): RGB 4:4:4, IT content (graphics), underscan, quantization q (0 default, 1 limited, 2 full).
uint32_t nv_avi_infoframe(uint8_t out[17], uint8_t vic, uint8_t aspect, uint8_t q);
//   SPD (31 bytes): vendor (<= 8 chars), product (<= 16), source "PC general".
uint32_t nv_spd_infoframe(uint8_t out[31], const char *vendor, const char *product);
//   HDMI vendor-specific (31 bytes): HDMI_VIC for the HDMI 1.4 4K formats.
uint32_t nv_hdmi_vsi_infoframe(uint8_t out[31], uint8_t hdmiVic);
