// NVIDIA register offsets and helpers used during bring-up.
// Offsets/decoding follow nouveau (drivers/gpu/drm/nouveau/nvkm/engine/device/base.c).
#pragma once

#include <stdint.h>

// BAR0 (MMIO) registers
#define NV_PMC_BOOT_0           0x00000000  // chip identification
#define NV_PMC_BOOT_42          0x00000a00  // extended id (used by newer drivers; logged only)
#define NV_PBUS_SW_SCRATCH_0E   0x00001438  // FWSEC-FRTS error code in bits 31:16
#define NV_PBUS_VBIOS_SCRATCH_15 0x00001454 // FWSEC-SB error code in bits 15:0
#define NV_PBUS_BAR0_WINDOW     0x00001700  // PRAMIN window: 23:0 base >> 16, 25:24 target (0 VRAM)
#define NV_UFLUSH_FB_FLUSH      0x00070000  // write 1: flush BAR writes to FB; bit 1 busy (nouveau g84_bar_flush)
#define NV_PBUS_BAR1_BLOCK      0x00001704  // 27:0 instance >> 12, 29:28 target (0 VRAM), 31 virtual mode
#define NV_PRAMIN               0x00700000  // 1 MiB window into VRAM at BAR0_WINDOW's base
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_PRIV_LEVEL_MASK 0x00118128  // bit 0: read protection lowered
#define NV_PGC6_AON_SECURE_SCRATCH_GROUP_05_0_GFW_BOOT      0x00118234  // 7:0 = 0xff: GFW boot completed
#define NV_PFB_PRI_MMU_LOCAL_MEMORY_RANGE 0x00100ce0
#define NV_PFB_PRI_MMU_WPR2_ADDR_LO 0x001fa824  // bits 31:4 = address >> 12
#define NV_PFB_PRI_MMU_WPR2_ADDR_HI 0x001fa828  // exclusive bound; 0 = WPR2 not set
#define NV_FUSE_STATUS_OPT_DISPLAY 0x00021c04   // bit 0: display disabled (gm107..tu1xx)
#define NV_FUSE_STATUS_OPT_DISPLAY_GA100 0x00820c04  // the same fuse on GA100 and later (ga100 dev_fuse.h)
#define NV_PDISP_VGA_WORKSPACE_BASE 0x00625f04  // bits 31:8 = addr >> 16, bit 3 = valid
// GA102+: usable FB size in MiB, set by the VBIOS (NV_PGC6_AON_SECURE_SCRATCH_GROUP_42, ga102
// dev_gc6_island_addendum.h NV_USABLE_FB_SIZE_IN_MB; nouveau ga102_fb_vidmem_size). 0 = unset.
#define NV_USABLE_FB_SIZE_IN_MB     0x001183a4
// GA10x+ fuse versions of PKC-signed ucodes, one register per ucode id (ga100 dev_fuse.h); the
// highest set bit + 1 is the version a signature must carry (kgspReadUcodeFuseVersion_GA100).
#define NV_FUSE_OPT_FPF_NVDEC_UCODE1_VERSION 0x00824100
#define NV_FUSE_OPT_FPF_SEC2_UCODE1_VERSION  0x00824140
#define NV_FUSE_OPT_FPF_GSP_UCODE1_VERSION   0x008241c0
#define NV_FUSE_OPT_FPF_UCODE_VERSION_COUNT  16
#define NV_THERM_TSENSOR        0x00020460  // bit 29 valid, 16:3 temperature in 1/32 °C (nouveau gp100_temp_get)

// GSP falcon (nova-core regs.rs, falcon/gsp.rs). Offsets are relative to NV_PGSP.
#define NV_PGSP                     0x00110000
#define NV_FALCON_MAILBOX0          0x040
#define NV_FALCON_MAILBOX1          0x044
#define NV_FALCON_RM                0x084
#define NV_FALCON_IRQMCLR           0x014   // write 1s to mask interrupts
#define NV_FALCON_ITFEN             0x048   // bit 0 ctxen, bit 1 mthden: interface enables
#define NV_FALCON_CPUCTL            0x100   // bit 6 alias_en, bit 4 halted, bit 1 startcpu
#define NV_FALCON_BOOTVEC           0x104
#define NV_FALCON_DMACTL            0x10c   // bit 2 imem scrubbing, bit 1 dmem scrubbing
#define NV_FALCON_CPUCTL_ALIAS      0x130   // bit 1 startcpu
#define NV_FALCON_IMEMC0            0x180   // bit 28 secure, bit 24 auto-increment, 15:0 offset
#define NV_FALCON_IMEMD0            0x184
#define NV_FALCON_IMEMT0            0x188   // 15:0 tag
#define NV_FALCON_DMEMC0            0x1c0   // bit 24 auto-increment, 15:0 offset
#define NV_FALCON_DMEMD0            0x1c4
#define NV_FALCON_ENGINE            0x3c0   // bit 0 reset
#define NV_FALCON_FBIF_TRANSCFG(i)  (0x600 + 4 * (i))  // 2:2 mem type (1 physical), 1:0 target
#define NV_FALCON_FBIF_CTL          0x624   // bit 7 allow_phys_no_ctx

#define NV_FALCON_OS                0x080   // GSP: RISC-V bootloader app version
#define NV_FALCON_HWCFG2            0x0f4   // bit 10: RISC-V core present; GA10x+: bit 12 memory scrubbing, bit 31 reset ready

// Falcon DMA engine (GA10x+ load HS ucodes with it: kgspExecuteHsFalcon_GA102, ga102 dev_falcon_v4.h)
#define NV_FALCON_DMATRFBASE        0x110   // source address >> 8, bits 31:0
#define NV_FALCON_DMATRFMOFFS       0x114   // destination offset in IMEM/DMEM (23:0)
#define NV_FALCON_DMATRFCMD         0x118   // bit 0 queue full, bit 1 idle, 3:2 sec, bit 4 imem, bit 5 write, 10:8 size, 14:12 ctxdma, bit 16 set_dmtag
#define NV_FALCON_DMATRFFBOFFS      0x11c   // source offset (added to DMATRFBASE; IMEM tag = offset >> 8)
#define NV_FALCON_DMATRFBASE1       0x128   // source address >> 40, bits 8:0
#define NV_DMATRFCMD_FULL           (1u << 0)
#define NV_DMATRFCMD_IDLE           (1u << 1)
#define NV_DMATRFCMD_SEC            (1u << 2)
#define NV_DMATRFCMD_IMEM           (1u << 4)
#define NV_DMATRFCMD_SIZE_256B      (6u << 8)
#define NV_HWCFG2_MEM_SCRUBBING     (1u << 12)
#define NV_HWCFG2_RESET_READY       (1u << 31)

// Second register space of a GA10x+ falcon (NV_FALCON2_*: GSP 0x111000, SEC2 0x841000; ga102
// dev_riscv_pri.h, dev_falcon_second_pri.h), 0x1000 past the falcon's own.
#define NV_FALCON2_OFFSET           0x1000
#define NV_PRISCV_RISCV_CPUCTL      0x388   // bit 4 halted, bit 7 RISC-V active (GA10x+)
#define NV_PRISCV_RISCV_CPUCTL_ACTIVE (1u << 7)
#define NV_PRISCV_RISCV_BCR_CTRL    0x668   // bit 0 valid, bit 4 core select (1 RISC-V), bit 8 BR fetch
#define NV_PRISCV_BCR_VALID         (1u << 0)
#define NV_PRISCV_BCR_CORE_RISCV    (1u << 4)
#define NV_PRISCV_BCR_BRFETCH       (1u << 8)
#define NV_PFALCON2_FALCON_MOD_SEL  0x180   // 7:0 algorithm: 1 = RSA3K (PKC boot ROM)
#define NV_PFALCON2_MOD_SEL_RSA3K   1u
#define NV_PFALCON2_FALCON_BROM_CURR_UCODE_ID 0x198  // 7:0 ucode id (fuse version register index)
#define NV_PFALCON2_FALCON_BROM_ENGIDMASK     0x19c
#define NV_PFALCON2_FALCON_BROM_PARAADDR(i)   (0x210 + 4 * (i))  // (0): DMEM offset of the PKC signature

// SEC2 falcon (r570 dev_sec_pri.h): same falcon register layout as the GSP's.
#define NV_PSEC                     0x00840000

// GSP extras (r570 dev_gsp.h, dev_riscv_pri.h)
#define NV_PGSP_QUEUE_HEAD(i)       (0x00110c00 + 8 * (i))  // doorbell: write after a cmdq push
#define NV_PRISCV_GSP_BASE          0x00111000
#define NV_PRISCV_CORE_SWITCH_RISCV_STATUS 0x240            // bit 0: RISC-V active

// Virtual-function register space (turing dev_vm.h, NV_VIRTUAL_FUNCTION_FULL_PHYS_OFFSET 0xb80000)
#define NV_VF_DOORBELL                  0x00bb0090  // 0xb80000 + 0x30090: write a channel's work submit token
#define NV_VF_MMU_INVALIDATE_PDB        0x00b830a0  // 0xb80000 + 0x30a0; 31:4 PDB >> 12, 1:1 aperture (0 VRAM)
#define NV_VF_MMU_INVALIDATE_UPPER_PDB  0x00b830a4
#define NV_VF_MMU_INVALIDATE            0x00b830b0
#define NV_VF_MMU_INVALIDATE_ALL_VA     (1u << 0)
#define NV_VF_MMU_INVALIDATE_HUBTLB_ONLY (1u << 2)  // BAR1/BAR2 translations live in the hub TLB
#define NV_VF_MMU_INVALIDATE_UP_TO_PDE3 (5u << 24)  // CACHE_LEVEL 26:24
#define NV_VF_MMU_INVALIDATE_TRIGGER    (1u << 31)

// Set by SEC2 once it has restarted GSP-RM (sequencer CORE_RESUME, r570 dev_gc6_island.h)
#define NV_PGC6_BSI_SECURE_SCRATCH_14 0x001180f8
#define NV_BSI_SCRATCH_14_BOOT_STAGE_3_HANDOFF (1u << 26)
// Ada: bits 31:29 = 3 once the scrubber ucode has run (kgspExecuteScrubberIfNeeded_AD102; logged only)
#define NV_PGC6_BSI_SECURE_SCRATCH_15 0x001180fc
#define NV_BSI_SCRATCH_15_SCRUBBER_HANDOFF_DONE 3u

#define NV_HWCFG2_RISCV             (1u << 10)
#define NV_IMEMC_SECURE             (1u << 28)
#define NV_CPUCTL_STARTCPU          (1u << 1)
#define NV_CPUCTL_HALTED            (1u << 4)
#define NV_CPUCTL_ALIAS_EN          (1u << 6)
#define NV_MEMC_AINCW               (1u << 24)
#define NV_FBIF_TARGET_COHERENT_SYSMEM 1u
#define NV_FBIF_MEM_TYPE_PHYSICAL   (1u << 2)

// VRAM size from LOCAL_MEMORY_RANGE (as nova-core decodes it): mag << (scale + 20),
// reduced by 1/16 when ECC is enabled.
static inline uint64_t nv_vram_size(uint32_t range)
{
    uint64_t size = (uint64_t)((range >> 4) & 0x3f) << ((range & 0xf) + 20);
    return (range & 0x40000000) ? size / 16 * 15 : size;
}

static inline uint64_t nv_wpr2_addr(uint32_t reg) { return (uint64_t)(reg >> 4) << 12; }

// NV_PMC_BOOT_0 decoding (NV50 and later)
static inline uint32_t nv_boot0_chipset(uint32_t boot0) { return (boot0 & 0x1ff00000) >> 20; }
static inline uint32_t nv_boot0_arch(uint32_t boot0)    { return nv_boot0_chipset(boot0) & 0x1f0; }
static inline uint32_t nv_boot0_rev(uint32_t boot0)     { return boot0 & 0xff; }


// CPU interrupt tree (dev_vm.h, TU102): the VF register space mirrored at BAR0 0xB80000.
// Vector v is leaf register v/32, bit v%32; a subtree is 64 vectors (two leaves), TOP bit = subtree.
#define NV_VF_BASE                     0x00B80000
#define NV_CPU_INTR_LEAF(i)            (NV_VF_BASE + 0x1000 + (i) * 4)   // write 1 to clear
#define NV_CPU_INTR_LEAF_EN_SET(i)     (NV_VF_BASE + 0x1200 + (i) * 4)
#define NV_CPU_INTR_LEAF_EN_CLEAR(i)   (NV_VF_BASE + 0x1400 + (i) * 4)
#define NV_CPU_INTR_TOP(i)             (NV_VF_BASE + 0x1600 + (i) * 4)
#define NV_CPU_INTR_TOP_EN_SET(i)      (NV_VF_BASE + 0x1608 + (i) * 4)
#define NV_CPU_INTR_TOP_EN_CLEAR(i)    (NV_VF_BASE + 0x1610 + (i) * 4)
#define NV_CPU_INTR_LEAF_COUNT         8
// MSI re-arm ("EOI") on Turing: any write to the PCI config mirror at NV_PCFG (0x88000) + NV_XVE_CYA_2
// (0x704); the value doesn't matter (kbifRearmMSI_GM107, used for TU10x/GA10x/AD10x, r570). The
// TOP_EN toggle (intrRetriggerTopLevel) is only for chips without it (GH100 and later).
#define NV_XVE_MSI_REARM               0x00088704

static inline const char *nv_arch_name(uint32_t arch)
{
    switch (arch) {
    case 0x110: return "Maxwell (GM10x)";
    case 0x120: return "Maxwell (GM20x)";
    case 0x130: return "Pascal";
    case 0x140: return "Volta";
    case 0x160: return "Turing";
    case 0x170: return "Ampere";
    case 0x190: return "Ada";
    default:    return "unknown";
    }
}
