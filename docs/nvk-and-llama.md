# NVK, NAK and llama.cpp

The Mesa changes are patches in [nvbringup-mesa](https://github.com/kvarun-p/nvbringup-mesa);
the llama.cpp change is on the `nvk-tuning` branch of the fork.

## The NVK port

- **A new `nvkmd` backend** (`nvkmd/macos`) over `libnvmac`, so NVK above it is unchanged. The
  kernel ABI and `libnvmac` are copies of this repository's `src/nv_uapi.h` and `tools/libnvmac.*`
  and must be kept in sync.
- **Device discovery without libdrm:** NVK normally finds GPUs through libdrm's device list; on
  macOS it uses Mesa's `vk_instance.physical_devices.enumerate` hook and creates the one device
  the kext reports.
- **Listing the GPU doesn't power it on.** At first NVK opened a full GPU connection just to list
  the GPU, for the lifetime of the Vulkan instance, which kept the GPU from ever powering off
  under a long-running server. The physical device now reads the GPU's details through the
  kext's info-only client, and only creating a `VkDevice` opens the GPU.
- **No LLVM:** NVK's two OpenCL C helper kernels (query copy, indirect copy; 85 lines) would need
  LLVM, libclc and SPIRV-Tools to build. They're rewritten with `nir_builder` instead.
- **Link with `-undefined dynamic_lookup`**, so weak symbols for features not built on macOS
  resolve to NULL instead of failing the link. KosmicKrisp, Mesa's Vulkan driver on Metal, does
  the same.
- **Not supported:** sparse binding, a transfer-only queue, video, dma-buf, compression,
  window-system integration.

## NAK fixes for Turing

llama.cpp's backend test suite found two compiler problems on TU117. Both were narrowed down by
experiment before fixing.

### IDP.4A with an immediate operand

`MUL_MAT` with iq1_m weights crashed the GPU (a graphics exception, not an MMU fault).

| variant | result |
|---|---|
| default | device lost |
| `NAK_DEBUG=serial` (no scheduling) | device lost, so not a scheduling bug |
| integer dot product off | passes |
| MMVQ off | passes |

The only shader affected is the one that calls `dotPacked4x8EXT` with a constant, which NAK encodes
as IDP.4A with an immediate operand; everywhere else IDP takes registers. **Fix:** below sm80,
the immediate is moved into a register first. iq1_m then passed 28 of 28.

### Conditional branches need a longer delay

`ARGSORT` failed intermittently (56 of 1960) and never under `NAK_DEBUG=serial`. Runs 17–21
narrowed down what serial mode changes:

| experiment | failures /1960 |
|---|---|
| barriers wait on everything; memory barrier before barriers | no change |
| every variable-latency instruction waits on everything | 28 (fewer, not zero) |
| stall of 15 cycles on every instruction | 0 |
| stall 15 on control instructions only | 0 |
| … on branches only | 0 |
| … on conditional branches only | 0 |
| every branch at least 3 / 5 cycles | 199 / 0 |
| conditional branches at least 4 / 5 / 6 | 517 / 0 / 0 |
| stall 15 on conditional branches in non-uniform blocks / uniform blocks | 0 / 43 |

**Fix:** on sm75, conditional branches in non-uniform (divergent) blocks get a delay of at least 6
cycles (one cycle of margin). The dependencies in the failing shader meet NAK's latency table
exactly, so the root cause is not understood: a hardware erratum or a divergence-timing rule
missing from NAK's model. llama.cpp works around an ARGSORT problem with NVIDIA's own driver on
Turing (`argsort_large.comp`, llama.cpp PR #28975), which suggests NVIDIA's compiler hits
something similar.

To rerun the checks (`test-backend-ops` is built with llama.cpp; `NAK_DEBUG=serial` turns NAK's
instruction scheduling off, which is how both problems were first told apart from scheduling):

```bash
test-backend-ops -b Vulkan0 -o ARGSORT                  # the branch-delay fix
test-backend-ops -b Vulkan0 -o MUL_MAT -p iq1_m         # the IDP.4A fix
NAK_DEBUG=serial test-backend-ops -b Vulkan0 -o ARGSORT # no scheduling, for comparison
```

Result: llama.cpp's backend tests pass 18,987 of 18,990; the rest are f16 `SQRT` precision
(error about 3e-7 against a 1e-7 limit).

## llama.cpp: matrix-vector workgroup size

**Why:** token generation was GPU-bound (run 24: GPU time accounts for all of the wall time) and
the small matrix-vector kernels ran far below the memory bandwidth large ones reach (29 GB/s vs
about 100). llama.cpp's NVIDIA tuning targets the proprietary driver: 128-thread workgroups only
when k ≥ 1024, but the 0.5B model's hot kernels have k = 896.

Sweep (tg128 t/s, 5 repetitions; baseline 87.0 for 0.5B, 61.9 for 1.5B):

| variant | 0.5B | 1.5B |
|---|---|---|
| 128-thread workgroups everywhere | 124.3 | 63.2 |
| always quantize input (MMVQ) | 101.6 | 64.3 |
| both | 128.2 | 60.0 |

Other variants (rows per workgroup, 32-thread workgroups, fusion off) were all slower. The
combination hurts 1.5B, so MMVQ stays at its default.

**Change:** on NVK, matrix-vector kernels use 128-thread workgroups for m ≤ 8192 regardless of
k. Result (run 27): 0.5B generation 87 → 119 t/s (+37 %), 1.5B 62 → 62.9 (unchanged within
noise), `MUL_MAT` and `MUL_MAT_ID` tests 2065 passed, 0 failed.

## Ollama

Ollama's bundled llama.cpp can load a Vulkan backend module built from the matching llama.cpp tag,
without modifying the app bundle. Three obstacles, for the record:
- The module's rpath pointed at the build tree's `libggml-base`, so a second copy loaded; it now
  points at the app's (`@loader_path/..`).
- The app's `llama-server` enforces library validation (hardened runtime), which refused the
  module; a separate copy of `llama-server` and the module were re-signed ad hoc.
- An Ollama bug numbers devices across all backends, including the zero-memory BLAS
  pseudo-device listed first on Intel Macs, so the GPU got index 1 and the runner ran CPU-only.
  Leaving the BLAS module out avoided it.

With those worked around, gemma2:2b generated at 30 t/s on the GPU vs 9.8 on the CPU. Ollama
has since been uninstalled; llama.cpp's `llama-server` is used directly.
