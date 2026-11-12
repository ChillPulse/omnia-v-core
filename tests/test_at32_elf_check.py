#!/usr/bin/env python3
"""Exercise ELF guard against a real built image and an invalid initial SP."""
import os
import pathlib
import struct
import subprocess
import sys
import tempfile

from elftools.elf.elffile import ELFFile


def main(elf_path):
    elf_path = pathlib.Path(elf_path)
    checker = pathlib.Path(__file__).resolve().parents[1] / 'scripts' / 'check_at32_elf.py'
    subprocess.run([sys.executable, str(checker), str(elf_path)], check=True)
    with open(elf_path, 'rb') as f:
        elf = ELFFile(f)
        vector_offset = elf.get_section_by_name('.isr_vector')['sh_offset']
        f.seek(0)
        contents = bytearray(f.read())
    struct.pack_into('<I', contents, vector_offset, 0x20038000)
    with tempfile.TemporaryDirectory() as tmp:
        corrupted = os.path.join(tmp, 'bad.elf')
        pathlib.Path(corrupted).write_bytes(contents)
        result = subprocess.run([sys.executable, str(checker), corrupted], capture_output=True, text=True)
        assert result.returncode != 0 and 'invalid initial SP' in result.stderr, result.stderr
    print('[OK] ELF guard rejects initial SP outside base SRAM')


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: python tests/test_at32_elf_check.py firmware.elf')
    main(sys.argv[1])
