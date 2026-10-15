# NVMetalAccel

An IOAccelerator for the GPU NVBringup drives, so Metal.framework lists the GPU in every process and loads
the NVMetal driver bundle (`MetalPluginName`) instead of needing `DYLD_INSERT_LIBRARIES`. It does no GPU
work itself: it gives IOAcceleratorFamily2 what its `start()` needs, modelled on Apple's paravirt GPU
driver (as Navi48-MacOS's Navi48Accel does). The bundle side (NVMetal.bundle, `NVMetalDevice : MTLIOAccelDevice`)
is nvmetal, built with NVK from nvbringup-mesa (branch `main`) and installed into `/Library/GPUBundles` by
`tools/nvmetal_root_install.sh` (`--system`: the system volume, the older way); installation is step 7 of the
top-level README.

Two personalities (`probe()` picks one):

- **IOResources** (`nvaccel=1`): Metal only, as before. Refused while NVBringup's `NVDisplayPipeArmed` is set.
- **IOPCIDevice, `NVMetalDisplayPipe`** (the display pipe): on the GPU's PCI device, only while NVBringup on that
  device has `NVDisplayPipeArmed = true` (`nvdpipe=1` or `nvdpipe-once`). It publishes the Metal plugin keys too,
  marked `NVDisplayPipeMode`, because CoreDisplay needs an MTLDevice for the pipe's accelerator, and sets
  class-code 0x030000 on itself so IOPresentment pairs it with NVAGDCControl. Its IOAccelDisplayPipe binds
  NVBringup's NVFramebuffer (a client of the same device's NVDisplayNub, at most two levels down) and puts plane 0
  of each WindowServer transaction on the screen:
  - `validateTransaction`: plane 0 must be a `'BGRA'` IOSurface.
  - `submitTransaction`: NVBringup's `NVPipeCopy` (`src/nv_pipe_copy.h`) copies the surface into one of three
    scanout buffers with the copy engine and flips window 0 to it at the next vblank. The CPU copy into the
    framebuffer's write-combined aperture is the fallback, for frames before the display is lit or across a mode
    change. The transaction completes once the copy is done (`isTransactionComplete`, then
    `signalTransactionInterrupt` kicks the family); the 20 ms completion hold used against mirror flicker is gone,
    since the cause was read-only wiring in NVBringup.
  - Setup: `enableAccelerator()` (accel+3192), accel+3528 set (every transaction completes inside submit), the
    config flag accel+3279 for the display-pipe user client, and a publish notification that binds the
    framebuffer once on its own work loop with `found_framebuffer`. The pipe count must grow or the bind fails.
  - Counters, as registry properties of the accelerator: `NVPipeSubmits`, `NVPipeCompletes`, `NVPipeErrors`,
    `NVPipeLastError`, `NVPipeBound`, `NVPipeActive`, `NVPipeGpuCopies`, `NVPipeCpuCopies`, `NVPipeCopyUs`,
    `NVPipeCopiedPct`. NVBringup publishes the flips' timing as `NVPipeTiming`.
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
