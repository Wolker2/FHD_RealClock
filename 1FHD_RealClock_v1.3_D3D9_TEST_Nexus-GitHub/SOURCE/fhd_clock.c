#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include "clock_geometry.h"
#include "clock_settings.h"

typedef struct {
    DWORD device_rva;
    DWORD ends[3];
    DWORD peek_rva;
    DWORD peek_iat_rva;
    DWORD loop_stack_rva;
    DWORD push_loop_rva;
    DWORD add_rect_rva;
} GameBuild;

static const GameBuild builds[] = {
    {0x47A798, {0x0C5313, 0x0C5408, 0x0C5613}, 0x0C4DF4, 0x48DD04, 0x47A7FC, 0x0C92F4, 0x0C8AB4},
    {0x41302C, {0x3CAF1F, 0x3CB014, 0x3CB1F5}, 0x3CAA3B, 0x41ED04, 0x40B710, 0x0C09D4, 0x082F64}
};

static const BYTE original_end[] = {0x8B,0x00,0xFF,0x90,0xA8,0x00,0x00,0x00};
static const BYTE present_args[] = {0x6A,0,0x6A,0,0x6A,0,0x6A,0,0xA1};
static const BYTE present_tail[] = {0x50,0x8B,0x00,0xFF,0x50,0x44};
static BYTE *sites[3];
static BYTE patch[3][sizeof(original_end)];
static BYTE *peek_site;
static BYTE peek_saved[5], peek_patch[5];
typedef BOOL (WINAPI *PeekMessageFn)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessageFn original_peek;
static HMODULE own_module;
static ClockMesh mesh;
static ClockReading reading;
static DWORD sampled_at, drawn_options_revision;
static UINT previous_width, previous_height;
static BOOL sampled;
static IDirect3DDevice9 *input_device;
static HWND input_window;
static BYTE **message_loop_stack;
static const BYTE *add_update_rect;
static RECT face_dirty, menu_dirty;
static BOOL redraw_face, redraw_menu;
static unsigned menu_cleanup_frames;

typedef struct { D3DRENDERSTATETYPE state; DWORD value; } RenderState;

static BOOL set_overlay_state(IDirect3DDevice9 *device)
{
    static const RenderState states[] = {
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE},
        {D3DRS_STENCILENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, TRUE}, {D3DRS_SRCBLEND, D3DBLEND_SRCALPHA},
        {D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA}, {D3DRS_BLENDOP, D3DBLENDOP_ADD},
        {D3DRS_SEPARATEALPHABLENDENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
        {D3DRS_LIGHTING, FALSE}, {D3DRS_CULLMODE, D3DCULL_NONE},
        {D3DRS_FILLMODE, D3DFILL_SOLID}, {D3DRS_SHADEMODE, D3DSHADE_GOURAUD},
        {D3DRS_SCISSORTESTENABLE, FALSE}, {D3DRS_CLIPPLANEENABLE, 0},
        {D3DRS_SRGBWRITEENABLE, FALSE}, {D3DRS_COLORWRITEENABLE, 0x0F}
    };
    if (FAILED(IDirect3DDevice9_SetVertexShader(device, NULL)) ||
        FAILED(IDirect3DDevice9_SetPixelShader(device, NULL)) ||
        FAILED(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_DIFFUSE)) ||
        FAILED(IDirect3DDevice9_SetTexture(device, 0, NULL)) ||
        FAILED(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLOROP, D3DTOP_SELECTARG1)) ||
        FAILED(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_COLORARG1, D3DTA_DIFFUSE)) ||
        FAILED(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1)) ||
        FAILED(IDirect3DDevice9_SetTextureStageState(device, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE)) ||
        FAILED(IDirect3DDevice9_SetTextureStageState(device, 1, D3DTSS_COLOROP, D3DTOP_DISABLE))) return FALSE;
    for (unsigned i = 0; i < sizeof(states)/sizeof(states[0]); ++i)
        if (FAILED(IDirect3DDevice9_SetRenderState(device, states[i].state, states[i].value)))
            return FALSE;
    return TRUE;
}

static RECT layout_bounds(ClockLayout place, unsigned width, unsigned height, unsigned limit_width,
                          unsigned limit_height)
{
    const LONG right = (LONG)(place.left+width*place.scale+1);
    const LONG bottom = (LONG)(place.top+height*place.scale+1);
    return (RECT){(LONG)place.left,(LONG)place.top,
                  right < (LONG)limit_width ? right : (LONG)limit_width,
                  bottom < (LONG)limit_height ? bottom : (LONG)limit_height};
}

static void schedule_overlay_redraw(UINT width, UINT height)
{
    face_dirty = layout_bounds(clock_layout(width,height),CLOCK_WIDTH,CLOCK_HEIGHT,width,height);
    redraw_face = TRUE;
    if (clock_menu_open) menu_cleanup_frames = 3;
    else if (menu_cleanup_frames) --menu_cleanup_frames;
    const ClockLayout menu = clock_menu_layout(width,height);
    redraw_menu = menu_cleanup_frames && menu.scale;
    if (redraw_menu) menu_dirty = layout_bounds(menu,MENU_WIDTH,MENU_HEIGHT,width,height);
}

static void queue_rect(BYTE *list, const RECT *rect)
{
    register BYTE *self __asm__("eax") = list;
    register const RECT *value __asm__("edx") = rect;
    __asm__ volatile("call *%2" : "+a"(self), "+d"(value) : "m"(add_update_rect) : "ecx","memory","cc");
}

static void refresh_overlay_background(void)
{
    if (!redraw_face || !message_loop_stack) return;
    redraw_face = FALSE;
    BYTE *stack = *message_loop_stack;
    if (!stack) return;
    // Delphi TList: item pointer array, count, then capacity.
    BYTE **items = *(BYTE ***)(stack+4);
    const int count = *(int *)(stack+8);
    if (!items || count < 1 || count > 64 || count > *(int *)(stack+12)) return;
    BYTE *loop = items[count-1];
    if (!loop) return;
    BYTE *rects = *(BYTE **)(loop+0x5C);
    if (!rects) return;
    queue_rect(rects,&face_dirty);
    if (redraw_menu) queue_rect(rects,&menu_dirty);
}

static void draw_clock(IDirect3DDevice9 *device, BOOL hardware)
{
    IDirect3DSurface9 *back = NULL, *target = NULL;
    D3DSURFACE_DESC desc;
    if (FAILED(IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &back))) return;
    const BOOL is_backbuffer =
        SUCCEEDED(IDirect3DDevice9_GetRenderTarget(device, 0, &target)) &&
        target == back && SUCCEEDED(IDirect3DSurface9_GetDesc(back, &desc));
    if (target) IDirect3DSurface9_Release(target);
    IDirect3DSurface9_Release(back);
    if (!is_backbuffer) return;

    const DWORD now = GetTickCount();
    BOOL rebuild = previous_width != desc.Width || previous_height != desc.Height ||
                   drawn_options_revision != clock_options_revision;
    if (!sampled || now - sampled_at >= 125) {
        SYSTEMTIME local;
        GetLocalTime(&local);
        rebuild |= !sampled || local.wMinute != reading.minute || local.wHour != reading.hour ||
                   local.wYear != reading.year || local.wMonth != reading.month ||
                   local.wDay != reading.day || local.wDayOfWeek != reading.weekday;
        reading = (ClockReading){local.wHour, local.wMinute, local.wYear, local.wMonth,
                                 local.wDay, local.wDayOfWeek};
        sampled_at = now;
        sampled = TRUE;
    }
    if (rebuild) {
        clock_mesh(&mesh, desc.Width, desc.Height, &reading, &clock_options, clock_menu_open);
        previous_width = desc.Width;
        previous_height = desc.Height;
        drawn_options_revision = clock_options_revision;
    }
    if (!mesh.count) return;

    IDirect3DStateBlock9 *state = NULL;
    D3DVIEWPORT9 prior;
    IDirect3DVertexBuffer9 *stream = NULL;
    UINT stream_offset = 0, stream_stride = 0;
    if (FAILED(IDirect3DDevice9_GetViewport(device, &prior)) ||
        FAILED(IDirect3DDevice9_GetStreamSource(device, 0, &stream, &stream_offset, &stream_stride)))
        return;
    if (SUCCEEDED(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &state))) {
        const D3DVIEWPORT9 full = {0, 0, desc.Width, desc.Height, 0, 1};
        if (SUCCEEDED(IDirect3DDevice9_SetViewport(device, &full)) && set_overlay_state(device)) {
            if (SUCCEEDED(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, mesh.count/3,
                                                           mesh.vertices, sizeof(ClockVertex)))) {
                if (input_device != device || !input_window) {
                    D3DDEVICE_CREATION_PARAMETERS parameters;
                    if (SUCCEEDED(IDirect3DDevice9_GetCreationParameters(device, &parameters))) {
                        input_device = device;
                        input_window = parameters.hFocusWindow;
                    }
                }
                clock_settings_view(input_window, desc.Width, desc.Height);
                if (hardware) schedule_overlay_redraw(desc.Width, desc.Height);
                else redraw_face = FALSE;
            }
        }
        IDirect3DStateBlock9_Apply(state);
        IDirect3DStateBlock9_Release(state);
        IDirect3DDevice9_SetViewport(device, &prior);
        IDirect3DDevice9_SetStreamSource(device, 0, stream, stream_offset, stream_stride);
    }
    if (stream) IDirect3DVertexBuffer9_Release(stream);
}

static HRESULT WINAPI draw_then_end_scene(IDirect3DDevice9 *device)
{
    // The first call site is the persistent hardware backbuffer path.
    const BOOL hardware = __builtin_return_address(0) == sites[0]+5;
    draw_clock(device,hardware);
    return IDirect3DDevice9_EndScene(device);
}

static BOOL WINAPI peek_clock_message(LPMSG msg, HWND window, UINT low, UINT high, UINT remove)
{
    const BOOL result = original_peek(msg, window, low, high, remove);
    if (!result && (remove & PM_REMOVE)) refresh_overlay_background();
    if (result && (remove & PM_REMOVE) && clock_settings_message(msg)) {
        // The game also forwards queued messages directly to the active form.
        // Clear the destination so a handled click cannot reach that route.
        msg->hwnd = NULL;
        msg->message = WM_NULL;
        msg->wParam = 0;
        msg->lParam = 0;
    }
    return result;
}

static BOOL equals(const BYTE *left, const BYTE *right, unsigned count)
{
    for (unsigned i = 0; i < count; ++i)
        if (left[i] != right[i]) return FALSE;
    return TRUE;
}

static BOOL expected_site(const BYTE *end, DWORD device_address)
{
    const BYTE *after = end + sizeof(original_end);
    return end[-6] == 0xA1 && end[-1] == 0x50 &&
           equals(end-5, (const BYTE *)&device_address, sizeof(device_address)) &&
           equals(end, original_end, sizeof(original_end)) &&
           equals(after, present_args, sizeof(present_args)) &&
           equals(after+sizeof(present_args), (const BYTE *)&device_address, sizeof(device_address)) &&
           equals(after+sizeof(present_args)+sizeof(device_address), present_tail, sizeof(present_tail));
}

static BOOL write_site(BYTE *site, const BYTE *bytes, unsigned length)
{
    DWORD protect, ignored;
    if (!VirtualProtect(site, length, PAGE_EXECUTE_READWRITE, &protect)) return FALSE;
    for (unsigned i = 0; i < length; ++i) site[i] = bytes[i];
    FlushInstructionCache(GetCurrentProcess(), site, length);
    VirtualProtect(site, length, protect, &ignored);
    return TRUE;
}

static void remove_sites(void)
{
    if (peek_site && equals(peek_site, peek_patch, sizeof(peek_patch)) &&
        write_site(peek_site, peek_saved, sizeof(peek_saved))) peek_site = NULL;
    for (unsigned i = 0; i < 3; ++i) {
        if (sites[i] && equals(sites[i], patch[i], sizeof(original_end))) {
            if (write_site(sites[i], original_end, sizeof(original_end))) sites[i] = NULL;
        }
    }
}

static void relative_call(BYTE *out, const BYTE *site, uintptr_t destination)
{
    const DWORD offset = (DWORD)destination - (DWORD)(uintptr_t)(site+5);
    out[0] = 0xE8;
    for (unsigned i = 0; i < 4; ++i) out[i+1] = (BYTE)(offset >> (8*i));
}

static BOOL expected_peek(const BYTE *base, const GameBuild *build)
{
    const BYTE *call = base + build->peek_rva;
    const BYTE *thunk = base + 0x86FC;
    static const BYTE tail[] = {0x83,0xF8,0x01,0x1B,0xC0,0x40,0x3C,0x01,0x0F,0x84};
    static const BYTE old_args[] = {0x6A,1,0x6A,0,0x6A,0,0x6A,0,0x8D,0x45,0xD4,0x50};
    static const BYTE new_args[] = {0x6A,1,0x6A,0,0x6A,0,0x6A,0,0x56};
    BYTE expected[5];
    relative_call(expected,call,(uintptr_t)thunk);
    const DWORD iat = (DWORD)(uintptr_t)(base + build->peek_iat_rva);
    return equals(call,expected,sizeof(expected)) && equals(call+5,tail,sizeof(tail)) &&
           thunk[0] == 0xFF && thunk[1] == 0x25 &&
           equals(thunk+2,(const BYTE *)&iat,sizeof(iat)) &&
           (build->peek_rva == 0x0C4DF4 ? equals(call-sizeof(old_args),old_args,sizeof(old_args))
                                        : equals(call-sizeof(new_args),new_args,sizeof(new_args)));
}

static BOOL expected_redraw(const BYTE *base, const GameBuild *build,
                            const IMAGE_SECTION_HEADER *code)
{
    static const BYTE old_push[] = {0x55,0x8B,0xEC,0x51,0x89,0x45,0xFC,0x83,0x3D};
    static const BYTE new_push[] = {0x53,0x8B,0xD8,0x83,0x3D};
    static const BYTE old_rect[] = {0x55,0x8B,0xEC,0x83,0xC4,0xDC,0x56,0x57,0x8B,0xF2,
                                   0x8D,0x7D,0xDC,0xA5,0xA5,0xA5,0xA5};
    static const BYTE new_rect[] = {0x53,0x56,0x57,0x55,0x83,0xC4,0xEC,0x8B,0xF2,0x8D,0x7C,
                                   0x24,0x04,0xA5,0xA5,0xA5,0xA5};
    if (build->push_loop_rva < code->VirtualAddress || build->add_rect_rva < code->VirtualAddress ||
        build->push_loop_rva+16 > code->VirtualAddress+code->Misc.VirtualSize ||
        build->add_rect_rva+17 > code->VirtualAddress+code->Misc.VirtualSize) return FALSE;
    const BYTE *push = base+build->push_loop_rva;
    const BYTE *rect = base+build->add_rect_rva;
    const DWORD stack = (DWORD)(uintptr_t)(base+build->loop_stack_rva);
    const BOOL old = build->peek_rva == 0x0C4DF4;
    return old ? equals(push,old_push,sizeof(old_push)) &&
                 equals(push+sizeof(old_push),(const BYTE *)&stack,sizeof(stack)) &&
                 equals(rect,old_rect,sizeof(old_rect))
               : equals(push,new_push,sizeof(new_push)) &&
                 equals(push+sizeof(new_push),(const BYTE *)&stack,sizeof(stack)) &&
                 equals(rect,new_rect,sizeof(new_rect));
}

int FHD_ClockInit(void)
{
    if (sites[0] && sites[1] && sites[2] && peek_site) return 1;
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    if (!base) return 0;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    const IMAGE_NT_HEADERS32 *pe = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER *section = IMAGE_FIRST_SECTION(pe);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || pe->Signature != IMAGE_NT_SIGNATURE ||
        pe->FileHeader.Machine != IMAGE_FILE_MACHINE_I386 ||
        !pe->FileHeader.NumberOfSections || !(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) return 0;

    for (unsigned b = 0; b < sizeof(builds)/sizeof(builds[0]); ++b) {
        const GameBuild *build = &builds[b];
        unsigned valid = 0;
        for (unsigned i = 0; i < 3; ++i) {
            const DWORD rva = build->ends[i];
            if (rva >= section->VirtualAddress + 6 &&
                rva + 27 <= section->VirtualAddress + section->Misc.VirtualSize &&
                expected_site(base + rva, (DWORD)(uintptr_t)(base + build->device_rva)))
                ++valid;
        }
        if (valid != 3 || build->peek_rva < section->VirtualAddress + 12 ||
            build->peek_rva + 15 > section->VirtualAddress + section->Misc.VirtualSize ||
            !expected_peek(base,build) || !expected_redraw(base,build,section) ||
            build->loop_stack_rva+4 > pe->OptionalHeader.SizeOfImage) continue;
        message_loop_stack = (BYTE **)(base+build->loop_stack_rva);
        add_update_rect = base+build->add_rect_rva;
        for (unsigned i = 0; i < 3; ++i) {
            BYTE *site = base + build->ends[i];
            relative_call(patch[i],site,(uintptr_t)draw_then_end_scene);
            for (unsigned byte = 5; byte < sizeof(original_end); ++byte) patch[i][byte] = 0x90;
            if (!write_site(site, patch[i], sizeof(original_end))) {
                remove_sites();
                return 0;
            }
            sites[i] = site;
        }
        BYTE *call = base + build->peek_rva;
        for (unsigned i = 0; i < sizeof(peek_saved); ++i) peek_saved[i] = call[i];
        original_peek = (PeekMessageFn)(base + 0x86FC);
        relative_call(peek_patch,call,(uintptr_t)peek_clock_message);
        if (!write_site(call,peek_patch,sizeof(peek_patch))) {
            remove_sites();
            return 0;
        }
        peek_site = call;
        clock_settings_load(own_module);
        return 1;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    if (reason == DLL_PROCESS_ATTACH) own_module = module;
    if (reason == DLL_PROCESS_DETACH && !reserved) remove_sites();
    return TRUE;
}
