#include "nv_vram.h"

#include <string.h>

static const uint64_t PAGE = 0x1000;

bool nv_vram_init(nv_vram_heap *h, uint64_t base, uint64_t limit)
{
    memset(h, 0, sizeof(*h));
    if (!base || (base & (PAGE - 1)) || ((limit + 1) & (PAGE - 1)) || limit <= base)
        return false;
    h->base = base;
    h->limit = limit;
    return true;
}

uint64_t nv_vram_alloc(nv_vram_heap *h, uint64_t size, uint64_t align)
{
    if (!size || align < PAGE || (align & (align - 1)) || h->count >= nv_vram_heap::MAX_ALLOCS)
        return 0;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (size > h->limit - h->base + 1)
        return 0;

    // Walk the gaps: before the first allocation, between them, after the last.
    uint64_t start = h->base;
    for (uint32_t i = 0; i <= h->count; i++) {
        uint64_t gapEnd = i < h->count ? h->a[i].addr : h->limit + 1;   // exclusive
        uint64_t addr = (start + align - 1) & ~(align - 1);
        if (addr >= start && addr <= gapEnd && gapEnd - addr >= size) {
            memmove(&h->a[i + 1], &h->a[i], (h->count - i) * sizeof(h->a[0]));
            h->a[i].addr = addr;
            h->a[i].size = size;
            h->count++;
            return addr;
        }
        if (i < h->count)
            start = h->a[i].addr + h->a[i].size;
    }
    return 0;
}

bool nv_vram_reserve(nv_vram_heap *h, uint64_t addr, uint64_t size)
{
    if (!size || (addr & (PAGE - 1)) || h->count >= nv_vram_heap::MAX_ALLOCS)
        return false;
    size = (size + PAGE - 1) & ~(PAGE - 1);
    if (addr < h->base || addr > h->limit || size > h->limit - addr + 1)
        return false;
    // First allocation that ends after addr: it must start at or after addr + size.
    uint32_t i = 0;
    while (i < h->count && h->a[i].addr + h->a[i].size <= addr)
        i++;
    if (i < h->count && h->a[i].addr < addr + size)
        return false;
    memmove(&h->a[i + 1], &h->a[i], (h->count - i) * sizeof(h->a[0]));
    h->a[i].addr = addr;
    h->a[i].size = size;
    h->count++;
    return true;
}

bool nv_vram_free(nv_vram_heap *h, uint64_t addr)
{
    for (uint32_t i = 0; i < h->count; i++) {
        if (h->a[i].addr == addr) {
            memmove(&h->a[i], &h->a[i + 1], (h->count - i - 1) * sizeof(h->a[0]));
            h->count--;
            return true;
        }
    }
    return false;
}

uint64_t nv_vram_used(const nv_vram_heap *h)
{
    uint64_t n = 0;
    for (uint32_t i = 0; i < h->count; i++)
        n += h->a[i].size;
    return n;
}
