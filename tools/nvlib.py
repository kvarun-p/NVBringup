"""Shared helpers for tools/nvcheck and tools/nvctl (Python 3.9, standard library only).

Paths that differ per machine come from the environment or local.env at the repository root (the same file the
Makefile and the zsh tools read):
  NVB_MESA_SRC      the Mesa checkout whose nvkmd/macos copy of nv_uapi.h NVK is built with (default /Volumes/NVBuild/mesa-main)
  NVB_MESA_BUILD    its build directory (default: from NVB_NVMETAL_BUNDLE, else /Volumes/NVBuild/build-nvk-main)
  NVB_SCRATCH       where the regression tests live (default ~/Projects/GPU/scratchpad)
  NVB_EFI           the OpenCore EFI folder (default: the one mounted volume with EFI/OC/Kexts/NVBringup.kext)
"""
import os
import plistlib
import re
import shlex
import subprocess
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


def load_env(repo=REPO):
    """os.environ over local.env (KEY=VALUE lines, shell quoting); returns a dict."""
    env = {}
    f = repo / "local.env"
    if f.is_file():
        for line in f.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            k = k.strip()
            if k.startswith("export "):
                k = k[7:].strip()
            try:
                parts = shlex.split(v, comments=True)
            except ValueError:
                continue
            env[k] = os.path.expanduser(parts[0]) if parts else ""
    env.update({k: v for k, v in os.environ.items() if k.startswith("NVB_")})
    return env


class Config:
    def __init__(self, repo=REPO, env=None):
        self.repo = Path(repo)
        e = load_env(self.repo) if env is None else env
        self.env = e
        self.mesa_src = Path(e.get("NVB_MESA_SRC", "/Volumes/NVBuild/mesa-main"))
        b = e.get("NVB_MESA_BUILD")
        if not b and e.get("NVB_NVMETAL_BUNDLE"):
            b = str(Path(e["NVB_NVMETAL_BUNDLE"]).parents[3])     # <build>/src/nouveau/air/NVMetal.bundle
        self.mesa_build = Path(b or "/Volumes/NVBuild/build-nvk-main")
        self.scratch = Path(e.get("NVB_SCRATCH", os.path.expanduser("~/Projects/GPU/scratchpad")))
        self.efi = e.get("NVB_EFI") or None

    @property
    def nvmetal_dylib(self):
        return self.mesa_build / "src/nouveau/air/libnvmetal.dylib"

    @property
    def nvmetal_bundle(self):
        b = self.env.get("NVB_NVMETAL_BUNDLE")
        return Path(b) if b else self.mesa_build / "src/nouveau/air/NVMetal.bundle"

    @property
    def nvk_lib(self):
        return self.mesa_build / "src/nouveau/vulkan/libvulkan_nouveau.dylib"

    @property
    def mesa_uapi(self):
        return self.mesa_src / "src/nouveau/vulkan/nvkmd/macos/nv_uapi.h"


def run(cmd, cwd=None, env=None, timeout=600, input=None):
    """Runs cmd (a list); returns (returncode, combined output, seconds). A timeout is returncode -9."""
    t = time.monotonic()
    try:
        p = subprocess.run(cmd, cwd=cwd, env=env, input=input, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           timeout=timeout)
        out, rc = p.stdout.decode("utf-8", "replace"), p.returncode
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode("utf-8", "replace") + "\n[timed out after %d s]" % timeout
        rc = -9
    except OSError as e:
        out, rc = str(e), -2
    return rc, out, time.monotonic() - t


def ioreg_class(cls):
    """Properties of the first registry entry of class cls (dict), or None."""
    rc, out, _ = run(["/usr/sbin/ioreg", "-a", "-r", "-c", cls, "-d", "1"], timeout=30)
    if rc != 0 or not out.strip():
        return None
    try:
        lst = plistlib.loads(out.encode())
    except Exception:
        return None
    return lst[0] if lst else None


def boot_args():
    rc, out, _ = run(["/usr/sbin/sysctl", "-n", "kern.bootargs"], timeout=10)
    return out.split() if rc == 0 else []


def plist(path):
    try:
        with open(path, "rb") as f:
            return plistlib.load(f)
    except Exception:
        return None


def exe_uuid(path):
    rc, out, _ = run(["/usr/bin/dwarfdump", "--uuid", str(path)], timeout=30)
    m = re.search(r"UUID: ([0-9A-F-]{36})", out)
    return m.group(1) if rc == 0 and m else None


def bundle_uuid(bundle):
    info = plist(Path(bundle) / "Contents/Info.plist")
    if not info or "CFBundleExecutable" not in info:
        return None
    return exe_uuid(Path(bundle) / "Contents/MacOS" / info["CFBundleExecutable"])


def find_efi(volumes=Path("/Volumes")):
    """The one mounted EFI folder that has OC/Kexts/NVBringup.kext, else None."""
    try:
        c = [v / "EFI" for v in volumes.iterdir() if (v / "EFI/OC/Kexts/NVBringup.kext").is_dir()]
    except OSError:
        return None
    return c[0] if len(c) == 1 else None


# ---- UAPI comparison --------------------------------------------------------------------------------------------
# The kext's src/nv_uapi.h and Mesa's copy (nvkmd/macos) are maintained by hand and Mesa's is allowed to lag (it is a
# subset). What must hold: every constant and struct both define has the same value / size / field offsets.

def _strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    return re.sub(r"//[^\n]*", "", s)


def uapi_names(text):
    """(constants, {struct: [fields]}) defined in a nv_uapi.h."""
    s = _strip_comments(text)
    consts = set(re.findall(r"^\s*#\s*define\s+(NVMAC_\w+)(?!\()\s+\S", s, flags=re.M))
    for body in re.findall(r"\benum\b[^{;]*\{(.*?)\}", s, flags=re.S):
        consts.update(re.findall(r"\b(NVMAC_\w+)\b\s*(?:=|,|$)", body))
    structs = {}
    for name, body in re.findall(r"\bstruct\s+(nvmac_\w+)\s*\{(.*?)\};", s, flags=re.S):
        fields = []
        for decl in body.split(";"):
            decl = decl.strip()
            if not decl or "(" in decl:
                continue
            m = re.match(r"^(?:const\s+)?(?:struct\s+)?\w+\s+(.*)$", decl, flags=re.S)
            if not m:
                continue
            for part in m.group(1).split(","):
                f = re.sub(r"\[.*?\]", "", part).strip().lstrip("*").strip()
                if re.fullmatch(r"\w+", f):
                    fields.append(f)
        structs[name] = fields
    return consts, structs


def uapi_values(header, consts, structs, cc="clang"):
    """{name: value} for the constants and 'sizeof(struct)' / 'offsetof(struct.field)', compiled against header."""
    lines = ['#include "%s"' % header, "#include <stdio.h>", "#include <stddef.h>", "int main(void) {"]
    for c in sorted(consts):
        lines.append('    printf("%s=%%lld\\n", (long long)(%s));' % (c, c))
    for st, fields in sorted(structs.items()):
        lines.append('    printf("sizeof(%s)=%%zu\\n", sizeof(struct %s));' % (st, st))
        for f in fields:
            lines.append('    printf("offsetof(%s.%s)=%%zu\\n", offsetof(struct %s, %s));' % (st, f, st, f))
    lines += ["    return 0;", "}"]
    with tempfile.TemporaryDirectory(prefix="nvcheck-uapi.") as d:
        src, exe = Path(d) / "u.c", Path(d) / "u"
        src.write_text("\n".join(lines) + "\n")
        rc, out, _ = run([cc, "-std=c11", "-w", "-o", str(exe), str(src)], timeout=120)
        if rc != 0:
            raise RuntimeError("compiling against %s failed:\n%s" % (header, out[-2000:]))
        rc, out, _ = run([str(exe)], timeout=30)
    vals = {}
    for line in out.splitlines():
        k, _, v = line.partition("=")
        vals[k] = int(v)
    return vals


# Counts that only bound a list Mesa may know less of: they differ while Mesa lags, and that is fine.
UAPI_SENTINELS = {"NVMAC_SELECTOR_COUNT"}


def uapi_compare(kext_hdr, mesa_hdr):
    """(ok, summary, problems): every NVMAC_* constant and nvmac_* struct both headers define must agree."""
    kc, ks = uapi_names(Path(kext_hdr).read_text())
    mc, ms = uapi_names(Path(mesa_hdr).read_text())
    consts = (kc & mc) - UAPI_SENTINELS
    structs = {s: [f for f in ms[s] if f in ks[s]] for s in ms if s in ks}
    kv = uapi_values(str(Path(kext_hdr).resolve()), consts, structs)
    mv = uapi_values(str(Path(mesa_hdr).resolve()), consts, structs)
    problems = ["%s: kext %s, mesa %s" % (k, kv.get(k), mv.get(k)) for k in sorted(set(kv) | set(mv))
                if kv.get(k) != mv.get(k)]
    for s in sorted(set(ms) - set(ks)):
        problems.append("struct %s: only in mesa" % s)
    for s in structs:
        missing = [f for f in ms[s] if f not in ks[s]]
        if missing:
            problems.append("struct %s: mesa has fields the kext doesn't: %s" % (s, ", ".join(missing)))
    only_mesa = sorted(mc - kc)
    if only_mesa:
        problems.append("constants only in mesa: " + ", ".join(only_mesa))
    summary = "%d constants, %d structs agree; kext-only (mesa lags, fine): %d constants" % (
        len(consts), len(structs), len(kc - mc - UAPI_SENTINELS))
    return not problems, summary, problems
