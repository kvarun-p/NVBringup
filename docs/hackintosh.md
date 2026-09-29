# Hackintosh integration

## Hiding the GPU from macOS graphics

On an Optimus laptop the internal panel is on the Intel iGPU. Once the NVIDIA GPU was exposed (the stock EFI's SSDT that disables it switched off so the
GPU stays powered), **brightness control stopped working.** ControlCenter logged `get brightness
failed` on every boot from the first one that exposed the GPU, and on none before. In that first
boot the kext only read the chip ID, so the kext code wasn't the cause.

| Option | Result |
|---|---|
| Take the framebuffer match category (`IOMatchCategory` `IOFramebuffer`, probe score 30000) so macOS's generic framebuffer doesn't attach | Kept (keeps macOS's display stack off a display-less GPU), but brightness stayed broken |
| WhateverGreen's `-wegnoegpu` | Brightness works, but WhateverGreen then terminates the PCI device, and boot waited about 350 s on the kext. Rejected (the kext now handles termination, see below) |
| **Inject `class-code` = `<00 00 FF 00>` for the GPU with `DeviceProperties`** | **Chosen**: macOS graphics no longer sees a display GPU, brightness works, the device stays alive |

To make that work, the kext doesn't match on PCI class through `IOPCIClassMatch` (which reads the
injected property). It matches NVIDIA's vendor ID and `probe()` reads the real class (display,
0x03) from config space, which also skips the GPU's HDMI audio and USB-C functions.

**Termination:** a terminated provider used to leave the kext holding the device, because it
closed it only in `stop()`. `didTerminate` now shuts the hardware down and closes the device, so a
provider going away can't hang boot.

## Dual boot

See [power.md](power.md#shutdown-and-restart): the kext clears the GPU's protected region at
shutdown and restart; when in doubt, shut down fully when switching operating systems.
