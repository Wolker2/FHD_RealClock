"""Run the shipped PE instructions against game code and a D3D9 test device."""
import hashlib
import json
from pathlib import Path
import struct
import sys

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP

STACK_TOP = 0x11100000
STUB = 0x10000000
DATA = 0x12000000
DEVICE, SURFACE, STATE, STREAM, OFFSCREEN = (DATA + x for x in range(0, 0x500, 0x100))
DWORD = struct.Struct("<I")
BUILDS = (
    ("Steam/GOG 2025-10-13", "83300344af802bc51e64389c58f047e5afdf195c133048098be3881fae29ed98",
     0x47A798, (0x0C5313, 0x0C5408, 0x0C5613)),
    ("Prerelease 2026-10-06", "e43a4b15503c705eebe2d8ce5778c2a2fb090db515a3fd86d319e5c5aa692ebf",
     0x41302C, (0x3CAF1F, 0x3CB014, 0x3CB1F5)),
)


class Sandbox:
    def __init__(self, game, dll, build, rebase=False):
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.uc.mem_map(STACK_TOP - 0x10000, 0x10000)
        self.uc.mem_map(STUB, 0x10000)
        self.uc.mem_map(DATA, 0x10000)
        self.stubs = {}
        self.game_base = self.map_pe(game, 0x2000000 if rebase else None)
        self.dll_base = self.map_pe(dll, 0x3000000 if rebase else None)
        self.install = self.dll_base + next(x.address for x in dll.DIRECTORY_ENTRY_EXPORT.symbols
                                           if x.name == b"FHD_ClockInit")
        self.detach = self.dll_base + dll.OPTIONAL_HEADER.AddressOfEntryPoint
        self.addresses = [self.game_base + rva for rva in build[3]]
        self.originals = [bytes(self.uc.mem_read(address, 8)) for address in self.addresses]
        self.write32(self.game_base + build[2], DEVICE)
        self.clock = (17, 42, 8)
        self.ticks = 1000
        self.width, self.height = 1920, 1080
        self.target = SURFACE
        self.fail_write = None
        self.fail_state = False
        self.fail_render_state = False
        self.viewport = (6, 8, 100, 100, 0.0, 1.0)
        self.events = []
        self.vertices = []
        self.time_reads = 0
        self.setup_imports(dll)
        self.setup_device()
        self.uc.hook_add(UC_HOOK_CODE, self.dispatch, begin=STUB, end=STUB + 0xFFFF)

    def map_pe(self, pe, new_base):
        base = new_base or pe.OPTIONAL_HEADER.ImageBase
        image = pe.get_memory_mapped_image(ImageBase=new_base)
        size = (max(len(image), pe.OPTIONAL_HEADER.SizeOfImage) + 0xFFF) & ~0xFFF
        self.uc.mem_map(base, size)
        self.uc.mem_write(base, image)
        return base

    def write32(self, address, value):
        self.uc.mem_write(address, DWORD.pack(value))

    def stub(self, name, count, callback):
        address = STUB + 16 + len(self.stubs) * 16
        self.stubs[address] = (name, count, callback)
        self.uc.mem_write(address, b"\xC2" + struct.pack("<H", count * 4))
        return address

    def dispatch(self, uc, address, size, _):
        name, count, call = self.stubs[address]
        stack = uc.reg_read(UC_X86_REG_ESP)
        args = struct.unpack("<" + "I" * count, uc.mem_read(stack + 4, 4 * count))
        result = call(*args)
        uc.reg_write(UC_X86_REG_EAX, result & 0xFFFFFFFF)

    def setup_imports(self, dll):
        def local_time(address):
            self.time_reads += 1
            self.uc.mem_write(address, struct.pack("<8H", 2026, 11, 2, 10, *self.clock, 0))
            return 0

        def protect(address, size, new_flags, old_flags):
            if new_flags == 0x40 and address == self.fail_write:
                return 0
            self.write32(old_flags, 0x20 if new_flags == 0x40 else 0x40)
            return 1

        imports = {
            b"GetModuleHandleW": (1, lambda name: self.game_base if name == 0 else 0),
            b"GetTickCount": (0, lambda: self.ticks),
            b"GetLocalTime": (1, local_time),
            b"GetCurrentProcess": (0, lambda: 0xFFFFFFFF),
            b"VirtualProtect": (4, protect),
            b"FlushInstructionCache": (3, lambda *args: 1),
        }
        preferred = dll.OPTIONAL_HEADER.ImageBase
        for library in dll.DIRECTORY_ENTRY_IMPORT:
            assert library.dll.lower() == b"kernel32.dll"
            for entry in library.imports:
                count, callback = imports[entry.name]
                address = self.stub(entry.name.decode(), count, callback)
                self.write32(self.dll_base + entry.address - preferred, address)

    def methods(self, obj, methods):
        table = obj + 0x800
        self.write32(obj, table)
        for slot, (name, count, callback) in methods.items():
            self.write32(table + slot * 4, self.stub(name, count, callback))

    def setup_device(self):
        def out(address, value):
            self.write32(address, value)
            return 0

        def desc(this, address):
            self.uc.mem_write(address, struct.pack("<8I", 21, 1, 0, 0, 0, 0, self.width, self.height))
            return 0

        def get_viewport(this, address):
            self.uc.mem_write(address, struct.pack("<4I2f", *self.viewport))
            return 0

        def set_viewport(this, address):
            self.viewport = struct.unpack("<4I2f", self.uc.mem_read(address, 24))
            self.events.append(("viewport", self.viewport))
            return 0

        def get_stream(this, index, obj, offset, stride):
            assert index == 0
            self.write32(obj, STREAM)
            self.write32(offset, 4)
            self.write32(stride, 20)
            return 0

        def set_stream(this, index, obj, offset, stride):
            assert (index, obj, offset, stride) == (0, STREAM, 4, 20)
            self.events.append(("stream",))
            return 0

        def draw(this, kind, count, data, stride):
            assert this == DEVICE and kind == 4 and stride == 20 and 0 < count < 400
            assert self.viewport == (0, 0, self.width, self.height, 0, 1)
            self.vertices = list(struct.iter_unpack("<4fI", self.uc.mem_read(data, count * 3 * stride)))
            self.events.append(("draw",))
            return 0

        def event(name, result=0):
            def add(*args):
                self.events.append((name,))
                return result
            return add

        noop = lambda *args: 0
        release = ("Release", 1, noop)
        self.methods(DEVICE, {
            18: ("GetBackBuffer", 5, lambda this, swap, index, kind, address: out(address, SURFACE)),
            38: ("GetRenderTarget", 3, lambda this, index, address: out(address, self.target)),
            42: ("EndScene", 1, event("end", 0x2468)),
            47: ("SetViewport", 2, set_viewport),
            48: ("GetViewport", 2, get_viewport),
            57: ("SetRenderState", 3, lambda *args: -1 if self.fail_render_state else 0),
            59: ("CreateStateBlock", 3, lambda this, mode, address: -1 if self.fail_state else out(address, STATE)),
            65: ("SetTexture", 3, noop),
            67: ("SetTextureStageState", 4, noop),
            83: ("DrawPrimitiveUP", 5, draw),
            89: ("SetFVF", 2, noop),
            92: ("SetVertexShader", 2, noop),
            100: ("SetStreamSource", 5, set_stream),
            101: ("GetStreamSource", 5, get_stream),
            107: ("SetPixelShader", 2, noop),
        })
        self.methods(SURFACE, {2: release, 12: ("GetDesc", 2, desc)})
        self.methods(OFFSCREEN, {2: release})
        self.methods(STATE, {2: release, 5: ("Apply", 1, event("restore"))})
        self.methods(STREAM, {2: release})

    def call(self, address, *args):
        stack = STACK_TOP - 0x100
        self.uc.reg_write(UC_X86_REG_ESP, stack)
        self.uc.mem_write(stack, struct.pack("<" + "I" * (len(args)+1), STUB, *args))
        self.uc.emu_start(address, STUB, count=100000)
        assert self.uc.reg_read(UC_X86_REG_EIP) == STUB
        return self.uc.reg_read(UC_X86_REG_EAX)

    def scene(self, site, should_draw=True):
        address = self.addresses[site]
        stack = STACK_TOP - 0x100
        self.events.clear()
        self.vertices = []
        self.uc.reg_write(UC_X86_REG_ESP, stack)
        self.uc.emu_start(address - 6, address + 8, count=100000)
        assert self.uc.reg_read(UC_X86_REG_EIP) == address + 8
        assert self.uc.reg_read(UC_X86_REG_ESP) == stack
        assert self.uc.reg_read(UC_X86_REG_EAX) == 0x2468
        assert self.events.count(("end",)) == 1 and self.events[-1] == ("end",)
        assert bool(self.vertices) == should_draw
        if should_draw:
            assert [e[0] for e in self.events] == ["viewport", "draw", "restore", "viewport", "stream", "end"]
        assert self.viewport == (6, 8, 100, 100, 0.0, 1.0)
        return self.vertices


def verify(dll_path, games, preview_data):
    dll_bytes = Path(dll_path).read_bytes()
    dll = pefile.PE(data=dll_bytes)
    assert dll.FILE_HEADER.Machine == 0x14C and dll.is_dll()
    assert [s.name for s in dll.DIRECTORY_ENTRY_EXPORT.symbols] == [b"FHD_ClockInit"]
    examples = []
    for game_path, build in zip(games, BUILDS, strict=True):
        assert hashlib.sha256(Path(game_path).read_bytes()).hexdigest() == build[1]
        game_bytes = Path(game_path).read_bytes()
        for rebase in (False, True):
            box = Sandbox(pefile.PE(data=game_bytes), pefile.PE(data=dll_bytes), build, rebase)
            box.fail_write = box.addresses[1]
            assert box.call(box.install) == 0
            assert [bytes(box.uc.mem_read(a, 8)) for a in box.addresses] == box.originals
            box.fail_write = None
            assert box.call(box.install) == 1
            assert box.call(box.install) == 1
            for i, (width, height) in enumerate(((1280, 960), (1920, 1080), (2560, 1080),
                                                  (3440, 1440), (3840, 2160), (1365, 768))):
                box.width, box.height = width, height
                verts = box.scene(i % 3)
                left, right = min(v[0] for v in verts), max(v[0] for v in verts)
                assert abs(left + right + 1 - width) < 0.001
                assert abs(min(v[1] for v in verts) - 2.5) < 0.001
                assert sum(v[4] in (0xFFF3DA96, 0xFFAF7942) for v in verts) == 12
                assert sum(v[4] in (0xFFFCF5C9, 0xFFE2AF61) for v in verts) == 14 * 12
                assert all(0 <= v[0] < width and 0 <= v[1] < height for v in verts)
                if game_path == games[0] and not rebase:
                    examples.append({"width": width, "height": height, "vertices": verts})
            assert box.time_reads == 1
            before = box.vertices
            box.clock = (17, 42, 59)
            box.ticks += 124
            assert box.scene(0) == before and box.time_reads == 1
            box.ticks += 1
            assert box.scene(0) == before and box.time_reads == 2
            box.clock = (17, 43, 0)
            box.ticks += 1000
            assert box.scene(0) != before and box.time_reads == 3
            box.clock = (23, 59, 59)
            box.ticks += 1000
            before_midnight = box.scene(1)
            box.clock = (0, 0, 0)
            box.ticks += 1000
            assert box.scene(1) != before_midnight and box.time_reads == 5
            box.target = OFFSCREEN
            box.scene(2, False)
            box.target = SURFACE
            box.fail_state = True
            box.scene(2, False)
            box.fail_state = False
            box.fail_render_state = True
            box.scene(2, False)
            assert [e[0] for e in box.events] == ["viewport", "restore", "viewport", "stream", "end"]
            assert box.call(box.detach, box.dll_base, 0, 0) == 1
            assert [bytes(box.uc.mem_read(a, 8)) for a in box.addresses] == box.originals
            box.uc.mem_write(box.addresses[1], b"\x90")
            assert box.call(box.install) == 0
            assert bytes(box.uc.mem_read(box.addresses[0], 8)) == box.originals[0]
            assert bytes(box.uc.mem_read(box.addresses[2], 8)) == box.originals[2]
        print(f"PASS {build[0]} / 3 draw paths, relocation, clock, resize and rollback")
    Path(preview_data).write_text(json.dumps(examples), encoding="utf-8")


if __name__ == "__main__":
    verify(sys.argv[1], sys.argv[2:4], sys.argv[4])
