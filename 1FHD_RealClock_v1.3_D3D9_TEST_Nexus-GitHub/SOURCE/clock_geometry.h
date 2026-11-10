#ifndef FHD_CLOCK_GEOMETRY_H
#define FHD_CLOCK_GEOMETRY_H

#include <stdint.h>

enum { CLOCK_WIDTH = 220, CLOCK_HEIGHT = 38, CLOCK_TOP = 3 };
enum { MENU_WIDTH = 290, MENU_HEIGHT = 220, COLOR_COUNT = 10 };
enum { CLOCK_PIXELS = CLOCK_WIDTH * CLOCK_HEIGHT, CLOCK_MAX_VERTICES = CLOCK_PIXELS * 6 };
enum { SLIDER_LEFT = 16, SLIDER_RIGHT = 274, SLIDER_TOP = 104, SLIDER_BOTTOM = 121 };
enum { PALETTE_LEFT = 16, PALETTE_STEP = 26, PALETTE_WIDTH = 22, PALETTE_HEIGHT = 19 };
enum { TIME_PALETTE_Y = 144, DATE_PALETTE_Y = 192 };

typedef struct {
    float x, y, z, rhw;
    uint32_t color;
} ClockVertex;

typedef struct {
    ClockVertex vertices[CLOCK_MAX_VERTICES];
    unsigned count;
} ClockMesh;

typedef struct {
    float left, top, scale;
} ClockLayout;

typedef struct {
    unsigned hour, minute, year, month, day, weekday;
} ClockReading;

typedef struct {
    unsigned show_date, show_frame, transparency, time_color, date_color;
} ClockOptions;

ClockLayout clock_layout(unsigned width, unsigned height);
ClockLayout clock_menu_layout(unsigned width, unsigned height);
void clock_mesh(ClockMesh *mesh, unsigned width, unsigned height,
                const ClockReading *reading, const ClockOptions *options, int menu_open);

#endif
