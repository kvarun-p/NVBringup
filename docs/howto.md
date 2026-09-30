# How to

Task recipes for an installed NVBringup (installation itself is in the top-level
[README](../README.md)). Commands run from the repository root, as the console user unless
they show `sudo`. llama.cpp commands assume its `build/bin` is on `PATH`.

## Check that the GPU is up

The kext publishes its state in the I/O Registry; no root needed.

```bash
ioreg -r -c NVBringup -d 1 | grep -E '"NVGpuName"|"NVFrtsResult"|"NVGspResult"'
build/nvgsp power status
tools/verify_install.sh          # every install step, PASS/WARN/FAIL; doesn't wake the GPU
```

`NVGspResult` is `running`, or `powered off (idle)` when nothing has used the GPU for 30 s (the
next program powers it back on in about 2 s). `NVFrtsResult` must be `success`; if it says
"WPR2 already set up", shut down fully and power on ([power.md](power.md#shutdown-and-restart)).

## Run everything as a test

`--full` also powers the GPU on and runs `nvtest` (the kernel interface, 55 checks), `vktest`
(NVK) and a short llama-bench:

```bash
tools/verify_install.sh --full
build/nvtest                     # just the kernel interface: "nvtest: 55 checks passed, 0 failed"
build/nvtest -v                  # with details: bind timings, copy bandwidth, latencies
```

## Benchmark llama.cpp on the GPU

```bash
llama-bench -m ~/models/qwen2.5-0.5b-instruct-q4_k_m.gguf -ngl 99 -p 512 -n 128 -r 5
```

`-ngl 99` puts every layer on the GPU. On the GTX 1650 expect about 2,250 t/s prompt
processing and 97 t/s generation for this model (about 112–114 with `nvkmapvram=1`). The first run after
boot can be much slower (once pp512 587 t/s) while shaders compile and clocks ramp up; repeat
it. On machines with more than one Vulkan
device, add `GGML_VK_VISIBLE_DEVICES=0` (the index from `llama-server --list-devices`).

## Profile where generation time goes

llama.cpp's Vulkan backend can time every GPU operation:

```bash
GGML_VK_PERF_LOGGER=1 GGML_VK_PERF_LOGGER_FREQUENCY=1 \
    llama-bench -m ~/models/qwen2.5-0.5b-instruct-q4_k_m.gguf -ngl 99 -p 0 -n 8 -r 1 2>&1 \
    | grep -A40 'Vulkan Timings'
```

Each line gives an operation, its count, time per dispatch and total. This is how token
generation was shown to be GPU-bound and dispatch-bound ([bar1-cpu-mappings.md](bar1-cpu-mappings.md),
[nvk-and-llama.md](nvk-and-llama.md)).

## Serve a model to other programs

```bash
llama-server -m ~/models/qwen2.5-1.5b-instruct-q4_k_m.gguf -ngl 99 --host 127.0.0.1 --port 8080 &
curl -s http://127.0.0.1:8080/v1/chat/completions -H 'Content-Type: application/json' \
    -d '{"messages": [{"role": "user", "content": "What is the capital of France?"}]}'
```

The GPU stays powered while the server holds its Vulkan device, even when llama-server's own idle
sleep has freed the model. Stop the server (or, in router mode, the model process) to let the
GPU power off.

## Faster generation: nvkmapvram

Puts NVK's command buffers and descriptors in CPU-mapped VRAM: about +15 % generation on a 0.5B
model and +6 % on 3B, at ~2.7 MiB of held BAR1 per program run until a restart (about 42 runs;
after that, programs run at default speed). Why the limit exists:
[bar1-cpu-mappings.md](bar1-cpu-mappings.md).

Try it on one program first:

```bash
NVK_MACOS_MAPVRAM=1 llama-bench -m ~/models/qwen2.5-0.5b-instruct-q4_k_m.gguf -ngl 99 -p 512 -n 128
ioreg -r -c NVBringup -d 1 | grep -oE '"Bar1Held"=[0-9]+'
```

To make it the default, add `nvkmapvram=1` to the boot-args in OpenCore's `config.plist` (the
path is an example; use your EFI's) and reboot:

```bash
cfg=/path/to/EFI/EFI/OC/config.plist
key=':NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args'
/usr/libexec/PlistBuddy -c "Set $key $(/usr/libexec/PlistBuddy -c "Print $key" "$cfg") nvkmapvram=1" "$cfg"
```

Held BAR1 is released only by a restart.

## Control GPU power

```bash
build/nvgsp power status         # GPU on, mode auto (idle power-off after 30 s); …; 0 connections
build/nvgsp power on             # keep it powered (no 2 s wait for the next program)
build/nvgsp power auto           # back to idle power-off
```

`power off` powers it off now and hides it from Vulkan. The mode resets to auto at every boot;
boot-arg `nvidle=<seconds>` changes the idle time (`0` keeps it on). GPU Monitor has the same
switch. Details: [power.md](power.md).

## Let other users use the GPU

By default only root and the user logged in at the console can open the GPU (like a Linux render
node). A program run by someone else fails with "NVBringup: not privileged". Add
`nvgpu_users=1` to the boot-args to admit everyone. Why: [gpu-interface.md](gpu-interface.md#who-can-open-the-gpu).

## Read the logs

```bash
ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o - - | tail -60   # kext log
build/nvgsp status               # recent GSP-RM message lines
ls -t /Library/Logs/NVBringup/ | head -3     # boot logs saved by the daemon
sudo build/nvgsp logs /tmp/gsp-logs         # GSP-RM's own log buffers (raw)
```

The kext keeps its log in the registry because its early-boot `IOLog` output is lost
([approach.md](approach.md#building-and-testing)).

## Investigate a "device lost"

A program gets `VK_ERROR_DEVICE_LOST` when its GPU context faulted; other programs keep
running.

```bash
ioreg -r -c NVBringup -d 1 | grep '"NVGspLastRC"'      # last fault type: 31 = MMU fault, 13 = graphics exception
ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o - - | grep 'RC triggered' | tail
```

A line reads like `RC triggered: engine 0x1 chid 3 exceptType 0x1f …, MMU fault addr
0x0000007000000000`: an MMU fault gives the faulting GPU address. A graphics exception (13) usually means a shader
problem; rerun with `NAK_DEBUG=serial` to rule out instruction scheduling, the first step used for
both NAK fixes ([nvk-and-llama.md](nvk-and-llama.md#nak-fixes-for-turing)). `nvtest` causes one
MMU fault on purpose (address `0x7000000000`); that one is expected.

## Check llama.cpp's results against the CPU

```bash
test-backend-ops -b Vulkan0                          # everything (takes a while)
test-backend-ops -b Vulkan0 -o MUL_MAT,ARGSORT       # selected operations
```

Expect all OK except three f16 `SQRT` precision cases.

## Use the GPU from C without Vulkan

`libnvmac` is the C library over the kext's interface. A complete example that copies memory with
the copy engine:

```bash
clang -std=c11 -Wall -Wextra -Isrc tools/libnvmac_example.c tools/libnvmac.c \
    -framework IOKit -framework CoreFoundation -o build/libnvmac_example
build/libnvmac_example           # NVIDIA GeForce GTX 1650, 3888 MiB VRAM / copy ok
```

Walkthrough and design: [gpu-interface.md](gpu-interface.md#using-the-interface-directly).

## Confirm a restart cleaned up

After a restart, the kext reports how the previous session's teardown went:

```bash
ioreg -r -c NVBringup -d 1 | grep '"NVLastTeardown"'  # e.g. "restart: ok (0x0), WPR2 hi 0x00000000"
```

`ok` with WPR2 hi 0 means the GPU was left unlocked for the next OS. Why it matters:
[firmware-and-boot.md](firmware-and-boot.md#teardown-clears-wpr2).

## Check the CPU-mapping protection

```bash
make build/bar1_alias_test
build/bar1_alias_test            # exit status 0 = safe
```

Each case prints its verdict (`remap: SAFE`, `fork: SAFE`, `reuse: PASS`), the other process's
memory must be intact (`victim: 8192 of 8192 words still B, 0 are X`), and the last line is
`overall: SAFE / PASS`.
Details: [bar1-cpu-mappings.md](bar1-cpu-mappings.md).
