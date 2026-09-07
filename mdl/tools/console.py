#!/usr/bin/env python3
"""
console.py -- interactive terminal for a device running the MDL framework.

    python mdl/tools/console.py --port COM5

Exists because Windows ships no serial terminal, and the alternatives
(PuTTY, TeraTerm) are an extra install for something this small.

Two things make it more than a dumb pipe, and both follow from one fact:
a serial port can only be open once, and this program is holding it.

  1. LOCAL COMMANDS. A line beginning with `/` is handled here instead of
     being sent to the device -- `/load foo.mdl` builds the MDLC frame
     and pushes it down the same already-open port. Without this,
     importing an MDL means quitting the terminal, running watch.py, and
     reopening it: three steps to do the one thing this project exists
     for.

  2. FRAME DECODING. The device answers a push with a binary MDLC frame.
     Printed raw that is line noise, so the reader below picks frames out
     of the stream and renders them as text, passing everything else
     through untouched. Console text and binary frames share one port on
     the wire -- the firmware demultiplexes them by matching the magic
     byte by byte -- so this side has to demultiplex them too.

Everything else is a byte pipe: keystrokes go out one at a time and
whatever comes back is printed. Deliberately no local echo for device
input, because the firmware echoes every character itself
(mdl/transport/console.c) and echoing here too would double every
keypress. Local commands DO echo here, since the device never sees them.

Type `/help` for the local commands, `help` for the device's own.
"""
import argparse
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

try:
    import serial  # pyserial
except ImportError:
    print("needs pyserial:  pip install pyserial", file=sys.stderr)
    sys.exit(2)

QUIT = b"\x1d"  # Ctrl+]

# Must match mdl/transport/protocol.h exactly -- the same wire format
# tools/watch.py speaks.
PROTO_MAGIC = b"MDLC"
CMD_LOAD, CMD_UNLOAD, CMD_STATUS, CMD_LOAD_PERSIST = 1, 2, 3, 4
RESP_NAMES = {0x81: "OK", 0x82: "ERROR", 0x83: "STATUS"}

BACKSPACE = (b"\x08", b"\x7f")
NEWLINE = (b"\r", b"\n")


def build_frame(cmd, payload=b""):
    magic = int.from_bytes(PROTO_MAGIC, "little")
    header = struct.pack("<IBI", magic, cmd, len(payload))
    crc = zlib.crc32(header[4:] + payload) & 0xFFFFFFFF
    return header + payload + struct.pack("<I", crc)


def out(text):
    sys.stdout.write(text)
    sys.stdout.flush()


class Reader(threading.Thread):
    """Serial -> stdout, lifting MDLC response frames out of the text.

    Holds back a few trailing bytes whenever they could still turn out to
    be the start of a magic, for the same reason the firmware's parser
    does: printing them and only then discovering they were a frame
    header would leave "MDL" scattered through the transcript.
    """

    def __init__(self, ser, stop):
        super().__init__(daemon=True)
        self.ser = ser
        self.stop = stop
        self.buf = bytearray()

    def run(self):
        while not self.stop.is_set():
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                break
            if not data:
                time.sleep(0.01)
                continue
            self.buf += data
            self.drain()

    def drain(self):
        while True:
            i = self.buf.find(PROTO_MAGIC)
            if i < 0:
                keep = 0
                for n in (3, 2, 1):
                    if self.buf.endswith(PROTO_MAGIC[:n]):
                        keep = n
                        break
                if len(self.buf) > keep:
                    self.emit(self.buf[:len(self.buf) - keep])
                    del self.buf[:len(self.buf) - keep]
                return

            if i:
                self.emit(self.buf[:i])
                del self.buf[:i]

            if len(self.buf) < 9:
                return  # magic + cmd + len not all here yet
            resp, length = struct.unpack("<BI", self.buf[4:9])
            if len(self.buf) < 9 + length + 4:
                return  # payload still arriving
            payload = bytes(self.buf[9:9 + length])
            del self.buf[:9 + length + 4]
            self.show_frame(resp, payload)

    def emit(self, raw):
        out(raw.decode("utf-8", "replace"))

    def show_frame(self, resp, payload):
        name = RESP_NAMES.get(resp, "0x%02x" % resp)
        if resp == 0x83 and len(payload) >= 12:
            state, pc, off = struct.unpack("<III", payload[:12])
            out("\r\n<- STATUS  slot=%d fault_pc=0x%08X text_off=0x%08X\r\n"
                % (state, pc, off))
        elif payload:
            out("\r\n<- %s: %s\r\n" % (name, payload.decode("utf-8", "replace")))
        else:
            out("\r\n<- %s\r\n" % name)


class LocalCommands:
    """The `/`-prefixed commands this program answers itself."""

    def __init__(self, ser):
        self.ser = ser

    def run(self, line):
        """Returns False if the terminal should exit."""
        parts = line.strip().split(None, 1)
        cmd = parts[0].lower() if parts else ""
        arg = parts[1].strip() if len(parts) > 1 else ""
        for quote in ('"', "'"):
            if len(arg) >= 2 and arg[0] == quote and arg[-1] == quote:
                arg = arg[1:-1]

        if cmd in ("/quit", "/exit", "/q"):
            return False
        if cmd in ("/help", "/?"):
            out("local commands (handled here, never sent to the device):\r\n"
                "  /load <path>   push an MDL over this same open port\r\n"
                "  /save <path>   push it AND keep it across power loss\r\n"
                "  /forget        erase the saved MDL\r\n"
                "  /unload        unload whatever is loaded\r\n"
                "  /status        ask for a STATUS frame\r\n"
                "  /quit          disconnect (same as Ctrl+])\r\n"
                "anything not starting with / goes to the device -- "
                "try `help` there.\r\n")
        elif cmd == "/load":
            self.load(arg)
        elif cmd == "/save":
            self.load(arg, persist=True)
        elif cmd == "/forget":
            # No frame command for this: erasing the store is a console
            # command on the device, so just type it there. Inventing a
            # protocol command to do what one already exists for would be
            # two ways to do one thing.
            self.ser.write(b"forget\r")
            self.ser.flush()
        elif cmd == "/unload":
            self.send(build_frame(CMD_UNLOAD))
        elif cmd == "/status":
            self.send(build_frame(CMD_STATUS))
        else:
            out("unknown local command: %s   (try /help)\r\n" % cmd)
        return True

    def send(self, frame):
        self.ser.write(frame)
        self.ser.flush()

    def load(self, arg, persist=False):
        if not arg:
            out("usage: /load <path to .mdl>\r\n")
            return
        # Relative paths resolve against the shell's working directory,
        # which is what someone typing a path expects; absolute paths work
        # unchanged.
        path = Path(arg).expanduser()
        if not path.is_file():
            out("no such file: %s\r\n" % path)
            return
        data = path.read_bytes()
        out("pushing %s (%dB) ...\r\n" % (path.name, len(data)))
        self.send(build_frame(CMD_LOAD_PERSIST if persist else CMD_LOAD, data))
        # The device's answer arrives as a frame and is rendered by the
        # reader thread, so there is nothing to wait for here.


def main(argv=None):
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored by the device (CDC baud is virtual), "
                         "but pyserial insists on a number")
    args = ap.parse_args(argv)

    try:
        ser = serial.Serial(args.port, args.baud, timeout=0.1)
    except Exception as e:
        print("could not open %s: %s" % (args.port, e), file=sys.stderr)
        print("if tools/watch.py is running, stop it first -- a serial port "
              "can only be open once.", file=sys.stderr)
        return 1

    print("connected to %s.  /help for local commands, Ctrl+] to quit.\n"
          % args.port)

    stop = threading.Event()
    Reader(ser, stop).start()
    local = LocalCommands(ser)

    # A local command counts only at the START of a line, so a `/` typed
    # inside an argument still reaches the device. at_line_start tracks
    # that the same way the device's own line editor does.
    state = {"at_line_start": True, "in_local": False, "buf": ""}

    def key(ch):
        """Handle one keystroke. Returns False to exit."""
        if state["in_local"]:
            if ch in NEWLINE:
                out("\r\n")
                line = state["buf"]
                state["buf"] = ""
                state["in_local"] = False
                state["at_line_start"] = True
                return local.run(line)
            if ch in BACKSPACE:
                if len(state["buf"]) > 1:  # never delete the leading '/'
                    state["buf"] = state["buf"][:-1]
                    out("\b \b")
                return True
            try:
                text = ch.decode()
            except UnicodeDecodeError:
                return True
            state["buf"] += text
            out(text)
            return True

        if state["at_line_start"] and ch == b"/":
            state["in_local"] = True
            state["buf"] = "/"
            out("/")
            return True

        ser.write(ch)
        state["at_line_start"] = ch in NEWLINE
        return True

    try:
        if sys.platform == "win32":
            import msvcrt
            while True:
                if msvcrt.kbhit():
                    ch = msvcrt.getch()
                    if ch == QUIT or not key(ch):
                        break
                else:
                    time.sleep(0.01)
        else:
            import termios
            import tty
            fd = sys.stdin.fileno()
            old = termios.tcgetattr(fd)
            try:
                tty.setraw(fd)
                while True:
                    ch = sys.stdin.buffer.read(1)
                    if not ch or ch == QUIT or not key(ch):
                        break
            finally:
                termios.tcsetattr(fd, termios.TCSADRAIN, old)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        ser.close()
        print("\ndisconnected.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
