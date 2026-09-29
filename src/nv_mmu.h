// Turing GPU page tables (GMMU "ver2", NVIDIA pascal/gp100/dev_mmu.h; TU10x uses the
// same format). Pure C++, shared by the kext and the host tool.
//
// 49-bit VA, 5 levels: PD3 (root) 48:47, PD2 46:38, PD1 37:29, PD0 28:21 with 16-byte
// dual entries (big-page PT in the low qword, small-page PT in the high qword), then
// the PT: 20:12 for 4 KiB pages, or 20:16 for 64 KiB ("big") pages. A 2 MiB PD0 entry can
// point at both a big-page PT and a small-page PT; for each 64 KiB the GPU uses the big PTE
// if it is valid and falls back to the small PT otherwise (VA spaces are created with a
// 64 KiB big page size). We never map one 64 KiB both ways.
//
// The builder only knows VRAM addresses; the caller supplies allocation and 64-bit
// access (the kext goes through PRAMIN, the self-test through a byte array).
#pragma once

#include <stdint.h>

enum { NV_MMU_PD3, NV_MMU_PD2, NV_MMU_PD1, NV_MMU_PD0, NV_MMU_PT, NV_MMU_LEVELS };

// Lowest VA bit a level's entries decode (GMMU "virtAddrBitLo"): 47, 38, 29, 21, 12.
uint32_t nv_mmu_level_shift(int level);
// Bytes of one level instance: 32 for PD3, 4096 for the others (4 KiB-page PT).
uint32_t nv_mmu_level_bytes(int level);
uint32_t nv_mmu_index(uint64_t va, int level);

// Aperture values (dev_mmu.h): PDEs use 1 = VRAM, PTEs 0 = VRAM, 2 = system memory
// (coherent, i.e. snooped; the address is the device/IOVA address).
enum { NV_MMU_PDE_APERTURE_INVALID = 0, NV_MMU_PDE_APERTURE_VRAM = 1,
       NV_MMU_PTE_APERTURE_VRAM = 0, NV_MMU_PTE_APERTURE_SYS_COH = 2 };
enum { NV_MMU_PTE_PRIVILEGE = 1u << 5, NV_MMU_PTE_READ_ONLY = 1u << 6 };

// PDE (also the small half of a dual PDE) pointing at a VRAM table; addr 4 KiB aligned.
uint64_t nv_mmu_pde_vram(uint64_t addr);
// PTE for a 4 KiB VRAM page: valid, kind 0 (pitch), flags from NV_MMU_PTE_*.
uint64_t nv_mmu_pte_vram(uint64_t addr, uint32_t flags);
// PTE for a 4 KiB system-memory page (coherent aperture, volatile: not cached in the GPU L2,
// as nouveau does for host memory); addr is the device (IOVA) address.
uint64_t nv_mmu_pte_sys(uint64_t addr, uint32_t flags);
// PTE kind (63:56); 0 = pitch/generic.
static inline uint64_t nv_mmu_pte_kind(uint8_t kind) { return (uint64_t)kind << 56; }
// Decoders: VRAM address of a valid VRAM PDE/PTE, or 0.
uint64_t nv_mmu_pde_addr(uint64_t pde);
uint64_t nv_mmu_pte_addr(uint64_t pte);

struct nv_mmu_ops {
    void *ctx;
    // A zeroed, 4 KiB-aligned VRAM block of `bytes`, or 0.
    uint64_t (*alloc)(void *ctx, uint32_t bytes);
    uint64_t (*rd64)(void *ctx, uint64_t addr);
    void     (*wr64)(void *ctx, uint64_t addr, uint64_t value);
    // Optional: called after a newly allocated table was linked at entry address `entry`
    // (so tables hung into someone else's directory can be unlinked again).
    void     (*linked)(void *ctx, uint64_t entry);
    // Both halves of every PD0 entry are ours (our own VA spaces). Enables 64 KiB pages and
    // mixing them with 4 KiB pages under one PD0 entry. False when mapping into tables
    // someone else owns (GSP-RM's BAR1 root): there a big-page-only PD0 entry is left alone.
    bool     ownPd0;
};

// What a range maps to: contiguous VRAM at `phys`, or system memory with one device
// address per 4 KiB page (pages[firstPage + i] for page i of the range).
struct nv_mmu_target {
    bool            sysmem;
    uint64_t        phys;
    const uint64_t *pages;
    uint64_t        firstPage;
    uint32_t        flags;      // NV_MMU_PTE_PRIVILEGE / READ_ONLY
    uint8_t         kind;
};

// Maps [va, va + size) under the root PD3 at `root`, creating missing tables through
// ops->alloc. With ops->ownPd0, VRAM is mapped with 64 KiB pages wherever the VA and the
// VRAM address are both 64 KiB aligned for a whole 64 KiB, and with 4 KiB pages elsewhere;
// system memory and !ownPd0 always use 4 KiB pages. Fails if any page of the range is
// already mapped (either size). All-or-nothing: on failure the PTEs written by this call
// are cleared again (tables it created stay, empty). The table walk is cached per 2 MiB.
bool nv_mmu_map(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t size,
                const nv_mmu_target *t);

// Maps [va, va + size) to VRAM [phys, phys + size) with 4 KiB pages under the root
// PD3 at `root`, creating missing PD2/PD1/PD0/PT instances through ops->alloc.
// va, phys and size must be 4 KiB aligned. Refuses to overwrite a valid PTE or to
// descend through an entry that isn't a VRAM PDE (e.g. a big-page-only PD0 entry).
bool nv_mmu_map_4k(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t phys,
                   uint64_t size, uint32_t pteFlags);

// Walks the tables for va; returns the PTE that translates it (with ops->ownPd0, a valid
// 64 KiB PTE takes precedence, as on the GPU; its address is the 64 KiB page's), or 0.
uint64_t nv_mmu_lookup(const nv_mmu_ops *ops, uint64_t root, uint64_t va);

// The valid 64 KiB PTE covering va, or 0 (only with ops->ownPd0).
uint64_t nv_mmu_lookup_big(const nv_mmu_ops *ops, uint64_t root, uint64_t va);

// Address of the first missing entry on the walk to va, i.e. where nv_mmu_map_4k would
// link its first new table (0 if every level down to the PT exists). Recorded before
// mapping into someone else's tables, so the link can be cleared again afterwards.
uint64_t nv_mmu_link_entry(const nv_mmu_ops *ops, uint64_t root, uint64_t va);

// Clears the PTEs of [va, va + size) where tables exist (skipping missing tables a whole
// 2 MiB at a time); returns how many 4 KiB pages were mapped. A 64 KiB page the range
// covers only partly is split into 4 KiB PTEs first (if that needs a table that can't be
// allocated, the whole 64 KiB page is unmapped: never leave a stale mapping).
uint32_t nv_mmu_unmap_4k(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t size);
