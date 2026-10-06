# Display output

How a monitor on the NVIDIA GPU's HDMI port becomes a macOS screen (boot-arg `nvdisp=2`), what was tried,
and what was found on hardware. The test laptop (Lenovo IdeaPad L340-15IRH, GTX 1650 TU117) wires its HDMI
port to the NVIDIA GPU: display `0x400`, TMDS, SOR 0. The Intel iGPU drives only the internal panel.

In short, the kext lights the port over GSP-RM and scans out a buffer in VRAM. A user-space helper,
`nvvdisplay` (`display/vdisplay/`), creates a macOS virtual display and copies what WindowServer draws there
into that buffer with the GPU's copy engine, flipping three buffers at vblank. Usage is in
[display/README.md](../display/README.md); installation is step 9 of the top-level README.

## Lighting the port (the kext)

**What:** nouveau's r535/r570 path for Turing. GSP-RM owns the output resource (SOR) and the PHY; the kext writes
display channel methods itself (c570 root, c57d core channel, c57e window channel; no cursor and no
window-immediate channel). Head 0 runs the EDID's first detailed timing and window 0 scans out a pitch-linear
X8R8G8B8 buffer. The code is in `src/NVGpu.cpp`:

- `probeDisplay`: the outputs, their connect state and EDIDs. `nvdisp=1` stops here, read-only.
- `dispInit`: once per GSP-RM boot. Display instance memory (RAMHT, context DMAs), the LUTs, the root object, the
  channels, and core init.
- `dispSetMode`: per connect. SOR assignment (`DFP_ASSIGN_SOR`), head timing, both buffers (colour bars until
  something draws), and window 0.
- `dispBlank`: per disconnect. Window 0 and the SOR are detached.

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

**Hot-plug:** the GSP poller checks the TMDS outputs' connect state once a second (`dispHotplugTick`). A change
must hold for two samples, since HPD bounces while a cable goes in. An output that fails to light is skipped
until it's unplugged. The push buffers wrap with a JUMP to 0 (nouveau's `nv50_dmac_wind`), so modesets can
repeat. A bigger mode gets new buffers. The old ones' VRAM stays allocated, because a user mapping may outlive the
mode. `NVDisplayGen` changes on every connect and disconnect.

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
| Under the NVIDIA PCI device (`nvfb=1`) | GPUWrangler registers the NVIDIA GPU as a discrete GPU with this framebuffer, CoreDisplay fails building its display pipe, and boot hangs at the Apple logo |

A display GPU needs its own Metal display pipeline (IOAccelDisplayPipe and friends). The accelerator test in
`accel/README.md` hit the same wall. The `class-code` injection that hides the GPU (see
[hackintosh.md](hackintosh.md)) didn't cause this: GPUWrangler adopted the GPU anyway. Making this route work means
writing that display pipeline, which is not attempted. NVBringup no longer publishes the resource the kext matches,
so it doesn't start.

### What works: a virtual display (`nvvdisplay`)

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

- **DVI-style signal:** no HDMI mode, no InfoFrames, no audio. TVs may overscan the picture; setting the TV input's
  device type to PC fixes that (verified on the Samsung). They also show a stale source name. An AVI InfoFrame
  (IT content, underscan) and an SPD InfoFrame would fix both.
- **One mode,** the EDID's preferred one, and one head. No mode switching from macOS.
- **Tested on Sonoma only.** The capture runs on Sequoia and Tahoe too. Those systems may ask from time to
  time whether `nvvdisplay` may keep recording the screen, and Sonoma may show a recording indicator in the menu bar.
- **Not scanned out directly:** every frame is composed on the Intel GPU and copied. Composing on the NVIDIA GPU
  needs the Metal display pipeline above.
- **The GPU stays powered with `nvdisp=2`,** monitor or not.
