#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d9.h>
#include "clock_geometry.h"

typedef struct {
    DWORD device_rva;
    DWORD ends[3];
} GameBuild;

static const GameBuild builds[] = {
    {0x47A798, {0x0C5313, 0x0C5408, 0x0C5613}}, // Steam/GOG 2025-10-13
    {0x41302C, {0x3CAF1F, 0x3CB014, 0x3CB1F5}}  // prerelease 2026-10-06
};

static const BYTE original_end[] = {0x8B,0x00,0xFF,0x90,0xA8,0x00,0x00,0x00};
static const BYTE present_args[] = {0x6A,0,0x6A,0,0x6A,0,0x6A,0,0xA1};
static const BYTE present_tail[] = {0x50,0x8B,0x00,0xFF,0x50,0x44};
static BYTE *sites[3];
static BYTE patch[3][sizeof(original_end)];
static ClockMesh mesh;
static SYSTEMTIME clock_time;
static DWORD sampled_at;
static UINT previous_width, previous_height;
static BOOL sampled;

typedef struct { D3DRENDERSTATETYPE state; DWORD value; } RenderState;

static BOOL set_overlay_state(IDirect3DDevice9 *device)
{
    static const RenderState states[] = {
        {D3DRS_ZENABLE, FALSE}, {D3DRS_ZWRITEENABLE, FALSE},
        {D3DRS_STENCILENABLE, FALSE}, {D3DRS_ALPHATESTENABLE, FALSE},
        {D3DRS_ALPHABLENDENABLE, FALSE}, {D3DRS_FOGENABLE, FALSE},
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

static void draw_clock(IDirect3DDevice9 *device)
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
    BOOL rebuild = previous_width != desc.Width || previous_height != desc.Height;
    if (!sampled || now - sampled_at >= 125) {
        SYSTEMTIME local;
        GetLocalTime(&local);
        rebuild |= !sampled || local.wMinute != clock_time.wMinute || local.wHour != clock_time.wHour;
        clock_time = local;
        sampled_at = now;
        sampled = TRUE;
    }
    if (rebuild) {
        clock_mesh(&mesh, desc.Width, desc.Height, clock_time.wHour, clock_time.wMinute);
        previous_width = desc.Width;
        previous_height = desc.Height;
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
            IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_TRIANGLELIST, mesh.count/3,
                                            mesh.vertices, sizeof(ClockVertex));
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
    draw_clock(device);
    return IDirect3DDevice9_EndScene(device);
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

static BOOL write_site(BYTE *site, const BYTE *bytes)
{
    DWORD protect, ignored;
    if (!VirtualProtect(site, sizeof(original_end), PAGE_EXECUTE_READWRITE, &protect)) return FALSE;
    for (unsigned i = 0; i < sizeof(original_end); ++i) site[i] = bytes[i];
    FlushInstructionCache(GetCurrentProcess(), site, sizeof(original_end));
    VirtualProtect(site, sizeof(original_end), protect, &ignored);
    return TRUE;
}

static void remove_sites(void)
{
    for (unsigned i = 0; i < 3; ++i) {
        if (sites[i] && equals(sites[i], patch[i], sizeof(original_end))) {
            if (write_site(sites[i], original_end)) sites[i] = NULL;
        }
    }
}

int FHD_ClockInit(void)
{
    if (sites[0] && sites[1] && sites[2]) return 1;
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
        if (valid != 3) continue;
        for (unsigned i = 0; i < 3; ++i) {
            BYTE *site = base + build->ends[i];
            const DWORD offset = (DWORD)(uintptr_t)draw_then_end_scene - (DWORD)(uintptr_t)(site + 5);
            patch[i][0] = 0xE8;
            for (unsigned byte = 0; byte < 4; ++byte) patch[i][byte+1] = (BYTE)(offset >> (8*byte));
            for (unsigned byte = 5; byte < sizeof(original_end); ++byte) patch[i][byte] = 0x90;
            if (!write_site(site, patch[i])) {
                remove_sites();
                return 0;
            }
            sites[i] = site;
        }
        return 1;
    }
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved)
{
    (void)module;
    if (reason == DLL_PROCESS_DETACH && !reserved) remove_sites();
    return TRUE;
}
