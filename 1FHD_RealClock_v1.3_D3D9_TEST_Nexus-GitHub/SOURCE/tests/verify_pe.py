"""Run the shipped PE instructions against game code and a D3D9 test device."""
import hashlib
import json
from pathlib import Path
import struct
import sys

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_EDX

STACK_TOP = 0x11100000
STUB = 0x10000000
DATA = 0x12000000
DEVICE, SURFACE, STATE, STREAM, OFFSCREEN = (DATA + x for x in range(0, 0x500, 0x100))
DWORD = struct.Struct("<I")
HWND = 0x334455
MSG_ADDR = DATA + 0x3000
DLL_PATH = r"C:\Rangers\Mods\FHD_MODS\FHD_RealClock\Native\FHDRealClock.dll"
INI_PATH = DLL_PATH.replace(".dll", ".ini")
BUILDS = (
    ("Steam/GOG 2025-10-13", "83300344af802bc51e64389c58f047e5afdf195c133048098be3881fae29ed98",
     0x47A798, (0x0C5313, 0x0C5408, 0x0C5613), 0x0C4DF4, 0x48DD04, 0x47A7FC, 0x0C8AB4),
    ("Prerelease 2026-10-06", "e43a4b15503c705eebe2d8ce5778c2a2fb090db515a3fd86d319e5c5aa692ebf",
     0x41302C, (0x3CAF1F, 0x3CB014, 0x3CB1F5), 0x3CAA3B, 0x41ED04, 0x40B710, 0x082F64),
)


class Sandbox:
    def __init__(self, game, dll, build, rebase=False, profile=None):
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
        self.peek = self.game_base + build[4]
        self.loop_stack = self.game_base + build[6]
        self.add_rect = self.game_base + build[7]
        self.queued = []
        self.original_peek = bytes(self.uc.mem_read(self.peek, 5))
        self.profile = dict(profile or {})
        self.pending_msg = None
        self.client_size = (1920,1080)
        self.write32(self.game_base + build[5], self.stub("PeekMessageW", 5, self.peek_message))
        self.originals = [bytes(self.uc.mem_read(address, 8)) for address in self.addresses]
        self.write32(self.game_base + build[2], DEVICE)
        self.clock = (17, 42, 8)
        self.calendar = (2026, 11, 2, 10)
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

    def wstr(self, ptr):
        words = bytearray()
        while True:
            char = self.uc.mem_read(ptr,2)
            if char == b"\0\0": return words.decode("utf-16le")
            words.extend(char)
            ptr += 2

    def peek_message(self, address, hwnd, low, high, flags):
        assert hwnd == low == high == 0
        if self.pending_msg is None: return 0
        self.uc.mem_write(address, struct.pack("<8I", *self.pending_msg, 0, 0, 0, 0))
        if flags & 1: self.pending_msg = None
        return 1

    def setup_imports(self, dll):
        def local_time(address):
            self.time_reads += 1
            self.uc.mem_write(address, struct.pack("<8H", *self.calendar, *self.clock, 0))
            return 0

        def protect(address, size, new_flags, old_flags):
            if new_flags == 0x40 and address == self.fail_write:
                return 0
            self.write32(old_flags, 0x20 if new_flags == 0x40 else 0x40)
            return 1

        def module_file(handle, out, capacity):
            assert handle == self.dll_base and capacity >= len(DLL_PATH)+1
            self.uc.mem_write(out, (DLL_PATH+"\0").encode("utf-16le"))
            return len(DLL_PATH)

        def profile_get(section, key, default, path):
            assert self.wstr(section) == "Clock" and self.wstr(path) == INI_PATH
            return self.profile.get(self.wstr(key), default)

        def profile_write(section, values, path):
            assert self.wstr(section) == "Clock" and self.wstr(path) == INI_PATH
            while value := self.wstr(values):
                key, number = value.split("=")
                self.profile[key] = int(number)
                values += (len(value)+1)*2
            return 1

        def client_rect(window, out):
            assert window == HWND
            self.uc.mem_write(out, struct.pack("<4i",0,0,*self.client_size))
            return 1

        imports = {
            b"GetModuleHandleW": (1, lambda name: self.game_base if name == 0 else 0),
            b"GetTickCount": (0, lambda: self.ticks),
            b"GetLocalTime": (1, local_time),
            b"GetCurrentProcess": (0, lambda: 0xFFFFFFFF),
            b"VirtualProtect": (4, protect),
            b"FlushInstructionCache": (3, lambda *args: 1),
            b"GetModuleFileNameW": (3, module_file),
            b"GetPrivateProfileIntW": (4, profile_get),
            b"WritePrivateProfileSectionW": (3, profile_write),
            b"GetClientRect": (2, client_rect),
        }
        preferred = dll.OPTIONAL_HEADER.ImageBase
        for library in dll.DIRECTORY_ENTRY_IMPORT:
            assert library.dll.lower() in (b"kernel32.dll",b"user32.dll")
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
            assert this == DEVICE and kind == 4 and stride == 20 and 0 < count <= 220 * 38 * 2
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
        def creation(this, address):
            self.uc.mem_write(address, struct.pack("<4I",0,1,HWND,0))
            return 0

        self.methods(DEVICE, {
            9: ("GetCreationParameters", 2, creation),
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
        self.uc.emu_start(address, STUB, count=5000000)
        assert self.uc.reg_read(UC_X86_REG_EIP) == STUB
        return self.uc.reg_read(UC_X86_REG_EAX)

    def enable_redraw_probe(self):
        stack, items, loop, rects = (DATA+n for n in (0x4000,0x4100,0x4200,0x4300))
        self.write32(self.loop_stack,stack)
        self.uc.mem_write(stack+4,struct.pack("<3I",items,1,4))
        self.write32(items,loop)
        self.write32(loop+0x5c,rects)

        def record(uc, address, size, _):
            assert uc.reg_read(UC_X86_REG_EAX) == rects
            location = uc.reg_read(UC_X86_REG_EDX)
            self.queued.append(struct.unpack("<4i",uc.mem_read(location,16)))
            esp = uc.reg_read(UC_X86_REG_ESP)
            target = struct.unpack("<I",uc.mem_read(esp,4))[0]
            uc.reg_write(UC_X86_REG_ESP,esp+4)
            uc.reg_write(UC_X86_REG_EIP,target)
        self.uc.hook_add(UC_HOOK_CODE,record,begin=self.add_rect,end=self.add_rect)

    def pump_empty(self):
        self.pending_msg = None
        self.queued.clear()
        stack = STACK_TOP-0x100
        self.uc.reg_write(UC_X86_REG_ESP, stack)
        self.uc.mem_write(stack,struct.pack("<5I",MSG_ADDR,0,0,0,1))
        self.uc.emu_start(self.peek,self.peek+5,count=100000)
        assert self.uc.reg_read(UC_X86_REG_EIP)==self.peek+5
        assert self.uc.reg_read(UC_X86_REG_ESP)==stack+20
        assert self.uc.reg_read(UC_X86_REG_EAX)==0
        return self.queued[:]

    def message(self, msg, x=0, y=0, *, wparam=0, window=HWND, flags=1, consumed=False):
        lparam = (x & 0xffff) | ((y & 0xffff) << 16)
        self.pending_msg = (window,msg,wparam,lparam)
        stack = STACK_TOP-0x100
        self.uc.reg_write(UC_X86_REG_ESP, stack)
        self.uc.mem_write(stack,struct.pack("<5I",MSG_ADDR,0,0,0,flags))
        self.uc.emu_start(self.peek,self.peek+5,count=100000)
        assert self.uc.reg_read(UC_X86_REG_EIP)==self.peek+5
        assert self.uc.reg_read(UC_X86_REG_ESP)==stack+20
        assert self.uc.reg_read(UC_X86_REG_EAX)==1
        got = struct.unpack("<4I",self.uc.mem_read(MSG_ADDR,16))
        if consumed: assert got==(0,0,0,0), (hex(msg),got)
        else: assert got[:2]==(window,msg), (hex(msg),got)
        return got

    def click(self,x,y,consumed=True):
        self.message(0x201,x,y,wparam=1,consumed=consumed)
        self.message(0x202,x,y,consumed=consumed)

    def scene(self, site, should_draw=True):
        address = self.addresses[site]
        stack = STACK_TOP - 0x100
        self.events.clear()
        self.vertices = []
        self.uc.reg_write(UC_X86_REG_ESP, stack)
        self.uc.emu_start(address - 6, address + 8, count=5000000)
        assert self.uc.reg_read(UC_X86_REG_EIP) == address + 8
        assert self.uc.reg_read(UC_X86_REG_ESP) == stack
        assert self.uc.reg_read(UC_X86_REG_EAX) == 0x2468
        assert self.events.count(("end",)) == 1 and self.events[-1] == ("end",)
        assert bool(self.vertices) == should_draw
        if should_draw:
            assert [e[0] for e in self.events] == ["viewport", "draw", "restore", "viewport", "stream", "end"]
        assert self.viewport == (6, 8, 100, 100, 0.0, 1.0)
        return self.vertices


def verify_settings(box):
    box.width, box.height = 1920,1080
    box.client_size = (1920,1080)
    box.enable_redraw_probe()
    closed = box.scene(0)
    assert box.pump_empty() == [(850,3,1071,42)]
    assert box.pump_empty() == []
    box.click(500,500,False)
    box.message(0x201,960,19,window=0x554433)
    box.message(0x201,960,19,flags=0)
    assert box.scene(0) == closed
    box.click(960,19)
    opened = box.scene(0)
    assert max(v[1] for v in opened) == 265.5
    assert box.pump_empty() == [(850,3,1071,42),(815,46,1106,267)]
    left,top = 815,46
    box.click(left+55,top+42)
    box.click(left+55,top+65)
    assert box.profile["ShowDate"] == box.profile["ShowFrame"] == 0
    box.message(0x201,left+16,top+112,wparam=1,consumed=True)
    moved = box.message(0x200,left+145,top+112,wparam=1)
    assert moved[2] == 0
    box.message(0x202,left+145,top+112,consumed=True)
    assert box.profile["Transparency"]==50
    for slot in range(10):
        box.click(left+19+26*slot,top+150)
        box.click(left+19+26*(9-slot),top+199)
        assert box.profile["TimeColor"]==slot and box.profile["DateColor"]==9-slot
    box.click(left+19+26*4,top+150)
    box.click(left+19+26*7,top+199)
    customized = box.scene(0)
    assert any(v[4] == 0xFF7DB4FF and v[1] < 41 for v in customized)
    box.message(0x100,wparam=27,consumed=True)
    box.message(0x101,wparam=27,consumed=True)
    digits = box.scene(0)
    assert max(v[1] for v in digits) < 40
    assert len(box.pump_empty()) == 2
    box.scene(0)
    assert len(box.pump_empty()) == 2
    box.scene(0)
    assert box.pump_empty() == [(850,3,1071,42)]
    box.scene(1)
    assert box.pump_empty() == []
    assert min(v[0] for v in digits) > 900 and max(v[0] for v in digits) < 1020
    assert not any(v[4] in (0xFF82D2F0,0xFFFF91C6) for v in digits)
    # Mouse input is expressed in client pixels, even if the backbuffer is larger.
    box.client_size = (960,540)
    box.click(480,9)
    box.click((left+55)//2,(top+42)//2)
    box.click((left+55)//2,(top+65)//2)
    box.click(300,200,False)
    tinted = box.scene(0)
    assert any(v[4] == 0xFFFF91C6 and 26 < v[1] < 41 for v in tinted)
    assert any(v[4] >> 24 == 128 and v[0] < 900 for v in tinted)
    assert box.profile == {"ShowDate":1,"ShowFrame":1,"Transparency":50,"TimeColor":4,"DateColor":7}
    return opened,digits,dict(box.profile)


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
            assert box.call(box.detach,box.dll_base,1,0) == 1
            box.fail_write = box.addresses[1]
            assert box.call(box.install) == 0
            assert [bytes(box.uc.mem_read(a, 8)) for a in box.addresses] == box.originals
            assert bytes(box.uc.mem_read(box.peek,5)) == box.original_peek
            box.fail_write = box.peek
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
                assert any(v[4] == 0xFFDCF8FF for v in verts)
                assert any(v[4] == 0xFF82D2F0 for v in verts)
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
            box.calendar = (2026, 11, 3, 11)
            box.ticks += 1000
            midnight = box.scene(1)
            assert midnight != before_midnight and box.time_reads == 5
            box.calendar = (2026, 12, 5, 11)
            box.ticks += 1000
            assert box.scene(1) != midnight and box.time_reads == 6
            opened, digits, profile = verify_settings(box)
            if game_path == games[0] and not rebase:
                examples.append({"width":1920,"height":1080,"menu":True,"vertices":opened})
                examples.append({"width":1920,"height":1080,"digits":True,"vertices":digits})
                restart = Sandbox(pefile.PE(data=game_bytes),pefile.PE(data=dll_bytes),build,
                                  profile={**profile,"TimeColor":99})
                restart.call(restart.detach,restart.dll_base,1,0)
                assert restart.call(restart.install)==1
                loaded = restart.scene(0)
                assert any(v[4] == 0xFFDCF8FF for v in loaded)
                assert any(v[4] == 0xFFFF91C6 for v in loaded)
                assert any(v[4] >> 24 == 128 and v[0] < 900 for v in loaded)
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
            assert bytes(box.uc.mem_read(box.peek,5)) == box.original_peek
            box.uc.mem_write(box.peek,b"\x90")
            assert box.call(box.install) == 0
            assert [bytes(box.uc.mem_read(a,8)) for a in box.addresses] == box.originals
            box.uc.mem_write(box.peek,box.original_peek)
            box.uc.mem_write(box.addresses[1], b"\x90")
            assert box.call(box.install) == 0
            assert bytes(box.uc.mem_read(box.addresses[0], 8)) == box.originals[0]
            assert bytes(box.uc.mem_read(box.addresses[2], 8)) == box.originals[2]
        print(f"PASS {build[0]} / 3 draw paths, mouse controls, colors, transparency, region repaint, settings persistence and rollback")
    Path(preview_data).write_text(json.dumps(examples), encoding="utf-8")


if __name__ == "__main__":
    verify(sys.argv[1], sys.argv[2:4], sys.argv[4])
