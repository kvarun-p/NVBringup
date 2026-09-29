# The user-space interface

## Shaped after Mesa's nvkmd

The interface (`src/nv_uapi.h`, wrapped by `tools/libnvmac`) maps one to one onto Mesa's `nvkmd`
layer, the boundary between NVK and its kernel driver. The NVK port is then one new backend
(`nvkmd_macos`) beside nouveau's, and nothing above it changes. One IOKit connection is one
device: its own GPU VA space, memory, contexts and syncs, all freed when the connection closes
or the process dies.

User space owns VA allocation and binds memory explicitly (`VM_BIND`), as with nouveau's
`VM_BIND`.

### Using the interface directly

[`tools/libnvmac_example.c`](../tools/libnvmac_example.c) copies 64 KiB between two
system-memory buffers with the copy engine and checks the result: open, allocate, map, bind,
build a push buffer, submit, wait.

```bash
clang -std=c11 -Wall -Wextra -Isrc tools/libnvmac_example.c tools/libnvmac.c \
    -framework IOKit -framework CoreFoundation -o build/libnvmac_example
build/libnvmac_example         # NVIDIA GeForce GTX 1650, 3888 MiB VRAM / copy ok
```

The core of it, after the buffers are allocated, mapped and bound at `va[0]` (source), `va[1]`
(destination) and `va[2]` (push buffer, CPU address `cpu[2]`):

```c
uint32_t ctx, seq_sync;                    // a channel plus its completion timeline
nvmac_ctx_create(d, NVMAC_ENGINE_COPY, &ctx, &seq_sync);

uint32_t *p = cpu[2];                      // Volta+ method headers on subchannel 4
p = mthd(p, 0x000, 1, (uint32_t[]){ 0xc5b5 });                       // SET_OBJECT: copy class
p = mthd(p, 0x400, 4, (uint32_t[]){ va[0] >> 32, (uint32_t)va[0],
                                     va[1] >> 32, (uint32_t)va[1] });  // OFFSET_IN, OFFSET_OUT
p = mthd(p, 0x418, 2, (uint32_t[]){ SIZE, 1 });                      // LINE_LENGTH_IN, LINE_COUNT
p = mthd(p, 0x300, 1, (uint32_t[]){ 2u | 1u << 2 | 1u << 7 | 1u << 8 });   // LAUNCH_DMA

struct nvmac_push push = { va[2], (uint32_t)((uint8_t *)p - (uint8_t *)cpu[2]), 0 };
uint64_t seqno;
nvmac_exec(d, ctx, NULL, 0, &push, 1, NULL, 0, &seqno);              // no extra waits or signals

struct nvmac_sync_point done = { seq_sync, 0, seqno };
nvmac_sync_wait(d, &done, 1, 1 /* all */, 1000000000ull /* 1 s */, NULL);
```

Every call returns 0 or an `IOReturn`; `nvmac_strerror()` names it. `nvmac_close()` frees
whatever the connection still owns. The full test of the interface is `tools/nvtest.c`
(`build/nvtest`, 55 checks).

## Submission goes through the kernel

`EXEC` writes the GPFIFO entries, updates GPPut and rings the doorbell. User-mode submission
would need BAR0's doorbell page mapped into every process. A submission costs 11.6 µs per EXEC
and 20 µs for EXEC plus wait (run 12). In run 24, GPU time accounted for all of the wall time per
generated token, so submission cost doesn't show in llama.cpp's speed.

## Syncs are host semaphores

After the user's pushes the kernel appends a release of 64-bit timeline values (the signals and
the context's own sequence number); waits on other syncs are GPU-side acquires before the pushes,
so cross-context waits need no CPU round trip. The sync values sit in a system-memory page
mapped read-only into the process, so a CPU can check them without a call. NVK sees them as one
timeline `vk_sync` type, and emulates binary semaphores on top with Mesa's `vk_sync_binary`.

## Waits: polling first, then interrupts

1. **Polling** (initially): spin about 50 µs, then sleep in 100 µs steps.
2. **Non-stall MSI interrupts** (run 33, on by default; boot-arg `nvintr=0` turns them off).
   The CPU services only the non-stall vectors of GR and the copy engines; GSP-RM keeps the stall
   ones. On Turing, the MSI has to be re-armed through a config-space mirror write (NVIDIA's
   `kbifRearmMSI_GM107`); the top-level enable toggle the first attempt used re-arms only on
   Hopper and later. A storm guard turns interrupts off above 100,000 per second.

Check or switch them:

```bash
sudo build/nvgsp intr status     # on/off, interrupt count, spurious, storms
ioreg -r -c NVBringup -d 1 | grep '"NVIntr"'
```

Result: speed and CPU time within noise of polling (run 32 vs 33: tg 115.7 vs 117.0 t/s,
0.93 vs 0.95 s system time), because most waits end within the initial spin. They stay on:
they cost nothing measurable, and a wait that outlasts the spin is woken by the GPU's interrupt,
with the CPU asleep (a 2 ms backup timeout), instead of polling every 100 µs.

## Isolation between processes

- A VA space per connection. `VM_BIND` accepts only the user range. `MEM_ALLOC` and `VM_BIND`
  accept only uncompressed PTE kinds (0x00–0x06); the compressed kinds 0x08–0x0e are turned into
  their uncompressed equivalents, and anything else is rejected.
- Unprivileged channels. The kernel's parts of each context (ring, kernel pushes, graphics context
  buffers) are mapped with privileged PTEs, so user shaders can't reach them.
- VRAM is scrubbed before hand-out, including each context's kernel push slots.
- A faulting context is marked lost (`RC_TRIGGERED` from GSP-RM) and reports device-lost; other
  connections keep working (run 12). Killing a process mid-wait frees its channel and VRAM.
- CPU mappings of VRAM that outlive a free: see [bar1-cpu-mappings.md](bar1-cpu-mappings.md).

Open: there is no per-connection limit on system memory (GART) yet. It's wired (it can't be
paged out), so one process can allocate it until the Mac runs out of RAM.

## Who can open the GPU

Like a Linux DRM render node: root or **the console user** (`kIOClientPrivilegeLocalUser`), so
programs run without sudo; boot-arg `nvgpu_users=1` admits any user. The GSP-RM control
client (firmware, boot, logs) stays root-only. A third client type answers status and power
questions without opening the GPU (see [power.md](power.md)).

## Graphics context

Following r570's GSP-client path: the kernel allocates each context's graphics buffers, maps
them, and promotes them to GSP-RM before creating the compute and 3D objects. The large global
buffers (about 9 MiB, mostly the attribute buffer) are allocated once per GPU and mapped into
each VA space. The copy object lives on the graphics channel (GSP-RM accepted COPY0 there), so
NVK's one-channel layout works without a second channel per context.

## Memory types offered to NVK

VRAM (device-local), system memory (host-visible, coherent: snooped), and VRAM mapped through
BAR1. With no resizable BAR (256 MiB aperture), large host-visible buffers are system memory;
llama.cpp stages weights through them into VRAM. Since 2026-09-29 NVK offers no host-visible
VRAM to apps at all (nvbringup-mesa patch 0007); see [bar1-cpu-mappings.md](bar1-cpu-mappings.md).
