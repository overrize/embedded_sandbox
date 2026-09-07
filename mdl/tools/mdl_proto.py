"""
The MDLC wire format, in one place.

Must match mdl/transport/protocol.h. It lives here because three tools now
speak it -- watch.py, console.py and the MCP server -- and three
hand-maintained copies of a frame layout do not stay equal. The failure
mode is quiet: a tool that builds a frame the device silently drops,
reported as "no response from device", which is exactly the symptom that
already cost a debugging session once.

Deliberately dependency-free, like packer.py. Only device.py, which has to
open a serial port, pulls in pyserial.
"""
import struct
import zlib

MAGIC = b"MDLC"
MAGIC_U32 = int.from_bytes(MAGIC, "little")

CMD_LOAD = 1
CMD_UNLOAD = 2
CMD_STATUS = 3
# Load AND write to flash, so the MDL survives power loss. A separate
# command rather than a flag because the costs differ: an ordinary push is
# free and repeatable, this one erases a flash sector.
CMD_LOAD_PERSIST = 4

RESP_OK = 0x81
RESP_ERROR = 0x82
RESP_STATUS = 0x83

RESP_NAMES = {RESP_OK: "OK", RESP_ERROR: "ERROR", RESP_STATUS: "STATUS"}

# Header is magic + cmd + length; the CRC covers cmd + length + payload,
# i.e. everything after the magic. Getting that span wrong produces a frame
# the device drops without a word, because a bad checksum is indistinguish-
# able from line noise and answering it would be answering noise.
_HEADER = "<IBI"
HEADER_LEN = struct.calcsize(_HEADER)   # 9
CRC_LEN = 4


def build_frame(cmd: int, payload: bytes = b"") -> bytes:
    header = struct.pack(_HEADER, MAGIC_U32, cmd, len(payload))
    crc = zlib.crc32(header[4:] + payload) & 0xFFFFFFFF
    return header + payload + struct.pack("<I", crc)


def parse_frame(buf: bytes):
    """Find and decode one response frame in `buf`.

    Returns (resp, payload, consumed_upto, leading_text) or None if no
    complete frame is present yet. `leading_text` is whatever came before
    the frame -- console output shares this port, so a caller that throws it
    away loses the device's own log lines.
    """
    i = buf.find(MAGIC)
    if i < 0:
        return None
    if len(buf) < i + HEADER_LEN:
        return None
    resp, length = struct.unpack("<BI", buf[i + 4:i + HEADER_LEN])
    end = i + HEADER_LEN + length + CRC_LEN
    if len(buf) < end:
        return None
    payload = bytes(buf[i + HEADER_LEN:i + HEADER_LEN + length])
    return resp, payload, end, bytes(buf[:i])


def resp_name(resp: int) -> str:
    return RESP_NAMES.get(resp, f"0x{resp:02x}")


def decode_status(payload: bytes) -> dict:
    """RESP_STATUS payload -> the three fields mdl_proto_status_t carries."""
    if len(payload) < 12:
        return {}
    state, fault_pc, fault_text_offset = struct.unpack("<III", payload[:12])
    return {
        "slot_state": state,
        "fault_pc": fault_pc,
        # 0xFFFFFFFF means the fault was not inside module text -- a host
        # bug, not something to addr2line into the module's own .so.
        "fault_text_offset": (None if fault_text_offset == 0xFFFFFFFF
                              else fault_text_offset),
    }


SLOT_STATES = {0: "EMPTY", 1: "LOADED", 2: "RUNNING", 3: "FAULTED"}
