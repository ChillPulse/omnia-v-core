#!/usr/bin/env python3
"""Check static SRAM layout of the AT32 image; this is not a hardware/startup proof.

Requires pyelftools (installed with PlatformIO). Usage: python scripts/check_at32_elf.py firmware.elf
"""
import argparse
import struct
import sys

from elftools.elf.elffile import ELFFile

BASE, SPLIT, END = 0x20000000, 0x20018000, 0x20038000


def check(path):
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf.elfclass != 32 or not elf.little_endian or elf.header["e_machine"] != "EM_ARM":
            raise ValueError("expected 32-bit little-endian ARM ELF")
        sections = {s.name: s for s in elf.iter_sections()}
        for name in (".isr_vector", ".data", ".bss", "._user_heap_stack", ".sram_ext"):
            if name not in sections:
                raise ValueError(f"missing {name}")
        syms = sections.get(".symtab")
        if syms is None:
            raise ValueError("symbol table is missing")
        def addr(name):
            values = syms.get_symbol_by_name(name)
            if not values:
                raise ValueError(f"missing symbol {name}")
            return values[0]["st_value"]
        vector = sections[".isr_vector"]
        if vector["sh_addr"] != 0x08000000 or vector["sh_size"] < 8:
            raise ValueError("unexpected vector table location/size")
        sp, reset = struct.unpack_from("<II", vector.data())
        if sp != SPLIT or sp != addr("_estack") or sp & 7:
            raise ValueError(f"invalid initial SP: {sp:#x}")
        if reset != (addr("Reset_Handler") | 1):
            raise ValueError("reset vector does not point to Reset_Handler in Thumb mode")
        for start, finish in (("_sdata", "_edata"), ("_sbss", "_ebss")):
            if not BASE <= addr(start) <= addr(finish) <= SPLIT:
                raise ValueError(f"{start}..{finish} outside base SRAM")
        if not SPLIT <= addr("_ssram_ext") <= addr("_esram_ext") <= END:
            raise ValueError("extended SRAM symbols out of bounds")
        for name, start, finish in ((".data", "_sdata", "_edata"),
                                    (".bss", "_sbss", "_ebss"),
                                    (".sram_ext", "_ssram_ext", "_esram_ext")):
            sec = sections[name]
            if sec["sh_addr"] != addr(start) or sec["sh_addr"] + sec["sh_size"] != addr(finish):
                raise ValueError(f"{name} section/symbol mismatch")
        if sections[".sram_ext"]["sh_type"] != "SHT_NOBITS":
            raise ValueError(".sram_ext must be NOLOAD")
        for sec in elf.iter_sections():
            begin, size, flags = sec["sh_addr"], sec["sh_size"], sec["sh_flags"]
            if not flags & 2 or not size or not (BASE <= begin < 0x30000000):
                continue
            end = begin + size
            if sec.name == ".sram_ext":
                if begin < SPLIT or end > END or sec["sh_type"] != "SHT_NOBITS":
                    raise ValueError(".sram_ext must be NOLOAD and inside extended SRAM")
            elif not (BASE <= begin < end <= SPLIT):
                raise ValueError(f"unexpected allocated SRAM section {sec.name} at {begin:#x}..{end:#x}")
        heap = sections["._user_heap_stack"]
        if heap["sh_addr"] + heap["sh_size"] > sp:
            raise ValueError("reserved heap/stack overlaps initial stack")
        print(f"AT32 ELF: SP={sp:#010x}; data={addr('_sdata'):#010x}..{addr('_edata'):#010x}; "
              f"bss={addr('_sbss'):#010x}..{addr('_ebss'):#010x}; "
              f"ext={addr('_ssram_ext'):#010x}..{addr('_esram_ext'):#010x} NOLOAD")
        print("Static layout only: review startup/constructors and test on real hardware.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    try:
        check(parser.parse_args().elf)
    except (ValueError, OSError) as exc:
        print(f"AT32 ELF check failed: {exc}", file=sys.stderr)
        sys.exit(1)
