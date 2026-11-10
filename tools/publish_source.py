#!/usr/bin/env python3
"""Losslessly publish original RealClock 0.1.1 source files from the install ZIP."""
import hashlib
import json
from pathlib import Path
from zipfile import ZipFile

BASE = Path(__file__).resolve().parents[1]
VERSION = BASE / "1FHD_RealClock_0.1.1_TEST_Nexus-GitHub"
INSTALL = BASE / "FHD_RealClock_0.1.1_TEST.zip"
ZIP_SHA256 = "e9aa825bb4d7a8e09607dc511912453920ce314e6159cfd0901984ca2d08a22c"
PREFIX = "FHD_RealClock_SOURCE/"
GAME = "Mods/FHD_MODS/FHD_RealClock/"


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def publish():
    assert INSTALL.is_file(), "Original install ZIP is missing"
    assert sha256(INSTALL.read_bytes()) == ZIP_SHA256, "Original ZIP checksum changed"
    file_hashes = {}
    with ZipFile(INSTALL) as archive:
        assert archive.testzip() is None, "Corrupt install ZIP"
        names = [entry.filename for entry in archive.infolist() if not entry.is_dir()]
        assert len(names) == 18, f"Unexpected number of files: {len(names)}"
        source = sorted(name for name in names if name.startswith(PREFIX))
        assert len(source) == 13, f"Expected 13 source files, found {len(source)}"
        manifest = json.loads(archive.read(PREFIX + "manifest.json").decode("utf-8"))
        for name in names:
            raw = archive.read(name)
            file_hashes[name] = sha256(raw)
            if name.startswith(PREFIX):
                expected = manifest["sources"].get(name[len(PREFIX):])
                if expected is not None:
                    assert expected == sha256(raw), f"Source checksum mismatch: {name}"
                dest = VERSION / "SOURCE" / name[len(PREFIX):]
            elif name == "README_RU.txt" or name == "SHA256SUMS.txt":
                dest = VERSION / "docs" / name
            else:
                assert name.startswith(GAME), f"Unexpected game path: {name}"
                expected = manifest["files"].get(name)
                assert expected == sha256(raw), f"Runtime checksum mismatch: {name}"
                continue
            assert dest.resolve().is_relative_to(VERSION.resolve()), "Unsafe ZIP path"
            dest.parent.mkdir(parents=True, exist_ok=True)
            if dest.is_file():
                assert dest.read_bytes() == raw, f"Existing source edited: {dest}"
            else:
                dest.write_bytes(raw)
        for line in archive.read("SHA256SUMS.txt").decode("utf-8").splitlines():
            expected, name = line.split("  ", 1)
            assert file_hashes[name] == expected, f"SHA256SUMS mismatch: {name}"
        assert len(manifest["files"]) == 4
        assert len(manifest["sources"]) == 12
        assert all((VERSION / "SOURCE" / x[len(PREFIX):]).read_bytes() == archive.read(x) for x in source)
        assert archive.read(GAME+"Native/FHDRealClock.dll")[:2] == b"MZ"
    checks = {
        "original_zip": INSTALL.name,
        "original_zip_sha256": ZIP_SHA256,
        "archive_file_count": len(names),
        "source_file_count": len(source),
        "runtime_file_count": len(manifest["files"]),
        "source_files_byte_exact": True,
        "original_file_sha256": file_hashes,
        "source_build_executed": False,
        "runtime_tested_in_game": False,
        "exe_compatibility_confirmed_by_this_publish": False
    }
    (VERSION / "VERIFICATION.json").write_text(
        json.dumps(checks, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print("PASS: ZIP SHA256, 18 entries, 13 source files, original checksums, MZ header")


if __name__ == "__main__":
    publish()
