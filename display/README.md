# HDMI display on the NVIDIA GPU

NVBringup lights the HDMI output at boot (boot-arg `nvdisp=2`, monitor connected): it does the modeset over
GSP-RM and scans out one pitch-linear X8R8G8B8 surface in VRAM. This directory gets macOS to draw into it.

## nvvdisplay (what works)

`vdisplay/nvvdisplay` creates a macOS virtual display (CGVirtualDisplay, the private API DeskPad and
BetterDisplay use) with the monitor's EDID name and mode. It takes the frames WindowServer composes for that display
(CGDisplayStream, cursor included) and copies each frame's dirty rectangles with the CPU into the scanout surface,
which NVBringup maps into it (`NVMAC_DISPLAY_MAP`, `nvmac_display_map` in libnvmac).

```
make -C display vdisplay
display/vdisplay/install.sh        # a LaunchAgent for this user; -u removes it
```

It needs the Screen Recording permission, and the first run asks for it. Grant it to `nvvdisplay` in System Settings →
Privacy & Security. The LaunchAgent retries every 10 s, so it starts once the permission is granted. It logs to
`~/Library/Logs/nvvdisplay.log`. When no display is lit, it exits at once. CGDisplayStream is removed in macOS 15, so
Sequoia and Tahoe will need ScreenCaptureKit instead.

## NVFramebuffer.kext (does not work, kept for reference)

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

What happened on hardware (2026-10-06):

- On IOResources (the default), it starts at 1920x1080, 60 Hz, with the EDID, but WindowServer's GPUWrangler never
  lists it. GPUWrangler collects framebuffers through a GPU's PCI device or accelerator.
- Under the NVIDIA PCI device (boot-arg `nvfb=1`), GPUWrangler registers the NVIDIA GPU as a discrete GPU with this
  framebuffer, and boot hangs at the Apple logo. A display GPU needs its own Metal display pipeline (IOAccelDisplayPipe),
  as the accelerator test in `accel/README.md` also showed.

Also not supported: mode changes, hot-plug, gamma, and sleep and wake. GSP-RM goes down with the GPU, and the
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
