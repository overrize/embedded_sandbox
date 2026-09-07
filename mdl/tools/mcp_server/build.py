"""
Source string -> .mdl, with every check that can be made before a device
is involved.

Ordered cheapest-first on purpose, because this is an agent's inner loop
and the point is to fail on a laptop rather than on hardware:

  1. native compile + run under mock_host   catches logic errors
  2. ARM cross compile                      catches target-only errors
  3. pack                                   ABI, sizes, event budget, and
                                            pin-level resource conflicts

Step 3 is where B2 lives: two claims landing on the same physical pin are
decidable from the source alone, so they are refused here rather than on
the device. By the time an image reaches a board someone is waiting on it.
"""
import contextlib
import importlib.util
import io
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

TOOLS = Path(__file__).resolve().parent.parent
MDL_ROOT = TOOLS.parent

# packer.py is imported rather than shelled out to, so its PackError text --
# already written to be read by a person -- becomes the diagnostic verbatim
# instead of being scraped out of stderr.
_spec = importlib.util.spec_from_file_location("mdl_packer", TOOLS / "packer.py")
packer = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(packer)

# How long module_init() gets under mock_host before it is assumed to be a
# resident module rather than a stuck one. Generous on purpose: 'loops
# forever by design' and 'wedged' look identical from outside, so this only
# has to outlast any honest init.
MOCK_TIMEOUT_S = 5

MODULE_CFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16",
    "-fPIC", "-msingle-pic-base", "-mpic-register=r9",
    "-mno-pic-data-is-text-relative",
    "-ffunction-sections", "-fdata-sections", "-nostdlib", "-nostartfiles",
    "-O1", "-g", "-Wall", "-Wextra",
]
MODULE_LDFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16",
    "-nostdlib", "-shared", "-Wl,-Bsymbolic", "-Wl,--no-undefined",
    "-Wl,--gc-sections", "-static-libgcc",
]


class BuildResult:
    def __init__(self):
        self.ok = False
        self.diagnostics = []
        self.manifest = {}
        self.mdl_path = None

    def fail(self, text):
        self.diagnostics.append(text)
        return self


def _run(cmd, timeout=None):
    return subprocess.run(cmd, capture_output=True, text=True,
                          encoding="utf-8", errors="replace", timeout=timeout)


def compile_check(source: str, gcc: str = "arm-none-eabi-gcc",
                   name: str = "mdl", run_mock: bool = True,
                   keep_dir: Path = None) -> BuildResult:
    r = BuildResult()
    if shutil.which(gcc) is None:
        return r.fail(f"{gcc} is not on PATH. This machine keeps the ARM "
                      f"toolchain outside the default PATH; prepend its bin "
                      f"directory (see mdl/tests/hil/build.ps1).")

    work = Path(keep_dir) if keep_dir else Path(tempfile.mkdtemp(prefix="mdl_"))
    work.mkdir(parents=True, exist_ok=True)
    src = work / "module.c"
    src.write_text(source, encoding="utf-8")

    inc = ["-I", str(MDL_ROOT / "host"), "-I", str(MDL_ROOT / "core")]

    # 1. mock_host: native, fast, and the only step that can catch a logic
    #    error rather than a compile error. Skipped without a native gcc
    #    rather than failing the whole check -- it is a bonus, not a gate.
    #
    #    The timeout is not defensive padding. mock_host runs module_init()
    #    natively, and a resident MDL's module_init() never returns by
    #    design -- sw3_blue and friends loop forever polling a pin. Without
    #    a bound, this step hangs on exactly the modules the project is
    #    built around. A timeout here means 'cannot be simulated', which is
    #    a fact about the module's shape, not a failure of the module.
    if run_mock:
        mock = TOOLS / "mock_host" / "run.py"
        if mock.is_file():
            try:
                m = _run([sys.executable, str(mock), str(src)],
                          timeout=MOCK_TIMEOUT_S)
                if m.returncode != 0:
                    r.diagnostics.append("mock_host failed:\n"
                                          + (m.stdout + m.stderr).strip())
                    return r
            except subprocess.TimeoutExpired:
                r.diagnostics.append(
                    f"mock_host: module_init() did not return within "
                    f"{MOCK_TIMEOUT_S}s -- treated as a resident module and "
                    f"skipped, not as a failure. The ARM build and pack "
                    f"below still ran.")

    # 2. cross compile
    obj = work / "module.o"
    c = _run([gcc, *MODULE_CFLAGS, *inc, "-c", str(src), "-o", str(obj)])
    if c.returncode != 0:
        return r.fail(c.stderr.strip())
    if c.stderr.strip():
        r.diagnostics.append(c.stderr.strip())      # warnings, not fatal

    so = work / "module.so"
    l = _run([gcc, *MODULE_LDFLAGS, str(obj), "-o", str(so)])
    if l.returncode != 0:
        return r.fail(l.stderr.strip())

    # 3. pack -- ABI, sizes, event budget, pin-level conflicts
    mdl = work / "module.mdl"
    try:
        abi = packer.parse_host_abi_version(MDL_ROOT / "host" / "host_api.h")
        # packer.pack() prints its summary line to stdout, and for the MCP
        # server stdout IS the JSON-RPC transport -- one stray line and the
        # client fails to parse a reply it never sees the cause of. Capture
        # it and keep it as a diagnostic instead.
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            packer.pack(so, mdl, abi_ver=abi, arch=1, name_hint=name,
                         text_region_size=16 * 1024, data_region_size=8 * 1024)
        if buf.getvalue().strip():
            r.diagnostics.append(buf.getvalue().strip())
    except packer.PackError as e:
        return r.fail(str(e))
    except Exception as e:                           # noqa: BLE001
        return r.fail(f"packer failed unexpectedly: {e!r}")

    r.ok = True
    r.mdl_path = mdl
    r.manifest = _manifest(mdl)
    return r


def _manifest(mdl_path: Path) -> dict:
    import struct
    d = mdl_path.read_bytes()
    v = struct.unpack(packer.MDL_HEADER_FMT, d[:packer.MDL_HEADER_SIZE])
    (magic, abi, arch, crc, text, data, bss, got_off, got_n, init_off,
     reloc_off, reloc_n, cmd_off, res_off, res_n, cmd_name, name,
     evt_off, evt_depth, evt_rate) = v
    board = packer.BoardPins()
    resources = []
    for i in range(res_n):
        kind, ident = d[packer.MDL_HEADER_SIZE + res_off + i * 4:
                        packer.MDL_HEADER_SIZE + res_off + i * 4 + 2]
        rname, pins = board.expand(kind, ident)
        resources.append({
            "kind": next((k for k, x in packer.RES_KIND.items() if x == kind), "?"),
            "id": ident,
            "name": rname,
            "pins": [packer.pin_name(p) for p in pins],
        })
    return {
        "name": name.split(b"\0")[0].decode(),
        "abi_ver": abi,
        "sizes": {"image": len(d), "text": text, "data": data, "bss": bss,
                  "got_entries": got_n, "relocs": reloc_n},
        "command": cmd_name.split(b"\0")[0].decode() or None,
        "events": ({"queue_depth": evt_depth, "declared_rate_hz": evt_rate}
                    if evt_off else None),
        "resources": resources,
    }
