#!/bin/bash
# Soak test for the early release of held BAR1 (NVPagerHook, docs/bar1-cpu-mappings.md): runs a program that
# CPU-maps VRAM many times and checks that held BAR1 goes back to where it started after every run.
#   tools/bar1_release_soak.sh [runs] [command ...]
# Default: 100 runs of llama-bench with NVK_MACOS_MAPVRAM=1 (NVB_LLAMA_BIN, NVB_BENCH_MODEL and optionally
# NVB_VK_ICD from the environment or local.env, as tools/run-llama.sh). Any other command works, e.g.
#   tools/bar1_release_soak.sh 500 build/bar1_alias_test release-none
# Without the hook each llama-bench run leaves about 2.7 MiB held and the cap is reached after about 42 runs.
# Output: one line per run, and runs/bar1-soak-<time>.txt. Exit status 0 if nothing stayed held.
set -u
cd "$(dirname "$0")/.."
[ -f local.env ] && { set -a; . ./local.env; set +a; }
runs=${1:-100}
[ $# -gt 0 ] && shift
if [ $# -eq 0 ]; then
    B=${NVB_LLAMA_BIN:?set NVB_LLAMA_BIN to the llama.cpp build/bin directory, or give a command}
    M=${NVB_BENCH_MODEL:?set NVB_BENCH_MODEL to a GGUF model, or give a command}
    [ -n "${NVB_VK_ICD:-}" ] && export VK_DRIVER_FILES=$NVB_VK_ICD
    export MESA_SHADER_CACHE_DISABLE=true GGML_VK_VISIBLE_DEVICES=0 NVK_MACOS_MAPVRAM=1
    set -- "$B/llama-bench" -m "$M" -ngl 99 -p 512 -n 128 -r 1
fi

stat() {    # NVStats is republished about once a second
    ioreg -r -c NVBringup -d 1 | grep '"NVStats"' | grep -oE "\"$1\"=[0-9]+" | cut -d= -f2
}
settled() { # held BAR1 once it is back at $1, or after 5 s
    local h
    for _ in $(seq 25); do
        h=$(stat Bar1Held)
        [ "${h:-x}" = "$1" ] && break
        sleep 0.2
    done
    echo "${h:-0}"
}

if [ "$(stat Bar1Hook)" != 2 ]; then
    echo "NVPagerHook isn't active (tools/check_pager_hook.sh): every run would leave its BAR1 held. Not running."
    exit 1
fi
mkdir -p runs
out=runs/bar1-soak-$(date +%Y%m%d-%H%M%S).txt
base=$(stat Bar1Held); rel0=$(stat Bar1Released)
echo "start: Bar1Held $((base >> 10)) KiB; $runs runs of: $*" | tee "$out"
worst=0; failed=0
for i in $(seq "$runs"); do
    "$@" > "runs/.bar1-soak-last.txt" 2>&1
    rc=$?
    held=$(settled "$base")
    grew=$(( held - base ))
    [ $grew -gt $worst ] && worst=$grew
    [ $rc -ne 0 ] && failed=$((failed + 1))
    echo "run $i: exit $rc, Bar1Held $((held >> 10)) KiB ($((grew >> 10)) KiB over the start)" | tee -a "$out"
    if [ "$(stat Bar1Hook)" != 2 ]; then
        echo "the hook is no longer active: stopping" | tee -a "$out"; failed=$((failed + 1)); break
    fi
done
rel=$(( $(stat Bar1Released) - rel0 ))
early=$(stat Bar1HookEarly)
echo "end: $((rel >> 20)) MiB released early over the runs; most left held after a run: $((worst >> 10)) KiB;" \
     "$failed runs failed; Bar1HookEarly ${early:-0}" | tee -a "$out"
[ $worst -eq 0 ] && [ $failed -eq 0 ] && [ "${early:-0}" = 0 ]
