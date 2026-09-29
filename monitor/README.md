# GPU Monitor

Menu bar app plus a WidgetKit widget (small and medium) showing the GPU's usage,
temperature, VRAM and BAR1, read from NVBringup's `NVStats` registry property.

```sh
monitor/build.sh              # builds and installs ~/Applications/GPU Monitor.app, registers the widget
open ~/Applications/"GPU Monitor.app"
open ~/Applications/"GPU Monitor.app" --args --demo   # synthetic data, no GPU needed
```

Add the widget from the desktop's right-click menu → Edit Widgets → "GPU Monitor".

## Where the numbers come from (`src/NVGpu.cpp`, `updateStats`)

The GSP message poller calls it every 100 ms. Once a second it publishes `NVStats`:

| key | meaning |
|---|---|
| `Utilization` | % of the 10 samples in which some context had an EXEC whose seqno wasn't released yet. This includes work blocked on a GPU-side semaphore wait. It's an estimate, not an engine counter. |
| `Temperature` | centi-°C from `NV_THERM_TSENSOR` (0x020460, nouveau `gp100_temp_get`); left out if the valid bit is clear or the read returns 0xbad… |
| `VramUsed` / `VramTotal` | the driver's VRAM heap, including its own page tables and buffers |
| `Bar1Used` / `Bar1Total` | CPU-visible BAR1 window |
| `Connections`, `Contexts`, `Sequence` | open clients, live contexts, publish counter (the app treats a frozen counter as stale) |

It's only published while GSP-RM runs and is removed when it unloads. Check it with
`ioreg -r -c NVBringup -d 1 | grep NVStats`.

## Widget refresh

macOS throttles widget reloads. The app asks for one every 15 s while it runs, and the
widget also schedules its own reload every 60 s. Expect the widget to be 15 s to a few
minutes behind; the menu bar updates every second. The widget is sandboxed. It reads the
app's snapshot (`~/Library/Application Support/GPUMonitor/snapshot.json`) for its
sparkline, and for its values when the registry isn't reachable from the sandbox.
