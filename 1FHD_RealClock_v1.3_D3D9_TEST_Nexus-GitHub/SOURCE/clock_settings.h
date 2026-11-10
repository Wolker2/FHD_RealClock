#ifndef FHD_CLOCK_SETTINGS_H
#define FHD_CLOCK_SETTINGS_H

#include <windows.h>
#include "clock_geometry.h"

extern ClockOptions clock_options;
extern DWORD clock_options_revision;
extern BOOL clock_menu_open;

void clock_settings_load(HMODULE module);
void clock_settings_view(HWND window, unsigned width, unsigned height);
BOOL clock_settings_message(MSG *msg);

#endif
