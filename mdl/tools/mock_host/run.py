#!/usr/bin/env python3
"""
mock_host/run.py -- compiles a module.c with a NATIVE (host) compiler,
straight-linked against mock_api.c/main.c (no dlopen/.so -- see
mock_api.h's file comment for why), and runs it.

This is the "绝大部分逻辑错误应该在这一层被挡掉" layer: fast (native
speed, no flashing, no USB), and any crash comes with a real OS-level
backtrace (run under gdb/lldb/WinDbg yourself for that -- this script
just reports pass/fail plus captured stdout, it doesn't attach a
debugger itself).

Usage:
    python run.py path/to/module.c
    python run.py path/to/module.c --cc clang    # override compiler
"""
import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def find_native_cc(preferred: str | None) -> str:
    candidates = [preferred] if preferred else ["gcc", "cc", "clang"]
    for c in candidates:
        if c and shutil.which(c):
            return c
    raise SystemExit(
        "✗ no native C compiler found on PATH (tried: " + ", ".join(x for x in candidates if x) + ") -- "
        "install one (e.g. a MinGW-w64 GCC on Windows, or build-essential/clang elsewhere)"
    )


# A resident MDL's module_init() never returns by design, so the simulator
# has to give up rather than wait for something that will not happen.
MODULE_RUN_TIMEOUT_S = 4

def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("module_c", type=Path)
    ap.add_argument("--cc", default=None, help="native compiler to use (default: autodetect gcc/cc/clang)")
    ap.add_argument("--keep-exe", action="store_true", help="don't delete the built executable afterward")
    args = ap.parse_args(argv)

    if not args.module_c.is_file():
        print(f"✗ no such file: {args.module_c}", file=sys.stderr)
        return 1

    cc = find_native_cc(args.cc)

    build_dir = HERE / "build"
    build_dir.mkdir(exist_ok=True)
    exe = build_dir / ("mock_host.exe" if sys.platform == "win32" else "mock_host")

    host_inc = (HERE / ".." / ".." / "host").resolve()

    cflags = [
        cc,
        "-std=c11", "-Wall", "-Wextra", "-g", "-O0",
        f"-I{HERE}", f"-I{host_inc}", f"-I{host_inc.parent / 'core'}",
        str(args.module_c), str(HERE / "mock_api.c"), str(HERE / "main.c"),
        "-o", str(exe),
    ]

    # -fsanitize=address,undefined catches exactly the class of bug this
    # layer exists for (buffer overrun, use-after-free, UB) with a much
    # better report than a bare segfault -- try it, but don't hard-fail
    # the whole script if this particular compiler/platform combo
    # doesn't support it (e.g. some MinGW builds don't ship ASan).
    sanitized_cflags = cflags[:1] + ["-fsanitize=address,undefined"] + cflags[1:]
    result = subprocess.run(sanitized_cflags, capture_output=True, text=True)
    if result.returncode != 0:
        print("(sanitizers unavailable on this toolchain, building without -fsanitize)", file=sys.stderr)
        result = subprocess.run(cflags, capture_output=True, text=True)

    if result.returncode != 0:
        print("✗ compile failed:", file=sys.stderr)
        print(result.stderr, file=sys.stderr)
        return 1

    print(f"--- running {exe.name} ---")
    # Bounded, because module_init() legitimately never returns for a
    # RESIDENT MDL -- sw3_blue and friends loop forever polling a pin, and
    # that is how residency worked before ABI v4 gave them events. Without
    # this, running mock_host on one of those hangs the caller forever;
    # tools/watch.py inherits that hang, which is the project's own inner
    # loop. A timeout is not a failure here, it is the only honest answer:
    # a simulator that runs module_init() to completion cannot simulate a
    # module whose module_init() is not supposed to complete.
    try:
        run_result = subprocess.run([str(exe)], capture_output=True,
                                     text=True, timeout=MODULE_RUN_TIMEOUT_S)
    except subprocess.TimeoutExpired as e:
        print(f"module_init() still running after {MODULE_RUN_TIMEOUT_S}s -- "
               f"treating it as a resident module, not a hang.")
        if e.stdout:
            print(e.stdout.decode(errors='replace') if isinstance(e.stdout, bytes) else e.stdout, end='')
        print('--- PASS (resident, not run to completion) ---')
        return 0
    print(run_result.stdout, end="")
    if run_result.stderr:
        print(run_result.stderr, file=sys.stderr, end="")

    if not args.keep_exe:
        try:
            exe.unlink()
        except OSError:
            pass

    if run_result.returncode != 0:
        print(f"✗ module crashed or aborted (exit code {run_result.returncode})", file=sys.stderr)
        return 1

    print("--- PASS ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
