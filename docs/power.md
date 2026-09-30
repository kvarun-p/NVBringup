# Power

## Runtime power-off

**Why:** on battery, the GPU idling with GSP-RM running costs about 3.3 W (2 rounds of
power off / power on, measured from the battery's voltage and current; ±1 W).

**What:** in auto mode (the default), 30 s after the last GPU connection closes (boot-arg
`nvidle=<s>`, `0` keeps it on) the kext tears GSP-RM down (clearing WPR2), saves the PCI
state and calls ACPI `PEGP._OFF`. The next open calls `_ON`, restores the PCI state and boots
GSP-RM again, taking about 1.9 s. `nvgsp power on|off|auto` and GPU Monitor switch modes.

```bash
build/nvgsp power status        # GPU on, mode auto (idle power-off after 30 s); … last power-on 1906 ms
build/nvgsp power on            # keep it on (until reboot or `power auto`)
ioreg -r -c NVBringup -d 1 | grep '"NVPower"'   # State, Mode, PowerOffs, PowerOns, LastPowerOnMs
```

- **`_OFF` was known to work on the test laptop:** its stock EFI calls it
  to disable the GPU for macOS.
- **After power-on, wait for the GPU's own boot firmware** before touching it, as NVIDIA does
  (`kgspWaitForGfwBootOk_TU102`, up to 4 s). The first attempt read the WPR2 registers too early,
  took the value it read (hi 0xe00) as "already set up", and refused to run FRTS. The
  wait takes 145–165 ms.
- **Machines without `_OFF` plus `_ON` or `_PS0`** (most desktop cards) stay powered: the mode is
  fixed at on.
- **Sleep, restart or shutdown with the GPU off:** it's powered on first, without GSP-RM, since
  the PCI family and the next OS expect a powered device; after wake it goes back off.

Verified: 11 off/on cycles clean, sleep with the GPU off, and a long-running
llama.cpp server that frees its models when idle so the GPU can power off.

## Sleep and wake

GSP-RM is unloaded before sleep (open programs get device-lost and must reopen) and booted again
after wake, on a separate thread so wake isn't blocked. The firmware stays in kernel memory from
boot (about 28.5 MB wired) so no user-space help is needed. Verified (before the BAR1 fix, so generation was faster than today's default of
about 92–97 t/s): llama-bench 2273/118.7 t/s after wake, against 2277/118.0 before; `nvtest` 55/55.

## Shutdown and restart

See [firmware-and-boot.md](firmware-and-boot.md#teardown-clears-wpr2): GSP-RM is unloaded and WPR2
cleared, so another OS finds the GPU in a clean state. Before the teardown worked, 9 of 9 warm
restarts on the test laptop also came up with WPR2 cleared. That may be the test platform
resetting the GPU, which other machines may not do, so the teardown is what's relied on. One rule stays: when switching to Windows, **shut down fully** unless the
teardown is known to have run, because a GPU left with WPR2 set can't be started until it loses
power.
