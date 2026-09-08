#!/usr/bin/env python3
"""
MCP server for the MDL framework -- stdio transport, JSON-RPC 2.0.

    python mdl/tools/mcp_server/server.py --port COM5

Implements maintain.md section 4's tool set. The protocol is written out by
hand rather than pulled from the `mcp` SDK: MCP over stdio is newline-
delimited JSON-RPC, which is about a hundred lines, and this project keeps
exactly one runtime dependency (pyserial, because talking to a serial port
from the standard library is not reasonable). packer.py has none at all.
Adding an SDK to save those lines would cost more than it saves.

WHAT THIS IS FOR: closing the loop for an agent. Without it, changing a
feature on the device means a human running three commands in the right
order with the right toolchain on PATH. With it, compile_check is an inner
loop that costs no hardware, and deploy_feature is the only step that
touches the board.

THE PORT IS EXCLUSIVE. This server holds it for its lifetime, so
tools/console.py and tools/watch.py cannot run at the same time. That is a
property of serial ports, not a limitation worth apologising for -- but it
does mean the server should be stopped before going back to manual work.
"""
import argparse
import json
import sys
import traceback
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent))

import build as build_mod          # noqa: E402
import mdl_proto as proto          # noqa: E402
from device import Device, DeviceError   # noqa: E402

PROTOCOL_VERSION = "2024-11-05"
SERVER_INFO = {"name": "mdl", "version": "0.1.0"}

TOOLS = [
    {
        "name": "describe_device",
        "description": (
            "What the target can do: ABI version, firmware build time, slot "
            "state, GPIO whitelist with the owner of each pin, and arena "
            "addresses. Call this first -- an MDL packed for a different ABI "
            "is refused by the loader, and the pin owners decide which "
            "resources a module may claim."
        ),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "compile_check",
        "description": (
            "Compile and pack MDL source WITHOUT touching the device. Runs "
            "mock_host natively, cross-compiles for ARM, then packs -- which "
            "validates the ABI, the sizes against the arena, the event queue "
            "budget, and resource conflicts at physical-pin level. This is "
            "the inner loop: it needs no hardware and no serial port, so use "
            "it until it passes and only then deploy."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "source": {"type": "string", "description": "Complete module.c source."},
                "name": {"type": "string", "description": "Name the device shows in status."},
                "against_device": {
                    "type": "boolean",
                    "description": (
                        "Also check against what the device is running right "
                        "now (ABI match, slot occupancy). Requires the port. "
                        "Default false, so this stays usable in CI and with no "
                        "board attached."
                    ),
                },
            },
            "required": ["source"],
        },
    },
    {
        "name": "deploy_feature",
        "description": (
            "compile_check, then unload whatever is loaded, then load this. "
            "Does not touch the device if the build fails. One slot exists, "
            "so deploying always replaces."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {
                "source": {"type": "string"},
                "name": {"type": "string"},
                "persist": {
                    "type": "boolean",
                    "description": (
                        "Also write it to flash, so the device reloads it "
                        "after power loss. Erases a flash sector, so leave it "
                        "off while iterating and turn it on for the version "
                        "meant to stay."
                    ),
                },
            },
            "required": ["source"],
        },
    },
    {
        "name": "remove_feature",
        "description": "Unload the current MDL and reclaim its resources. Idempotent.",
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "device_status",
        "description": (
            "Slot state and the last fault. fault_text_offset, when present, "
            "is the offset into the module's own text -- feed it to "
            "arm-none-eabi-addr2line against that module's .so to get the "
            "line that faulted."
        ),
        "inputSchema": {"type": "object", "properties": {}},
    },
    {
        "name": "read_logs",
        "description": (
            "Recent device console output, including anything the loaded MDL "
            "printed with host->log(). Buffered by this server as it arrives; "
            "the device itself keeps no history."
        ),
        "inputSchema": {
            "type": "object",
            "properties": {"max_lines": {"type": "integer", "default": 50}},
        },
    },
]


class Server:
    def __init__(self, port, baud):
        self.dev = Device(port, baud)
        self.port = port

    # ---- tools ------------------------------------------------------

    def describe_device(self, _args):
        ver = self.dev.console("ver")
        pins = self.dev.console("pins")
        arena = self.dev.console("arena")
        resp, payload = self.dev.request(proto.CMD_STATUS)
        st = proto.decode_status(payload) if resp == proto.RESP_STATUS else {}
        return {
            "ok": True,
            "firmware": _kv(ver),
            "slot_state": proto.SLOT_STATES.get(st.get("slot_state"), "?"),
            # Drop the echoed command and the prompt: the device echoes
            # what it was sent, and "mdl>" in a list of pins is noise a
            # model would have to learn to ignore.
            "gpio_whitelist": [l.strip() for l in pins.splitlines()
                                if l.strip() and not l.startswith(("pins", "mdl>"))],
            "arena": [l.strip() for l in arena.splitlines() if ".." in l],
            "transport": f"USB CDC on {self.port}",
        }

    def compile_check(self, args):
        r = build_mod.compile_check(args["source"], name=args.get("name", "mdl"))
        out = {"ok": r.ok, "diagnostics": r.diagnostics, "manifest": r.manifest}
        if not r.ok:
            out["error"] = r.diagnostics[0] if r.diagnostics else "build failed"
            return out
        if args.get("against_device"):
            out["device_check"] = self._against_device(r)
            if not out["device_check"]["ok"]:
                out["ok"] = False
                out["error"] = out["device_check"]["error"]
        return out

    def _against_device(self, r):
        """The half of checking that needs the board in front of you.

        Offline packing already proved the image is self-consistent. What it
        cannot know is what the device is running: an ABI mismatch is
        refused by the loader, and a busy slot means this deploy replaces
        something. Both are facts about a particular board at a particular
        moment, which is why this is opt-in rather than part of the build.
        """
        ver = _kv(self.dev.console("ver"))
        dev_abi = ver.get("abi", "").lstrip("v").split()[0] if "abi" in ver else None
        mdl_abi = str(r.manifest.get("abi_ver"))
        if dev_abi and dev_abi != mdl_abi:
            return {"ok": False, "error":
                    f"device runs ABI v{dev_abi}, this MDL is packed for v{mdl_abi}. "
                    f"The loader will refuse it. Rebuild the firmware or repack."}
        resp, payload = self.dev.request(proto.CMD_STATUS)
        st = proto.decode_status(payload) if resp == proto.RESP_STATUS else {}
        state = proto.SLOT_STATES.get(st.get("slot_state"), "?")
        return {"ok": True, "device_abi": dev_abi, "slot_state": state,
                "will_replace": state not in ("EMPTY", "?")}

    def deploy_feature(self, args):
        r = build_mod.compile_check(args["source"], name=args.get("name", "mdl"))
        if not r.ok:
            # Contract: a failed build does not touch the device at all.
            return {"ok": False, "error": r.diagnostics[0] if r.diagnostics
                    else "build failed", "diagnostics": r.diagnostics}

        # Deliberately no unload first. The device replaces atomically
        # now (F3): it validates the new image while the old MDL is still
        # running, and only then tears it down. Unloading here would
        # recreate exactly the gap that removed -- and would leave the
        # board empty if the new image then turned out to be bad.
        resp, payload = self.dev.request(proto.CMD_STATUS)
        st = proto.decode_status(payload) if resp == proto.RESP_STATUS else {}
        replaced = proto.SLOT_STATES.get(st.get("slot_state")) not in (None, "EMPTY")

        image = r.mdl_path.read_bytes()
        cmd = (proto.CMD_LOAD_PERSIST if args.get("persist")
                else proto.CMD_LOAD)
        resp, payload = self.dev.request(cmd, image, timeout=10.0)
        text = payload.decode("utf-8", "replace")
        ok = resp == proto.RESP_OK
        return {
            "ok": ok,
            "error": None if ok else text,
            "device_response": proto.resp_name(resp) + (f": {text}" if text else ""),
            "unloaded_previous": replaced,
            "persisted": bool(args.get("persist")),
            "manifest": r.manifest,
            "logs": self.dev.logs(20),
        }

    def remove_feature(self, _args):
        resp, payload = self.dev.request(proto.CMD_UNLOAD)
        text = payload.decode("utf-8", "replace")
        return {"ok": resp == proto.RESP_OK,
                "device_response": proto.resp_name(resp) + (f": {text}" if text else "")}

    def device_status(self, _args):
        resp, payload = self.dev.request(proto.CMD_STATUS)
        if resp != proto.RESP_STATUS:
            return {"ok": False, "error": f"unexpected response {proto.resp_name(resp)}"}
        st = proto.decode_status(payload)
        out = {"ok": True, "slot_state": proto.SLOT_STATES.get(st["slot_state"], "?")}
        if st["fault_pc"]:
            out["fault_pc"] = f"0x{st['fault_pc']:08X}"
            out["fault_text_offset"] = (
                None if st["fault_text_offset"] is None
                else f"0x{st['fault_text_offset']:08X}")
            out["hint"] = ("fault_text_offset is an offset into the module's own "
                            "text; addr2line it against that module's .so. A null "
                            "offset means the fault was in host code, not the MDL.")
        return out

    def read_logs(self, args):
        return {"ok": True, "lines": self.dev.logs(int(args.get("max_lines", 50)))}

    # ---- JSON-RPC ---------------------------------------------------

    def handle(self, msg):
        method = msg.get("method")
        mid = msg.get("id")

        if method == "initialize":
            return _ok(mid, {"protocolVersion": PROTOCOL_VERSION,
                              "capabilities": {"tools": {}},
                              "serverInfo": SERVER_INFO})
        if method in ("notifications/initialized", "initialized"):
            return None                      # notification, no reply
        if method == "tools/list":
            return _ok(mid, {"tools": TOOLS})
        if method == "tools/call":
            name = msg["params"]["name"]
            args = msg["params"].get("arguments", {}) or {}
            fn = getattr(self, name, None)
            if fn is None:
                return _err(mid, -32601, f"no such tool: {name}")
            try:
                result = fn(args)
            except DeviceError as e:
                # Reported, never retried -- see maintain.md section 4's error
                # contract. Retrying a LOAD that actually landed is not
                # harmless, so the decision belongs to the caller.
                result = {"ok": False, "error": str(e)}
            except Exception as e:            # noqa: BLE001
                result = {"ok": False, "error": f"{e!r}",
                          "traceback": traceback.format_exc()}
            return _ok(mid, {"content": [{"type": "text",
                                            "text": json.dumps(result, indent=2)}],
                              "isError": not result.get("ok", False)})
        if mid is None:
            return None
        return _err(mid, -32601, f"unknown method: {method}")


def _kv(text):
    """`ver`-style 'key : value' lines into a dict."""
    out = {}
    for line in text.splitlines():
        if ":" in line and not line.startswith("mdl>"):
            k, _, v = line.partition(":")
            k = k.strip()
            if k and " " not in k:
                out[k] = v.strip()
    return out


def _ok(mid, result):
    return {"jsonrpc": "2.0", "id": mid, "result": result}


def _err(mid, code, message):
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": code, "message": message}}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="e.g. COM5 or /dev/ttyACM0")
    ap.add_argument("--baud", type=int, default=115200)
    args = ap.parse_args(argv)

    server = Server(args.port, args.baud)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        reply = server.handle(msg)
        if reply is not None:
            sys.stdout.write(json.dumps(reply) + "\n")
            sys.stdout.flush()
    server.dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
