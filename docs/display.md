# Display output

How a monitor on the NVIDIA GPU's HDMI port becomes a macOS screen (boot-arg `nvdisp=2`), what was tried,
and what was found on hardware. The test laptop (Lenovo IdeaPad L340-15IRH, GTX 1650 TU117) wires its HDMI
port to the NVIDIA GPU: display `0x400`, TMDS, SOR 0. The Intel iGPU drives only the internal panel.

In short, the kext lights the port over GSP-RM and scans out a buffer in VRAM. Since 2026-10-11 the HDMI port is a
real macOS display (boot-arg `nvdpipe=1`): NVFramebuffer is its IOFramebuffer, NVMetalAccel gives it an
IOAccelDisplayPipe, and WindowServer composites it with NVMetal on the NVIDIA GPU; each frame is copied into the
scanout by the copy engine and flipped at vblank ([the display pipe](#what-works-the-display-pipe-nvdpipe1)). The
earlier route, `nvvdisplay` (`display/vdisplay/`), a virtual display whose frames are captured and copied across,
still works without the auxiliary kexts. Usage is in [display/README.md](../display/README.md); installation is
step 9 of the top-level README.

## Lighting the port (the kext)

**What:** nouveau's r535/r570 path for Turing. GSP-RM owns the output resource (SOR) and the PHY; the kext writes
display channel methods itself (c570 root, c57d core channel, c57e window channel; no cursor and no
window-immediate channel). Head 0 runs the EDID's first detailed timing and window 0 scans out a pitch-linear
X8R8G8B8 buffer. The code is in `src/NVGpu.cpp`:

- `probeDisplay`: the outputs, their connect state and EDIDs. `nvdisp=1` stops here, read-only.
- `dispInit`: once per GSP-RM boot. Display instance memory (RAMHT, context DMAs), the LUTs, the root object, the
  channels, and core init.
- `dispSetMode`: per connect. The mode (below), SOR assignment (`DFP_ASSIGN_SOR`), HDMI or DVI
  (`SET_HDMI_ENABLE`), head timing and output range, both buffers (colour bars until something draws), window 0,
  then the HDMI InfoFrames.
- `dispBlank`: per detach. Window 0 is cleared, the SOR loses its owner (keeping its protocol, as NVKMS's
  `ORSetControl` does), the head's display ID goes to 0, and the InfoFrames stop.
- `dispPower`: DPMS, from NVFramebuffer when macOS sleeps or wakes the display.

**Found on hardware.** Each item cost a boot or more to find. All are now in the code:

| Symptom | Cause |
|---|---|
| Channel allocation: `NV_ERR_INVALID_PARAMETER` | r570's structures differ from r535's: the push buffer control is 56 bytes, the channel allocation parameters 40 (with `subDeviceId`) |
| Channels consume their push buffers, but nothing changes (assembly state at defaults, the core notifier never written) | The display DMA header takes the method's byte offset as is (`METHOD_OFFSET` is bits 13:2), not `>> 2`. Every method landed at a quarter of its offset |
| Window UPDATE: `INVALID_STATE`, code 0x2d | nvdisplay 3 needs an input LUT on the window to convert fixed-point surfaces to FP16 (nvkms `EvoFlipC5Common` says so). Identity, DIRECT10, 1025 FP16 entries after a 32-byte header |
| Window UPDATE: `INVALID_STATE`, code 0x2e | The head needs an output LUT too (nouveau `headc57d` `olut_identity`). Identity, DIRECT10, 16-bit fixed point |
| Picture for ~30 s, then "no signal" | The idle power-off cut the GPU. A lit display counts as use |

The exception codes aren't documented. They were mapped by bisection: on one boot, the kext changed one window
setting at a time and recorded the code each window UPDATE left (0x611028 + chid × 12; the register is sticky).
Every window-only variant kept 0x2e, except removing the ILUT (back to 0x2d) and a zero output size (0x0b), which
placed 0x2e right after the ILUT check. Turning the head's OLUT off and on confirmed it. The surface's context DMA
(big pages, as nouveau's), CSC11, the input scaler and the scale-factor bounds made no difference, but
match nouveau or nvkms and stay.

**EDID and mode:** the kext keeps the whole EDID, extensions included (up to 8 blocks), and publishes it in
`NVDisplayFB` (NVFramebuffer serves each block over DDC) and `NVDisplayEDID-<id>`. `src/nv_edid.cpp` parses the
CTA-861 block: the HDMI VSDB (an HDMI sink, its Max_TMDS_Clock), the HDMI Forum VSDB, the video capability block
(QS, overscan), the short video descriptors and audio; `NVDisplaySink` shows the result. The mode is the preferred
detailed timing if the link can carry it: GSP-RM's `GET_PCLK_LIMIT` for the OR, at most 340 MHz (above that HDMI
2.0 needs scrambling and SCDC, `SET_HDMI_SINK_CAPS`, not set up), 165 MHz for a sink without an HDMI VSDB
(single-link DVI). Otherwise the biggest timing that fits at 50 Hz or more, also within the sink's own maximum: a
4K60 TV gets 1080p60 rather than 4K30. If nothing fits, the preferred timing is tried anyway.

**HDMI:** a sink with an HDMI VSDB gets an HDMI signal, following NVKMS (`nvkms-hdmi.c`, the `hdmipacket` library):
`SET_HDMI_ENABLE` (0x730273) after the SOR assignment, and after the modeset the InfoFrames and an
AVMUTE-clearing general control packet, preceded by `CTRL_HDMI` (0x730274). On Turing NVKMS writes the packets
through the display's SF registers, not through RM (the C57x display uses the `C571` packet class with 9171's
register writers; the 0073 `SET_OD_PACKET` path is for pre-Kepler GPUs), so the kext does too:
`NV_PDISP_SF_USER_0` is at 0x6f0000 on nvdisplay 3 (`dev_disp.h` v03_00), 0x400 per head, a slot per packet
(AVI, generic, GCP, vendor-specific) at index × 64, each enabled every frame with the hardware checksum. The kext
checks that they read sanely first and logs them before and after.

- AVI: RGB 4:4:4, IT content (graphics), "composed for underscan", the timing's CTA video format (VIC), and the
  quantization range below. HDMI 1.4 sinks get the 4K24-30 formats as an HDMI_VIC in the vendor-specific
  InfoFrame instead.
- SPD: "NVIDIA", "GeForce (macOS)", PC. Whether a TV shows it instead of a stale source name is up to the TV;
  Samsung's name likely comes from CEC, which Turing can't send.
- RGB range: full, unless the sink can't be told the range (no QS bit in the video capability block) and the
  timing is a CTA video format other than 640x480, for which such a sink expects limited range. Then the head
  compresses to 16-235 (OCSC1 and its clamp, procamp CEA range, as NVKMS `EvoSetOCsc1C5`) and the AVI InfoFrame
  says "default". With QS, full range and the AVI InfoFrame says so. `nvrange=1`/`2` forces full/limited.
- `nvhdmi=0` keeps the DVI-style signal. `NVDisplaySignal` says what was sent.
- `nvhdmi20=1` (default off, untested on hardware): HDMI 2.0 modes above 340 MHz TMDS. Only for an HDMI sink whose
  HDMI Forum VSDB has SCDC and a Max_TMDS_Character_Rate above 340 MHz; the link limit is then the sink's rate or the
  SOR's 600 MHz, whichever is lower (the OR's 165 MHz from GET_PCLK_LIMIT is RM's single-link DVI figure, not used).
  Before the modeset: the sink's caps to RM (SET_HDMI_SINK_CAPS), then SCDC 0x20 = 0x03 (ratio 1/40 and scrambling)
  over the sink's DDC port (DISP_I2C_READ_WRITE). After it: SCDC 0x20 and the scrambler status 0x21 are read and logged
  as `HDMI 2.0:`. A sink that doesn't take it gets no signal, so turn the boot-arg off to go back.

**Display sleep (DPMS):** NVFramebuffer can't be a power driver (`registerPowerDriver` panicked, see below), so
IOGraphics never sends it `kIOPowerAttribute`, and WindowServer sends no `kConnectionPower` either. Instead a small
object in NVFramebuffer (`NVDisplaySleepWatch`) registers as an interested driver of the IODisplayWrangler, whose
power state is the display's (4 on, 3 dimmed, below that asleep). Below 3 the kext turns the signal off with
`SET_MONITOR_POWER` (0x730295, NVKMS's DPMS for TMDS), keeping the head, the mode and the surfaces, so the sink goes
to standby; waking turns it on. If GSP-RM refuses, the signal stays on (detaching the SOR instead led into the
relight race below). It is ignored while the display is unplugged. `NVDisplayPower` shows the state; `nvdpms=0`
turns it off. Verified 2026-10-15 with `pmset displaysleepnow`.

**Hot-plug:** GSP-RM handles the HPD interrupts and posts `NV2080_NOTIFIERS_HOTPLUG` to an event object the kext
allocates on its subdevice (`NV01_EVENT_KERNEL_CALLBACK_EX` with `NV01_EVENT_CLIENT_RM`, then
`EVENT_SET_NOTIFICATION` REPEAT, as nouveau). The `POST_EVENT` message comes through the status queue, which the
100 ms GSP poller and every RPC wait drain; there is no interrupt for it. After an event the poller
(`dispHotplugTick`) checks the TMDS outputs' connect state every 300 ms until a change has held for two checks,
since HPD bounces while a cable goes in; otherwise every 5 s as a fallback. Without the event object (refused, or
`nvhpdevent=0`) it checks once a second, as before. `NVDisplayHotplugEvents` counts the events. An output that fails
to light is skipped until it's unplugged. The push buffers wrap with a JUMP to 0 (nouveau's `nv50_dmac_wind`), so modesets can
repeat. A bigger mode gets new buffers. The old ones' VRAM stays allocated, because a user mapping may outlive the
mode. `NVDisplayGen` changes on every connect and disconnect.

**Unplug and replug (2026-10-15):**

- An unplug keeps the output running: macOS sees the display go offline (`Online` false on the nub), but the head
  and the SOR stay lit (`parkedId`). A replug of the same sink, the same EDID compared over its own length (GSP-RM's
  EDID buffer is padded: 384 bytes for a 256-byte EDID), only brings it back online, without a modeset. Another
  sink on the port gets a detach and a full modeset. `nvunplug=0` detaches on every unplug; the NVRAM one-shot
  `nvunplug-detach=1` does it for the next unplug only, to test the relight.
- The relight after a detach raced GSP-RM: on some tries the SOR's control read back 0x100 (no owner) in the
  assembly and armed state after the core UPDATE, with no channel exception and the notifier finished, and the TV
  got no signal. Others worked with the same sequence. GSP-RM runs the attach (the supervisor) itself, so the race
  can't be seen from here. `dispSetMode` now checks the armed SOR control after the update and sends the attach
  again, up to 5 times 200 ms apart ("attaching again (n of 5)" in the log; one retry was needed in 1 of 3 forced
  relights); a SOR that never takes fails the modeset.
- Found on the way: `dispBlank` never cleared `HEAD_SET_DISPLAY_ID`, so GSP-RM went on reporting head 0 -> 0x400
  (`SYSTEM_GET_ACTIVE`) after a detach. NVKMS (`nvkms-evo.c`, "Tell RM that there is no DisplayID is associated
  with this head anymore") and nouveau (`display_id(head, 0)`) clear it; so does the kext now.
- The old once-a-second poll used GSP-RM's cached connect state while lit, which never showed a short unplug: the
  output stayed lit through it, and that is why replugging seemed to work before hot-plug events.

**Power:** to see a plug-in, the GPU has to be on, so with `nvdisp=2` the idle power-off never happens, even with
no monitor. Without `nvdisp=2` nothing changes.

**Sleep and wake** need no code of their own. GSP-RM unloads before sleep (`dispLost` reports the display gone),
and the wake path boots GSP-RM again, which relights the port. Verified: `wake: GSP-RM boot ok`, the display lit
again, and `nvvdisplay` picked up the new generation.

## Getting macOS to draw there

### Tried first: an IOFramebuffer (`display/`, NVFramebuffer.kext)

A framebuffer driver that hands IOGraphics the scanout buffer as its aperture. It can't be part of NVBringup:
IOGraphicsFamily is in the system kernel collection, which an OpenCore-injected kext can't link against. So it's a
separate kext in the auxiliary collection, sharing no symbols, with everything passed through NVBringup's
`NVDisplayFB` property. `make verify` checks all 350 of its IOFramebuffer vtable slots against the running
kernel's, since the SDK is newer than the OS.

| Placement | Result |
|---|---|
| On IOResources | Starts (1920x1080, 60 Hz, EDID). WindowServer's GPUWrangler never lists it: it collects framebuffers through a GPU's PCI device or its accelerator |
| Under the NVIDIA PCI device (`nvfb=1`, 2026-10-06) | GPUWrangler registers the NVIDIA GPU as a discrete GPU with this framebuffer, CoreDisplay fails building its display pipe, and boot hangs at the Apple logo. The aperture was wrong too (below) |
| Under NVBringup's `NVDisplayNub` (2026-10-09) | Starts at the right aperture with a vblank timer; WindowServer opens it and treats it as unaccelerated (`isAcc=NO`), but logs `Unable to find display pipe for IOFB service` every frame, and ~1 min later its compositor stops, the internal panel included |

So CoreDisplay on Sonoma wants a display pipe (IOAccelDisplayPipe, from an accelerator on the same GPU) for every
framebuffer once another GPU is accelerated; an unaccelerated framebuffer, as a VESA display is, isn't enough. The
accelerator test in `accel/README.md` hit the same wall. The `class-code` injection that hides the GPU (see
[hackintosh.md](hackintosh.md)) didn't cause this: GPUWrangler adopted the GPU anyway. Making this route work means an
accelerator under the GPU's PCI device whose display pipe takes WindowServer's frames (NullMoth's driver does this on
Sequoia, over NVKMS). That is what the display pipe below does.

Found on the way, each a boot:

| Symptom | Cause |
|---|---|
| Kernel panic, `vm_page_validate_no_references`, on a page inside the aperture | `IODeviceMemory::getPhysicalAddress()` of BAR1 is an I/O address of the system mapper (VT-d), which to the CPU is RAM (here 0x101000000 for BAR1 at 0x7f80000000): WindowServer drew into the kernel's memory. `NVDisplayFB`'s `PhysAddr` and the CPU mappings of `NVMAC_MEM_CAN_MAP` buffers now use `getPhysicalSegment(…, kIOMemoryMapperNone)` |
| Panic in `IOFramebuffer::powerStateWillChangeTo` (NULL controller) | `registerPowerDriver` in `start()`: IOGraphics creates the framebuffer's controller later. Removed; IONDRVFramebuffer registers at the very end |
| WindowServer started a minute late, the panel stayed on the boot text | `probe()` slept up to 60 s for a lit display, holding the PCI device busy. NVBringup now publishes `NVDisplayNub` once a display is lit (and removes or replaces it on unplug or replug); NVFramebuffer matches that and never waits |
| `nvfb-once` deleted from the kernel came back | `setProperty(kIONVRAMDeletePropertyKey)` doesn't delete; `setProperties()` with it and `kIONVRAMSyncNowPropertyKey`, as `nvram -d` does, does |

Turning it on: boot-arg `nvfb=1`, or for one boot `sudo nvram nvfb-once=1`, which NVBringup deletes when it lights the
display, so a boot that hangs is followed by a normal one. Uninstall `nvvdisplay` first: both would use the scanout
buffers.

**Framebuffer at boot (display pipe, `nvdpipe=1` or `nvdpipe-once`):** WindowServer maps a GPU's framebuffers only
when it enumerates the GPU, so a framebuffer that exists only while lit misses a later hot-plug. Armed, NVBringup
publishes `NVDisplayNub` as soon as the display engine is up (`dispInit`), lit or not. The nub carries `NVDisplayFB`
(the last lit mode, else CEA-861 1920x1080@60, with the scanout surface allocated at `dispInit` so it has an address
while unlit) and `Online`. A plug or unplug updates `Online` and `NVDisplayFB` on that nub in place and sends it a
message (`kNVDisplayMsgState`, 'NVDB'), and it is not terminated or republished. NVFramebuffer answers through the
IOFramebuffer connection attributes: `kConnectionEnable` is `Online`, `kConnectionCheckEnable` stays on,
`kConnectionChanged` is raised until IOGraphics acknowledges it, and a change fires the connect interrupt
(`kIOFBConnectInterruptType`) so IOGraphics senses the connection again. The aperture stays valid while offline. A
mode that needs a bigger surface gets a new aperture. Sleep still tears the nub down and republishes it at wake.

### What works: the display pipe (`nvdpipe=1`)

Working since 2026-10-11: the macOS desktop on the HDMI display, composited by WindowServer with NVMetal on the
NVIDIA GPU. Armed with boot-arg `nvdpipe=1`, or for one boot `sudo nvram nvdpipe-once=1`. The pieces:

- **NVBringup** publishes `NVDisplayNub` once the display engine is up (above), holds the PCI device busy for up to
  50 s so WindowServer's `IOKitWaitQuiet` waits for the framebuffer, and does the frame copies and flips
  (`NVPipeCopy` through `callPlatformFunction`, `src/nv_pipe_copy.h`).
- **NVFramebuffer** (`display/`) is the IOFramebuffer on that nub: one mode, the EDID, a vblank timer re-phased from
  the head's raster line, a 256x256 hardware cursor (C57A PIO channel; `nvswcursor=1` for WindowServer's), and
  display sleep (above). **NVAGDCControl** in the same kext is the AppleGraphicsDeviceControl provider IOPresentment
  needs to find a framebuffer at all (selectors 1, 0x980, 0x921 and 0x711; details in display/README.md).
- **NVMetalAccel** (`accel/`, PCI personality) is the accelerator with the `IOAccelDisplayPipe`, and publishes the
  Metal plugin keys so CoreDisplay gets an MTLDevice for it. It sets class-code 0x030000 on itself, because OpenCore
  spoofs the GPU's, and IOPresentment pairs an AGDC with an accelerator by that.
- **NVMetal** (Mesa, `src/nouveau/air/metal`) is loaded into WindowServer from `/Library/GPUBundles` (searched before
  `/System/Library/Extensions`; WindowServer's sandbox allows it), with its ICD and libraries embedded and ad-hoc
  signed, because WindowServer enforces library validation; `amfi_get_out_of_my_way=0x1` lets the ad-hoc signature
  through (see the README).
- **Per frame:** WindowServer submits a transaction; `submitTransaction` has NVBringup copy the IOSurface into one
  of three scanout buffers with the copy engine (one push, one wait), then flips window 0 to it at the next vblank.
  `NVPipeTiming` on NVBringup (copy, queue, latch and flip-gap means and maxima per 120 flips) and `NVPipeSubmits`,
  `NVPipeGpuCopies`, `NVPipeCpuCopies` on the accelerator show how it goes. The CPU copy is the fallback for
  frames before the display is lit and across mode changes.

### Zero-copy scanout (`nvzerocopy=1`)

Default off, with `nvdpipe=1` (also ignored in a safe-mode boot). NVMetal in WindowServer renders each display
surface into a VRAM image and writes it back to the IOSurface (for Intel's display hardware in mirror mode and for
anyone who reads the surface). With this on, the pipe scans that VRAM image out instead of copying the IOSurface into
a scanout buffer.

- **Registration** (`NVMAC_SURFACE_SCANOUT`, `src/nv_uapi.h`): after the last write-back of a surface's only image
  has landed, and before the command buffer counts as submitted (which WindowServer's commit waits for), NVMetal
  calls the ICD's exported `nvk_macos_register_scanout`, which sends the kext the IOSurface ID, the image's GPU VA,
  row pitch, size, layout and the surface's seed (`IOSurfaceGetSeed`, which moves on every write lock). The layout is
  NVK's own: pitch-linear, or block-linear with 2^n GOBs per block (a 1920x1080 BGRA image: pitch 7680, n = 4,
  checked on this machine with the build tree's ICD). The kext resolves the VA through the connection's bindings to a
  VRAM address (one binding of VRAM memory; 256-byte aligned, 4 KiB for block-linear) and keeps the entry until the
  memory is freed or the connection closes. Only WindowServer (uid 88) or root may register; the selector and feature
  bit exist only with the boot-arg.
- **Per frame** (`NVDisplayPipe::submitTransaction`): the pipe first asks `NVPipeScanout` (`src/nv_pipe_copy.h`)
  with the surface's ID and its current lock seed (`IOSurface::getLockSeed`). The kext refuses (and the pipe copies as
  before) unless the surface is registered at that seed (`nvzerocopy=2`: whatever the seed), is the lit mode's size,
  and the display is lit with flips on. Otherwise it programs window 0 for the image (storage: block height and
  layout; pitch; a second DMA object with a kind for block-linear, `kDispHandleFbBl`, built like nouveau's gv100
  ctxdma; the offset) and flips at the next vblank, with the same queued-flip bookkeeping as the copy path.
- **Lifetime:** the image's VRAM is held (`DispHw::scanHeld`) while it is scanned out and until the flip that replaces
  it has been shown (`pipeShowAt`); if NVMetal frees it meanwhile, the block is freed once the replacing flip has
  been shown (checked at the next flip or allocation, and at a modeset). The transaction completes only when its own flip is
  shown (about a frame): from then on WindowServer may reuse the previous surface, as with a real pipe. A copy
  after a zero-copy frame completes 40 ms later for the same reason. That WindowServer never renders into the
  surface it last presented is its own behaviour on every GPU (inferred; it is not enforced here).
- **Safety net:** the seed. Surfaces NVMetal did not render (an Intel-rendered mirror master, a video layer), or that
  anyone wrote after NVMetal's write-back, have another seed or none registered, and are copied.
- **Not done:** the aperture copy (`fb`) the copy path keeps current for mirror mode is skipped on a zero-copy
  frame. A mirror that reads the NVIDIA framebuffer's aperture would show a stale picture (inferred not to be the
  case: hardware mirroring scans the master's IOSurface, which write-back keeps current).
- **Switches:** boot-arg `nvzerocopy` (kext side; `=2` no seed check); `NVMETAL_ZEROCOPY=0` in
  `/Library/Preferences/io.github.kvarun-p.nvmetal.env` stops NVMetal in WindowServer registering images (the pipe
  then copies everything), `=1` registers from other processes too. `NVMETAL_ZEROCOPY_DEBUG=1` prints each
  registration's address, pitch and layout from the ICD.
- **Counters:** `ZeroCopyFlips`, `ZeroCopyFallbacks`, `ZeroCopyLastResult` on NVMetalAccelerator; NVBringup's
  `NVZeroCopy` (flips and the refusals by reason). `tools/nvframes` prints them.

Found on hardware, each a boot or more:

| Symptom | Cause |
|---|---|
| CoreDisplay: `Unable to find display pipe` every frame | IOPresentment lists framebuffers only through AppleGraphicsDeviceControl services: NVAGDCControl |
| Panic in `IOAccelDisplayPipe::init` | The family creates a resource on the accelerator: `newResource` and the sysmem/vidmem/memory-map classes must exist |
| Menu bar items missing | IOSurface row alignment: NVMetal reported 256 bytes, CoreAnimation uses 16 |
| Black flashes on the laptop panel in mirror mode | The kext wired WindowServer's IOSurface read-only (`kIODirectionOut`); Intel's driver then failed to wire it. Never wire a shared IOSurface read-only |
| Video stutter, ~65 ms stalls | The once-a-second hot-plug check (full detection, DDC) held `gspLock_`; GSP-RM's cached state while lit, then hot-plug events |
| Frames out of order, then flicker | Reads of IOSurfaces deferred to submit time, and `commitAndWaitUntilSubmitted` returning before the frame reached the surface; both fixed in NVMetal, with regression tests (scratchpad `s6/tests`) |
| WindowServer aborts in `get_tile_pipeline` | NVMetal's `supportsFamily:` answered yes for every family (a missing brace), so CoreAnimation took the tile-shader blur path |
| Slow window animations after login | Cold pipeline compiles in WindowServer, whose Mesa disk cache is off (ruid != euid): NVMetal's own pipeline and translation caches |

### Earlier, and still working: a virtual display (`nvvdisplay`)

`nvvdisplay` creates a virtual display (CGVirtualDisplay, the private CoreGraphics API DeskPad and BetterDisplay
use) with the monitor's EDID name, size and mode. WindowServer treats it as an ordinary display: arrangement,
mirroring, full-screen spaces. The helper takes the frames WindowServer composes for it, cursor included, with
dirty rectangles, and puts them in the scanout buffer:

- **Capture:** ScreenCaptureKit (macOS 12.3 and later), so one path serves Sonoma, Sequoia and Tahoe. The
  virtual display shows up in its display list shortly after it's made; the stream runs at the display's size and
  refresh rate, BGRA, and each complete frame's IOSurface and dirty rectangles go to the copy below. If the stream
  stops on its own, the helper restarts it, up to 3 times per display generation. CGDisplayStream, the first
  version's capture, is removed in macOS 15. It stays as the fallback on Sonoma when ScreenCaptureKit fails, or with
  `--cgdisplaystream`, for comparison.

- **Copy engine:** each frame's IOSurface is imported into the GPU once (`NVMAC_MEM_IMPORT`; the stream reuses a
  small pool, cached up to 8). The scanout buffers are bound as VRAM objects (`NVMAC_DISPLAY_MEM`, borrowed:
  freeing the handle leaves the display's VRAM alone). Each frame is one push buffer of 2D pitch copies, one per
  dirty rectangle, waited for before the surface is unlocked. Measured: every frame by the copy engine, up to
  ~55 frames/s and ~180 MB/s, with `nvvdisplay` at ~1.6 % CPU and 4 MB. The first, CPU-copy version used ~2 % at
  3–13 frames/s.
- **No tearing:** three buffers. A frame is copied into one neither scanned out nor being flipped to, then
  `NVMAC_DISPLAY_FLIP` points window 0 at it. Flips run on their own queue, so copies go on while one waits;
  a copied frame still waiting for its flip is taken over by a newer one. Each buffer remembers which frame it
  holds and gets the dirty rectangles of every frame since (a history of 8, else the whole frame). Frames
  ScreenCaptureKit delivers while a copy runs wait in a one-frame mailbox, a newer one replacing a waiting one
  with its damage merged, so no backlog builds up.
- **When a flip is done:** the window notifier (0x100 in the display's sync page) never leaves NOT_BEGUN on
  hardware, although its context DMA and control read back as set in both the assembly and the armed state; the
  cause is unknown. Window 0's armed offset (0x690a60: nvkm's `.prev`, 0x800 above the assembly state at
  0x690000 + method) is set as soon as the UPDATE arrives, anywhere in the frame (`NVDisplayFlipLines` records the
  raster line), and becomes active only at the next vblank. Returning then let `nvvdisplay` write the buffer
  still scanned out: tearing while scrolling. So once the offset is armed, the kernel waits until head 0's raster
  line (0x616330) has passed through a vblank and is active again; a flip armed inside a vblank waits for the
  next. The wait runs outside `gspLock_`. `NVDisplayFlips` counts how flips completed; `NVDisplayFlipDiag` holds the
  state of the first flips the notifier didn't complete.
- **History, measured on hardware:**

  | Version | Result |
  |---|---|
  | Wait for the notifier | Every flip waited the 100 ms timeout: <10 frames/s |
  | Done once armed | 5–7 ms per flip, ~50 frames/s, tearing |
  | Wait for the vblank, two buffers | No tearing, flips 11–13 ms, copies blocked meanwhile: frames queued, and a short lag every 2–3 s |
  | Three buffers, flips on their own queue | No tearing, 30–40 frames/s while scrolling, all frames by the copy engine |

  What's left is about one frame dropped or doubled every few seconds. The virtual display's 60 Hz and the
  monitor's 60 Hz come from different clocks and drift against each other; showing every frame would mean timing
  the virtual display from the monitor's vblank. `nvvdisplay` still goes back to one buffer after 3 flips in a row
  time out, or a failed one.
- **Fallback:** if anything on the GPU path fails, the CPU copies through a write-combined mapping of buffer 0
  (`NVMAC_DISPLAY_MAP`), after flipping back to it.
- **Hot-plug:** it follows `NVDisplayGen` once a second. It removes the virtual display while nothing is lit (macOS
  moves the windows back) and makes it again at the new monitor's mode.

It runs as a per-user LaunchAgent and needs the Screen Recording permission. That permission is tied to the
binary's ad-hoc signature, so a rebuild needs it granted again.

## Limitations and next steps

- **HDMI without audio:** HDMI mode, InfoFrames and the range (verified 2026-10-15) are there; HDMI audio needs the
  RM side (ELD, `SET_ELD_AUDIO_CAPS`) and a macOS driver for the GPU's HDA function. No HDMI 2.0 (above 340 MHz),
  no deep colour, no YCbCr output.
- **One mode,** the preferred one or the best the port carries, and one head. No mode switching or HiDPI modes.
- **A copy per frame** unless `nvzerocopy=1` is on and the surface is NVMetal's own ([zero-copy](#zero-copy-scanout-nvzerocopy1);
  untested on hardware as of 2026-10-16).
- **Tested on Sonoma only.** The private interfaces the display pipe uses (IOAcceleratorFamily2's vtables, AGDC
  selectors) must be checked again on Sequoia and Tahoe (`make verify` in `accel/` and `display/`).
- **The GPU stays powered with `nvdisp=2`,** monitor or not.

## HDMI audio (boot-arg `nvhdmiaudio`, default off)

`src/NVHdmiAudio.cpp` is the GPU's half of HDMI audio on the HDMI port. Bit 0 (1): after an HDMI modeset, at a
reconnect with the same EDID, and at a hot-plug it sends GSP-RM the unmuting general control packet, the stream
unmute and the ELD built from the EDID (`nv_eld_build`) through `DFP_SET_ELD_AUDIO_CAPS` with PD=1 ELDV=1, as
NVKMS's `nvHdmiDpEnableDisableAudio` does. Before a modeset the ELD goes invalid, at an unplug or detach it is
cleared. Bit 1 (2): reads the ELD back through the GPU's HDA function (PCI function 1) when no driver owns it.
Bit 2 (4): sets bit 25 of function 0's config register 0x488 if function 1 is absent (Linux quirk). Bit 3 (8):
turns on function 1's snoop bits (config 0x4e, 0x4d, 0x4c) at start, as Linux does for NVIDIA HDA controllers;
the three bytes are logged either way. The audio InfoFrame, ACR and the stream belong to the HDA driver, not to
this kext. Results: ioreg `NVHdaFunction`, `NVDisplayAudio` on the NVBringup service and the `HDMI audio:` lines of
ioreg `NVLog` (`nvgsp status` shows only the `GSP:` lines).

On this TU117 laptop (2026-10-16): with `nvhdmiaudio=3` the codec (vendor 0x10de0094) reports the TV on pin 4 with
a valid ELD identical to the one sent. macOS needs DeviceProperties for AppleHDAController to take function 1:
`hda-gfx` = `onboard-2` on the GPU and on `PciRoot(0x0)/Pci(0x1,0x0)/Pci(0x0,0x1)`, which also gets `layout-id`
7 (the Intel pair keeps `onboard-1`). That alone attaches nothing: AppleHDAController::probe accepts only the PCI
IDs in its own table (16 NVIDIA ones, up to GK10x; not 10de:10fa) and, for a provider not named HDEF/HDAU, only with
the boot-arg `hdax=1` (scratchpad/hda-attach-2026-10-16/REPORT.md). Bit 3's snoop bytes read 0 and ignore writes on
TU117, so they are not it.

`hdmiaudio/` builds NVHDAPatch.kext, a Lilu plugin (OpenCore Kernel → Add after Lilu) that rewrites the table's
10de:0e1b record to 10de:10fa (boot-arg `nvhdadid` for another ID) when AppleHDAController loads; `-nvhdaoff` turns
it off. With `hdax=1` the controller should then attach. Building it needs a Lilu source tree and acidanthera's
MacKernelSDK (see its Makefile). Verified 2026-10-16: with it, AppleHDAController, IOHDACodecDevice and AppleHDAHDMI_DPDriver attach to function 1.
The output itself needs the graphics side: AppleHDAController finds the IODisplay below a node with its `hda-gfx`,
reads that node's `connector-type` (0x800 HDMI) and `port-number`, and on IOFramebuffer notifications its
`av-signal-type` (8, HDMI). With `nvhdmiaudio` bit 0, NVFramebuffer publishes those: `hda-gfx` copied from the GPU's
PCI device, `connector-type` 0x800, `port-number` 0 at start, and `av-signal-type` 8 while online with a sink whose
EDID announces audio (0 otherwise), set before the connect interrupt.
