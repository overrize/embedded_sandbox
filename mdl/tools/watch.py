#!/usr/bin/env python3
"""
watch.py -- watches a module's module.c for changes and, on every save:

    1. compile module.c for ARM (the real device flags, mdl/host/host_api.h's
       compile-flags block)
    2. mock_host/run.py it natively (fast, catches logic bugs before
       spending a USB round trip on them)
    3. tools/packer.py it into a .mdl
    4. push the .mdl over the USB CDC serial port (mdl/transport/protocol.h's
       framing) as a CMD_LOAD request, and print the device's response

Goal (per the project's own spec): "存盘到设备上看到效果 < 2 秒". Every
step above is quick by construction (native mock is native-speed; the
ARM compile is one small .c file, not a full firmware rebuild; the
serial round trip is a few KB at worst) -- the sum should comfortably
clear that bar on any machine with the toolchain already warm.

Usage:
    python watch.py path/to/module_dir --port COM5
    python watch.py path/to/module_dir --port /dev/ttyACM0 --baud 115200

Requires pyserial ("pip install pyserial") -- the one external
dependency in this whole toolchain (packer.py deliberately has none),
because there is no reasonable stdlib way to talk to a serial port.
"""
import argparse
import struct
import subprocess
import sys
import time
import zlib
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:
    serial = None

HERE = Path(__file__).resolve().parent
MDL_ROOT = HERE.parent

PROTO_MAGIC = 0x434C444D  # 'MDLC'
CMD_LOAD = 1
CMD_UNLOAD = 2
CMD_STATUS = 3
RESP_OK = 0x81
RESP_ERROR = 0x82
RESP_STATUS = 0x83

# Text commands sent down the same CDC pipe right after a successful load,
# to turn "the device said OK" into "here is the module's data, read back
# out of the arena". Same port, same session, no reconnect: on a board
# whose only power comes from that one USB cable, unplugging to run
# console.py separately would reset the MCU and wipe the very module that
# is being verified (modules live in SRAM -- flash persistence is F2, not
# built yet).
#
# 0x20024000 is the arena's data base, fixed by the linker script, so the
# first words dumped are the module's GOT (every slot should read
# 0x2002xxxx -- a slot still holding a link-time address means relocation
# didn't happen) followed by its own variables.
VERIFY_CMDS = ["status", "mem 0x20024000 12"]

MODULE_CFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16",
    "-fPIC", "-msingle-pic-base", "-mpic-register=r9", "-mno-pic-data-is-text-relative",
    "-ffunction-sections", "-fdata-sections", "-nostdlib", "-nostartfiles",
    "-O1", "-g",
]
MODULE_LDFLAGS = [
    "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=hard", "-mfpu=fpv4-sp-d16",
    "-nostdlib", "-shared", "-Wl,-Bsymbolic", "-Wl,--no-undefined", "-Wl,--gc-sections",
    "-static-libgcc",
]


def find_step(msg: str) -> None:
    print(f"\n\033[1m>>> {msg}\033[0m")


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def build_frame(cmd: int, payload: bytes) -> bytes:
    header = struct.pack("<IBI", PROTO_MAGIC, cmd, len(payload))
    crc = zlib.crc32(header[4:] + payload) & 0xFFFFFFFF
    return header + payload + struct.pack("<I", crc)


def read_frame(ser, timeout_s: float = 5.0):
    """Reads one response frame; returns (resp_code, payload) or None on timeout."""
    deadline = time.time() + timeout_s
    buf = bytearray()
    # resync on magic
    while time.time() < deadline:
        b = ser.read(1)
        if not b:
            continue
        buf += b
        if len(buf) > 4:
            buf = buf[-4:]
        if len(buf) == 4 and struct.unpack("<I", bytes(buf))[0] == PROTO_MAGIC:
            break
    else:
        return None

    header = ser.read(5)
    if len(header) < 5:
        return None
    resp, length = struct.unpack("<BI", header)
    payload = ser.read(length)
    crc_bytes = ser.read(4)
    if len(payload) < length or len(crc_bytes) < 4:
        return None
    expected = struct.unpack("<I", crc_bytes)[0]
    got = zlib.crc32(header + payload) & 0xFFFFFFFF
    if got != expected:
        print("✗ response frame failed checksum -- dropped, treat as no response")
        return None
    return resp, payload


def drain(ser, quiet_s: float = 0.35, cap_s: float = 3.0) -> str:
    """Reads until the device has been quiet for `quiet_s` (or `cap_s` total).

    The console has no end-of-output marker -- it is a human-facing text
    stream, not a framed protocol -- so "it stopped talking" is the only
    available terminator. The cap keeps a device that chatters forever
    (an unloaded module's own prints, say) from hanging the tool.
    """
    out = bytearray()
    deadline = time.time() + cap_s
    quiet_until = time.time() + quiet_s
    while time.time() < deadline and time.time() < quiet_until:
        n = ser.in_waiting
        if n:
            out += ser.read(n)
            quiet_until = time.time() + quiet_s
        else:
            time.sleep(0.02)
    return out.decode("utf-8", errors="replace")


def console_cmd(ser, cmd: str) -> str:
    """Runs one text command on the device console and returns what it said.

    Safe to interleave with the binary framing on the same port: the
    firmware matches the MDLC magic byte by byte and hands anything that
    fails the match to the console line editor (mdl/transport/protocol.c),
    so plain text can never be mistaken for a frame header.
    """
    ser.reset_input_buffer()
    ser.write(cmd.encode("ascii") + b"\r")
    ser.flush()
    return drain(ser)


def compile_for_arm(module_c: Path, gcc: str) -> Path | None:
    build_dir = module_c.parent / "build"
    build_dir.mkdir(exist_ok=True)
    obj = build_dir / "module.o"
    so = build_dir / "module.so"
    host_inc = MDL_ROOT / "host"

    r = run([gcc, *MODULE_CFLAGS, f"-I{host_inc}", "-c", str(module_c), "-o", str(obj)])
    if r.returncode != 0:
        print("✗ ARM compile failed:")
        print(r.stderr)
        return None

    r = run([gcc, *MODULE_LDFLAGS, str(obj), "-o", str(so)])
    if r.returncode != 0:
        print("✗ ARM link failed:")
        print(r.stderr)
        return None
    return so


def mock_test(module_c: Path, python_exe: str) -> bool:
    r = run([python_exe, str(HERE / "mock_host" / "run.py"), str(module_c)])
    print(r.stdout, end="")
    if r.stderr:
        print(r.stderr, end="", file=sys.stderr)
    return r.returncode == 0


def pack_module(so_path: Path, python_exe: str) -> Path | None:
    mdl_path = so_path.with_suffix(".mdl")
    r = run([python_exe, str(MDL_ROOT / "tools" / "packer.py"), str(so_path), str(mdl_path)])
    print(r.stdout, end="")
    if r.returncode != 0:
        print(r.stderr, end="", file=sys.stderr)
        return None
    return mdl_path


def push_module(mdl_path: Path, port: str, baud: int, verify: bool = False) -> None:
    if serial is None:
        print("✗ pyserial not installed -- run: pip install pyserial", file=sys.stderr)
        return

    data = mdl_path.read_bytes()
    frame = build_frame(CMD_LOAD, data)

    try:
        with serial.Serial(port, baud, timeout=1) as ser:
            ser.write(frame)
            ser.flush()
            result = read_frame(ser)

            # Still inside the same `with`, on purpose -- see VERIFY_CMDS.
            if verify and result is not None and result[0] == RESP_OK:
                print("✓ device: loaded and running")
                for cmd in VERIFY_CMDS:
                    find_step(f"verify: {cmd}")
                    print(console_cmd(ser, cmd), end="")
                print()
                return
    except serial.SerialException as e:
        print(f"✗ could not open {port}: {e}")
        return

    if result is None:
        print("✗ no response from device within timeout (is it plugged in and running M4 firmware?)")
        return

    resp, payload = result
    if resp == RESP_OK:
        print("✓ device: loaded and running")
    elif resp == RESP_ERROR:
        print(f"✗ device rejected the module: {payload.decode('utf-8', errors='replace')}")
    else:
        print(f"(unexpected response code 0x{resp:02x})")


def do_one_cycle(module_dir: Path, port: str, baud: int, gcc: str, python_exe: str,
                 verify: bool = False) -> None:
    module_c = module_dir / "module.c"
    t0 = time.time()

    find_step("mock_host (native, fast)")
    if not mock_test(module_c, python_exe):
        print(f"✗ stopped after mock_host failure ({time.time() - t0:.1f}s)")
        return

    find_step("compiling for ARM")
    so_path = compile_for_arm(module_c, gcc)
    if so_path is None:
        print(f"✗ stopped after ARM compile failure ({time.time() - t0:.1f}s)")
        return

    find_step("packing")
    mdl_path = pack_module(so_path, python_exe)
    if mdl_path is None:
        print(f"✗ stopped after packer.py failure ({time.time() - t0:.1f}s)")
        return

    find_step(f"pushing to {port}")
    push_module(mdl_path, port, baud, verify)

    print(f"\n(total: {time.time() - t0:.2f}s)")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("module_dir", type=Path, help="directory containing module.c")
    ap.add_argument("--port", required=True, help="serial port (e.g. COM5, /dev/ttyACM0)")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--gcc", default="arm-none-eabi-gcc")
    ap.add_argument("--python", default=sys.executable)
    ap.add_argument("--once", action="store_true", help="run one cycle and exit, instead of watching")
    ap.add_argument("--verify", action="store_true",
                    help="after a successful load, run VERIFY_CMDS on the device console "
                         "in the same port session and print the replies (use this when "
                         "reconnecting would power-cycle the board and lose the module)")
    args = ap.parse_args(argv)

    module_c = args.module_dir / "module.c"
    if not module_c.is_file():
        print(f"✗ no such file: {module_c}", file=sys.stderr)
        return 1

    if args.once:
        do_one_cycle(args.module_dir, args.port, args.baud, args.gcc, args.python,
                     args.verify)
        return 0

    print(f"watching {module_c} -- Ctrl+C to stop")
    last_mtime = module_c.stat().st_mtime  # baseline -- don't build on startup, only on a real save
    try:
        while True:
            mtime = module_c.stat().st_mtime
            if mtime != last_mtime:
                last_mtime = mtime
                do_one_cycle(args.module_dir, args.port, args.baud, args.gcc, args.python,
                             args.verify)
            time.sleep(0.3)
    except KeyboardInterrupt:
        print("\nstopped.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
