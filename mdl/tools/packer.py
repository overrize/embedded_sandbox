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

MDL_HEADER_FMT = "<IHHIIIIIIIII"
MDL_HEADER_SIZE = struct.calcsize(MDL_HEADER_FMT)
# magic,crc32,text_size,data_size,bss_size,got_off,got_count,init_off,
# reloc_off,reloc_count = 10 u32 fields; abi_ver,arch = 2 u16 fields.
# Must equal sizeof(mdl_header_t) in mdl/core/mdl_format.h exactly.
assert MDL_HEADER_SIZE == 4 * 10 + 2 * 2 == 44, MDL_HEADER_SIZE

MDL_RELOC_FMT = "<IB3x"
MDL_RELOC_SIZE = struct.calcsize(MDL_RELOC_FMT)
assert MDL_RELOC_SIZE == 8

MDL_RELOC_TEXT_BASE = 0
MDL_RELOC_DATA_BASE = 1


class PackError(Exception):
    """Raised with an already-user-facing '✗ ...' message."""


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

    crc = zlib.crc32(bytes(payload)) & 0xFFFFFFFF

    header = struct.pack(
        MDL_HEADER_FMT,
        MDL_MAGIC, abi_ver, arch, crc,
        len(text_blob), len(data_bytes), bss_size,
        got_off, got_count,
        init_off,
        reloc_off, len(reloc_entries),
    )

    out_path.write_bytes(header + bytes(payload))
    print(f"packed {so_path.name} -> {out_path} "
          f"(text={len(text_blob)}B data={len(data_bytes)}B bss={bss_size}B "
          f"got={got_count} relocs={len(reloc_entries)})")


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
    args = ap.parse_args(argv)

    try:
        abi_ver = parse_host_abi_version(args.host_api_h)
        pack(args.module_so, args.output_mdl,
             abi_ver=abi_ver, arch=args.arch,
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
