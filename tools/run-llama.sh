#!/bin/bash
# Run llama.cpp's Vulkan backend on NVK. Needs root or the console user (or boot-arg nvgpu_users=1).
#   sudo tools/run-llama.sh <run-name> [ops|bench|gen|all]
# Output goes to runs/<run-name>-*.txt.
set -u
cd "$(dirname "$0")/.."
RUN=${1:?run name}; WHAT=${2:-all}
# Paths come from the environment or local.env in the repo root:
#   NVB_LLAMA_BIN     llama.cpp's build/bin (llama-bench, llama-simple, test-backend-ops)
#   NVB_BENCH_MODEL   the GGUF to run
#   NVB_VK_ICD        NVK's ICD manifest (optional; else the Vulkan loader's search)
[ -f local.env ] && { set -a; . ./local.env; set +a; }
B=${NVB_LLAMA_BIN:?set NVB_LLAMA_BIN to the llama.cpp build/bin directory}
M=${NVB_BENCH_MODEL:?set NVB_BENCH_MODEL to a GGUF model}
[ -n "${NVB_VK_ICD:-}" ] && export VK_DRIVER_FILES=$NVB_VK_ICD
export MESA_SHADER_CACHE_DISABLE=true
export GGML_VK_VISIBLE_DEVICES=0

[ -x "$B/llama-bench" ] || { echo "no llama-bench in $B (is its volume mounted?)"; exit 1; }

log() { echo "== $*"; }
if [ "$WHAT" = ops ] || [ "$WHAT" = all ]; then
  log "test-backend-ops (Vulkan vs CPU) -> runs/$RUN-ops.txt"
  "$B/test-backend-ops" -b Vulkan0 2>&1 | tee "runs/$RUN-ops.txt" | grep -E 'FAIL|passed|Backend|error|lost' | tail -40
fi
if [ "$WHAT" = bench ] || [ "$WHAT" = all ]; then
  log "llama-bench -> runs/$RUN-bench.txt"
  "$B/llama-bench" -m "$M" -ngl 99 -p 512 -n 128 2>&1 | tee "runs/$RUN-bench.txt"
fi
if [ "$WHAT" = gen ] || [ "$WHAT" = all ]; then
  log "llama-simple -> runs/$RUN-gen.txt"
  "$B/llama-simple" -m "$M" -ngl 99 -n 64 "The capital of France is" 2>&1 | tee "runs/$RUN-gen.txt" | tail -15
fi
ioreg -a -r -c NVBringup -d 1 | plutil -extract 0.NVLog raw -o - - > "runs/nvlog-$RUN.txt" 2>/dev/null
log "kext log -> runs/nvlog-$RUN.txt"
