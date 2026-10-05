# Firmware and boot

Boot order: FWSEC-FRTS at kext start (sets up the WPR2 protected region), then GSP-RM after
login, when the daemon hands the firmware to the kext.

## Firmware version: r570.144

linux-firmware ships two GSP-RM versions for Turing, 535.113.01 and 570.144. The message
interface differs between versions, so the driver targets exactly one: **570.144**, the newer,
and the one nova-core follows. NVIDIA's own 570.144 driver package was checked: its
`gsp_tu10x.bin` is byte-identical to linux-firmware's `gsp-570.144.bin`, and it contains no
log format strings (`gsp_log_*`), so GSP-RM's logs can only be decoded raw.

## Where structure layouts come from

| Option | Verdict |
|---|---|
| nouveau's copies of NVIDIA's headers | Copies can be trimmed or lag behind; the original was available |
| nova-core's bindgen output | GPL, and not the authority |
| Transcribing by hand | Error-prone for deeply nested control structs |
| **NVIDIA's open-gpu-kernel-modules, tag 570.144** | **Chosen**: MIT, the authority; names kept as NVIDIA's so each struct traces back to its header |

Plain structs with `static_assert`s on sizes and offsets (natural alignment matches the RISC-V
GSP). For large nested structs such as `GspStaticConfigInfo`, whose header needs NVIDIA's kernel
build, `tools/gsp_layout_probe.c` compiles NVIDIA's SDK headers and generates the offsets as a
header (`src/nv_gsp_static.h`, `make gsp_static_h`).

Sequences follow NVIDIA's r570 Turing code (`kgspBootstrap_TU102`,
`kgspExecuteBooterLoad_TU102`, `kflcnReset_TU102`, `kgspExecuteSequencerBuffer`), cross-checked
against nouveau and nova-core; register offsets were cross-checked against nova-core before any
write.

## FWSEC-FRTS: bootloader plus DMA, not PIO

The first plan loaded FWSEC by PIO (register writes only, no DMA). Both nouveau and nova-core
load Turing FWSEC through a generic bootloader that DMA-copies it from system memory. Neither
loads it by PIO, so there was no working example of that. The driver **follows the bootloader
path**. VT-d
is active on the machine, so every GPU DMA buffer is mapped with `IODMACommand` and the GPU gets
an IOVA.

On GA10x and Ada the VBIOS carries a v3 descriptor instead: the ucode is a PKC-signed HS image
(RSA-3K signatures, one per fuse version, stored after the descriptor) that the GSP falcon's own
DMA engine loads and its boot ROM verifies; there is no generic bootloader. See the HAL section.

Other choices here:
- **Bootloader format:** linux-firmware ships `.bin` files, not nova-core's `.tlv`, so they're
  parsed the way nouveau does.
- **VBIOS source:** ACPI `_ROM` first; on the test machine it holds only the PCI-AT and EFI images, so
  the kext falls back to the PROM window in BAR0, which has the full ROM with the FWSEC images.
- **FRTS placement:** nova-core reads the VGA workspace base and uses the top 1 MiB of VRAM when
  it isn't valid. The first assumption (top 128 KiB) was corrected before running.
- **Gates:** boot-arg `nvfwsec=1`, a Turing chip, WPR2 not already set up, and the computed
  FRTS range in its expected place (1 MiB directly below the VGA workspace at the top of
  VRAM). On timeout the falcon is reset before its DMA buffer is unmapped.

## GSP-RM firmware comes from user space

An OpenCore-injected kext can't read files itself. Embedding the firmware would make the kext
about 29 MB, loaded into the kernel at every boot, and would tie each kext build to one firmware
file. **A command-line tool passes the files through the kext's user client** (IOKit hands inputs
over 4 KiB to the kernel as a memory descriptor), then a separate call boots. A GSP-RM boot
therefore happens after startup and can't hold it up. A running GSP-RM can be unloaded and booted
again (`nvgsp unload`, `nvgsp boot`) without rebooting; only a failed boot needs a cold boot
(see below).

**Automatic start:** a LaunchDaemon (runs at boot as root) rather than a LaunchAgent (after login,
as the user; booting needs root). It was added after manual boots had succeeded 9 times in a
row, and it does nothing unless boot-args contain `nvgsp=1`.

## GSP-RM boot details

- **LibOS log regions:** nova-core passes three; r570 passes four on Turing (adds `LOGMNOC`).
  NVIDIA's set is used: the firmware looks regions up by id, so an extra region is ignored if
  unused, while a missing one could break a lookup.
- **Registry:** nova-core's set (`RMForcePcieConfigSave`, `RMSecBusResetEnable`,
  `RMDevidCheckIgnore` = 1).
- **Heap size:** NVIDIA's formula: 8 MiB + 96 KiB per GB of VRAM (rounded up to 1 MiB) + 96 MiB,
  clamped to 64–256 MiB; 105 MiB on a 4 GB card. nova-core's `− 1` on the 256 MiB maximum is
  dropped (NVIDIA doesn't have it).
- **Sequencer:** executed with bounds checks (register addresses inside BAR0 and aligned,
  polls capped at 10 s).
  `CORE_RESUME` was first refused because of its name; it turned out to be part of every Turing
  boot (GSP-RM restarts itself through SEC2). Lesson: check how an opcode is used before
  refusing it.
- **Interrupts during boot:** a level-triggered INTx that nothing services could storm the
  system, so the kext sets PCI *Interrupt Disable* before booting and polls (interrupts came
  later; see [gpu-interface.md](gpu-interface.md)).
- **Failure:** reset GSP and SEC2 (stops their DMA) before unmapping buffers; no second attempt
  until a cold boot.

## Teardown clears WPR2

WPR2 persists until the GPU is reset. Another OS then finds the GPU locked (Windows can report
Code 43) until a power cycle. The kext follows NVIDIA's unload: RPC `UNLOADING_GUEST_DRIVER`, reset
GSP, FWSEC-SB, then `booter_unload`, which clears WPR2. It runs on sleep, restart, shutdown and
runtime power-off.

Shutdown hooks: `systemWillShutdown()` is only called on drivers in the power plane, which
NVBringup never joined, so in the first version the unload at shutdown and restart never ran. The kext's priority sleep/wake
handler, which `IOPMrootDomain` also notifies on halt and restart, handles power-off and restart
too. After each teardown the kext writes a marker (reason, result, WPR2) to NVRAM and logs it at
the next start (`NVLastTeardown`), since the kernel log of a shutdown isn't kept. A test confirmed
"power off: ok, WPR2 hi 0". Check it after a restart, along with the boot result and the log:

```bash
ioreg -r -c NVBringup -d 1 | grep -E '"NVLastTeardown"|"NVGspResult"|"NVFrtsResult"'
ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o - - | tail -40
```

## All Turing chips

Nothing assumes TU117 or 4 GiB: the gates check for any supported chip, the heap and WPR2 layout
scale with VRAM, and the firmware signature and booters are chosen per chip (TU102/104/106 vs
TU116/117). Only TU117 has been tested.

## Per-architecture code: a HAL inside the kext

What differs between GPU architectures is small next to what GSP-RM does for every one of them,
so it lives in one kext behind a table, as in NVIDIA's own driver (its `_TU102`, `_GA102`
function variants), nouveau and nova-core:

| Part | Where | Turing | Ampere GA10x, Ada |
|---|---|---|---|
| Chips: name, firmware folders, ELF signature section, experimental flag | `nv_chips` in `src/nv_hal.cpp` | TU102/104/106 (booters `tu102`), TU116/117 (`tu116`) | GA102..GA107, AD102..AD107, booters from the chip's own folder; experimental |
| Architecture data: engine classes, SM level, GSP-RM heap and WPR2 layout, display fuse and FB-size registers | `nv_arch` in `src/nv_hal.cpp` | `TURING_*` classes, sm 7.5, LibOS2 heap | `AMPERE_*`/`ADA_*` classes, sm 8.6/8.9, LibOS3 heap (22 MiB carve-out, 88..280 MiB), `NV_USABLE_FB_SIZE_IN_MB` |
| Boot sequences: falcon reset, FWSEC, GSP-RM start, sequencer CORE_RESUME, teardown | `NVBringup::Hal`, `src/hal_tu1xx.cpp`, `src/hal_ga10x.cpp` | generic bootloader + FWSEC v2; booters PIO-loaded on SEC2 | FWSEC v3 and booters are PKC-signed HS ucodes loaded by the falcon's DMA engine; the GSP is switched to RISC-V through `BCR_CTRL` |

Everything else (GSP-RM messages and RPCs, channels, memory, the user-space interface, power) is
shared. The host tools use the same table: `nvgsp boot` picks the firmware files from it, and
`vbios_tool --gsp <dir> <vram> <chip>` checks the firmware and WPR2 layout for any listed chip.

Separate kexts per architecture were considered and rejected: the per-architecture code is a few
hundred lines, while a C++ interface between separately built kexts is fragile, and every extra
kext is another file OpenCore must load in order at boot.

### Ampere GA10x and Ada: what the HAL does differently, and what is untested

`hal_ga10x.cpp` follows r570's `_GA102` functions (`kflcnResetIntoRiscv_GA102`,
`kgspExecuteHsFalcon_GA102`, `kgspExecuteSequencerCommand_GA102`; `kgspBootstrap_TU102` and
`kgspTeardown_TU102` are shared with Turing) and nouveau's `ga102.c` files, which boot the same
firmware on Linux. It has **not run on hardware**: every GA10x and AD10x chip is marked
experimental, so FWSEC and GSP-RM only run with boot-arg `nvexperimental=1`.

- **Falcon reset** (`kflcnReset_TU102` as built for GA102): wait for `HWCFG2.RESET_READY`, toggle
  the engine reset, wait for `HWCFG2.MEM_SCRUBBING`, and switch a falcon that had its RISC-V core
  selected back to falcon mode (`BCR_CTRL`). The GSP is reset *into* RISC-V before the booter
  starts it (`BCR_CTRL` = core select RISC-V, valid, BR fetch). RISC-V is active when
  `NV_PRISCV_RISCV_CPUCTL` says so, not `CORE_SWITCH_RISCV_STATUS`.
- **HS ucodes** (FWSEC v3 from the VBIOS, the booters): one system-memory image, mapped through
  the IOMMU. The falcon DMAs its code into IMEM (secure, tagged with its virtual base, which is
  also the boot vector) and its data into DMEM in 256-byte blocks; the boot ROM is told where
  the PKC signature sits in DMEM, the engine id mask and the ucode id, and verifies it before
  the code runs. The signature is chosen by the chip's fuse version register for that ucode id
  (`NV_FUSE_OPT_FPF_GSP_UCODE<n>_VERSION` for FWSEC, `..._SEC2_...` for the booters): FWSEC's
  descriptor lists the versions it carries signatures for as a bit mask (`nv_fwsec_sig_index`),
  a booter's meta data gives the version of its first signature (`nv_booter_sig_index`, as
  nouveau's `ga100_flcn_fw_signature`).
- **Heap and layout**: GSP-RM is LibOS3 on GA10x and later, so the heap gets a 22 MiB OS
  carve-out and 88..280 MiB bounds (`gsp_fw_heap.h`); the WPR2 layout around it is Turing's.
  On Ada NVIDIA runs a scrubber ucode from its driver package first
  (`kgspExecuteScrubberIfNeeded_AD102`), which lets the heap leave the 256 MiB the VBIOS
  pre-scrubs. linux-firmware doesn't ship it, so the heap stays inside that region, as it does
  for nouveau; the scrubber's handoff state (`BSI_SECURE_SCRATCH_15`) is logged.
- **Registers that moved**: the display fuse (`NV_FUSE_STATUS_OPT_DISPLAY` at 0x820c04 on GA100
  and later) and the VRAM size, which the VBIOS reports in `NV_USABLE_FB_SIZE_IN_MB` (nouveau
  `ga102_fb_vidmem_size`), with `LOCAL_MEMORY_RANGE` as the fallback. The GFW-boot wait
  (`NVPower.cpp`), the WPR2 registers, the interrupt tree, the MSI re-arm, the PRAMIN window and
  the page-table kinds are the same as Turing's in r570.
- **Log buffers**: r570 adds a `LOGKRNL` region for LibOS3; the four Turing regions are passed,
  as nouveau passes three, since the firmware looks them up by id.
- **GA100** (A100) is left out: no FRTS, LibOS2, Turing-style falcons with a GA100-specific
  signature section, and nothing to test it on.

Checks before running on hardware, in order: `vbios_tool <rom>` on a dump of the card's VBIOS
(v3 descriptor, DMEMMAPPER, signature versions), `vbios_tool --gsp firmware/nvidia <vram> GA104`
(or the chip's name) for the firmware and layout, then boot with `nvfwsec=1` alone and read
`NVFrtsResult` and the log before adding `nvgsp=1`.
