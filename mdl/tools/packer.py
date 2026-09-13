#!/usr/bin/env python3
"""
packer.py -- converts a module ELF (.so, built per mdl/host/host_api.h's
compile flags) into the flat .mdl format the MCU loader understands.

The MCU never parses ELF (see mdl/core/mdl_format.h's header comment);
this is where all of that complexity lives instead, on the PC, where a
bad diagnostic costs nothing and a good one saves an agent several retry
loops. Every rejection path below is meant to be directly actionable --
see the module docstring examples in the project's task description:

    text section 18432 bytes exceeds arena text region 16384 bytes
    undefined symbol 'printf' -- not in host_api.h vtable
    abi_ver mismatch: module built against v2, host expects v3

No external dependencies (no pyelftools) -- this hand-rolls the small
slice of ELF32 little-endian parsing actually needed, deliberately, so
watch.py's <2s edit-to-device loop never waits on a pip install.

Layout contract with mdl/core/loader.c (the two must agree, this is not
independently variable -- see loader.c's memory-layout comment):
  on disk (this file writes):    [text][data][got][reloc_table]
  in the MCU's data arena (loader.c places it): [got][data][bss]
"""
import argparse
import re
import struct
import sys
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
MDL_ROOT = HERE.parent

MDL_MAGIC = 0x304C444D  # 'MDL0'
ARCH_ARMV7M = 1

R_ARM_RELATIVE = 23

SHT_NOBITS = 8
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4
SHF_WRITE = 0x1

PT_LOAD = 1
PF_X = 1
PF_W = 2

# Sections that exist only for the ELF's own dynamic-linking machinery --
# never copied into the deployed payload, on either the text or data side.
DYNLINK_SECTIONS = {".hash", ".dynsym", ".dynstr", ".rel.dyn", ".rela.dyn", ".dynamic"}

MDL_CMD_NAME_MAX = 16
MDL_NAME_MAX = 16
MDL_EVT_QUEUE_MAX = 16   # must equal mdl_format.h
MDL_HEADER_FMT = ("<IHH" + "I" * 12 + f"{MDL_CMD_NAME_MAX}s{MDL_NAME_MAX}s" + "IHH")
MDL_HEADER_SIZE = struct.calcsize(MDL_HEADER_FMT)
# magic,crc32,text_size,data_size,bss_size,got_off,got_count,init_off,
# reloc_off,reloc_count = 10 u32; abi_ver,arch = 2 u16; then the ABI v2
# additions cmd_off,res_off,res_count = 3 u32 and cmd_name[16].
# Must equal sizeof(mdl_header_t) in mdl/core/mdl_format.h exactly.
assert MDL_HEADER_SIZE == 96, MDL_HEADER_SIZE   # + evt_off, evt_queue_depth, evt_rate_hz

MDL_RES_FMT = "<BB2x"
assert struct.calcsize(MDL_RES_FMT) == 4

# Metadata sections emitted by MDL_MODULE_COMMAND()/MDL_MODULE_RESOURCES()
# in host_api.h. Read out here into header fields; deliberately NOT removed
# from the text blob, because they sit inside .rodata and cutting a hole
# there would shift every offset around them for a few dozen bytes saved.
SEC_COMMAND = ".mdl_command"
SEC_RESOURCES = ".mdl_resources"
SEC_NAME = ".mdl_name"
SEC_EVENTS = ".mdl_events"

MDL_RELOC_FMT = "<IB3x"
MDL_RELOC_SIZE = struct.calcsize(MDL_RELOC_FMT)
assert MDL_RELOC_SIZE == 8

MDL_RELOC_TEXT_BASE = 0
MDL_RELOC_DATA_BASE = 1


class PackError(Exception):
    """Raised with an already-user-facing '✗ ...' message."""

# ---- board pin facts, parsed from the SAME file the firmware compiles ----
#
# mdl/host/board_pins.def is included by mdl/host/host_resources.c with the
# macros defined, and parsed here with a regex. One source, deliberately:
# the packer refuses a conflicting MDL at build time and the firmware
# refuses it at load time, and two hand-kept copies of this table would not
# stay equal. The failure mode of drift is nasty -- a build that passes and
# a device that refuses, the two disagreeing about a fact neither is
# locally wrong about.
BOARD_PINS_DEF = MDL_ROOT / "host" / "board_pins.def"

RES_KIND = {"GPIO": 1, "I2C": 2, "UART": 3, "TIMER": 4, "ADC": 5}


def _pin(port, num):
    return (int(port) << 4) | int(num)


def pin_name(pin):
    return f"P{chr(ord('A') + (pin >> 4))}{pin & 0x0F}"


class BoardPins:
    """Peripheral -> pins, pin -> host holder, gpio index -> pin."""

    def __init__(self, path=BOARD_PINS_DEF):
        text = path.read_text(encoding="utf-8")
        self.periph = {}   # (kind, id) -> (name, [pins])
        self.owner = {}    # pin -> (HARD|YIELDS, who)
        self.gpio = {}     # whitelist index -> (pin, name)

        pin_re = r"P\(\s*(\d+)\s*,\s*(\d+)\s*\)|NONE"
        for m in re.finditer(
                r'MDL_PERIPH\(\s*(\w+)\s*,\s*(\d+)\s*,\s*"([^"]*)"\s*,(.*?)\)\s*$',
                text, re.S | re.M):
            kind, inst, name, rest = m.groups()
            pins = [_pin(a, b) for a, b in
                    [g for g in re.findall(r"P\(\s*(\d+)\s*,\s*(\d+)\s*\)", rest)]]
            self.periph[(RES_KIND[kind], int(inst))] = (name, pins)

        for m in re.finditer(
                r'MDL_PIN_OWNER\(\s*P\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*,\s*(\w+)\s*,\s*"([^"]*)"',
                text):
            a, b, how, who = m.groups()
            self.owner[_pin(a, b)] = (how, who)

        for m in re.finditer(
                r'MDL_GPIO_PIN\(\s*(\d+)\s*,\s*P\(\s*(\d+)\s*,\s*(\d+)\s*\)\s*,\s*"([^"]*)"',
                text):
            idx, a, b, name = m.groups()
            self.gpio[int(idx)] = (_pin(a, b), name)

        # A silent parse failure would turn every conflict check into a
        # no-op that reports success, which is the one outcome worse than
        # not having the check.
        if not self.periph or not self.owner or not self.gpio:
            raise PackError(f"could not parse {path} -- "
                            f"{len(self.periph)} peripherals, {len(self.owner)} "
                            f"owned pins, {len(self.gpio)} gpio rows")

    def expand(self, kind, ident):
        """One claim -> (name, [pins]), or (None, []) if unplaceable."""
        if kind == RES_KIND["GPIO"]:
            if ident not in self.gpio:
                return None, []
            pin, name = self.gpio[ident]
            return name, [pin]
        if (kind, ident) not in self.periph:
            return None, []
        return self.periph[(kind, ident)]


def check_resource_conflicts(res_bytes, board):
    """Refuse here what the device would refuse on load.

    Only the statically decidable half is checkable: a claim against a pin
    the host holds permanently, and two of this MDL's own claims landing on
    the same pin. Both are facts about the image, so failing the build beats
    failing the deploy -- by the time an MDL reaches a device, someone is
    already waiting on it.
    """
    seen = {}
    for off in range(0, len(res_bytes), 4):
        kind, ident = res_bytes[off], res_bytes[off + 1]
        name, pins = board.expand(kind, ident)
        if name is None:
            kn = next((k for k, v in RES_KIND.items() if v == kind), "?")
            raise PackError(f"{kn.lower()} instance {ident} is not something this "
                            f"board knows how to place")
        for pin in pins:
            how_who = board.owner.get(pin)
            if how_who and how_who[0] == "HARD":
                raise PackError(f"{name} needs {pin_name(pin)}, held by {how_who[1]}")
            if pin in seen:
                raise PackError(f"{name} and {seen[pin]} are both {pin_name(pin)} -- "
                                f"one MDL cannot claim the same pin twice")
            seen[pin] = name


class Elf32:
    def __init__(self, data: bytes):
        self.data = data
        if data[:4] != b"\x7fELF":
            raise PackError("not an ELF file (bad magic)")
        if data[4] != 1:
            raise PackError("not a 32-bit ELF (only ELF32 is supported)")
        if data[5] != 1:
            raise PackError("not little-endian (only LE is supported)")

        (self.e_type, self.e_machine, self.e_version, self.e_entry,
         self.e_phoff, self.e_shoff, self.e_flags, self.e_ehsize,
         self.e_phentsize, self.e_phnum, self.e_shentsize, self.e_shnum,
         self.e_shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", data, 16)

        self.sections = []
        for i in range(self.e_shnum):
            off = self.e_shoff + i * self.e_shentsize
            (sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size,
             sh_link, sh_info, sh_addralign, sh_entsize) = struct.unpack_from(
                "<IIIIIIIIII", data, off)
            self.sections.append({
                "name_off": sh_name, "type": sh_type, "flags": sh_flags,
                "addr": sh_addr, "offset": sh_offset, "size": sh_size,
                "link": sh_link, "info": sh_info, "entsize": sh_entsize,
            })

        shstrtab = self.sections[self.e_shstrndx]
        strtab_data = data[shstrtab["offset"]:shstrtab["offset"] + shstrtab["size"]]
        for s in self.sections:
            s["name"] = _cstr_at(strtab_data, s["name_off"])
        self.by_name = {s["name"]: s for s in self.sections}

        self.segments = []
        for i in range(self.e_phnum):
            off = self.e_phoff + i * self.e_phentsize
            (p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz,
             p_flags, p_align) = struct.unpack_from("<IIIIIIII", data, off)
            self.segments.append({
                "type": p_type, "offset": p_offset, "vaddr": p_vaddr,
                "filesz": p_filesz, "memsz": p_memsz, "flags": p_flags,
            })

    def section_bytes(self, name):
        s = self.by_name.get(name)
        if s is None or s["type"] == SHT_NOBITS:
            return b""
        return self.data[s["offset"]:s["offset"] + s["size"]]

    def symbols(self, symtab_name, strtab_name):
        symtab = self.by_name.get(symtab_name)
        if symtab is None:
            return []
        strtab = self.sections[symtab["link"]] if symtab["link"] else self.by_name.get(strtab_name)
        strtab_data = self.data[strtab["offset"]:strtab["offset"] + strtab["size"]]
        raw = self.data[symtab["offset"]:symtab["offset"] + symtab["size"]]
        count = len(raw) // 16
        out = []
        for i in range(count):
            st_name, st_value, st_size, st_info, st_other, st_shndx = \
                struct.unpack_from("<IIIBBH", raw, i * 16)
            out.append({
                "name": _cstr_at(strtab_data, st_name),
                "value": st_value, "size": st_size,
                "bind": st_info >> 4, "type": st_info & 0xF,
                "shndx": st_shndx,
            })
        return out

    def relocations(self, relsec_name):
        s = self.by_name.get(relsec_name)
        if s is None:
            return []
        raw = self.data[s["offset"]:s["offset"] + s["size"]]
        count = len(raw) // 8
        out = []
        for i in range(count):
            r_offset, r_info = struct.unpack_from("<II", raw, i * 8)
            out.append({"offset": r_offset, "type": r_info & 0xFF, "sym": r_info >> 8})
        return out


def _cstr_at(buf: bytes, off: int) -> str:
    end = buf.index(b"\x00", off)
    return buf[off:end].decode("utf-8")


def find_segment(elf: Elf32, want_exec: bool):
    for seg in elf.segments:
        if seg["type"] != PT_LOAD:
            continue
        is_x = bool(seg["flags"] & PF_X)
        if is_x == want_exec:
            return seg
    return None


def read_u32(blob: bytes, off: int) -> int:
    return struct.unpack_from("<I", blob, off)[0]


def write_u32(blob: bytearray, off: int, val: int) -> None:
    struct.pack_into("<I", blob, off, val & 0xFFFFFFFF)


def parse_host_abi_version(host_api_h: Path) -> int:
    text = host_api_h.read_text(encoding="utf-8")
    m = re.search(r"#define\s+HOST_API_ABI_VERSION\s+(\d+)", text)
    if not m:
        raise PackError(f"could not find HOST_API_ABI_VERSION in {host_api_h}")
    return int(m.group(1))


def pack(so_path: Path, out_path: Path, *, abi_ver: int, arch: int,
         name_hint: str = "",
         allow_conflicts: bool = False,
         text_region_size: int, data_region_size: int) -> None:
    data = so_path.read_bytes()
    elf = Elf32(data)

    if elf.e_type != 3:  # ET_DYN
        raise PackError(f"{so_path}: not a shared object (ET_DYN) -- "
                         f"did the module link with -shared?")

    # ---- undefined-symbol check (defense in depth -- `--no-undefined`
    # at link time should already have caught this, but a module built
    # without the exact spec flags could slip through) ----
    for sym in elf.symbols(".dynsym", ".dynstr"):
        if sym["shndx"] == 0 and sym["name"] and sym["bind"] != 0 and sym["type"] in (1, 2):
            raise PackError(
                f"undefined symbol '{sym['name']}' -- not in host_api.h vtable "
                f"(module must not reference any host symbol by name; pass it "
                f"through host_api_t instead, or check for a typo)")

    # ---- abi_ver cross-check against __mdl_abi_ver (see host_api.h's
    # MDL_MODULE_ABI_DECLARE()) ----
    dynsyms = elf.symbols(".dynsym", ".dynstr")
    abi_sym = next((s for s in dynsyms if s["name"] == "__mdl_abi_ver"), None)
    if abi_sym is None:
        raise PackError(
            "module does not export '__mdl_abi_ver' -- did you forget "
            "MDL_MODULE_ABI_DECLARE(); at file scope? (see host_api.h)")
    module_abi_ver = _read_symbol_u32(elf, abi_sym)
    if module_abi_ver != abi_ver:
        raise PackError(
            f"abi_ver mismatch: module built against v{module_abi_ver}, "
            f"host expects v{abi_ver}")

    # ---- module_init ----
    init_sym = next((s for s in dynsyms if s["name"] == "module_init"), None)
    if init_sym is None:
        raise PackError("module does not export 'module_init' -- "
                         "every module must define int module_init(const host_api_t *host)")

    # ---- text blob: RX segment, minus dynamic-linking-only sections ----
    rx_seg = find_segment(elf, want_exec=True)
    if rx_seg is None:
        raise PackError("no executable (RX) PT_LOAD segment found")
    text_sections = [s for s in elf.sections
                      if s["flags"] & SHF_ALLOC
                      and rx_seg["vaddr"] <= s["addr"] < rx_seg["vaddr"] + rx_seg["filesz"]
                      and s["name"] not in DYNLINK_SECTIONS
                      and s["size"] > 0]
    if not text_sections:
        raise PackError("RX segment has no real code/rodata sections")
    text_link_base = min(s["addr"] for s in text_sections)
    text_link_end = rx_seg["vaddr"] + rx_seg["filesz"]
    text_blob = data[rx_seg["offset"] + (text_link_base - rx_seg["vaddr"]):
                      rx_seg["offset"] + (text_link_end - rx_seg["vaddr"])]

    if len(text_blob) > text_region_size:
        raise PackError(f"text section {len(text_blob)} bytes exceeds "
                         f"arena text region {text_region_size} bytes")

    # ---- data/bss/got: RW segment ----
    rw_seg = find_segment(elf, want_exec=False)
    got_sec = elf.by_name.get(".got")
    data_sec = elf.by_name.get(".data")
    bss_sec = elf.by_name.get(".bss")

    for forbidden in (".persistent", ".noinit"):
        s = elf.by_name.get(forbidden)
        if s is not None and s["size"] > 0:
            raise PackError(
                f"module defines a nonzero '{forbidden}' section -- not "
                f"supported in v1, remove any __attribute__((section(\"{forbidden}\"))) usage")

    got_bytes = elf.section_bytes(".got") if got_sec else b""
    got_count = len(got_bytes) // 4
    data_bytes = elf.section_bytes(".data") if data_sec else b""
    bss_size = bss_sec["size"] if bss_sec else 0

    data_region_needed = len(got_bytes) + len(data_bytes) + bss_size
    if data_region_needed > data_region_size:
        raise PackError(f"got+data+bss {data_region_needed} bytes exceeds "
                         f"arena data region {data_region_size} bytes")

    # ---- relocations: classify each R_ARM_RELATIVE GOT slot as
    # text-based or data-based by checking which section its ORIGINAL
    # (link-time, base-0) stored value falls inside; rewrite the slot's
    # value in-place (in a mutable copy of got_bytes) to be a pure
    # in-blob offset from that base, per mdl_reloc_kind_t's contract. ----
    if rw_seg is None and got_bytes:
        raise PackError("module has a .got section but no writable (RW) PT_LOAD segment")

    got_mut = bytearray(got_bytes)
    got_vaddr = got_sec["addr"] if got_sec else 0
    reloc_entries = []

    for rel in elf.relocations(".rel.dyn"):
        if rel["type"] != R_ARM_RELATIVE:
            raise PackError(
                f"unsupported relocation type {rel['type']} at offset "
                f"0x{rel['offset']:x} -- only R_ARM_RELATIVE (GOT self-"
                f"relocation) is supported; check for an external symbol "
                f"reference or unexpected linker behavior")
        if not (got_sec and got_sec["addr"] <= rel["offset"] < got_sec["addr"] + got_sec["size"]):
            raise PackError(
                f"relocation at offset 0x{rel['offset']:x} does not "
                f"target .got -- unexpected linker output")

        slot_off_in_got = rel["offset"] - got_vaddr
        value = read_u32(got_mut, slot_off_in_got)

        kind, normalized = _classify_and_normalize(
            value, text_link_base, len(text_blob),
            data_sec, bss_sec, len(got_bytes))
        write_u32(got_mut, slot_off_in_got, normalized)
        reloc_entries.append((slot_off_in_got, kind))

    # ---- module_init's offset relative to text start ----
    init_off = init_sym["value"] - text_link_base
    if not (0 <= init_off < len(text_blob)):
        raise PackError("module_init's address falls outside the packed text blob "
                         "(unexpected linker layout)")

    # ---- module_cmd (ABI v2, optional) ----
    # Same Thumb-bit convention as init_off: the ELF symbol value for a
    # Thumb function has bit 0 set, and the loader branches to it with blx,
    # so the bit must survive into the header. 0 means "no command", which
    # is why a module whose module_cmd sat at text offset 0 would be
    # ambiguous -- it cannot, since module_init is also in text and one of
    # them is not at 0.
    cmd_sym = next((y for y in dynsyms if y["name"] == "module_cmd"), None)
    cmd_off = 0
    cmd_name = b""
    if cmd_sym is not None:
        cmd_off = cmd_sym["value"] - text_link_base
        if not (0 < cmd_off < len(text_blob)):
            raise PackError("module_cmd's address falls outside the packed text blob")
        cmd_name = elf.section_bytes(SEC_COMMAND) if elf.by_name.get(SEC_COMMAND) else b""
        if not cmd_name or cmd_name[0] == 0:
            raise PackError("module_cmd() is defined but MDL_MODULE_COMMAND(\"name\") is "
                            "missing -- the host would have no name to bind it to")
        cmd_name = cmd_name[:MDL_CMD_NAME_MAX]
    elif elf.by_name.get(SEC_COMMAND):
        raise PackError('MDL_MODULE_COMMAND() is declared but module_cmd() is not defined')

    # ---- the MDL's own name (ABI v2) ----
    # Defaults to the source directory, because build/module.so tells a
    # person nothing and requiring every author to declare a name would
    # mean most MDLs shipped without one. MDL_MODULE_NAME() overrides.
    if elf.by_name.get(SEC_NAME):
        mdl_name = elf.section_bytes(SEC_NAME).split(b"\0")[0]
    elif name_hint:
        mdl_name = name_hint.encode()[:MDL_NAME_MAX - 1]
    else:
        mdl_name = b""

    # ---- declared resources (ABI v2, optional) ----
    res_bytes = elf.section_bytes(SEC_RESOURCES) if elf.by_name.get(SEC_RESOURCES) else b""
    if len(res_bytes) % 4 != 0:
        raise PackError(f"{SEC_RESOURCES} is {len(res_bytes)}B, not a multiple of "
                        f"sizeof(mdl_res_t)=4 -- mismatched host_api.h?")
    res_count = len(res_bytes) // 4

    # ---- events (ABI v4, optional) ----
    evt_sym = next((y for y in dynsyms if y["name"] == "module_event"), None)
    evt_off = 0
    if evt_sym is not None:
        evt_off = evt_sym["value"] - text_link_base
        if not (0 < evt_off < len(text_blob)):
            raise PackError("module_event's address falls outside the packed text blob")

    depth = rate = 0
    if elf.by_name.get(SEC_EVENTS):
        ev = elf.section_bytes(SEC_EVENTS)
        if len(ev) < 4:
            raise PackError(f"{SEC_EVENTS} is {len(ev)}B, expected 4 -- mismatched host_api.h?")
        depth, rate = struct.unpack("<HH", ev[:4])

    # This is the half of 'will it overflow' that CAN be settled before an
    # image ever reaches a device: a declared depth against a fixed host
    # budget. The other half -- how fast events actually arrive -- is a
    # fact about the world, not about this file, so it is recorded as a
    # contract for the host to check at runtime rather than pretended to
    # be provable here.
    # Pin-level conflicts, refused here rather than only on the device.
    #
    # allow_conflicts exists to keep the DEVICE's own check testable. That
    # check is the last line of defence against an image that did not come
    # through this packer, so it has to stay exercised -- and once this
    # gate works, a deliberately-conflicting image can no longer be built
    # by accident. Never pass it for anything real.
    if not allow_conflicts:
        check_resource_conflicts(res_bytes, BoardPins())

    if depth > MDL_EVT_QUEUE_MAX:
        raise PackError(f"MDL_MODULE_EVENTS asks for {depth} queue slots; the host "
                        f"provides {MDL_EVT_QUEUE_MAX}. Lower the depth, or drain "
                        f"events faster.")
    if evt_sym is not None and depth == 0:
        raise PackError("module_event() is defined but MDL_MODULE_EVENTS(depth, rate) "
                        "is missing -- the host would have nowhere to queue events")
    if evt_sym is None and depth != 0:
        raise PackError("MDL_MODULE_EVENTS() is declared but module_event() is not "
                        "defined -- events would be queued and never delivered")

    # An edge-triggered claim with no handler is the same mistake seen
    # from the resource side, and worth its own message.
    for off in range(0, len(res_bytes), 4):
        if res_bytes[off + 2] != 0 and evt_sym is None:
            raise PackError(f"MDL_RES_GPIO_IRQ(pin {res_bytes[off + 1]}, ...) asks for "
                            f"interrupts but the MDL defines no module_event()")

    # ---- assemble payload ----
    reloc_table = bytearray()
    for off, kind in reloc_entries:
        reloc_table += struct.pack(MDL_RELOC_FMT, off, kind)

    payload = bytearray()
    payload += text_blob
    payload += data_bytes
    got_off = len(payload)
    payload += got_mut
    reloc_off = len(payload)
    payload += reloc_table
    res_off = len(payload)
    payload += res_bytes

    crc = zlib.crc32(bytes(payload)) & 0xFFFFFFFF

    header = struct.pack(
        MDL_HEADER_FMT,
        MDL_MAGIC, abi_ver, arch, crc,
        len(text_blob), len(data_bytes), bss_size,
        got_off, got_count,
        init_off,
        reloc_off, len(reloc_entries),
        cmd_off, res_off, res_count,
        cmd_name.ljust(MDL_CMD_NAME_MAX, b"\0")[:MDL_CMD_NAME_MAX],
        mdl_name.ljust(MDL_NAME_MAX, b"\0")[:MDL_NAME_MAX],
        evt_off, depth, rate,
    )

    out_path.write_bytes(header + bytes(payload))
    print(f"packed {so_path.name} -> {out_path} "
          f"(text={len(text_blob)}B data={len(data_bytes)}B bss={bss_size}B "
          f"got={got_count} relocs={len(reloc_entries)}"
          + (f" cmd={cmd_name.split(b"\0")[0].decode()}" if cmd_off else "")
          + (f" res={res_count}" if res_count else "")
          + (f" evt(depth={depth},rate={rate})" if evt_off else "") + ")")


def _read_symbol_u32(elf: Elf32, sym: dict) -> int:
    """Reads the 4-byte value stored at a symbol's link-time address (used
    for __mdl_abi_ver -- a `const uint32_t`, so it lives in .rodata, not
    .data; resolved via st_shndx rather than assuming a section name)."""
    if sym["shndx"] == 0 or sym["shndx"] >= len(elf.sections):
        raise PackError(f"'{sym['name']}' has no defining section (undefined?)")
    sec = elf.sections[sym["shndx"]]
    if sec["type"] == SHT_NOBITS:
        raise PackError(f"'{sym['name']}' is in a NOBITS (.bss-like) section -- "
                         f"must be a 'const' with a real initializer")
    file_off = sec["offset"] + (sym["value"] - sec["addr"])
    return read_u32(elf.data, file_off)


def _classify_and_normalize(value, text_link_base, text_len,
                             data_sec, bss_sec, got_len):
    if text_link_base <= value < text_link_base + text_len:
        return MDL_RELOC_TEXT_BASE, value - text_link_base

    if data_sec and data_sec["addr"] <= value < data_sec["addr"] + data_sec["size"]:
        return MDL_RELOC_DATA_BASE, got_len + (value - data_sec["addr"])

    if bss_sec and bss_sec["addr"] <= value < bss_sec["addr"] + bss_sec["size"]:
        data_len = data_sec["size"] if data_sec else 0
        return MDL_RELOC_DATA_BASE, got_len + data_len + (value - bss_sec["addr"])

    raise PackError(
        f"a GOT slot's link-time value (0x{value:x}) falls outside both "
        f"the text blob and the data/bss sections -- unexpected linker "
        f"output, or the module references something packer.py doesn't "
        f"know how to classify yet")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("module_so", type=Path, help="linked module .so (ET_DYN, -fPIC -shared)")
    ap.add_argument("output_mdl", type=Path, help="output .mdl path")
    ap.add_argument("--host-api-h", type=Path,
                     default=Path(__file__).resolve().parent.parent / "host" / "host_api.h",
                     help="path to host_api.h to read HOST_API_ABI_VERSION from")
    ap.add_argument("--arch", type=int, default=ARCH_ARMV7M,
                     help="mdl_arch_t value to embed (default: 1 = armv7m)")
    ap.add_argument("--text-region-size", type=int, default=16 * 1024,
                     help="arena text region size in bytes (default: 16384, v1 arena)")
    ap.add_argument("--data-region-size", type=int, default=8 * 1024,
                     help="arena data region size in bytes -- got+data+bss must fit "
                          "(default: 8192, v1 arena)")
    ap.add_argument("--allow-conflicts", action="store_true",
                     help="pack even if the declared resources conflict. ONLY for "
                          "building deliberately-bad images to test the device-side "
                          "check, which is the last line of defence for images that "
                          "did not come through this packer.")
    ap.add_argument("--name", default="",
                     help="name the device shows in `status`; defaults to the source "
                          "directory name")
    args = ap.parse_args(argv)

    try:
        abi_ver = parse_host_abi_version(args.host_api_h)
        # build/module.so says nothing useful; the directory holding it
        # is what the MDL is actually called (mdl/tests/modules/<name>/
        # build/module.so -> <name>). --name overrides, and so does
        # MDL_MODULE_NAME() inside the source.
        hint = args.name or args.module_so.resolve().parent.parent.name
        pack(args.module_so, args.output_mdl,
             abi_ver=abi_ver, arch=args.arch, name_hint=hint,
             allow_conflicts=args.allow_conflicts,
             text_region_size=args.text_region_size,
             data_region_size=args.data_region_size)
    except PackError as e:
        print(f"✗ {e}", file=sys.stderr)
        return 1
    except FileNotFoundError as e:
        print(f"✗ file not found: {e.filename}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
