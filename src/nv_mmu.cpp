#include "nv_mmu.h"

static const uint32_t SHIFT[NV_MMU_LEVELS] = { 47, 38, 29, 21, 12 };
static const uint32_t BITS[NV_MMU_LEVELS]  = { 2, 9, 9, 8, 9 };
static const uint32_t ENTRY[NV_MMU_LEVELS] = { 8, 8, 8, 16, 8 };

// VRAM address fields: PDE/PTE bits 32:8 = addr >> 12 (NV_MMU_VER2_*_ADDRESS_VID)
static const uint64_t ADDR_MASK = ((1ull << 25) - 1) << 8;
// System-memory PTE address: bits 53:8 = addr >> 12 (NV_MMU_VER2_PTE_ADDRESS_SYS)
static const uint64_t SYS_ADDR_MASK = ((1ull << 46) - 1) << 8;
static const uint64_t PTE_VOL = 1ull << 3;

uint32_t nv_mmu_level_shift(int level) { return SHIFT[level]; }
uint32_t nv_mmu_level_bytes(int level) { return (1u << BITS[level]) * ENTRY[level]; }

uint32_t nv_mmu_index(uint64_t va, int level)
{
    return (uint32_t)(va >> SHIFT[level]) & ((1u << BITS[level]) - 1);
}

uint64_t nv_mmu_pde_vram(uint64_t addr)
{
    return ((addr >> 4) & ADDR_MASK) | ((uint64_t)NV_MMU_PDE_APERTURE_VRAM << 1);
}

uint64_t nv_mmu_pte_vram(uint64_t addr, uint32_t flags)
{
    return ((addr >> 4) & ADDR_MASK) | ((uint64_t)NV_MMU_PTE_APERTURE_VRAM << 1) |
           (flags & (NV_MMU_PTE_PRIVILEGE | NV_MMU_PTE_READ_ONLY)) | 1;
}

uint64_t nv_mmu_pte_sys(uint64_t addr, uint32_t flags)
{
    return ((addr >> 4) & SYS_ADDR_MASK) | ((uint64_t)NV_MMU_PTE_APERTURE_SYS_COH << 1) | PTE_VOL |
           (flags & (NV_MMU_PTE_PRIVILEGE | NV_MMU_PTE_READ_ONLY)) | 1;
}

uint64_t nv_mmu_pde_addr(uint64_t pde)
{
    return ((pde >> 1) & 3) == NV_MMU_PDE_APERTURE_VRAM ? (pde & ADDR_MASK) << 4 : 0;
}

uint64_t nv_mmu_pte_addr(uint64_t pte)
{
    return (pte & 1) && ((pte >> 1) & 3) == NV_MMU_PTE_APERTURE_VRAM ? (pte & ADDR_MASK) << 4 : 0;
}

// Address of the entry for va in the table at `table`; PD0 entries use the small half.
static uint64_t entryAddr(uint64_t table, uint64_t va, int level)
{
    return table + (uint64_t)nv_mmu_index(va, level) * ENTRY[level] + (level == NV_MMU_PD0 ? 8 : 0);
}

static const uint64_t PAGE = 0x1000, BIG = 0x10000;
static const uint64_t PT_SPAN = 1ull << 21;     // one PD0 entry (and its PTs) covers 2 MiB

// Entry for va in a 64 KiB-page PT (VA bits 20:16).
static uint64_t bigEntryAddr(uint64_t bigPt, uint64_t va)
{
    return bigPt + ((va >> 16) & 31) * 8;
}

// Links a new zeroed table at entry `e`; 0 if allocation fails.
static uint64_t linkTable(const nv_mmu_ops *ops, uint64_t e, uint32_t bytes)
{
    uint64_t t = ops->alloc(ops->ctx, bytes);
    if (!t || (t & 0xfff))
        return 0;
    ops->wr64(ops->ctx, e, nv_mmu_pde_vram(t));
    if (ops->linked)
        ops->linked(ops->ctx, e);
    return t;
}

// Table below `level` for va; allocated and linked if missing and `create`.
static uint64_t nextTable(const nv_mmu_ops *ops, uint64_t table, uint64_t va, int level, bool create)
{
    uint64_t e = entryAddr(table, va, level);
    uint64_t pde = ops->rd64(ops->ctx, e);
    if (pde)
        return nv_mmu_pde_addr(pde);
    // PD0: a big-page PT in the low half alone is not ours to extend, unless we own both halves
    if (level == NV_MMU_PD0 && !ops->ownPd0 && ops->rd64(ops->ctx, e - 8))
        return 0;
    return create ? linkTable(ops, e, nv_mmu_level_bytes(level + 1)) : 0;
}

// The PD0 table covering va (walk from the root), or 0.
static uint64_t findPd0(const nv_mmu_ops *ops, uint64_t root, uint64_t va, bool create)
{
    uint64_t t = root;
    for (int l = NV_MMU_PD3; l < NV_MMU_PD0 && t; l++)
        t = nextTable(ops, t, va, l, create);
    return t;
}

// The 64 KiB-page PT under PD0 table `pd0` for va (ownPd0 only), or 0. Allocated as a
// zeroed 4 KiB block like every other table, so the big half of the dual PDE has the same
// encoding as the small half (NV_MMU_VER2_DUAL_PDE_ADDRESS_BIG_*: addr >> 4 from bit 4).
static uint64_t bigPt(const nv_mmu_ops *ops, uint64_t pd0, uint64_t va, bool create)
{
    if (!pd0 || !ops->ownPd0)
        return 0;
    uint64_t e = pd0 + (uint64_t)nv_mmu_index(va, NV_MMU_PD0) * 16;
    uint64_t pde = ops->rd64(ops->ctx, e);
    if (pde)
        return nv_mmu_pde_addr(pde);
    return create ? linkTable(ops, e, 0x1000) : 0;
}

// Walk cache for one 2 MiB span: PD0 table, small PT and big PT (0 = absent).
struct Span {
    uint64_t base = ~0ull, pd0 = 0, spt = 0, bpt = 0;
};

static void spanAt(const nv_mmu_ops *ops, uint64_t root, uint64_t a, Span *s)
{
    uint64_t base = a & ~(PT_SPAN - 1);
    if (base == s->base)
        return;
    s->base = base;
    s->pd0 = findPd0(ops, root, a, false);
    s->spt = s->pd0 ? nextTable(ops, s->pd0, a, NV_MMU_PD0, false) : 0;
    s->bpt = bigPt(ops, s->pd0, a, false);
}

// Splits the valid 64 KiB PTE at big entry `be` (page at VA `a64`) into 16 small PTEs.
static bool splitBig(const nv_mmu_ops *ops, uint64_t root, uint64_t a64, uint64_t be, Span *s)
{
    if (!s->pd0 && !(s->pd0 = findPd0(ops, root, a64, true)))
        return false;
    if (!s->spt && !(s->spt = nextTable(ops, s->pd0, a64, NV_MMU_PD0, true)))
        return false;
    uint64_t pte = ops->rd64(ops->ctx, be);
    uint64_t base = nv_mmu_pte_addr(pte), keep = pte & ~(ADDR_MASK);
    for (uint64_t i = 0; i < BIG / PAGE; i++)
        ops->wr64(ops->ctx, entryAddr(s->spt, a64 + i * PAGE, NV_MMU_PT),
                  keep | (((base + i * PAGE) >> 4) & ADDR_MASK));
    ops->wr64(ops->ctx, be, 0);
    return true;
}

static uint32_t clearRange(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t size)
{
    uint32_t n = 0;
    uint64_t end = va + size;
    Span s;
    for (uint64_t a = va; a < end;) {
        spanAt(ops, root, a, &s);
        uint64_t spanEnd = s.base + PT_SPAN;
        if (!s.pd0) {                           // nothing mapped in this 2 MiB: skip to its end
            a = spanEnd;
            continue;
        }
        if (s.bpt) {
            uint64_t a64 = a & ~(BIG - 1);
            uint64_t be = bigEntryAddr(s.bpt, a64);
            if (ops->rd64(ops->ctx, be) & 1) {
                if (a64 >= va && a64 + BIG <= end) {
                    ops->wr64(ops->ctx, be, 0);
                    n += BIG / PAGE;
                    a = a64 + BIG;
                    continue;
                }
                if (!splitBig(ops, root, a64, be, &s))
                    ops->wr64(ops->ctx, be, 0);  // fail safe: unmap it all
            }
        }
        if (s.spt) {
            uint64_t e = entryAddr(s.spt, a, NV_MMU_PT);
            if (ops->rd64(ops->ctx, e) & 1) {
                ops->wr64(ops->ctx, e, 0);
                n++;
            }
        }
        a += PAGE;
    }
    return n;
}

bool nv_mmu_map(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t size,
                const nv_mmu_target *t)
{
    if (!root || ((va | size | root) & 0xfff) || !size || va + size < va || va + size > (1ull << 49))
        return false;
    if (t->sysmem ? !t->pages : ((t->phys & 0xfff) || (t->phys + size) >> 37))
        return false;
    Span s;
    uint64_t checked64 = ~0ull;                 // 64 KiB page whose big PTE is known invalid
    uint64_t off = 0;
    while (off < size) {
        uint64_t a = va + off;
        spanAt(ops, root, a, &s);
        if (!s.pd0 && !(s.pd0 = findPd0(ops, root, a, true)))
            break;
        bool big = ops->ownPd0 && !t->sysmem && !(a & (BIG - 1)) && !((t->phys + off) & (BIG - 1)) &&
                   size - off >= BIG;
        if (big) {
            if (!s.bpt && !(s.bpt = bigPt(ops, s.pd0, a, true)))
                break;
            uint64_t be = bigEntryAddr(s.bpt, a);
            if (ops->rd64(ops->ctx, be) & 1)
                break;
            bool clash = false;
            for (uint64_t i = 0; s.spt && i < BIG / PAGE && !clash; i++)
                clash = ops->rd64(ops->ctx, entryAddr(s.spt, a + i * PAGE, NV_MMU_PT)) & 1;
            if (clash)
                break;
            ops->wr64(ops->ctx, be, nv_mmu_pte_vram(t->phys + off, t->flags) | nv_mmu_pte_kind(t->kind));
            off += BIG;
            continue;
        }
        if (!s.spt && !(s.spt = nextTable(ops, s.pd0, a, NV_MMU_PD0, true)))
            break;
        uint64_t a64 = a & ~(BIG - 1);
        if (s.bpt && a64 != checked64) {
            if (ops->rd64(ops->ctx, bigEntryAddr(s.bpt, a64)) & 1)
                break;
            checked64 = a64;
        }
        uint64_t e = entryAddr(s.spt, a, NV_MMU_PT);
        if (ops->rd64(ops->ctx, e) & 1)
            break;
        uint64_t pte = t->sysmem ? nv_mmu_pte_sys(t->pages[t->firstPage + off / PAGE], t->flags)
                                 : nv_mmu_pte_vram(t->phys + off, t->flags);
        ops->wr64(ops->ctx, e, pte | nv_mmu_pte_kind(t->kind));
        off += PAGE;
    }
    if (off == size)
        return true;
    if (off)
        clearRange(ops, root, va, off);
    return false;
}

bool nv_mmu_map_4k(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t phys,
                   uint64_t size, uint32_t pteFlags)
{
    nv_mmu_target t = { false, phys, nullptr, 0, pteFlags, 0 };
    return nv_mmu_map(ops, root, va, size, &t);
}

uint64_t nv_mmu_lookup_big(const nv_mmu_ops *ops, uint64_t root, uint64_t va)
{
    uint64_t b = bigPt(ops, findPd0(ops, root, va, false), va, false);
    uint64_t pte = b ? ops->rd64(ops->ctx, bigEntryAddr(b, va)) : 0;
    return (pte & 1) ? pte : 0;
}

uint64_t nv_mmu_lookup(const nv_mmu_ops *ops, uint64_t root, uint64_t va)
{
    if (uint64_t big = nv_mmu_lookup_big(ops, root, va))
        return big;
    uint64_t t = findPd0(ops, root, va, false);
    t = t ? nextTable(ops, t, va, NV_MMU_PD0, false) : 0;
    return t ? ops->rd64(ops->ctx, entryAddr(t, va, NV_MMU_PT)) : 0;
}

uint64_t nv_mmu_link_entry(const nv_mmu_ops *ops, uint64_t root, uint64_t va)
{
    uint64_t t = root;
    for (int l = NV_MMU_PD3; l < NV_MMU_PT; l++) {
        uint64_t next = nextTable(ops, t, va, l, false);
        if (!next)
            return entryAddr(t, va, l);
        t = next;
    }
    return 0;
}

uint32_t nv_mmu_unmap_4k(const nv_mmu_ops *ops, uint64_t root, uint64_t va, uint64_t size)
{
    return clearRange(ops, root, va, size);
}
