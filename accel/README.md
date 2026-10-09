# NVMetalAccel

An IOAccelerator for the GPU NVBringup drives, so Metal.framework lists the GPU in every process and loads
the NVMetal driver bundle (`MetalPluginName`) instead of needing `DYLD_INSERT_LIBRARIES`. It does no GPU
work itself: it gives IOAcceleratorFamily2 what its `start()` needs, modelled on Apple's paravirt GPU
driver (as Navi48-MacOS's Navi48Accel does). The bundle side (NVMetal.bundle, `NVMetalDevice : MTLIOAccelDevice`)
is nvmetal, built with NVK from nvbringup-mesa (branch `main`) and installed on the system volume by
`tools/nvmetal_root_install.sh`; installation is step 7 of the top-level README.

Two personalities (`probe()` picks one):

- **IOResources** (`nvaccel=1`): Metal only, as before. Refused while NVBringup's `NVDisplayPipeArmed` is set.
- **IOPCIDevice, `NVMetalDisplayPipe`** (S1, display pipe, no Metal plugin): on the GPU's PCI device, only while
  NVBringup on that device has `NVDisplayPipeArmed = true`. Its IOAccelDisplayPipe binds NVBringup's
  NVFramebuffer (a client of the same device's NVDisplayNub, at most two levels down) and copies plane 0 of
  each WindowServer transaction into the scanout by CPU:
  - `validateTransaction`: plane 0 must be a `'BGRA'` IOSurface.
  - `submitTransaction`: copies the dirty rect (whole surface if empty or invalid), clamped to the smaller of
    surface and framebuffer, into the framebuffer's system aperture (write-combined). Completion is
    synchronous: `isTransactionComplete` reports it, then `signalTransactionInterrupt` kicks the family.
  - Setup: `enableAccelerator()` (accel+3192), accel+3528 set (every transaction completes inside submit), the
    config flag accel+3279 for the display-pipe user client, and a publish notification that binds the
    framebuffer once on its own work loop with `found_framebuffer`. The pipe count must grow or the bind fails.
  - Counters, as registry properties of the accelerator: `NVPipeSubmits`, `NVPipeCompletes`, `NVPipeErrors`,
    `NVPipeLastError`, `NVPipeBound`.
  - Memory objects: the family makes every resource through `newResource` (and its IOSurface device cache, the
    sysmem and vidmem it wraps through `newSysMemory`/`newVidMemory`, and task mappings through `newMemoryMap`).
    Those return `NVMetalResource`, `NVMetalSysMemory`, `NVMetalVidMemory` and `NVMetalMemoryMap`, which answer
    the pure slots as AppleParavirtGPU does, minus its host side: system memory only, one level, no aperture,
    no GPU page table. Without them `createResource` returns NULL and `IOAccelDisplayPipe::init` faults on it.
  - Guard: `bindPendingFB` calls `createResource(0, 64)` once before `found_framebuffer`. A NULL result logs,
    sets `NVPipeLastError` and binds nothing.
  - Its probe score (1000) sits below NVBringup's (30000), so NVBringup has started and set the gate first.
- Not under the GPU's PCI device in the Metal-only personality: GPUWrangler adopts a PCI GPU that has an
  accelerator as a display GPU, and the screen went black. The S1 personality accepts that trade-off by design.
- `src/IOAccelFamily2_decl.h` declares the family's classes in this macOS's vtable order. It is generated
  from the kernel collections (`make decl`, needs `/opt/local/libexec/llvm-20/bin/llvm-mc`) and must be
  regenerated after a macOS update. It covers the classes we subclass or call (accelerator, machine, pipe,
  framebuffer slots 309/317/318, resource and the sysmem/vidmem/memory-map objects), plus non-virtual getters
  of IOSurface and the transaction. `probe()`
  refuses if a family class size changed.
- `make verify` checks the vtable slots of our nine classes against the kernel's and that the committed
  header is what `gen_decl.py` produces now; `make check` prints the kext's undefined symbols with the kernel
  collection that provides each one (it exits 0 and does not compare the list, so read it).

Loading: IOAcceleratorFamily2 is in the system kernel collection, so OpenCore can't inject this kext; it
has to go in the auxiliary collection (`/Library/Extensions`, approved in System Settings, then a reboot),
which for an ad-hoc signed kext needs SIP's kext-signing check relaxed.
