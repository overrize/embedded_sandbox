"""
The one serial connection, and everything that has to be true about it.

A serial port can only be open once, which is not an implementation detail
here -- it is the constraint the whole module is shaped around. The MCP
server holds the port for its lifetime and serialises every call through a
lock, because two tools talking at once would interleave bytes inside a
single MDLC frame and the device would drop both.

Console text and binary frames share the port on the wire (the firmware
demultiplexes by matching the magic byte by byte), so this side keeps the
text as well rather than discarding it: an MDL's own log output arrives
that way, and read_logs has nothing else to read.
"""
import threading
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover - reported through the tool result
    serial = None

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import mdl_proto as proto  # noqa: E402


class DeviceError(Exception):
    """Raised with a message meant for the model, not a stack trace."""


class Device:
    def __init__(self, port: str, baud: int = 115200, log_lines: int = 500):
        self.port = port
        self.baud = baud
        self._lock = threading.Lock()
        self._ser = None
        self._buf = bytearray()
        self._log = []            # device console text, newest last
        self._log_max = log_lines

    # ---- connection -------------------------------------------------

    def _open(self):
        if self._ser is not None:
            return
        if serial is None:
            raise DeviceError("pyserial is not installed: pip install pyserial")
        try:
            self._ser = serial.Serial(self.port, self.baud, timeout=0.05)
        except Exception as e:
            raise DeviceError(
                f"could not open {self.port}: {e}. "
                f"If tools/watch.py or tools/console.py is running, stop it -- "
                f"a serial port can only be open once."
            )

    def close(self):
        with self._lock:
            if self._ser is not None:
                self._ser.close()
                self._ser = None

    # ---- reading ----------------------------------------------------

    def _pump(self, seconds: float, stop_on_frame: bool = False):
        """Read for up to `seconds`, keeping text and collecting frames.

        stop_on_frame returns the moment a complete frame arrives instead of
        sitting out the rest of the timeout. Without it every request paid
        its worst case -- a 6s timeout meant 6s even when the device
        answered in 20ms, and a deploy (status, unload, load) took half a
        minute of doing nothing. The timeout is meant to bound failure, not
        to price success.
        """
        frames = []
        end = time.time() + seconds
        while time.time() < end:
            try:
                n = self._ser.in_waiting
                if n:
                    self._buf += self._ser.read(n)
                else:
                    time.sleep(0.01)
            except Exception as e:
                raise DeviceError(f"{self.port} went away mid-transfer: {e}")

            while True:
                got = proto.parse_frame(self._buf)
                if got is None:
                    break
                resp, payload, upto, text = got
                self._keep_text(text)
                del self._buf[:upto]
                frames.append((resp, payload))
            if frames and stop_on_frame:
                break
        # Text with no frame after it still belongs in the log, but a
        # partial magic at the tail must not be printed as text -- it may
        # yet turn out to be a frame header.
        keep = 0
        for k in (3, 2, 1):
            if self._buf.endswith(proto.MAGIC[:k]):
                keep = k
                break
        if len(self._buf) > keep and proto.MAGIC not in self._buf:
            self._keep_text(bytes(self._buf[:len(self._buf) - keep]))
            del self._buf[:len(self._buf) - keep]
        return frames

    def _keep_text(self, raw: bytes):
        if not raw:
            return
        text = raw.decode("utf-8", "replace")
        for line in text.replace("\r", "").split("\n"):
            if line.strip():
                self._log.append(line)
        del self._log[:-self._log_max]

    # ---- operations -------------------------------------------------

    def request(self, cmd: int, payload: bytes = b"", timeout: float = 6.0):
        """Send one command frame, return (resp, payload).

        No automatic retry, per the error contract in maintain.md section 4:
        a timeout is reported and the decision to try again is the agent's,
        because a retry of a LOAD that actually succeeded is not harmless.
        """
        with self._lock:
            self._open()
            self._ser.reset_input_buffer()
            self._buf.clear()
            self._ser.write(proto.build_frame(cmd, payload))
            self._ser.flush()
            frames = self._pump(timeout, stop_on_frame=True)
        if not frames:
            raise DeviceError(
                f"no response from {self.port} within {timeout:.0f}s. "
                f"Check the board is powered and running M4 firmware "
                f"(`ver` on the console reports its build time)."
            )
        return frames[0]

    def console(self, line: str, settle: float = 1.5) -> str:
        """Run a text console command and return what came back.

        Safe to interleave with frames: the firmware hands anything that
        fails the magic match to its line editor, so plain text can never be
        mistaken for a frame header.
        """
        with self._lock:
            self._open()
            self._ser.reset_input_buffer()
            self._buf.clear()
            before = len(self._log)
            self._ser.write(line.encode("ascii") + b"\r")
            self._ser.flush()
            self._pump(settle)
            return "\n".join(self._log[before:])

    def logs(self, max_lines: int = 50):
        with self._lock:
            if self._ser is not None:
                self._pump(0.15)   # sweep up anything pending
            return list(self._log[-max_lines:])
