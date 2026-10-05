# NVBringup: NVIDIA Turing GPUs on macOS

NVBringup is a macOS kernel extension that brings an NVIDIA Turing GPU up on an Intel Mac or
hackintosh. It boots NVIDIA's GSP-RM firmware and gives user space a GPU interface. Together
with a macOS port of Mesa's open-source Vulkan driver (NVK), it lets Vulkan compute programs,
such as llama.cpp for local LLMs, run on the NVIDIA GPU. Optionally, a Metal driver built on NVK
(nvmetal) lets Metal apps that opt in use the GPU too.

> **Status: research prototype.** This is a proof of concept, published so the approach and the
> measurements can be read and reproduced. It is not a supported product or a community project:
> there is no release schedule, no compatibility promise, and pull requests and feature requests
> aren't being accepted. It was built and tested on one laptop with one GPU (GTX 1650, TU117).
> See [Status](#status).
>
> **Warning: experimental kernel software.** NVBringup runs in the macOS kernel and writes to GPU
> hardware and its firmware. A bug can cause kernel panics, data loss, an unbootable system or a
> GPU left unusable until power-cycled. Use it at your own risk, test from a separate copy of
> your EFI, and keep backups. See [Disclaimer](#disclaimer).

**Supported chips:** every Turing chip that GSP-RM r570 supports: **TU102, TU104, TU106**
(GeForce RTX 20 series, Quadro RTX, Titan RTX) and **TU116, TU117** (GeForce GTX 16 series).
Developed and tested on a laptop GeForce GTX 1650 (TU117). The other chips use the same code
paths but have **not been tested**.

**Experimental, never run on hardware:** Ampere **GA102..GA107** (GeForce RTX 30 series) and Ada
**AD102..AD107** (RTX 40 series). The kext has a HAL for their boot path (PKC-signed FWSEC v3
and booters loaded by the falcon's DMA engine, following NVIDIA's r570 and nouveau's GA102
code), but it was written without such a GPU. It identifies the chip and parses its firmware
on any of them; FWSEC and GSP-RM only run with the boot-arg `nvexperimental=1`. See
[docs/firmware-and-boot.md](docs/firmware-and-boot.md).

## How it fits together

| Layer | Component | Where |
|---|---|---|
| Kernel | `NVBringup.kext`: runs FWSEC at boot, boots GSP-RM, manages memory, channels, interrupts and power | this repository, loaded by OpenCore |
| Boot | LaunchDaemon that hands the GSP-RM firmware (27 MB) to the kext after boot | this repository (`tools/install_daemon.sh`) |
| Driver | NVK, Mesa's Vulkan driver, with a macOS backend that talks to the kext through `libnvmac` | [nvbringup-mesa](https://github.com/kvarun-p/nvbringup-mesa), branch `main` |
| Metal (optional) | nvmetal: a Metal driver on NVK, loaded by Metal.framework as `NVMetal.bundle`; it translates Metal shaders (AIR) for NVK's compiler | nvbringup-mesa, branch `main` |
| Metal (optional) | `NVMetalAccel.kext`: an IOAccelerator that makes Metal.framework list the GPU and load `NVMetal.bundle` | this repository (`accel/`) |
| Loader | Khronos Vulkan loader, which finds NVK through an ICD manifest | built from Khronos sources |
| Apps | Any Vulkan compute program, for example llama.cpp | [llama.cpp fork](https://github.com/kvarun-p/llama.cpp/tree/nvk-tuning) (optional) |
| UI | GPU Monitor: menu bar app and widget | this repository (`monitor/`) |

## What it can do

- **Vulkan 1.4 compute** through NVK: memory allocation, user-managed VA binding, compute
  queues, timeline semaphores, and multiple processes at once, each isolated in its own GPU
  address space.
- **Local LLMs** with llama.cpp's Vulkan backend. On the GTX 1650: Qwen2.5 0.5B Q4_K_M at about
  2,200 tokens/s prompt processing and about 92–97 tokens/s generation (about 112–114 with boot-arg
  `nvkmapvram=1`, see the limitations), Qwen2.5 1.5B at about 800 and 54–55 (about 60 with `nvkmapvram=1`).
  llama.cpp's backend tests pass (18,987 of 18,990; the rest are f16 SQRT precision).
- **Runtime power management:** the GPU is powered off 30 s after the last program closes it
  and powered back on (about 2 s) when a program opens it. On the test laptop this
  saves about 3 W. Needs ACPI power methods on the GPU, which laptops with switchable graphics
  have; see the limitations.
- **Sleep and wake, restart and shutdown:** GSP-RM is shut down cleanly and brought back.
- **Interrupt-driven waits:** MSI non-stall interrupts wake waiting programs instead of polling.
- **Without root:** once installed, the user logged in at the console can use the GPU.
- **GPU Monitor**, a menu bar app and widget: usage, temperature, VRAM, and a power switch.
- **Metal (optional, opt-in per app):** compute (llama.cpp's Metal backend passes all of ggml's
  per-op tests) and graphics: render pipelines, textures, argument buffers and heaps, MSAA,
  tessellation, framebuffer fetch, layered rendering, IOSurfaces, and windows through
  CAMetalLayer, which WindowServer composites on the Intel GPU (unmodified wgpu and MoltenVK
  apps run, for example vkcube). Only apps listed in an allow file get the GPU.
- **H.264 video decode** through Vulkan Video (`NVK_EXPERIMENTAL=video`), on NVDEC.

## What it can't do

- **No display output.** NVBringup drives no monitors. A display must come from another GPU,
  such as the Intel iGPU on a laptop, the path the internal panel uses on Optimus laptops.
- **No OpenGL, OpenCL or CUDA.** Metal only with the optional Metal step, and only for apps
  that opt in: macOS itself (WindowServer, system apps) keeps using the Intel GPU. Metal
  features Turing or NVK lack aren't there: mesh and object shaders, ray tracing, imageblocks and
  tile shaders, stencil export. Metal needs SIP partly off (see Requirements).
- **No Vulkan window presentation.** NVK is built without a window system; windows work only
  through Metal (CAMetalLayer).
- **No video encode**, decode only H.264 and only through Vulkan Video (not VideoToolbox, so
  QuickTime and Safari don't use it), and no copy-only transfer queue.
- **Only Turing has been tested.** Pascal and older have no GSP. Ampere GA10x and Ada have a HAL
  here that has never run (see Supported chips); Hopper and Blackwell need different firmware
  and code paths; GA100 is left out.
- **Intel x86-64 only**, with OpenCore loading the kext. Apple Silicon can't use it.
- **No CPU-visible VRAM for apps.** Memory a program maps for the CPU comes from system memory,
  because macOS gives the driver no way to revoke a CPU mapping of VRAM once a program has copied
  it. This costs about 6–15 % generation speed in llama.cpp. Boot-arg `nvkmapvram=1` wins it back
  for about 42 program runs per boot; later runs go at the default speed until a restart. See [docs/bar1-cpu-mappings.md](docs/bar1-cpu-mappings.md).
- **One GPU**, the first Turing chip found. No multi-GPU.
- **Runtime power-off needs ACPI `_OFF` plus `_ON` or `_PS0`** on the GPU's ACPI node. Desktop
  cards usually have neither; they stay powered, and the power switch is disabled.
- **Dual boot with Windows:** shut down fully, don't restart, when switching operating systems.
  A GPU that another OS left in a secured state (WPR2 set) can't be started until it loses power.
- **Not upstream.** The Mesa changes (NVK on macOS, NAK fixes) are patches against a Mesa
  release in [nvbringup-mesa](https://github.com/kvarun-p/nvbringup-mesa) (private for
  now); the llama.cpp change is in a fork.

## Requirements

| | |
|---|---|
| Computer | Intel x86-64 Mac or hackintosh with an NVIDIA Turing GPU |
| macOS | Sonoma 14 (developed on 14.8.9). Other versions are untested |
| Boot loader | OpenCore, to load the kext (Kernel → Add). SIP stays enabled, except for the optional Metal step: `csr-active-config` `0x803` (kext signing, filesystem protection and authenticated root off) and FileVault off |
| GPU visibility | The GPU must be powered and on the PCI bus. No SSDT that powers it off (the ACPI patches many hackintosh EFIs ship to disable the discrete GPU), no `-wegnoegpu`, no `disable-gpu` property |
| Firmware | NVIDIA GSP firmware r570.144 from linux-firmware (not included; step 1). The kext is built for exactly this version |
| Build tools | Xcode or the Command Line Tools (macOS SDK 14 or newer) |
| For Vulkan | Mesa's build tools, the Khronos Vulkan loader and headers (see nvbringup-mesa), and `glslc` from shaderc for llama.cpp's shaders |
| For Metal | LLVM 20 or newer (`sudo port install llvm-20`), to build and run nvmetal |

Root is needed once, to install the boot daemon (and for the Metal step, to install its kext and
bundle). Everything else runs as the console user.

## Installation

`tools/install.sh` does the macOS side of steps 1–4, 7 and 8 for you, once the GPU is visible
and Mesa is built (step 5). It downloads the missing firmware, runs `make`, backs up the EFI and
copies the kext into `EFI/OC/Kexts`, installs the boot daemon, and, if you've set up the Metal
step, NVMetalAccel and `NVMetal.bundle`. It skips anything already current, so the same command
updates an installation. It never edits `config.plist`: it prints the entries and boot-args you
still need to add. Run it as yourself (it asks for sudo where needed), with `-n` first to see what
it would do:

```bash
tools/install.sh -n          # dry run: print the commands
tools/install.sh             # every component that applies; or name some: firmware build efi daemon accel bundle monitor
```

The steps below are what it does, and what you do by hand on a first install.

### 1. Firmware

Download the GSP-RM r570.144 files from
[linux-firmware](https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/tree/nvidia)
into `firmware/nvidia/`, keeping the directory layout:

| File | For |
|---|---|
| `tu102/gsp/gsp-570.144.bin` | all Turing chips |
| `tu102/gsp/bootloader-570.144.bin` | all Turing chips |
| `tu102/gsp/gen_bootloader-570.144.bin` | all Turing chips (built into the kext) |
| `tu102/gsp/booter_load-570.144.bin`, `booter_unload-570.144.bin` | TU102, TU104, TU106 |
| `tu116/gsp/booter_load-570.144.bin`, `booter_unload-570.144.bin` | TU116, TU117 |
| `ga102/gsp/gsp-570.144.bin`, `bootloader-570.144.bin` | Ampere GA10x (experimental) |
| `ad102/gsp/gsp-570.144.bin`, `bootloader-570.144.bin` | Ada AD10x (experimental) |
| `<chip>/gsp/booter_load-570.144.bin`, `booter_unload-570.144.bin` | each GA10x or AD10x chip (`ga104`, `ad107`, ...) has its own |

```bash
base=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/nvidia
for f in tu102/gsp/gsp tu102/gsp/bootloader tu102/gsp/gen_bootloader \
         tu102/gsp/booter_load tu102/gsp/booter_unload tu116/gsp/booter_load tu116/gsp/booter_unload; do
    mkdir -p firmware/nvidia/$(dirname $f) && curl -fL -o firmware/nvidia/$f-570.144.bin $base/$f-570.144.bin
done
```

### 2. Build the kext and tools

```bash
make            # build/NVBringup.kext (ad-hoc signed), nvgsp, nvtest, vbios_tool
make test       # VBIOS/firmware parser self-test
```

### 3. Load the kext with OpenCore

1. Copy `build/NVBringup.kext` to `EFI/OC/Kexts/`.
2. Add it to `config.plist` → Kernel → Add: `BundlePath` `NVBringup.kext`, `ExecutablePath`
   `Contents/MacOS/NVBringup`, `PlistPath` `Contents/Info.plist`, `Arch` `x86_64`,
   `MinKernel` `23.0.0`, `Enabled` true.
3. Add the boot-args `nvfwsec=1 nvgsp=1`. They opt in to running NVIDIA's firmware; without
   them the kext only identifies the GPU.
4. Make sure the GPU is visible (see Requirements).
5. **Laptops with switchable graphics:** if exposing the GPU breaks the panel's brightness
   control, hide it from macOS's graphics stack. Add a DeviceProperties entry for the GPU's PCI
   path (for example `PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x0)`) with `class-code` =
   `<00 00 FF 00>` (data). NVBringup reads the real class from config space and still attaches.
6. Test from a USB copy of your EFI first, and keep a backup.

Reboot, then check that the kext found the chip and that FWSEC ran:

```bash
ioreg -r -c NVBringup -d 1 | grep -E 'NVChipName|NVFrtsResult'   # expect NVFrtsResult = "success"
```

### 4. Install the boot daemon

```bash
sudo tools/install_daemon.sh     # nvgsp and firmware to /Library/Application Support/NVBringup
```

From the next boot on, a LaunchDaemon boots GSP-RM automatically. Boot logs are kept in
`/Library/Logs/NVBringup/`. To boot GSP-RM now without rebooting, run `sudo build/nvgsp boot`
from the repository (it reads `firmware/nvidia`). Check the result:

```bash
ioreg -r -c NVBringup -d 1 | grep NVGspResult   # "running", or "powered off (idle)" after 30 s idle
```

### 5. Build NVK (Mesa)

Follow the README of [nvbringup-mesa](https://github.com/kvarun-p/nvbringup-mesa), branch
`main`: it fetches Mesa at the commit the patches are for, applies the 70 patches (the macOS
backend `nvkmd/macos`, NAK compiler fixes for Turing, video decode, and nvmetal), builds NVK and
`NVMetal.bundle`, installs the Khronos Vulkan loader (macOS has none), and registers NVK in
`~/.config/vulkan/icd.d/nouveau_icd.x86_64.json`. (Branch `26.2` has only the first 7 patches,
on Mesa 26.2.3: Vulkan compute, no Metal or video.)

Then run the Vulkan compute test against your NVK build:

```bash
make build/vktest NVB_VULKAN_PREFIX=/usr/local          # where the Vulkan headers are
build/vktest /path/to/mesa/build/src/nouveau/vulkan/libvulkan_nouveau.dylib
```

It prints `ok:` lines for device creation, a compute dispatch and a buffer copy.

### 6. llama.cpp (optional)

The fork [kvarun-p/llama.cpp](https://github.com/kvarun-p/llama.cpp/tree/nvk-tuning),
branch `nvk-tuning`, is upstream release `v0.5.0` plus one change: the larger mat-vec workgroup
on NVK, which speeds up generation on models narrower than 1,024 by roughly 30 % (Qwen2.5 0.5B:
about 72 to 92 tokens/s with default settings, about 112–114 with `nvkmapvram=1`; 87 to 119 was measured
before the BAR1 fix, see docs/nvk-and-llama.md). Upstream llama.cpp works too, only slower on those models.

```bash
git clone -b nvk-tuning https://github.com/kvarun-p/llama.cpp.git && cd llama.cpp
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DGGML_VULKAN=ON -DGGML_METAL=OFF \
    -DGGML_BLAS=OFF -DLLAMA_CURL=OFF -DCMAKE_PREFIX_PATH=/usr/local   # where Vulkan and glslc are
cmake --build build
build/bin/llama-server --list-devices     # expect: Vulkan0: NVIDIA GeForce ... (NVK TU1xx)
```

Keep `-DGGML_METAL=OFF -DGGML_BLAS=OFF`. The defaults on macOS turn both on, and a build with Metal
runs the model on the Mac's own GPU instead, which can hang macOS (a GPU reset, then a
WindowServer watchdog reboot); the BLAS module also shows up as a zero-memory device numbered
first. `--list-devices` should list only the NVK GPU.

### 7. Metal acceleration (optional)

Lets Metal apps you choose use the GPU. It changes the system volume, needs SIP partly off and
is the least tested part: skip it if Vulkan is enough. Needs `NVMetal.bundle` from step 5.

1. In `config.plist`: `csr-active-config` `03080000` (0x803) under NVRAM → Add →
   `7C436110-AB2A-4BBB-A880-FE41995C9F82`, and add `nvaccel=1` to the boot-args. Turn FileVault
   off. Reboot.
2. Build and install the accelerator kext (it can't go on the EFI: it links against a family
   in the system kernel collection, so it goes in the auxiliary one):

   ```bash
   make -C accel
   sudo cp -R accel/build/NVMetalAccel.kext /Library/Extensions/
   sudo chown -R root:wheel /Library/Extensions/NVMetalAccel.kext
   sudo kmutil load -p /Library/Extensions/NVMetalAccel.kext   # approve in System Settings → Privacy & Security
   ```

   Reboot, then `kmutil showloaded | grep nvmetalaccel` should list it.
3. Install `NVMetal.bundle` into `/System/Library/Extensions` (it mounts the system volume and
   blesses a new snapshot, as OpenCore Legacy Patcher does), then reboot:

   ```bash
   sudo accel/tools/nvmetal_root_install.sh install /path/to/mesa/build/src/nouveau/air/NVMetal.bundle
   ```

   `remove` takes it out again; `revert` boots Apple's last sealed snapshot (undoes every root
   change). Keep the Mesa build directory: the bundle loads NVK from there.
4. Let apps in: list their executable names or paths, one per line, in
   `/Library/Preferences/io.github.kvarun-p.nvmetal.allow` (root-owned), or run a program with
   `NVMETAL_ALLOW=1`. Apps that are let in see the GPU as their default Metal device. System
   executables (in `/System`, `/usr`, ...) and system users are let in only with
   `NVMETAL_ALLOW=1`, never through the file. `sudo touch /Library/Preferences/io.github.kvarun-p.nvmetal.disabled`
   turns it off, without a reboot, for every process started after that.

```bash
echo llama-server | sudo tee -a /Library/Preferences/io.github.kvarun-p.nvmetal.allow
```

Without installing anything, `DYLD_INSERT_LIBRARIES=/path/to/mesa/build/src/nouveau/air/libnvmetal.dylib
NVMETAL_DEFAULT=1 program` runs one program (not a SIP-protected one) on nvmetal. That also
works with SIP fully on.

### 8. GPU Monitor (optional)

```bash
monitor/build.sh     # builds and installs ~/Applications/GPU Monitor.app
```

Enable "Launch at login" in its menu. Add the widget from the desktop's widget gallery.

## Verify the installation

```bash
tools/verify_install.sh          # checks every step above; read-only, doesn't wake the GPU
tools/verify_install.sh --full   # also runs nvtest, vktest, a Metal test and a short llama-bench
```

Each step reports PASS, WARN or FAIL with what to fix, for example a missing boot-arg, a
kext on the EFI that differs from the build, a daemon that needs reinstalling, or a Vulkan
manifest that points nowhere. The Metal step (optional, `accel/README.md`) checks NVMetalAccel,
the SIP settings, whether the NVMetal.bundle on the system volume is the current build, the
kill switch and the apps let in; `--full` runs `tools/metaltest.m` (compute, render, GPU times)
through it and reports whether the kext supports host memory import. The exit code is non-zero
if anything failed.

### Per-machine paths (`local.env`)

Set paths that differ per machine in the environment or in `local.env` at the repository root.
Git ignores the file; the Makefile, `verify_install.sh` and `run-llama.sh` read it:

```bash
NVB_VULKAN_PREFIX=/usr/local                          # Vulkan loader and headers
NVB_LLAMA_SERVER=/path/to/llama.cpp/build/bin/llama-server
NVB_LLAMA_BIN=/path/to/llama.cpp/build/bin           # for run-llama.sh
NVB_BENCH_MODEL=/path/to/model.gguf                   # for --full and run-llama.sh
NVB_VK_ICD=/path/to/nouveau_icd.x86_64.json           # optional, for run-llama.sh
NVB_NVK_LIB=/path/to/mesa/build/src/nouveau/vulkan/libvulkan_nouveau.dylib   # for vktest
NVB_NVMETAL_BUNDLE=/path/to/mesa/build/src/nouveau/air/NVMetal.bundle   # the build the Metal step compares with
```

## Run a local LLM on the GPU

Serve a GGUF model with llama.cpp and query its OpenAI-compatible API:

```bash
llama-server -m ~/models/qwen2.5-1.5b-instruct-q4_k_m.gguf -ngl 99 --host 127.0.0.1 --port 8080

curl -s http://127.0.0.1:8080/v1/chat/completions -H 'Content-Type: application/json' \
    -d '{"messages": [{"role": "user", "content": "What is the capital of France?"}]}'
```

`-ngl 99` puts every layer on the GPU. A model and its context must fit in VRAM (4 GB on a GTX
1650: models up to about 3B parameters at Q4). Run as the console user; other users need the
`nvgpu_users=1` boot-arg. On machines with several Vulkan devices, pick the GPU with
`GGML_VK_VISIBLE_DEVICES=0`.

Power behaviour: in *auto* mode the GPU switches off 30 s after the last process using it
exits. llama-server's own idle sleep (`--sleep-idle-seconds`) frees the model but keeps its
Vulkan device open, so the GPU stays on until the server (or, in router mode, the model
process) exits.

## Everyday use

| Task | Command |
|---|---|
| GPU and power status | `build/nvgsp power status` |
| Power mode | `build/nvgsp power auto`, `on` or `off` (no sudo; also in GPU Monitor) |
| Interrupts | `sudo build/nvgsp intr on`, `off` or `status` |
| GSP-RM log lines | `build/nvgsp status` |
| Full kext log | `ioreg -a -r -c NVBringup -d 1 \| plutil -extract 0.NVLog raw -o - -` |
| Test the interface | `build/nvtest` |

In *auto* mode, the first program to use the GPU after it powered off waits about 2 s while it
powers on. *On* keeps it powered; *off* keeps it off and hidden from Vulkan. The mode isn't
saved: every boot starts in *auto* (or *on* with `nvidle=0`, or on machines without ACPI power
control).

## Update after changes

The kext, `nvgsp` and NVK share one interface (`src/nv_uapi.h`, `NVMAC_ABI_VERSION`). After
pulling changes, run `tools/install.sh`: it rebuilds and reinstalls whatever differs from the
build. By hand:

| Changed | Do |
|---|---|
| `src/` | `make`, copy `build/NVBringup.kext` to `EFI/OC/Kexts/`, reboot |
| `tools/nvgsp.cpp` or `tools/daemon/` | `make`, then `sudo tools/install_daemon.sh` |
| `src/nv_uapi.h` or `tools/libnvmac.*` | also rebuild NVK with matching copies (nvbringup-mesa) |
| `accel/` | `make -C accel`, copy the kext to `/Library/Extensions` (step 7), reboot |
| the Mesa build (NVMetal.bundle) | `sudo accel/tools/nvmetal_root_install.sh install <bundle>`, reboot |
| `monitor/` | `monitor/build.sh` |

`tools/verify_install.sh` reports a kext on the EFI, a daemon, or an installed NVMetal.bundle
that differs from the build.

## Uninstall

`tools/uninstall.sh` (`-n` for a dry run) removes NVMetal.bundle, NVMetalAccel, the Metal allow
list and kill switch, the boot daemon, your NVK Vulkan manifest and GPU Monitor, then lists the
EFI changes to undo by hand (step 1 below, and `csr-active-config`). It keeps the logs, the
firmware and your Mesa and llama.cpp builds. By hand:

1. Remove `NVBringup.kext` from `EFI/OC/Kexts/`, its Kernel → Add entry, and the `nv*`
   boot-args from `config.plist`.
2. `sudo tools/uninstall_daemon.sh` (logs stay in `/Library/Logs/NVBringup/`).
3. Delete `~/.config/vulkan/icd.d/nouveau_icd.x86_64.json` and your Mesa build.
4. Quit GPU Monitor, turn off "Launch at login", and delete `~/Applications/GPU Monitor.app`.
5. Metal step: `sudo accel/tools/nvmetal_root_install.sh remove` (or `revert`), reboot; delete
   `/Library/Extensions/NVMetalAccel.kext` and the files in `/Library/Preferences/` named
   `io.github.kvarun-p.nvmetal.*`; remove `nvaccel=1` and restore `csr-active-config`.

## Boot-args

| Boot-arg | Effect |
|---|---|
| `nvfwsec=1` | Run FWSEC-FRTS at boot (required for GSP-RM) |
| `nvgsp=1` | Allow booting GSP-RM (the daemon does it after login) |
| `nvidle=<s>` | Idle time before power-off, default 30; `0` keeps the GPU on |
| `nvintr=0` | Keep interrupts off (waits poll) |
| `nvboost=<n>` | P-state boost after each submission: `2` adaptive (default: full clocks on the first submission after idle and while the GPU stays busy, cleared after 150 ms idle; `nvgsp perf policy adaptive burst=1level` starts one level up instead and goes to full clocks at ≥50 % busy over ~80 ms), `1` fixed (full clocks for 2 s), `0` off (GSP-RM alone takes ~250 ms of load to raise the memory clock). `build/nvgsp perf` shows the P-state; `nvgsp perf policy …` changes the policy and its thresholds at runtime; `nvgsp perf watch` follows the P-state |
| `nvgpu_users=1` | Let any user open the GPU, not only root and the console user |
| `nvtest=1` | Run the kext's boot-time self-tests |
| `nvexperimental=1` | Let chips whose support is marked experimental run FWSEC and GSP-RM: Ampere GA10x and Ada, whose HAL has never run on hardware (see docs/firmware-and-boot.md) |
| `nvaccel=1` | Start NVMetalAccel, so Metal.framework lists the GPU (Metal step) |
| `nvkmapvram=1` | NVK keeps its push buffers and descriptors in CPU-mapped VRAM: faster generation (+15 % on a 0.5B model, +6 % on 3B), but each program run holds ~2.7 MiB of BAR1 until a restart; after ~42 runs it falls back to system memory (default speed). `desc` / `cmd:<n>` select parts; see [docs/bar1-cpu-mappings.md](docs/bar1-cpu-mappings.md) |

## Troubleshooting

- **Start with `tools/verify_install.sh`:** it points at the failing step. More recipes (logs,
  device-lost, power) are in [docs/howto.md](docs/howto.md).
- **`NVFrtsResult` is "refused" with "WPR2 already set up":** the GPU is still secured from a
  previous session, usually after restarting from another OS. Shut down fully and power on.
- **GSP-RM doesn't boot:** check `/Library/Logs/NVBringup/boot-*.txt`. The boot-args must
  include `nvfwsec=1 nvgsp=1`, and the firmware must be installed.
- **Vulkan finds no device:** check `build/nvgsp power status` (mode *off* hides the GPU) and
  that the loader finds the NVK manifest (`VK_LOADER_DEBUG=driver`).
- **"NVBringup: not privileged":** the program runs as a user who isn't logged in at the
  console. Run it as the console user or root, or boot with `nvgpu_users=1`.
- **"NVBringup: open failed" (unsupported):** NVK and the kext were built from different
  `nv_uapi.h` versions. Rebuild the older one (see Update after changes).
- **Brightness control stops working on a laptop:** use the `class-code` property from step 3.
- **A Metal app doesn't get the GPU:** it must be in the allow file (or run with
  `NVMETAL_ALLOW=1`), and not be a system executable. `log show --last 5m --predicate
  'eventMessage CONTAINS "NVMetal"'` says why a process was refused. If the screen or an app
  misbehaves, the kill file (step 7) turns nvmetal off at once.
- **Known issue:** a debug-optimized NVK build can end a program with
  `Assertion failed: (cache->object_cache->entries == 0), function vk_pipeline_cache_destroy`.
  It happens at teardown, after the work is done, and doesn't affect results.

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The kext: chips and the per-architecture HAL (`nv_hal.*`, `hal_tu1xx.cpp` for Turing, `hal_ga10x.cpp` for Ampere GA10x and Ada), GSP-RM boot (`NVGsp.cpp`), GPU interface (`NVGpu.cpp`), runtime power (`NVPower.cpp`), MMU, VRAM heap, VBIOS/FWSEC/GSP firmware parsers, r570 structures (`nv_gsp_rm`), user-space ABI (`nv_uapi.h`) |
| `tools/` | `nvgsp`, `nvtest`, `vktest`, `libnvmac` (C library over `nv_uapi.h`), `vbios_tool`, the boot daemon (`daemon/`, `install_daemon.sh`), `install.sh`, `uninstall.sh`, `verify_install.sh`, `run-llama.sh`, `gsp_test.sh` |
| `accel/` | NVMetalAccel, the IOAccelerator kext for Metal, and `tools/nvmetal_root_install.sh` |
| `monitor/` | GPU Monitor (SwiftUI menu bar app and WidgetKit widget) |
| `docs/` | Task recipes ([howto](docs/howto.md)) and design decisions: what was chosen, the alternatives, and the measurements behind them ([index](docs/README.md)) |

`src/nv_uapi.h` and `tools/libnvmac.*` are the kext's user-space ABI. The Mesa patches carry
copies in `src/nouveau/vulkan/nvkmd/macos/`; keep them in sync.

## Status

NVBringup is a prototype. The code and [docs/](docs/README.md) show what worked, what was tried
instead, and the evidence, which is the main value of publishing it.

- **Maintenance:** best effort, with no guarantees. Issues may go unanswered, and the design can
  change or be abandoned.
- **Contributions:** not being accepted for now. You are free to fork it under the MIT license.
- **Security problems:** see [SECURITY.md](SECURITY.md).

## Credits and license

MIT, see `LICENSE`. Register definitions and GSP-RM structures follow NVIDIA's
[open-gpu-kernel-modules](https://github.com/NVIDIA/open-gpu-kernel-modules) (MIT). Boot
sequences follow nouveau and nova-core in Linux. NVK and NAK are part of
[Mesa](https://mesa3d.org). NVIDIA's firmware comes from linux-firmware under NVIDIA's license
and isn't redistributed here. All third-party components, licenses and trademarks are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

## Disclaimer

- **No warranty.** The software is provided "as is", as the MIT license says in full. The author
  is not liable for damage to hardware, data or software, or for any other loss arising from its
  use.
- **Prototype.** It is research code, not a finished or supported product. Behaviour, interfaces
  and boot-args can change without notice.
- **Risk.** It is a kernel extension that loads firmware onto, and programs, a GPU using
  interfaces NVIDIA doesn't document for this use. It has been tested on one machine and one
  chip; other chips, Macs and macOS versions are untested. Not for production or
  safety-critical use.
- **No affiliation.** NVBringup is an independent project. It isn't made, endorsed, sponsored or
  supported by NVIDIA, Apple, Intel, Khronos, the Mesa or OpenCore projects, or any of the
  other companies and projects named here. Names and trademarks belong to their owners and are
  used only to describe compatibility.
- **Your own hardware and software.** Use it only on hardware you own or are authorized to
  modify. Running an unsigned or ad-hoc-signed kext through OpenCore departs from Apple's
  supported configuration, and running macOS on non-Apple hardware may not comply with Apple's
  software license agreement; check the terms that apply to you. Modifying a device may void its
  warranty.
- **Third-party files.** NVIDIA's GSP-RM firmware isn't included and is subject to NVIDIA's
  license; download it yourself. A GPU VBIOS is copyrighted and machine-specific; don't share or
  commit a dump.
- **Security.** The kext's user interface is designed to isolate programs from one another, but
  it hasn't had an independent security audit. Don't rely on it to contain untrusted code.
