"""Unit tests for tools/nvctl and tools/nvcheck's pure parts, against a fake system root. No sudo, no GPU, nothing
outside a temporary folder changes:  python3 -m unittest discover -s tools/tests -v
"""
import importlib.machinery
import importlib.util
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(TOOLS))


def load(name):
    loader = importlib.machinery.SourceFileLoader(name, str(TOOLS / name))
    spec = importlib.util.spec_from_loader(name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod


nvctl = load("nvctl")
nvcheck = load("nvcheck")
import nvlib  # noqa: E402

GATE_OK = """#!/usr/bin/env python3
import json, sys
p = sys.argv[sys.argv.index("--json") + 1]
open(p, "w").write(json.dumps({"ok": %s, "checks": [{"name": "kpi.accel", "status": "%s"}]}))
sys.exit(%d)
"""


def kext(path, exe, payload):
    (path / "Contents/MacOS").mkdir(parents=True, exist_ok=True)
    (path / "Contents/Info.plist").write_text(
        '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>CFBundleExecutable</key>'
        "<string>%s</string></dict></plist>\n" % exe)
    (path / "Contents/MacOS" / exe).write_text(payload)


class FakeSystem(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="nvctl-test.")
        t = Path(self.tmp.name)
        self.root, self.repo = t / "root", t / "repo"
        # the checkout's build products
        kext(self.repo / "build/NVBringup.kext", "NVBringup", "nvbringup v2")
        kext(self.repo / "accel/build/NVMetalAccel.kext", "NVMetalAccel", "accel v2")
        kext(self.repo / "display/build/NVFramebuffer.kext", "NVFramebuffer", "fb v2")
        (self.repo / "src").mkdir(parents=True)
        (self.repo / "src/nv_stack.h").write_text("#define NV_STACK_ABI            1\n")
        # the installed system: older versions of all three
        self.efi = self.root / "Volumes/OPENCORE/EFI"
        kext(self.efi / "OC/Kexts/NVBringup.kext", "NVBringup", "nvbringup v1")
        (self.efi / "OC/config.plist").write_text("<plist/>")
        kext(self.root / "Library/Extensions/NVMetalAccel.kext", "NVMetalAccel", "accel v1")
        kext(self.root / "Library/Extensions/NVFramebuffer.kext", "NVFramebuffer", "fb v1")
        self.gate = t / "gate"
        self.set_gate(True)
        self.log = []

    def tearDown(self):
        self.tmp.cleanup()

    def set_gate(self, ok):
        self.gate.write_text(GATE_OK % ("True" if ok else "False", "pass" if ok else "fail", 0 if ok else 1))
        self.gate.chmod(0o755)

    def nvctl(self, *argv):
        self.log = []
        rc = nvctl.main(["--root", str(self.root), "--repo", str(self.repo), "--efi", str(self.efi)] + list(argv),
                        out=self.log.append)
        return rc

    def installed(self, rel):
        return (self.root / rel / "Contents/MacOS" / Path(rel).stem).read_text()

    def efi_kext(self):
        return (self.efi / "OC/Kexts/NVBringup.kext/Contents/MacOS/NVBringup").read_text()

    def manifest(self):
        return json.loads((self.root / nvctl.STATE.lstrip("/") / "installed.json").read_text())

    def install(self, *comps, apply=True, stamp="20261014-100000"):
        return self.nvctl("install", *comps, "--nvcheck", str(self.gate), "--stamp", stamp,
                          *(["--apply"] if apply else []))


class TestInstall(FakeSystem):
    def test_plan_changes_nothing(self):
        self.assertEqual(self.install(apply=False), 0)
        self.assertEqual(self.efi_kext(), "nvbringup v1")
        self.assertEqual(self.installed("Library/Extensions/NVMetalAccel.kext"), "accel v1")
        self.assertFalse((self.root / nvctl.STATE.lstrip("/")).exists())
        self.assertTrue(any("plan only" in l for l in self.log))
        self.assertTrue(any("kmutil load -p" in l for l in self.log))

    def test_apply_installs_backs_up_and_records(self):
        self.assertEqual(self.install(), 0, "\n".join(self.log))
        self.assertEqual(self.efi_kext(), "nvbringup v2")
        self.assertEqual(self.installed("Library/Extensions/NVMetalAccel.kext"), "accel v2")
        self.assertEqual(self.installed("Library/Extensions/NVFramebuffer.kext"), "fb v2")
        bset = self.root / nvctl.STATE.lstrip("/") / "backups/20261014-100000"
        self.assertEqual((bset / "accel/NVMetalAccel.kext/Contents/MacOS/NVMetalAccel").read_text(), "accel v1")
        self.assertEqual((bset / "efi/NVBringup.kext/Contents/MacOS/NVBringup").read_text(), "nvbringup v1")
        self.assertTrue((self.efi.parent / "EFI-backup-2026-10-14/OC/config.plist").exists())
        m = self.manifest()
        self.assertEqual(sorted(m["components"]), ["accel", "efi", "fb"])
        self.assertEqual(m["components"]["accel"]["stack_abi"], 1)
        self.assertEqual(m["transactions"][-1]["action"], "install")
        self.assertIsNone(m["transactions"][-1]["before"]["accel"])

    def test_second_install_is_a_no_op(self):
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.install(stamp="20261014-110000"), 0)
        self.assertTrue(any("nothing to install" in l for l in self.log), self.log)

    def test_failed_gate_installs_nothing(self):
        self.set_gate(False)
        self.assertEqual(self.install(), 1)
        self.assertEqual(self.efi_kext(), "nvbringup v1")
        self.assertTrue(any("nvcheck FAILED: kpi.accel" in l for l in self.log), self.log)

    def test_skip_check_refused_with_apply(self):
        self.assertEqual(self.nvctl("install", "--skip-check", "--apply"), 1)
        self.assertEqual(self.efi_kext(), "nvbringup v1")

    def test_aux_without_efi_refused_on_stack_abi(self):
        self.assertEqual(self.install("accel", "fb"), 1)
        self.assertTrue(any("stack ABI" in l for l in self.log))
        self.assertEqual(self.installed("Library/Extensions/NVMetalAccel.kext"), "accel v1")

    def test_aux_alone_after_efi_from_this_checkout(self):
        self.assertEqual(self.install("efi"), 0)
        kext(self.repo / "accel/build/NVMetalAccel.kext", "NVMetalAccel", "accel v3")
        self.assertEqual(self.install("accel", stamp="20261014-120000"), 0, "\n".join(self.log))
        self.assertEqual(self.installed("Library/Extensions/NVMetalAccel.kext"), "accel v3")

    def test_failure_undoes_finished_steps(self):
        real = nvctl.Runner.cmd

        def failing(runner, argv, root=False, check=True):
            if argv[:2] == ["ditto", "--norsrc"] and str(argv[-1]).endswith("Extensions/NVFramebuffer.kext"):
                if runner.apply:
                    raise nvctl.StepError("simulated copy failure")
            return real(runner, argv, root, check)
        nvctl.Runner.cmd = failing
        try:
            self.assertEqual(self.install(), 1)
        finally:
            nvctl.Runner.cmd = real
        self.assertEqual(self.efi_kext(), "nvbringup v1")
        self.assertEqual(self.installed("Library/Extensions/NVMetalAccel.kext"), "accel v1")
        self.assertEqual(self.installed("Library/Extensions/NVFramebuffer.kext"), "fb v1")
        self.assertFalse((self.root / nvctl.STATE.lstrip("/") / "installed.json").exists())
        self.assertTrue(any(l.startswith("undo accel") for l in self.log), self.log)


class TestRollback(FakeSystem):
    def test_rollback_restores_previous(self):
        self.assertEqual(self.install(), 0)
        self.assertEqual(self.nvctl("rollback", "--stamp", "20261014-130000"), 0)     # plan
        self.assertEqual(self.efi_kext(), "nvbringup v2")
        self.assertEqual(self.nvctl("rollback", "--apply", "--stamp", "20261014-130000"), 0, "\n".join(self.log))
        self.assertEqual(self.efi_kext(), "nvbringup v1")
        self.assertEqual(self.installed("Library/Extensions/NVMetalAccel.kext"), "accel v1")
        self.assertEqual(self.installed("Library/Extensions/NVFramebuffer.kext"), "fb v1")
        m = self.manifest()
        self.assertTrue(m["transactions"][-1]["action"].startswith("rollback of 20261014-100000"))

    def test_rollback_removes_what_was_new(self):
        kext(self.repo / "build/x", "x", "")      # unrelated
        (self.root / "Library/Extensions/NVFramebuffer.kext").rename(self.root / "fb-gone")
        self.assertEqual(self.install("efi", "accel", "fb"), 0, "\n".join(self.log))
        self.assertEqual(self.installed("Library/Extensions/NVFramebuffer.kext"), "fb v2")
        self.assertEqual(self.nvctl("rollback", "--apply", "--stamp", "20261014-130000"), 0, "\n".join(self.log))
        self.assertFalse((self.root / "Library/Extensions/NVFramebuffer.kext").exists())
        self.assertNotIn("fb", self.manifest()["components"])

    def test_rollback_without_transactions(self):
        self.assertEqual(self.nvctl("rollback"), 1)


class TestOther(FakeSystem):
    def test_status_fake(self):
        self.assertEqual(self.nvctl("status", "--json"), 0)
        st = json.loads("\n".join(self.log))
        self.assertFalse(st["components"]["accel"]["current"])
        self.install()
        self.nvctl("status", "--json")
        st = json.loads("\n".join(self.log))
        self.assertTrue(st["components"]["accel"]["current"])
        self.assertEqual(st["components"]["efi"]["manifest"]["stack_abi"], 1)

    def test_uninstall(self):
        self.install()
        self.assertEqual(self.nvctl("uninstall", "fb", "--apply", "--stamp", "20261014-140000"), 0, "\n".join(self.log))
        self.assertFalse((self.root / "Library/Extensions/NVFramebuffer.kext").exists())
        self.assertNotIn("fb", self.manifest()["components"])
        self.assertTrue((self.root / nvctl.STATE.lstrip("/") /
                         "backups/20261014-140000/fb/NVFramebuffer.kext").is_dir())

    def test_prune(self):
        b = self.root / nvctl.STATE.lstrip("/") / "backups"
        for i in range(7):
            (b / ("2026101%d-000000" % i)).mkdir(parents=True)
        self.assertEqual(self.nvctl("prune", "--keep", "3", "--apply"), 0)
        self.assertEqual(sorted(p.name for p in b.iterdir()), ["20261014-000000", "20261015-000000", "20261016-000000"])

    def test_boot_ok_is_recorded_not_run(self):
        self.assertEqual(self.nvctl("boot-ok", "--apply"), 0)
        self.assertTrue(any("nvram -d nv-boot-try" in l for l in self.log))


class TestNvcheckParts(unittest.TestCase):
    def test_parse_perf(self):
        a = ("  VRAM:             1.223 ms   109.7 GB/s\n  system memory:   21.348 ms     6.3 GB/s\n"
             "    56.628 ms  0.61 TFLOPS  (= 296 MHz)\n  median 0.063 ms, p90 0.093 ms\n"
             "  record 23.48 us/dispatch on the CPU, GPU 0.77 us/dispatch, wall 1.053 ms\n")
        n = ("    round trip (exec + wait): avg 15 us, worst 48 us\n    VRAM -> VRAM 256 MiB: 35.3 GB/s\n"
             "    system memory -> VRAM 64 MiB: 5.5 GB/s\n    VRAM -> system memory 64 MiB: 4.5 GB/s\n")
        m = nvcheck.parse_perf(a, n)
        self.assertEqual(set(m), set(nvcheck.METRICS))
        self.assertEqual(m["vram_copy_gbs"], 109.7)
        self.assertEqual(m["exec_roundtrip_us"], 15.0)

    def test_uapi_compare(self):
        with tempfile.TemporaryDirectory() as d:
            k, m = Path(d) / "k.h", Path(d) / "m.h"
            base = ("#include <stdint.h>\n#define NVMAC_ABI_VERSION 1\nenum s { NVMAC_A = 0, NVMAC_B, %s NVMAC_SELECTOR_COUNT };\n"
                    "struct nvmac_info { uint32_t version; %s uint64_t size; };\n")
            k.write_text(base % ("NVMAC_C,", ""))
            m.write_text(base % ("", ""))
            ok, _, problems = nvlib.uapi_compare(k, m)
            self.assertTrue(ok, problems)          # Mesa lagging by a selector is fine
            m.write_text(base % ("", "uint32_t pad;"))
            ok, _, problems = nvlib.uapi_compare(k, m)
            self.assertFalse(ok)
            self.assertTrue(any("nvmac_info" in p for p in problems), problems)
            m.write_text(base.replace("NVMAC_ABI_VERSION 1", "NVMAC_ABI_VERSION 2") % ("", ""))
            ok, _, problems = nvlib.uapi_compare(k, m)
            self.assertFalse(ok)
            self.assertTrue(any("NVMAC_ABI_VERSION" in p for p in problems))

    def test_select(self):
        class A:
            tier, only, skip = 2, "metal", "metal.crossgpu"
        names = [c.name for c in nvcheck.select(A)]
        self.assertIn("metal.textest", names)
        self.assertNotIn("metal.crossgpu", names)
        self.assertNotIn("gpu.nvtest", names)


if __name__ == "__main__":
    unittest.main()
