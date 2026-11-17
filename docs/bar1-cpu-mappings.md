# CPU mappings of VRAM

## The problem

A process maps VRAM for the CPU through a slice of the 256 MiB BAR1 aperture. The kext creates the
mapping with IOKit (`IOMemoryDescriptor::withPhysicalAddress` + `createMappingInTask`). The
process can then make copies of that mapping that IOKit doesn't track: `mach_vm_remap`, `fork`
with shared inheritance, or a memory entry sent to another process. Freeing the memory removed
only IOKit's own mapping. A copy kept pointing at the BAR1 slice, and once the slice and the VRAM
behind it went to another process, the copy could read and write that process's memory.
`tools/bar1_alias_test` reproduced it (remap, fork and reuse cases).

**How Linux avoids it.** A mapping holds a reference on the buffer, copies of a mapping are
counted, and the driver can revoke every CPU mapping of a buffer at once
(`unmap_mapping_range()`); later accesses fault back into the driver. macOS gives a kext no
equivalent for device memory it has mapped into a task: the kext can change what the slice
points at, but it can't find or remove the copies.

## The kext fix

When VRAM that was ever CPU-mapped is freed:
1. its BAR1 slice is pointed at a shared, read-only, zeroed dummy page, and the BAR1 TLB is
   flushed;
2. the VRAM itself is freed (it's scrubbed before anyone else gets it);
3. the slice stays out of the BAR1 allocator ("held"). Only the connection that owned it may
   reuse it for a new mapping.

Held BAR1 is capped at half the aperture (about 110 MiB); past that, new CPU mappings of VRAM
fail. Held slices are kept across GSP-RM reboots (sleep, runtime power-off) and released only
when the kext reloads, i.e. at restart, unless NVPagerHook is installed
([below](#releasing-held-slices-early-nvpagerhook)). `NVStats.Bar1Held` shows the total. With the fix,
`bar1_alias_test` passes: copies read zeroes and their writes are dropped:

```bash
make build/bar1_alias_test
build/bar1_alias_test          # remap, fork and reuse; exit status 0 = safe
```

A passing case ends with lines like `victim: 8192 of 8192 words still B, 0 are X` and
`remap: SAFE`. Each run holds 448 KiB of BAR1 itself (its own test buffers). The `cap` case isn't
run by default: it fills the cap on purpose, and then every program's CPU mappings of VRAM fail
until a restart.

**Cost:** every process that CPU-maps VRAM leaves its peak mapped amount held until restart.
With apps allowed to map VRAM, one llama.cpp run held about 49 MiB, so the cap was reached after
2–3 runs, and after that programs failed with a map error.

### Alternatives considered

| Option | Verdict |
|---|---|
| Revoke with `IOMemoryMap::redirect` and `kIOMapUnique` | `kIOMapUnique` only makes IOKit create its own map object; it doesn't make the VM system report copies. Whether `redirect` also reaches copies of the mapping isn't known yet. The test is `bar1_alias_test` against a kext that revokes with `redirect` and frees the slice at once, instead of holding it. If the copies read zeroes or fault, the hold and the cap can go |
| Never let the CPU map VRAM | Removes the problem entirely; NVK does this by default (next section). The kext still allows CPU mappings of VRAM, for three users: NVK with `nvkmapvram`, the tests (`nvtest`, `bar1_alias_test`), and NVK builds from nvbringup-mesa patches before 0007, which gave apps host-visible VRAM |

## Releasing held slices early (NVPagerHook)

**Status: built and run on a TU117 laptop (macOS 14.8.9, xnu-10063.141.1) with `nvvrammap=1`,
2026-11-17/18.** The self-test passed, including its in-kernel direct-I/O case. The four release
cases and the alias suite passed, and 53 llama-bench runs with CPU-mapped VRAM left nothing held.
The design comes from reading XNU (`xnu-10002.1.13` to `xnu-12377.121.6`, Sonoma 14.0 to Tahoe
26). On that laptop, peer-to-peer isn't allowed (below), so by default no process gets a CPU
mapping of VRAM and the hook has nothing to release. It matters where peer-to-peer is allowed,
or with `nvvrammap=1`.

The kernel does know when the last copy of a mapping goes; it just tells no driver.

- Behind a `withPhysicalAddress` mapping sits one VM object with a device pager. Every copy
  (remap, shared fork, memory entry) references that same object.
- The object's reference count is 1 for itself, 1 for the kext's descriptor, and 1 for each
  mapping, copy or memory entry.
- When the count is about to drop from 2 to 1, the kernel calls `device_pager_last_unmap`.
  A mapping's page-table entries are removed and the TLB flushed before its reference is dropped.
- `memFree` releases the kext's own mapping and descriptor. After that, the call comes exactly
  when the last copy is gone: inside `memFree` if there was none, otherwise when the process
  holding it unmaps it, exits, or drops its memory entry.

`pagerhook/` builds **NVPagerHook.kext**, a Lilu plugin that routes `device_pager_last_unmap`
and passes each call to NVBringup (`src/nv_pager_hook.h`). NVBringup then:

1. puts the pager of each fresh CPU mapping of VRAM on a watch list (`memMap`);
2. at `memFree`, gives the slice straight back to the allocator if the pager reported during
   the release (no copy existed), and nothing is held;
3. otherwise holds the slice on the dummy page as before, and releases it from a kernel
   thread when the pager reports;
4. in either case, only once no I/O is in flight on the pages (below).

A watched slice is never reused by its owner meanwhile, so a copy only ever reads zeroes.
Slices held without a watch (before the hook was active, or reused ones) stay held until restart.

**No mapping left doesn't mean nobody is using the pages.** Direct I/O (`F_NOCACHE`) on a mapping
of VRAM goes through `cluster_write_contig` / `cluster_read_contig`, and the disk DMAs straight to
or from the BAR1 address. Its I/O page list (UPL) holds no reference on the VM object, only the
object's `activity_in_progress` count, and the kernel doesn't wait for that before reporting the
last unmap. So a process that frees the memory during such an I/O gets the report with the DMA
still running. NVPagerHook version 1 released the slice then. On 2026-11-17 the Mac panicked with
"nvme: Controller fatal bit set. Write" on its internal SSD, which is what a disk DMA from released
or unmapped BAR1 does, though it was never confirmed that `release-dio` caused it. Version 2 asks
before treating a report as final:

- the listener reads the object's in-flight count (`io`) while the object is still alive. With
  I/O in flight it takes a device pager reference (`pin`), which keeps the object from being
  destroyed, and the slice waits on the dummy page;
- the release thread polls the pinned pagers (2 ms, backing off to 100 ms) and releases the
  slice, dropping the pin, once the count is 0;
- if the object doesn't look as expected (`io` returns -1), the slice stays held for good, as it
  would without the hook.

There is no race with an I/O just starting: `vm_map_create_upl` raises the count before it drops
its temporary object reference, so the report then comes from that drop, with the count already
raised. After the report, no new I/O can start: that needs a mapping or a memory entry.
`io` reads two private `vm_object` fields (layout in `pagerhook/src/NVPagerHook.cpp`, checked
against xnu-10063.141.1's source and its shipped code). Each call checks the pager and object
point at each other, and the self-test checks the count itself.

**It fails closed.** Without Lilu, without the kext, with `-nvpgroff` or `nvbar1hook=0`, if
the symbol can't be resolved, or if the self-test fails, the driver behaves exactly as described
above. The self-test runs at the first CPU mapping of VRAM, on a private kernel mapping of one
BAR1 page: the pager read from the descriptor must be the one the kernel reports, it must not
report while the descriptor is alive, and it must report when the descriptor is released. It
also replays the direct-I/O case without any DMA: `io` must read 0, then at least 1 while an
`IOMemoryDescriptor::prepare()` page list is held on the mapping. The report that comes while it
is held must be deferred (pinned), and after `complete()` the entry must settle and be released.
NVBringup refuses NVPagerHook version 1, so a mixed install keeps everything held.

### Install and check

```bash
tools/check_pager_hook.sh symbol        # before anything else: is the symbol in this kernel?
make -C pagerhook LILU=<Lilu checkout>/Lilu KSDK=<MacKernelSDK checkout>
make -C pagerhook check
```

Copy `pagerhook/build/NVPagerHook.kext` to `EFI/OC/Kexts`, add it to `config.plist` → Kernel →
Add after Lilu.kext, and reboot. It and NVBringup load in either order. Then:

```bash
make build/bar1_alias_test
build/bar1_alias_test release           # the four release cases; exit status 0 = pass
tools/check_pager_hook.sh status        # hook: active; Bar1Released grows, Bar1HookEarly stays 0
build/bar1_alias_test                   # the whole suite must still be SAFE / PASS
tools/bar1_release_soak.sh 100          # 100 llama-bench runs with CPU-mapped VRAM: nothing stays held
```

| case | must show |
|---|---|
| `release-none` | no copy: BAR1 in use is back right after the free |
| `release-remap` | a `mach_vm_remap` copy: still held 0.5 s after the free, released when the copy is deallocated |
| `release-fork` | a child sharing the mapping: held until the child exits |
| `release-entry` | a memory entry never mapped: held until its port is deallocated |

`NVStats` gains `Bar1Hook` (0 none, 1 attached, 2 active, 3 failed, 4 off), `Bar1Released` and
`Bar1ReleasedSlices` (given back early), and `Bar1HookEarly` (a pager that reported before its
memory was freed; must stay 0, anything else means the kernel doesn't count as described here),
`Bar1IoDeferred` (releases that waited for I/O in flight) and `Bar1IoUnknown` (reports that
couldn't be checked for I/O, whose slices stay held; must stay 0).
`nvmac_info.features` has `NVMAC_FEATURE_BAR1_RELEASE` while the hook is active.

**Released slices stay on the zero page.** A released CPU-mapped slice goes back to the
allocator with its PTEs still on the read-only dummy page, not unmapped. `bar1Map` clears them
only where a new allocation overlaps (the driver lists those ranges). So a device still reading a
released range, a DMA the I/O check missed, reads zeroes instead of hitting unmapped BAR1. That
fault, not the data, is what can take a disk controller down. Writes to the dummy page are
dropped, as for held slices. While `bar1Map` maps a new allocation over such a range, the range
is unmapped for a moment between clearing the dummy PTEs and writing the new ones.
`nvmac_info.features` has `NVMAC_FEATURE_BAR1_ZERO_RELEASE` for this.

`build/bar1_alias_test release-dio` frees VRAM while a 64 MiB `F_NOCACHE` `pwrite` from it is in
flight. It must show BAR1 coming back only after the write returns, and nothing in the file but
pattern A and zeroes. Zeroes are expected: from the free on, the slice is on the zero page.

- `release-dio` (safe mode) allocates nothing during the write, so a release the I/O check
  misses leaves the DMA reading the zero page. That shows as VULNERABLE without the disk reading
  unmapped BAR1. It needs `NVMAC_FEATURE_BAR1_IO_CHECK`, `NVMAC_FEATURE_BAR1_ZERO_RELEASE` and
  `NVMAC_FEATURE_P2P` (below).
  The remaining risk is another program being handed the range within the write's milliseconds.
- `release-dio-reuse` also maps 64 MiB of pattern B over the released range, to show another
  allocation's data leaking. While that mapping is written, the range is briefly unmapped, so
  run safe mode first.

**Don't run either against a version 1 install, or a build without the zero page**: a disk DMA
into unmapped BAR1 panicked the Mac on 2026-11-17, though it wasn't confirmed the test was running
then. Both refuse to run without the feature bits.

**Peer-to-peer has to be allowed too.** A disk DMAing to BAR1 is a peer-to-peer transfer, and a
root complex that doesn't forward those may drop it, which a disk controller can take as fatal.
NVBringup follows Linux's `p2pdma` policy (`src/NVP2p.cpp`) and sets `NVMAC_FEATURE_P2P` only if,
for every PCI device with a disk under it (NVMe, AHCI, the USB controller of a USB disk; the GPU's
own functions aside), one of these holds:

1. it shares an upstream bridge with the GPU (a PCIe switch) and no port on the path between them
   has ACS P2P redirect (RR, CR or EC) on;
2. the CPU is AMD family 17h (Zen) or newer;
3. the host bridge device (00.0 on the root bus, or the first root port) is on Linux's list
   (some Xeon, Haswell client, Skylake-E and later Intel server parts, Google, NVIDIA, Zhaoxin and
   Alibaba CPUs; some entries only for two devices under the same host bridge).

The peer of a direct I/O is whichever disk the file is on, which the driver can't know when it
maps VRAM, so every disk controller has to pass. The check reruns at most once a second, as disks
come and go. The answer and the reason are logged (`P2P:`) and in the GPU's `NVP2P` property
(`ioreg -r -c NVBringup -d 1 | grep NVP2P`). Both `release-dio` cases refuse to run without the
bit.

This Mac (i5-9300H, host bridge 8086:3e10, the GPU on root port PEG0 and the disks on other
root ports) passes none of the three, so `release-dio` refuses here.

**Without peer-to-peer, no process gets a CPU mapping of VRAM.** The driver can't see a direct I/O
on a mapping it handed out, so the only way to keep one from reaching BAR1 is not to hand out the
mapping. Where `NVMAC_FEATURE_P2P` is clear, `NVMAC_MEM_MAP` of CPU-mappable VRAM and
`NVMAC_DISPLAY_MAP` return `kIOReturnNotPermitted` (logged once, counted in `NVStats`
`VramMapRefused`), and `nvmac_info.features` lacks `NVMAC_FEATURE_VRAM_MAP`. System memory (GART)
maps as always.

- NVK needs no change: it offers no host-visible VRAM anyway, and the buffers `nvkmapvram` would put
  in CPU-mapped VRAM fall back to system memory when the map fails. So on such a machine
  `nvkmapvram` has no effect, and runs go at the default speed.
- `nvvdisplay` loses its CPU-copy fallback into the scanout buffer (`NVMAC_DISPLAY_MAP`); its
  GPU-copy path is unaffected.
- `bar1_alias_test` skips its VRAM cases and checks the refusal instead (`refusal`: the map must
  return `kIOReturnNotPermitted` and GART must still map). `nvtest` does the same in its memory
  test, and its copy round trip ends in GART instead of CPU-visible VRAM.
- With no CPU mappings of VRAM, nothing is held, so the hook (`NVPagerHook`) has nothing to release.
  Its self-test runs at the first CPU mapping of VRAM, so it stays `attached`.

**A disk that comes later takes the mappings back.** A disk without peer-to-peer support can arrive
after mappings were handed out (a Thunderbolt or USB4 NVMe drive). Each new `IOMedia` triggers a
recheck at once, and `fillInfo`'s once-a-second recheck catches the rest. When the answer turns from
allowed to not allowed, every CPU mapping of VRAM the driver handed out is removed from its process
(logged, counted in `NVStats` `VramMapRevoked`). A direct I/O on one from then on fails with `EFAULT`,
and a process that touches one again faults. The memory stays allocated and is freed as before. Not
covered: an I/O the new disk already has in flight when it appears, and duplicates a process made
itself (`mach_vm_remap`, `fork`), which the driver can't see.

Boot-arg `nvvrammap=1` allows the mappings anyway: you accept that a program doing direct I/O on
one can hang the machine. That's for testing `nvkmapvram`, the hook and the alias cases here.
`nvvrammap=0` refuses them even where peer-to-peer is allowed.

### What was seen on the TU117 laptop

- `_device_pager_last_unmap` is in macOS 14.8.9's kernel symbol table
  (`check_pager_hook.sh symbol`), and so are `device_pager_ops`, `device_pager_reference` and
  `device_pager_deallocate`. The `io` offsets match that kernel's shipped code.
- Direct I/O from CPU-mapped VRAM to the internal NVMe SSD can take the machine down:
  - 2026-11-17: a kernel panic, "nvme: Controller fatal bit set. Write" (it wasn't confirmed
    what was running);
  - 2026-11-18: a hard hang during a plain 64 MiB `F_NOCACHE` `pwrite` from mapped VRAM, with no
    free at all;
  - one `release-dio` safe-mode round got `EFAULT` (the free came before the I/O was set up), so
    no DMA happened.

  So the SSD DMAing from the GPU's BAR1 fails on this machine in itself, release or not. That's
  why peer-to-peer is checked and CPU mappings of VRAM are refused (above).

### What is not known yet

- Shipped kernels other than 14.8.9: run `check_pager_hook.sh symbol` first. It checks
  `_device_pager_last_unmap` and the three symbols the I/O check needs (`_device_pager_ops`,
  `_device_pager_reference`, `_device_pager_deallocate`), and fails when any is missing or the
  kernel isn't the one the layout was checked on. They're non-static in every source release
  checked.
- Direct I/O with real DMA against version 2 hasn't been run anywhere: it needs a machine where
  peer-to-peer is allowed. The self-test covers it without DMA.
- Another driver that took the physical address another way, not through an I/O page list on
  this object (for example by reading the PTEs of a mapping it was given), isn't covered: the
  count only knows about page lists.
- The `io` layout (VM object offsets 0x50, 0x74, 0x78) was checked only on xnu-10063.141.1
  (macOS 14.8.9). NVPagerHook routes `last_unmap` on Sonoma through Tahoe, but it turns the I/O
  check (and with it the early release) off on any kernel but Darwin 23.6 (macOS 14.6 to 14.8).
  `check_pager_hook.sh symbol` says so. Boot-arg `-nvpgrlayout` turns it on anyway, leaving only
  the per-call checks and the self-test between a wrong layout and a slice released under a DMA.
- macOS releases after `xnu-12377.121.6`: re-run the symbol check and the release cases after
  each update. The self-test catches a changed layout or count on its own and switches the
  release off.
- A routed kernel function can't be unrouted, so NVPagerHook never unloads.

## Where NVK puts CPU-mapped memory

The kext fix makes CPU-mapped VRAM a limited resource, so NVK uses it sparingly:

- **Default (option A):** no host-visible VRAM memory type or heap on macOS; every buffer the CPU
  maps, the app's and NVK's own, is system memory. Nothing is ever held.
- **Boot-arg `nvkmapvram=<list>`:** NVK's own CPU-mapped buffers go back in VRAM:

  | value | what goes in CPU-mapped VRAM |
  |---|---|
  | `1` or `all` | every such buffer (option B) |
  | `desc` | descriptor pools and tables |
  | `cmd`, `cmd:<n>` | command buffer chunks (64 KiB each), at most n per device |

  Values combine with commas (`desc,cmd:32`). NVK maps these buffers when it allocates them, so
  once the kext's cap is reached they come from system memory instead of failing: programs
  slow down to option A speed instead of crashing. `NVK_MACOS_MAPVRAM` overrides the boot-arg
  for one process (for measuring; the kext's cap applies either way).
- **Where peer-to-peer isn't allowed** (no `NVMAC_FEATURE_VRAM_MAP`), the kext refuses every CPU
  mapping of VRAM, so `nvkmapvram` falls back to system memory for all of it and has no effect:
  option A speed. On the TU117 laptop, a llama-bench run with `NVK_MACOS_MAPVRAM=1` had 63 maps
  refused and ran at 93 t/s. Add `nvvrammap=1` as well to measure option B there, accepting the
  direct-I/O risk.

### Turn on `nvkmapvram`

Add it to the boot-args in OpenCore's `config.plist` (NVRAM → Add →
`7C436110-AB2A-4BBB-A880-FE41995C9F82` → `boot-args`), keeping what's there, then reboot. The
path below is an example; use your EFI's:

```bash
cfg=/path/to/EFI/EFI/OC/config.plist
key=':NVRAM:Add:7C436110-AB2A-4BBB-A880-FE41995C9F82:boot-args'
old=$(/usr/libexec/PlistBuddy -c "Print $key" "$cfg")
/usr/libexec/PlistBuddy -c "Set $key $old nvkmapvram=1" "$cfg"
```

After the reboot, confirm it and watch held BAR1 grow per program run:

```bash
sysctl -n kern.bootargs | tr ' ' '\n' | grep nvkmapvram      # nvkmapvram=1
ioreg -r -c NVBringup -d 1 | grep -oE '"Bar1Held"=[0-9]+'    # bytes held until restart
```

To try a mode for one program without rebooting (an unknown value prints a
`nvkmapvram: ignoring` warning and is skipped):

```bash
NVK_MACOS_MAPVRAM=desc,cmd:32 llama-bench -m model.gguf -ngl 99 -p 512 -n 128 -r 5
```

**Why option A is slower.** A profile of llama.cpp's token generation (`GGML_VK_PERF_LOGGER`)
shows it's GPU-bound with a floor of about 10 µs per dispatch across about 300 dispatches per
token. With NVK's command buffers and descriptors in system memory, the GPU fetches them over
PCIe for every dispatch.

## Measurements

llama-bench `-p 512 -n 128`, 5 repetitions, GTX 1650, shader cache disabled. "Held" is the growth
of `Bar1Held` per run; "runs" is how many runs fit under the ~110 MiB cap before the fallback.

Qwen2.5 0.5B Instruct Q4_K_M (prompt processing 2234–2267 t/s in every mode):

| mode | generation t/s | held per run | runs |
|---|---|---|---|
| none (A, default) | 96.2–97.4 | 0 | unlimited |
| `desc` | 98.8 | 384 KiB | ~300 |
| `desc,cmd:2` / `:4` / `:8` | 99.0 / 98.6 / 99.3 | 0.5 / 0.6 / 0.9 MiB | 120–220 |
| `desc,cmd:16` | 105.2 | 1.4 MiB | ~80 |
| `cmd:24` | 106.8 | 1.5 MiB | ~75 |
| `cmd` (all 36 chunks) | 107.6 | 2.3 MiB | ~50 |
| `desc,cmd:32` | 113.4 | 2.4 MiB | ~47 |
| `desc,cmd` | 112.9 | 2.7 MiB | ~42 |
| `1` (B) | 113.8–114.1 | 2.7 MiB | ~42 |

Llama 3.2 3B Instruct Q4_K_M: A 32.1–32.7 t/s generation, B 34.1–34.4 (+6 %); prompt processing
about 420 t/s in both; B held 2.6 MiB per run.

What the numbers show:
- The gain needs command chunks and descriptors in VRAM together: chunks alone +11 t/s,
  descriptors alone +2, both +17.
- Token generation cycles through about 28 command chunks (1.75 MiB). A small chunk budget does
  nothing, because the first chunks go to whichever command pools allocate first (model load,
  prompt processing), not the ones generation reuses. A budget big enough to help holds almost
  as much as B.
- The larger the model, the smaller the gain: per-dispatch overhead is a smaller share of each
  token.

## Decision

- **Option A is the default.** Nothing is held, a restart is never needed, and there is nothing
  left for a copy of a mapping to reach. The price is about 15 % slower generation on a 0.5B
  model and about 6 % on a 3B one.
- **`nvkmapvram=1` for speed**, if the machine usually restarts before about 42 llama.cpp runs:
  those run at full speed, later ones at option A speed until the next restart. `desc` and `cmd:<n>`
  remain available but aren't worth it on these numbers.
- **Not done: command buffers in VRAM without a CPU mapping** (the CPU writes them to system
  memory and the copy engine moves them to VRAM before each submit). It would hold no BAR1, but
  the descriptors would stay in system memory, so at most the chunks' share of the gain is
  available (+11 of the +17 t/s), and every submit would gain a copy and a wait for it. To be reconsidered only if the `redirect` test fails and the speed
  matters.
- If `IOMemoryMap::redirect` turns out to revoke copies, the hold and the cap go away, and B can
  become the default.
- NVPagerHook passed its tests on the TU117 laptop (with `nvvrammap=1`). Wherever peer-to-peer is
  allowed and the hook is active, `nvkmapvram=1` no longer runs into the cap for ordinary programs,
  so B could become the default there. The cap stays for processes that keep copies alive.
- **Without peer-to-peer, no CPU mappings of VRAM at all** (2026-11-18). Direct I/O on such a
  mapping hung the TU117 laptop, and the driver can't see that I/O to stop it. So where Linux's
  `p2pdma` rules don't allow peer-to-peer (most Intel client machines), the kext refuses the
  mappings and option A is the only mode, unless `nvvrammap=1` overrides it.
