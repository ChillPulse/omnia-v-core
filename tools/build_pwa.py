#!/usr/bin/env python3
"""
Сборка PWA для раздела LittleFS (ТЗ, Том 4, Гл. 1):
  web/index.html  ->  littlefs_data/index.html.gz  (gzip -9, лимит 150 КБ)

Запуск: python3 tools/build_pwa.py
"""
import gzip
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "web" / "index.html"
DST = ROOT / "littlefs_data" / "index.html.gz"
LIMIT = 150 * 1024


def main() -> int:
    if not SRC.exists():
        print(f"Не найден {SRC}", file=sys.stderr)
        return 1
    data = SRC.read_bytes()
    packed = gzip.compress(data, compresslevel=9, mtime=0)   # mtime=0 -> воспроизводимая сборка
    if len(packed) > LIMIT:
        print(f"index.html.gz {len(packed)} байт превышает лимит {LIMIT}", file=sys.stderr)
        return 2
    DST.parent.mkdir(parents=True, exist_ok=True)
    DST.write_bytes(packed)
    print(f"OK: {DST.relative_to(ROOT)} — {len(packed)} байт (исходник {len(data)} байт)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
