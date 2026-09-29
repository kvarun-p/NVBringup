# Third-party notices

NVBringup itself is MIT-licensed (see [LICENSE](LICENSE)). It builds on, or works with, the
projects below. Each keeps its own license and copyright; check the upstream repository for the
authoritative text.

| Project | How it is used | License |
|---|---|---|
| [NVIDIA open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) (tag 570.144) | Register definitions, GSP-RM structure layouts and sequences in `src/` follow its headers and code; names are kept so each structure traces back to its source | MIT (the repository is dual MIT/GPL; only the MIT-licensed parts are used) |
| [nouveau](https://nouveau.freedesktop.org/) and nova-core (Linux kernel) | Reference for Turing boot sequences and firmware parsing. Behaviour was cross-checked; GPL-licensed code (including nova-core's generated bindings) is not copied | GPL-2.0 (reference only) |
| [linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git) | Source of NVIDIA's GSP-RM firmware and booters. **Not included in this repository**; the user downloads it | NVIDIA's firmware license (`LICENCE.nvidia` in linux-firmware) |
| [Mesa](https://mesa3d.org) (NVK, NAK) | Patched by the separate `nvbringup-mesa` repository, not by this one | MIT |
| [Khronos Vulkan loader and headers](https://github.com/KhronosGroup/Vulkan-Loader) | Built and installed by the user | Apache-2.0 |
| [llama.cpp](https://github.com/ggml-org/llama.cpp) | Optional workload; a fork carries one change | MIT |
| [OpenCore](https://github.com/acidanthera/OpenCorePkg), [Lilu](https://github.com/acidanthera/Lilu), [WhateverGreen](https://github.com/acidanthera/WhateverGreen) | Boot loader and companion kexts the kext is designed to coexist with; none of their code is included | BSD-3-Clause / GPL-3.0 (not distributed here) |

## Files that are not redistributed

- **NVIDIA GSP-RM firmware** (`firmware/`): downloaded by the user under NVIDIA's license.
- **GPU VBIOS** (`*.rom`): copyrighted by the card or system vendor. The kext reads it from the
  user's own device at runtime; do not commit or share a dump.

## Trademarks

NVIDIA, GeForce, Quadro, Titan, RTX, CUDA and Turing are trademarks of NVIDIA Corporation.
Apple, macOS, Mac, Metal and Apple Silicon are trademarks of Apple Inc. Intel is a trademark of
Intel Corporation. Vulkan is a registered trademark of the Khronos Group Inc. Windows is a
trademark of Microsoft Corporation. All other names belong to their owners. They are used here
only to identify the hardware and software NVBringup works with.
