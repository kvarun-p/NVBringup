# HDMI display on the NVIDIA GPU

NVBringup lights the HDMI output at boot (boot-arg `nvdisp=2`, monitor connected): it does the modeset over
GSP-RM and scans out pitch-linear X8R8G8B8 surfaces in VRAM. This directory gets macOS to draw into them, in one
of two ways:

- **The display pipe** (`nvdpipe=1`, with NVMetalAccel and NVMetal installed): NVFramebuffer.kext below is a real
  macOS display, composited by WindowServer on the NVIDIA GPU. This is the main route since 2026-10-11.
- **nvvdisplay:** a virtual display whose frames are captured and copied across. It needs no auxiliary kexts.

## nvvdisplay (the virtual display)

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
(removed in macOS 15), which is also the fallback if ScreenCaptureKit fails. With the default ad-hoc signature the
permission is tied to the binary's cdhash, so after a rebuild and reinstall, switch it off and on again. To keep it
across rebuilds, sign with a local self-signed certificate: `tools/devsign.sh howto` shows how to make one (once, by
hand), then set `NVB_SIGN_IDENTITY` in `local.env`; `make -C display vdisplay` signs with it, and
`tools/devsign.sh check <binary>` says whether a signature keeps its grants.

Hot-plug: while GSP-RM runs with `nvdisp=2`, NVBringup listens for GSP-RM's hot-plug events and then checks the TMDS
outputs' connect state every 300 ms until a change has held for two checks (otherwise every 5 s; once a second
without events, or with `nvhpdevent=0`). On plug-in it reads the EDID and lights the output at its preferred mode
(or the best one the port can carry), in HDMI mode with InfoFrames for an HDMI sink (see docs/display.md).
On unplug it keeps the output running and only takes the display offline; the same sink plugged back in comes
back without a modeset, another one gets a full modeset (`nvunplug=0`: detach on every unplug). Either way it
bumps `NVDisplayGen`. nvvdisplay follows that counter: it
removes the virtual display while nothing is lit, so macOS moves the windows back, and makes it again at the new
monitor's mode. To be able to see a plug-in, `nvdisp=2` keeps the GPU powered even with no monitor, so the idle
power-off never happens.

## NVFramebuffer.kext (the display pipe's framebuffer)

An IOFramebuffer for the display NVBringup lights, so macOS sees the NVIDIA GPU's HDMI output as a
screen. NVBringup does the modeset over GSP-RM (boot-arg `nvdisp=2`) and scans out one pitch-linear
X8R8G8B8 surface in VRAM. This driver hands that surface to IOGraphics as the aperture (its physical
BAR1 range), with the one mode it was lit at, and the sink's EDID over DDC (every block). With NVMetalAccel's
display pipe (`accel/`), WindowServer composites with NVMetal and the pipe copies each frame into the scanout; on its
own it is an unaccelerated framebuffer, which WindowServer doesn't accept (below). The vertical blank comes from a
timer at the mode's refresh rate, re-phased from the head's raster line. It drives NVBringup's hardware cursor
(`nvswcursor=1`: WindowServer's) and display sleep: `NVDisplaySleepWatch` follows the IODisplayWrangler's power
state and turns the HDMI signal off below "dimmed" (`nvdpms=0`: never).

- It matches `NVDisplayNub`, which NVBringup publishes while a display is lit, as a client of the GPU's
  IOPCIDevice (NVBringup is another client of that device, and owns the nub). Only with boot-arg `nvfb=1` or
  `nvdpipe=1`, or, for one boot, the NVRAM variable `nvfb-once` or `nvdpipe-once` (NVBringup deletes it when it
  lights the display). Uninstall nvvdisplay first. The driver reads nothing from NVBringup itself, only the nub.
- The nub's `NVDisplayFB` property carries everything it needs: the physical address and size of the surface,
  the mode, and the EDID. The two kexts share no symbols.
- Armed (`nvdpipe=1` or `nvdpipe-once`): the nub is up from boot, lit or not, with `Online` false until a display
  lights. Its `NVDisplayFB` is the last lit mode, else 1920x1080@60. A plug or unplug changes `Online` and
  `NVDisplayFB` on the same nub and sends it `kMsgState` ('NVDB', the value NVBringup's `kNVDisplayMsgState` also
  uses). `kConnectionEnable` follows `Online`, `kConnectionCheckEnable` stays on, `kConnectionChanged` is raised until
  IOGraphics acknowledges it, and a change fires the connect interrupt (`kIOFBConnectInterruptType`) so IOGraphics
  senses the connection again. The aperture stays valid while offline; a new surface gets a new aperture.
  Verified with hot-plug on 2026-10-15.
- Timing for the display pipe: `IOFBCurrentPixelCount` (htotal x vtotal) and `IOFBCurrentPixelClock` (Hz) are
  published on the framebuffer when the mode is set. The VBL timer delivers `'vbl '` through
  `registerForInterruptType` / `setInterruptState` at absolute deadlines of one frame, and `getVBLTime` returns
  the last deadline. `NVFBVblanks` counts the timer's vertical blanks (published about once a second).
- `make verify` checks all of NVFramebuffer's vtable slots against the kernel's IOFramebuffer, and NVAGDCControl's
  (below) against the kernel's AppleGraphicsDeviceControl: slots, parameter types, the 272-byte size, and that every C++
  import exists in the kernel collections. Run it after a macOS update. `make check` resolves the kext's symbols against
  the running kernel.
- NVAGDCControl (src/NVAGDCControl.cpp, same kext) is the display's AppleGraphicsDeviceControl provider. IOPresentment
  (CoreDisplay's display pipe) finds a framebuffer only through an AGDC: for each AGDC service it opens type 2, asks
  selector 1 (vendor info) and selector 0x980 (the framebuffer registry IDs it serves). Nothing served NVFramebuffer, so
  CoreDisplay found no pipe for it. Its selector 0x980 lists NVFramebuffer as an object pointer, read when asked; AGDC
  turns each pointer into its registry ID before userspace sees it (a registry ID in its place panics in
  getRegistryEntryID). Its layout is in src/AppleGraphicsDeviceControl_decl.h. probe() refuses to attach unless the
  kernel's class is 272 bytes.
  The pairing rule (CreateAssociationMap, GetNodeMap): every AGDC and every IOAccelerator gets a key, its first
  IOService-plane ancestor whose class-code is a display class, else its own registry ID; a device gets a node only
  when an AGDC and an accelerator share a key (else "Interface creation error: 0x815"). OpenCore spoofs PEGP@0's
  class-code (docs/hackintosh.md), so the GPU can't be that ancestor. Instead NVMetalAccelerator, in display-pipe mode,
  sets class-code 0x030000 on itself, and NVAGDCControl matches on it (IOProviderClass NVMetalAccelerator, IOPropertyMatch
  NVDisplayPipeMode): both keys are then the accelerator's registry ID. Intel's pair keys on IGPU@2.
  After the association, IOPresentment gathers the display's capabilities through two more selectors: 0x921 (Intel's
  GetLinkConfig: the current detailed timing, which NVAGDCControl takes from the nub's NVDisplayFB) and 0x711 (the
  plane block, one BGRA8 plane, and the pipeline-scaler block). Without them the set fails with 0x80e and CoreDisplay
  logs "Attempting to get capabilities when none exist" and presents nothing.

Alone (2026-10-09), WindowServer opened it as an unaccelerated display, then CoreDisplay looked for its display
pipe every frame (`Unable to find display pipe for IOFB service`) and its compositor stopped within a minute, the
internal panel's too. With NVMetalAccel's display pipe and NVAGDCControl it works (2026-10-11). The attempts and
the bugs found on the way are in [docs/display.md](../docs/display.md).

It doesn't support mode changes or gamma. Sleep and wake: the nub goes away at sleep and comes back at wake.

Loading: IOGraphicsFamily is in the system kernel collection, so OpenCore can't inject this kext. It has to go in the
auxiliary collection, the same way as NVMetalAccel. `tools/nvctl install efi accel fb` installs it with the matching
NVBringup and NVMetalAccel (they check each other's stack ABI and refuse a mismatch). By hand:

```
sudo cp -R display/build/NVFramebuffer.kext /Library/Extensions/
sudo chown -R root:wheel /Library/Extensions/NVFramebuffer.kext
sudo kmutil load -p /Library/Extensions/NVFramebuffer.kext   # approve in System Settings → Privacy & Security
```

Then reboot with the monitor connected and `nvdisp=2 nvdpipe=1` (or `sudo nvram nvdpipe-once=1` for one boot):
`ioreg -c NVFramebuffer` should show it on `NVDisplayNub`.
