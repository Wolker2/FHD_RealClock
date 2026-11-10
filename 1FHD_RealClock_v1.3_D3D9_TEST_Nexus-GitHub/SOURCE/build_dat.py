#!/usr/bin/env python3
"""Compile a BlockPar source with rangers-utils and add the SRHD resource checksum."""
from pathlib import Path
from struct import pack
from sys import argv
from zlib import compress, crc32

from rangers.blockpar import BlockPar
from rangers.io import Buffer

SEED = 0x51A2C3D4
SEED_KEY = 0xB1E8C689
CRC_KEY1 = 0x7DB6C99D
CRC_KEY2 = 0xC83FCBF3

source = Path(argv[2]) if len(argv) > 2 else Path(__file__).with_name("Main.txt")
dest = Path(argv[1]) if len(argv) > 1 else source.with_suffix(".dat")
tree = BlockPar.from_txt(str(source), encoding="utf-8")
plain = Buffer.from_bytes(b"")
tree.save(plain, new_format=True)
raw = plain._io.getvalue()
compressed = b"ZL01" + pack("<I", len(raw)) + compress(raw, 9)
encrypted = Buffer.from_bytes(pack("<II", crc32(compressed), SEED ^ SEED_KEY) + compressed)
encrypted.seek(8)
encrypted.cipher(SEED)
body = encrypted._io.getvalue()
header = pack("<II", len(body) ^ CRC_KEY1 ^ CRC_KEY2,
              crc32(body, crc32(pack("<I", crc32(body) ^ CRC_KEY1))) ^ CRC_KEY2)
dest.write_bytes(header + body)
