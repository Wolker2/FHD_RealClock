#define WIN32_LEAN_AND_MEAN
#include "clock_settings.h"
#include <windowsx.h>

ClockOptions clock_options = {1, 1, 0, 0, 0};
DWORD clock_options_revision;
BOOL clock_menu_open;

static WCHAR settings_path[1024];
static const WCHAR section[] = L"Clock";
static const WCHAR *const keys[] = {L"ShowDate", L"ShowFrame", L"Transparency",
                                    L"TimeColor", L"DateColor"};
static HWND game_window;
static unsigned view_width, view_height;
static BOOL left_owned, right_owned, escape_owned, sliding, dirty;

void clock_settings_load(HMODULE module)
{
    if (!module) return;
    const DWORD capacity = sizeof(settings_path)/sizeof(settings_path[0]);
    DWORD length = GetModuleFileNameW(module, settings_path, capacity);
    if (!length || length >= capacity) {
        settings_path[0] = 0;
        return;
    }
    while (length && settings_path[length-1] != L'\\' && settings_path[length-1] != L'/') --length;
    static const WCHAR name[] = L"FHDRealClock.ini";
    if (!length || length + sizeof(name)/sizeof(name[0]) > capacity) {
        settings_path[0] = 0;
        return;
    }
    for (unsigned i = 0; i < sizeof(name)/sizeof(name[0]); ++i) settings_path[length+i] = name[i];
    unsigned *const values[] = {&clock_options.show_date,&clock_options.show_frame,
                                &clock_options.transparency,&clock_options.time_color,
                                &clock_options.date_color};
    const unsigned max[] = {1,1,100,COLOR_COUNT-1,COLOR_COUNT-1};
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        const unsigned value = GetPrivateProfileIntW(section,keys[i],(int)*values[i],settings_path);
        if (value <= max[i]) *values[i] = value;
    }
    ++clock_options_revision;
}

static void save_options(void)
{
    if (!dirty || !settings_path[0]) return;
    WCHAR data[96], *at = data;
    const unsigned values[] = {clock_options.show_date,clock_options.show_frame,
                               clock_options.transparency,clock_options.time_color,
                               clock_options.date_color};
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); ++i) {
        for (const WCHAR *c = keys[i]; *c; ++c) *at++ = *c;
        *at++ = L'=';
        const unsigned value = values[i];
        if (value >= 100) *at++ = L'0' + value/100;
        if (value >= 10) *at++ = L'0' + value/10%10;
        *at++ = L'0' + value%10;
        *at++ = 0;
    }
    *at = 0;
    if (WritePrivateProfileSectionW(section,data,settings_path)) dirty = FALSE;
}

static void set_option(unsigned *option, unsigned value)
{
    if (*option == value) return;
    *option = value;
    dirty = TRUE;
    ++clock_options_revision;
}

static void set_menu(BOOL open)
{
    if (clock_menu_open == open) return;
    clock_menu_open = open;
    ++clock_options_revision;
}

void clock_settings_view(HWND window, unsigned width, unsigned height)
{
    if (game_window != window) {
        set_menu(FALSE);
        save_options();
        left_owned = right_owned = escape_owned = sliding = FALSE;
    }
    game_window = window;
    view_width = width;
    view_height = height;
}

static BOOL inside(float x, float y, int left, int top, int right, int bottom)
{
    return x >= left && x < right && y >= top && y < bottom;
}

static void transparency(float x)
{
    unsigned value = 0;
    if (x >= SLIDER_RIGHT) value = 100;
    else if (x > SLIDER_LEFT)
        value = (unsigned)((x-SLIDER_LEFT)*100/(SLIDER_RIGHT-SLIDER_LEFT)+0.5f);
    set_option(&clock_options.transparency,value);
}

static void menu_click(float x, float y)
{
    if (inside(x,y,258,3,285,28)) set_menu(FALSE);
    else if (inside(x,y,12,31,278,54)) set_option(&clock_options.show_date,!clock_options.show_date);
    else if (inside(x,y,12,54,278,78)) set_option(&clock_options.show_frame,!clock_options.show_frame);
    else if (inside(x,y,SLIDER_LEFT-4,SLIDER_TOP,SLIDER_RIGHT+5,SLIDER_BOTTOM)) {
        sliding = TRUE;
        transparency(x);
        return;
    } else if (x >= PALETTE_LEFT) {
        const unsigned offset = (unsigned)(x-PALETTE_LEFT);
        const unsigned slot = offset/PALETTE_STEP;
        if (slot < COLOR_COUNT && offset%PALETTE_STEP < PALETTE_WIDTH) {
            if (y >= TIME_PALETTE_Y && y < TIME_PALETTE_Y+PALETTE_HEIGHT)
                set_option(&clock_options.time_color,slot);
            if (y >= DATE_PALETTE_Y && y < DATE_PALETTE_Y+PALETTE_HEIGHT)
                set_option(&clock_options.date_color,slot);
        }
    }
    save_options();
}

BOOL clock_settings_message(MSG *msg)
{
    if (!game_window || msg->hwnd != game_window) return FALSE;
    if (msg->message == WM_KEYDOWN && msg->wParam == VK_ESCAPE &&
        (clock_menu_open || escape_owned)) {
        set_menu(FALSE);
        escape_owned = TRUE;
        save_options();
        return TRUE;
    }
    if (msg->message == WM_KEYUP && msg->wParam == VK_ESCAPE && escape_owned) {
        escape_owned = FALSE;
        return TRUE;
    }
    if (msg->message == WM_LBUTTONUP && left_owned) {
        left_owned = sliding = FALSE;
        save_options();
        return TRUE;
    }
    if (msg->message == WM_RBUTTONUP && right_owned) {
        right_owned = FALSE;
        return TRUE;
    }
    if (msg->message == WM_MOUSEMOVE && left_owned && !(msg->wParam & MK_LBUTTON)) {
        left_owned = sliding = FALSE;
        save_options();
    }
    const BOOL left = msg->message == WM_LBUTTONDOWN || msg->message == WM_LBUTTONDBLCLK;
    const BOOL right = msg->message == WM_RBUTTONDOWN || msg->message == WM_RBUTTONDBLCLK;
    const BOOL drag = msg->message == WM_MOUSEMOVE && sliding && (msg->wParam & MK_LBUTTON);
    if (!left && !right && !drag) return FALSE;
    RECT rect;
    if (!GetClientRect(game_window,&rect) || rect.right <= 0 || rect.bottom <= 0) return FALSE;
    const float x = (float)GET_X_LPARAM(msg->lParam)*view_width/rect.right;
    const float y = (float)GET_Y_LPARAM(msg->lParam)*view_height/rect.bottom;
    const ClockLayout menu = clock_menu_layout(view_width,view_height);
    if (!menu.scale) return FALSE;
    const float mx = (x-menu.left)/menu.scale, my = (y-menu.top)/menu.scale;
    if (drag) {
        transparency(mx);
        // Keep the in-game cursor moving while the menu owns the drag.
        msg->wParam &= ~MK_LBUTTON;
        return FALSE;
    }
    const BOOL over_menu = clock_menu_open && inside(mx,my,0,0,MENU_WIDTH,MENU_HEIGHT);
    if (right) {
        right_owned = over_menu;
        return over_menu;
    }
    if (left_owned) {
        left_owned = sliding = FALSE;
        save_options();
    }
    const ClockLayout clock = clock_layout(view_width,view_height);
    if (inside((x-clock.left)/clock.scale,(y-clock.top)/clock.scale,0,0,CLOCK_WIDTH,CLOCK_HEIGHT)) {
        set_menu(!clock_menu_open);
        left_owned = TRUE;
        save_options();
        return TRUE;
    }
    if (over_menu) {
        left_owned = TRUE;
        menu_click(mx,my);
        return TRUE;
    }
    set_menu(FALSE);
    return FALSE;
}
