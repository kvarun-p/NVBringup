# Approach

## Kext plus NVK, not a VM or CUDA

**Problem.** On an Intel Mac, llama.cpp and Ollama run on the CPU only (no Metal backend for such
GPUs, and NVIDIA has had no macOS driver since High Sierra; Turing never had one).

| Option | Verdict |
|---|---|
| Boot Linux or Windows and use CUDA | The practical choice for real use; kept as the fallback |
| Pass the GPU through to a VM or container | Not possible: macOS's Hypervisor and Virtualization frameworks have no PCI device assignment (Linux has VFIO, Windows DDA). Mac VM apps' GPUs are paravirtualized onto Metal, which this GPU doesn't have |
| Linux as the host, macOS in a VM | Possible, but then Linux is the main OS and macOS a guest, the opposite of the goal |
| Forward CUDA calls from a Linux VM to a macOS kext | Rejected: memory coherence across the VM boundary is the hardest part of GPU virtualization |
| Port NVIDIA's CUDA user space | Impossible: closed Linux ELF binaries |
| **macOS kext + Mesa's NVK (Vulkan) + llama.cpp's Vulkan backend** | **Chosen** |

**Why it's feasible.** Since Turing, NVIDIA's GSP-RM firmware does most resource management on the
GPU itself, so the kernel side stays small: boot the firmware, manage memory and page tables,
submit work. NVK is an open Vulkan driver that already supports Turing, and llama.cpp has a
Vulkan backend.

## Kext, not DriverKit

`IOPCIDevice` in a kext is deprecated in favour of PCIDriverKit. A DriverKit extension was
ruled out because:
- it's installed as a system extension by an app, not injected by OpenCore;
- running a PCI dext needs a PCI entitlement from Apple, or SIP turned off for development
  loading.

The kext needs neither. It uses the deprecated APIs, with the deprecation warnings silenced.

## Loaded by OpenCore

| Option | Verdict |
|---|---|
| `kmutil load` | Needs SIP partly disabled, and still a reboot |
| **OpenCore `Kernel → Add`** | **Chosen**: no SIP change, the same path Lilu and WhateverGreen use |

Consequences:
- An injected kext has no bundle on disk, so it can't load its own firmware files
  (`OSKextRequestResource`); a daemon hands them over after boot ([firmware-and-boot.md](firmware-and-boot.md)).
- Development tests boot from a separate copy of the EFI, so the main EFI stays a known-good
  fallback. Verbose-boot and debug boot-args keep a panic on screen with symbols.

## Building and testing

- **Build:** a plain Makefile with the Command Line Tools SDK (it includes `Kernel.framework`
  headers), ad-hoc signed. Mesa needs a case-sensitive filesystem with symlink support.
- **Firmware and ROM parsers are portable C++** shared by the kext and a host tool
  (`build/vbios_tool`): self-tests on synthetic input, and fuzzing under ASan/UBSan (hundreds
  of thousands of mutated inputs, no errors) before any of it ran in the kernel.
- **Structure layouts are checked at compile time** (`static_assert` on sizes and offsets).
- **Staged risk:** each capability that touches hardware was added behind its own gate: first
  read-only and logging, then behind a boot-arg (`nvfwsec=1`, `nvgsp=1`). Code that writes GPU
  registers or starts DMA was reviewed and approved before it was written, and cross-checked
  against nova-core's register definitions.
- **Logging:** the kext's `IOLog` output at early boot is lost (it runs before `logd`), so the
  kext keeps its log in the `NVLog` registry property (readable without root).
- **Hardware tests:** `nvtest` (the kernel interface, 55 checks), `vktest` (NVK), llama.cpp's
  `test-backend-ops` and `llama-bench`; `tools/verify_install.sh --full` runs the set.
