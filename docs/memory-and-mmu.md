# Memory and the GPU MMU

## The host owns VRAM and page tables

The first plan assumed GSP-RM allocates VRAM for the host. NVIDIA's code says otherwise: the
CPU-side driver allocates memory itself and only registers it with GSP-RM when an object needs
it (`memdescSendMemDescToGSP`, the `ALLOC_MEMORY` RPC). nouveau, the proven Turing r570 path, goes further and manages VRAM and page tables itself,
passing raw physical descriptors in allocation parameters. **The kext follows nouveau:**
- `src/nv_vram.cpp`: a first-fit allocator over the usable VRAM region GSP-RM reports (the other
  regions hold GSP-RM's data).
- Page tables for each VA space in host VRAM, written by the kext (`src/nv_mmu.cpp`, a pure
  builder over a write callback, so the same code runs against fake memory in the self-test).

## Split VA spaces

NVIDIA's split VA space model (nouveau does the same): the host allocates `FERMI_VASPACE_A`
through GSP-RM, then hands over its upper page-directory levels with
`COPY_SERVER_RESERVED_PDES`. GSP-RM keeps one 512 MiB range per VA space for its own buffers
and fills the entries below it. The host owns everything else. Proven in run 9, when a copy
engine ran through these tables.

## CPU access to VRAM

| Path | Use |
|---|---|
| PRAMIN window (BAR0) | A movable 1 MiB window for kernel accesses: tests, and page-table writes outside the pool below. NVIDIA's driver uses it too with GSP-RM running |
| BAR1 (256 MiB aperture, through the GPU MMU) | User CPU mappings of VRAM, mapped write-combined (the first, uncached kernel mapping wrote at 46 MB/s), and the page-table pool below |

GSP-RM owns the BAR1 root page directory; the kext builds the levels below it. See
[bar1-cpu-mappings.md](bar1-cpu-mappings.md) for what happens to user mappings of VRAM when
the memory is freed.

## 64 KiB pages

**Why:** memory-bound work ran far below the hardware (llama.cpp generation read weights at about
34 GB/s against ~128 GB/s peak, and every GPU mapping used 4 KiB PTEs).

**What:** VRAM is mapped with 64 KiB PTEs wherever the VA and the VRAM address are both 64 KiB
aligned for a whole 64 KiB, and 4 KiB elsewhere; system memory always uses 4 KiB. VRAM
allocations of 64 KiB or more are 64 KiB aligned, and NVK aligns their VAs to match. An unbind
that would split a big page is rejected. Measured afterwards (run 23): large matrix-vector
kernels reach 97–112 GB/s, about 85 % of peak.

## Page tables written through BAR1

**Why:** binds wrote 4 KiB PTEs one at a time through PRAMIN (binding 576 MiB took about 0.4 s).

**What:** a 16 MiB pool of page-table memory mapped once through BAR1, with a kernel shadow copy
for reads; tables of connection VA spaces come from it, and a full pool falls back to PRAMIN. The
top-level directories stay outside the pool, because GSP-RM writes into them. Measured (run 28):
bind 576 MiB 90 → 31 ms, unbind 81 → 41 ms. `nvtest -v` prints the current numbers:

```bash
build/nvtest -v | grep -E 'took [0-9]+ ms'   # binding 576 MiB took … ms / freeing (unbind) took … ms
```

## Scrubbing and deferred frees

- VRAM is cleared by a kernel copy-engine channel before it's handed out, so one process never
  sees another's data. New VRAM reads zero (run 12).
- Freeing memory, or closing a connection, waits until that connection's submitted GPU work has
  completed, so stale GPU work can't touch reused VRAM.
- Memory is unbound (PTEs cleared, TLB flushed) before it's freed.
