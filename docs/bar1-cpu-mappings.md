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
when the kext reloads, i.e. at restart. `NVStats.Bar1Held` shows the total. With the fix,
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
