# HDMI display on the NVIDIA GPU

NVBringup lights the HDMI output at boot (boot-arg `nvdisp=2`, monitor connected): it does the modeset over
GSP-RM and scans out one pitch-linear X8R8G8B8 surface in VRAM. This directory gets macOS to draw into it.

## nvvdisplay (what works)

`vdisplay/nvvdisplay` creates a macOS virtual display (CGVirtualDisplay, the private API DeskPad and
BetterDisplay use) with the monitor's EDID name and mode. It takes the frames WindowServer composes for that display
(CGDisplayStream, cursor included) and copies each frame's dirty rectangles into the scanout surface. The GPU's copy
engine does the copies: each frame's IOSurface is imported once (`NVMAC_MEM_IMPORT`, at most 8 cached) and the
scanout surface bound as a VRAM object (`NVMAC_DISPLAY_MEM`), so only dirty rectangles cross PCIe, as DMA. If any of
that fails, the CPU copies through a write-combined mapping instead (`NVMAC_DISPLAY_MAP`). The log says which.

No tearing: the display has two buffers. The copy engine writes the one not scanned out, then `NVMAC_DISPLAY_FLIP`
makes it visible from the next vblank and returns once the old one is free (the window notifier). The back buffer
missed the previous frame, so each frame also copies the previous frame's dirty rectangles. The flip waits outside
`gspLock_`, so other GPU work isn't held up by it.

Sleep and wake: GSP-RM goes down before sleep, so the display is reported gone. After wake NVBringup boots GSP-RM again,
which relights the output, and nvvdisplay makes the virtual display again.

```
make -C display vdisplay
display/vdisplay/install.sh        # a LaunchAgent for this user; -u removes it
```

It needs the Screen Recording permission, and the first run asks for it. Grant it to `nvvdisplay` in System Settings →
Privacy & Security. The LaunchAgent retries every 10 s, so it starts once the permission is granted. It logs to
`~/Library/Logs/nvvdisplay.log`. CGDisplayStream is removed in macOS 15, so Sequoia and Tahoe will need
ScreenCaptureKit instead. The permission is tied to the binary's ad-hoc signature, so after a rebuild and reinstall,
switch it off and on again.

Hot-plug: while GSP-RM runs with `nvdisp=2`, NVBringup checks the TMDS outputs' connect state once a second. A
change must hold for 2 s before it acts. On plug-in it reads the EDID and lights the output at its preferred mode;
on unplug it detaches window 0 and the SOR. Either way it bumps `NVDisplayGen`. nvvdisplay follows that counter: it
removes the virtual display while nothing is lit, so macOS moves the windows back, and makes it again at the new
monitor's mode. To be able to see a plug-in, `nvdisp=2` keeps the GPU powered even with no monitor, so the idle
power-off never happens.

## NVFramebuffer.kext (does not work, kept for reference)

An IOFramebuffer for the display NVBringup lights, so macOS sees the NVIDIA GPU's HDMI output as a
screen. NVBringup does the modeset over GSP-RM (boot-arg `nvdisp=2`) and scans out one pitch-linear
X8R8G8B8 surface in VRAM. This driver hands that surface to IOGraphics as the aperture (its physical
BAR1 range), with the one mode it was lit at, and the sink's EDID over DDC. There's no acceleration:
WindowServer draws into the aperture with the CPU.

- It attaches to IOResources and matches the `NVDisplayLit` resource. NVBringup published that resource after a
  modeset that scans out, but since hot-plug it no longer does, so this kext doesn't start. It sits outside the GPU's
  PCI device for the same reason as NVMetalAccel (`accel/README.md`).
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

It never supported mode changes, hot-plug, gamma, or sleep and wake (nvvdisplay handles the last three).
The design and the findings are in [docs/display.md](../docs/display.md).

Loading: IOGraphicsFamily is in the system kernel collection, so OpenCore can't inject this kext. It has to go in the
auxiliary collection, the same way as NVMetalAccel:

```
sudo cp -R display/build/NVFramebuffer.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/NVFramebuffer.kext
sudo kmutil load -p /Library/Extensions/NVFramebuffer.kext   # approve in System Settings → Privacy & Security
```

Reboot. With the monitor connected and `nvdisp=2`, `kmutil showloaded | grep nvframebuffer` should list it and
`ioreg -c NVFramebuffer` should show it.
