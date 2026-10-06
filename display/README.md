# NVFramebuffer

An IOFramebuffer for the display NVBringup lights, so macOS sees the NVIDIA GPU's HDMI output as a
screen. NVBringup does the modeset over GSP-RM (boot-arg `nvdisp=2`) and scans out one pitch-linear
X8R8G8B8 surface in VRAM. This driver hands that surface to IOGraphics as the aperture (its physical
BAR1 range), with the one mode it was lit at, and the sink's EDID over DDC. There's no acceleration:
WindowServer draws into the aperture with the CPU.

- It attaches to IOResources and matches the `NVDisplayLit` resource, which NVBringup publishes only after a modeset
  that scans out. Without `nvdisp=2` it never starts. It sits outside the GPU's PCI device for the same reason as
  NVMetalAccel (`accel/README.md`).
- NVBringup's `NVDisplayFB` property carries everything it needs: the physical address and size of the surface,
  the mode, and the EDID. The two kexts share no symbols.
- `make verify` checks all of NVFramebuffer's vtable slots against the kernel's IOFramebuffer. Run it after a macOS
  update. `make check` resolves the kext's symbols against the running kernel.

Not yet supported: mode changes, hot-plug, gamma, and sleep and wake. GSP-RM goes down with the GPU, and the
framebuffer doesn't follow it yet.

Loading: IOGraphicsFamily is in the system kernel collection, so OpenCore can't inject this kext. It has to go in the
auxiliary collection, the same way as NVMetalAccel:

```
sudo cp -R display/build/NVFramebuffer.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/NVFramebuffer.kext
sudo kmutil load -p /Library/Extensions/NVFramebuffer.kext   # approve in System Settings → Privacy & Security
```

Reboot. With the monitor connected and `nvdisp=2`, `kmutil showloaded | grep nvframebuffer` should list it and
`ioreg -c NVFramebuffer` should show it.
