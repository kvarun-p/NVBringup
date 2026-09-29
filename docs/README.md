# Documentation

Installation and everyday use are in the top-level [README](../README.md). Here:
- [howto.md](howto.md): task recipes (benchmarks, power, logs, `nvkmapvram`, faults), each with
  commands checked on the test machine.
- The design documents: why NVBringup is built the way it is. Each lists, per decision, what was
  chosen, what else was considered, why, and the evidence (hardware runs and measurements on
  the test laptop's GTX 1650, TU117).

| Document | Covers |
|---|---|
| [howto.md](howto.md) | Task recipes: check and test the install, benchmark and profile llama.cpp, `nvkmapvram`, power, logs, device-lost, using `libnvmac` from C |
| [approach.md](approach.md) | Kext plus NVK instead of VMs or CUDA; kext vs DriverKit; loading with OpenCore; how the code is tested |
| [firmware-and-boot.md](firmware-and-boot.md) | Firmware version, FWSEC-FRTS, GSP-RM boot and teardown, where structure layouts come from, the boot daemon |
| [memory-and-mmu.md](memory-and-mmu.md) | Host-owned VRAM, split VA spaces, BAR1, 64 KiB pages, the page-table pool |
| [gpu-interface.md](gpu-interface.md) | The user-space interface: shape, submission, syncs, interrupts, isolation, access |
| [bar1-cpu-mappings.md](bar1-cpu-mappings.md) | CPU mappings of VRAM that outlive a free: the kext fix, and where NVK puts CPU-mapped memory (`nvkmapvram`) |
| [nvk-and-llama.md](nvk-and-llama.md) | The NVK port, the two NAK fixes for Turing, llama.cpp tuning |
| [power.md](power.md) | Runtime power-off, sleep and wake, shutdown |
| [hackintosh.md](hackintosh.md) | Hiding the GPU from macOS graphics without breaking brightness; dual boot |

Sources used throughout: NVIDIA's open-gpu-kernel-modules (r570.144, the authority for
structures and sequences), nouveau and nova-core in Linux (proven Turing boot paths), and
linux-firmware (the firmware files).
