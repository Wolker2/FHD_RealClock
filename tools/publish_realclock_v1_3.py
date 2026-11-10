#!/usr/bin/env python3
"""Publish original FHD_RealClock v1.3 D3D9 TEST source from its ZIP.
Byte-exact export only: never runs the game, compiles a DLL, or edits game files.
"""
from pathlib import Path, PurePosixPath
from zipfile import ZipFile
from hashlib import sha256
import json

REPO = Path(__file__).resolve().parents[1]
VERSION = REPO / "1FHD_RealClock_v1.3_D3D9_TEST_Nexus-GitHub"
INSTALL = VERSION / "install/FHD_RealClock_v1.3_D3D9_TEST.zip"
EXPECTED = "11ba1462e902d221a8f6241edbbe3be507852ed61a92345af021a9f9c8ab7859"
SOURCE_PREFIX = "FHD_RealClock_SOURCE/"
GAME_PREFIX = "Mods/FHD_MODS/FHD_RealClock/"

def hash_bytes(value):
    return sha256(value).hexdigest()

def safe_write(relative, data):
    path = VERSION / relative
    assert path.resolve().is_relative_to(VERSION.resolve()), f"Unsafe output path: {relative}"
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        assert path.read_bytes() == data, f"Existing source differs: {relative}"
    else:
        path.write_bytes(data)

def publish():
    assert hash_bytes(INSTALL.read_bytes()) == EXPECTED, "Install ZIP SHA256 mismatch"
    with ZipFile(INSTALL) as z:
        assert z.testzip() is None, "Damaged ZIP"
        names = [info.filename for info in z.infolist() if not info.is_dir()]
        assert len(names) == 24, f"Unexpected file count: {len(names)}"
        assert len(names) == len(set(names)), "Duplicate ZIP entries"
        assert all(not PurePosixPath(n).is_absolute() and
                   ".." not in PurePosixPath(n).parts for n in names)
        manifest = json.loads(z.read(SOURCE_PREFIX + "manifest.json").decode("utf-8"))
        assert manifest["version"] == "1.3 D3D9 TEST"
        files = manifest["files"]
        sources = manifest["sources"]
        assert len(files) == 5 and len(sources) == 17
        expected_paths = (set(files) |
                          {SOURCE_PREFIX + name for name in sources} |
                          {SOURCE_PREFIX + "manifest.json", "SHA256SUMS.txt"})
        assert set(names) == expected_paths, "Unexpected or missing ZIP entries"
        original_hashes = {}
        for name in names:
            raw = z.read(name)
            digest = hash_bytes(raw)
            original_hashes[name] = digest
            if name in files:
                assert files[name] == digest, f"Runtime hash mismatch: {name}"
            elif name.startswith(SOURCE_PREFIX) and name != SOURCE_PREFIX + "manifest.json":
                rel = name[len(SOURCE_PREFIX):]
                assert sources[rel] == digest, f"Source hash mismatch: {rel}"
            if name.startswith(SOURCE_PREFIX):
                safe_write(Path("SOURCE") / name[len(SOURCE_PREFIX):], raw)
            elif name in ("SHA256SUMS.txt", "README_RU.txt"):
                safe_write(Path("docs") / name, raw)
        for line in z.read("SHA256SUMS.txt").decode("utf-8").splitlines():
            digest, name = line.split("  ", 1)
            assert original_hashes[name] == digest, "SHA256SUMS mismatch: " + name
        assert z.read(GAME_PREFIX + "Native/FHDRealClock.dll")[:2] == b"MZ"
        assert z.read(GAME_PREFIX + "ModuleInfo.txt")[:2] == b"\xff\xfe"
        report = {
            "mod": "FHD_RealClock",
            "version": "v1.3 D3D9 TEST",
            "original_zip_sha256": EXPECTED,
            "original_zip_size_bytes": INSTALL.stat().st_size,
            "total_files": len(names),
            "source_files_from_manifest": len(sources),
            "source_manifest_copied": True,
            "runtime_files_including_readme": len(files),
            "all_original_files_hash_verified": True,
            "sources_byte_identical": True,
            "build_and_dll_recompile": "not executed",
            "full_game_runtime": "not tested",
            "test_claims": "from original included manifest, not rerun",
            "original_file_sha256": original_hashes,
        }
        (VERSION / "VERIFICATION.json").write_text(
            json.dumps(report, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    print(f"PASS: {len(names)} files, {len(sources)} source files, SHA256 valid, DLL present")

if __name__ == "__main__":
    publish()
