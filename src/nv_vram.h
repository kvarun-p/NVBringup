// VRAM allocator for the host side of the driver. With GSP-RM, the CPU-side
// driver owns the VRAM that GSP-RM reports as usable (NVIDIA's CPU-RM runs its PMA
// allocator there; nouveau its own); GSP-RM keeps its own heap in WPR2 and the
// reserved regions. Pure C++, shared by the kext and the host tool.
//
// First fit over a sorted list of allocations; enough for page tables, channels
// and buffers during bring-up (a buddy allocator can replace it later).
#pragma once

#include <stdint.h>

struct nv_vram_heap {
    enum { MAX_ALLOCS = 32768 };
    uint64_t base, limit;               // usable range [base, limit], inclusive
    uint32_t count;
    struct { uint64_t addr, size; } a[MAX_ALLOCS];     // sorted by addr
};

// Uses [base, limit]; both must be 4 KiB aligned (limit + 1).
bool nv_vram_init(nv_vram_heap *h, uint64_t base, uint64_t limit);

// Returns the VRAM address, or 0 on failure (0 is never usable VRAM here).
// size is rounded up to 4 KiB; align must be a power of two >= 4 KiB.
uint64_t nv_vram_alloc(nv_vram_heap *h, uint64_t size, uint64_t align);

// Allocates exactly [addr, addr + size) (size rounded up to 4 KiB; addr 4 KiB aligned), for
// ranges that must keep their address, e.g. after nv_vram_init reset the heap. False if it
// is outside the heap or overlaps an allocation. Freed with nv_vram_free(addr).
bool nv_vram_reserve(nv_vram_heap *h, uint64_t addr, uint64_t size);

// Frees an address returned by nv_vram_alloc; false if it wasn't allocated.
bool nv_vram_free(nv_vram_heap *h, uint64_t addr);

uint64_t nv_vram_used(const nv_vram_heap *h);
