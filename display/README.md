# HDMI display on the NVIDIA GPU

NVBringup lights the HDMI output at boot (boot-arg `nvdisp=2`, monitor connected): it does the modeset over
GSP-RM and scans out one pitch-linear X8R8G8B8 surface in VRAM. This directory gets macOS to draw into it.

## nvvdisplay (what works)

`vdisplay/nvvdisplay` creates a macOS virtual display (CGVirtualDisplay, the private API DeskPad and
BetterDisplay use) with the monitor's EDID name and mode. It takes the frames WindowServer composes for that display
(ScreenCaptureKit, cursor included) and copies each frame's dirty rectangles into the scanout surface. The GPU's copy
engine does the copies: each frame's IOSurface is imported once (`NVMAC_MEM_IMPORT`, at most 8 cached) and the
scanout surface bound as a VRAM object (`NVMAC_DISPLAY_MEM`), so only dirty rectangles cross PCIe, as DMA. If any of
that fails, the CPU copies through a write-combined mapping instead (`NVMAC_DISPLAY_MAP`). The log says which.

No tearing: the display has three buffers. The copy engine writes one that's neither scanned out nor being flipped
to, and `NVMAC_DISPLAY_FLIP`, on its own queue, makes it visible from the next vblank and returns once the old one
is out of scanout. Each buffer gets the dirty rectangles of every frame since the one it holds. A newer frame takes
over a buffer still waiting for its flip, so the screen shows the newest frame. About one frame is dropped or
doubled every few seconds, because the virtual display's 60 Hz drifts against the monitor's. The flip waits outside
`gspLock_`, so other GPU work isn't held up by it.

Sleep and wake: GSP-RM goes down before sleep, so the display is reported gone. After wake NVBringup boots GSP-RM again,
which relights the output, and nvvdisplay makes the virtual display again.

```
make -C display vdisplay
display/vdisplay/install.sh        # a LaunchAgent for this user; -u removes it
```

It needs the Screen Recording permission, and the first run asks for it. Grant it to `nvvdisplay` in System Settings →
Privacy & Security. The LaunchAgent retries every 10 s, so it starts once the permission is granted. It logs to
`~/Library/Logs/nvvdisplay.log`. ScreenCaptureKit exists from macOS 12.3, so the same capture serves Sonoma,
Sequoia and Tahoe. On Sonoma, `nvvdisplay --cgdisplaystream` captures with the older CGDisplayStream instead
(removed in macOS 15), which is also the fallback if ScreenCaptureKit fails. The permission is tied to the binary's ad-hoc signature, so after a rebuild and reinstall,
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
WindowServer draws into the aperture with the CPU. The vertical blank comes from a timer at the mode's refresh rate.

- It matches `NVDisplayNub`, which NVBringup publishes under itself (so under the GPU's PCI device, where
  GPUWrangler looks) while a display is lit, and only with boot-arg `nvfb=1` or, for one boot, the NVRAM variable
  `nvfb-once` (`sudo nvram nvfb-once=1`; NVBringup deletes it when it lights the display). Uninstall nvvdisplay first.
- The nub's `NVDisplayFB` property carries everything it needs: the physical address and size of the surface,
  the mode, and the EDID. The two kexts share no symbols.
- `make verify` checks all of NVFramebuffer's vtable slots against the kernel's IOFramebuffer. Run it after a macOS
  update. `make check` resolves the kext's symbols against the running kernel.

On hardware (2026-10-09) WindowServer opens it as an unaccelerated display, then CoreDisplay looks for its display
pipe every frame (`Unable to find display pipe for IOFB service`) and its compositor stops within a minute, the
internal panel's too. It needs an accelerator with a display pipe on the NVIDIA GPU. The earlier attempts and the
bugs found on the way are in [docs/display.md](../docs/display.md).

It never supported mode changes, gamma, or sleep and wake (nvvdisplay handles the last).

Loading: IOGraphicsFamily is in the system kernel collection, so OpenCore can't inject this kext. It has to go in the
auxiliary collection, the same way as NVMetalAccel:

```
sudo cp -R display/build/NVFramebuffer.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/NVFramebuffer.kext
sudo kmutil load -p /Library/Extensions/NVFramebuffer.kext   # approve in System Settings → Privacy & Security
```

Then `sudo nvram nvfb-once=1` and reboot with the monitor connected and `nvdisp=2`: `ioreg -c NVFramebuffer` should
show it on `NVDisplayNub`.
