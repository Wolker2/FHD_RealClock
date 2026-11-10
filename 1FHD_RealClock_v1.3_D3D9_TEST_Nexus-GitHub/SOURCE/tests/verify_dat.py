"""Check the game resource wrapper, decode the DAT and compare its script bindings."""
import importlib.util
from pathlib import Path
import struct
import sys
import zlib


def load_codec(path):
    spec = importlib.util.spec_from_file_location("verified_blockpar", path)
    codec = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(codec)
    return codec


def unwrap(path):
    data = path.read_bytes()
    size, checksum = struct.unpack_from("<2I", data)
    key1, key2 = 0x7DB6C99D, 0xC83FCBF3
    body = data[8:]
    assert size ^ key1 ^ key2 == len(body)
    inner_crc = struct.pack("<I", zlib.crc32(body) ^ key1)
    assert checksum == zlib.crc32(inner_crc + body) ^ key2
    return body


def entries(item):
    if isinstance(item, str):
        return item
    pairs = [(entry["name"], entries(entry["value"])) for entry in item["entries"]]
    assert len(pairs) == len(dict(pairs))
    return dict(pairs)


def verify(path, codec_path, lang_path):
    codec = load_codec(codec_path)
    tree = codec.decode(unwrap(path), "blockpar")
    call = "unknown fhdClock=ImportedFunction('FHDRealClock','FHD_ClockInit');"
    handler = {"00": call, "01": "fhdClock();"}
    assert entries(tree["root"]) == {
        "Data": {"ScriptLibs": {"FHDRealClock": {
            "Path": r"Mods\FHD_MODS\FHD_RealClock\Native\FHDRealClock.dll",
            "FHD_ClockInit": "int,FHD_ClockInit",
        }}},
        "BV": {
            "OnStart": {"FHDRealClockStart": handler},
            "OnLoad": {"FHDRealClockLoad": handler},
        },
    }
    assert codec.decode(codec.encode(tree), "blockpar") == tree
    print("PASS Main.dat / outer checksum, independent decode, exact tree and round trip")
    lang = codec.decode(unwrap(lang_path), "blockpar")
    root = entries(lang["root"])
    assert set(root) == {"ShipInfo"}
    ship_info = root["ShipInfo"]
    assert set(ship_info) == {"AddInfo"}
    add_info = ship_info["AddInfo"]
    assert set(add_info) == {"CustomInfos"}
    legacy = add_info["CustomInfos"]
    assert set(legacy) == {"FHDRealClockCore"}
    assert set(legacy["FHDRealClockCore"]) == {"Name", "Description", "Icon"}
    assert codec.decode(codec.encode(lang), "blockpar") == lang
    print("PASS Lang.dat / legacy save type without action code")


if __name__ == "__main__":
    verify(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
