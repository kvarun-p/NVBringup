# NVMetalAccel

An IOAccelerator for the GPU NVBringup drives, so Metal.framework lists the GPU in every process and loads
the NVMetal driver bundle (`MetalPluginName`) instead of needing `DYLD_INSERT_LIBRARIES`. It does no GPU
work itself: it gives IOAcceleratorFamily2 what its `start()` needs, modelled on Apple's paravirt GPU
driver (as Navi48-MacOS's Navi48Accel does). The bundle side (NVMetal.bundle, `NVMetalDevice : MTLIOAccelDevice`)
is not written yet.

- Matches NVBringup's service (category `IOAccelerator`); off unless boot-arg `nvaccel=1`.
- `src/IOAccelFamily2_decl.h` declares the family's classes in this macOS's vtable order. It is generated
  from the kernel collections (`make decl`, needs `/opt/local/libexec/llvm-20/bin/llvm-mc`) and must be
  regenerated after a macOS update; `probe()` refuses if the family's class sizes changed.
- `make verify` checks all 780 vtable slots of our four classes against the kernel's; `make check`
  resolves the kext's symbols against the running kernel.

Loading: IOAcceleratorFamily2 is in the system kernel collection, so OpenCore can't inject this kext; it
has to go in the auxiliary collection (`/Library/Extensions`, approved in System Settings, then a reboot),
which for an ad-hoc signed kext needs SIP's kext-signing check relaxed.
