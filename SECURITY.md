# Security

NVBringup is a research prototype and has not had an independent security audit. It is a macOS
kernel extension that gives user-space programs access to GPU memory, channels and (through
NVIDIA's GSP-RM firmware) GPU control, so bugs in it can affect the whole system. Don't run it on
a machine that holds anything you can't afford to lose, and don't rely on it to contain untrusted
code.

If you find a vulnerability, please report it privately through this repository's Security tab
("Report a vulnerability") rather than in a public issue. This is a prototype maintained on a
best-effort basis: there is no fix timeline and no bug bounty.

The isolation the kext aims for (who can open the GPU, per-connection address spaces, scrubbed
VRAM, the GSP-RM control filter and the BAR1 mapping fix) is described in
[docs/gpu-interface.md](docs/gpu-interface.md) and
[docs/bar1-cpu-mappings.md](docs/bar1-cpu-mappings.md). A way around any of it is worth
reporting. Problems in NVIDIA's firmware, Mesa, llama.cpp, OpenCore or macOS belong with those
projects.
