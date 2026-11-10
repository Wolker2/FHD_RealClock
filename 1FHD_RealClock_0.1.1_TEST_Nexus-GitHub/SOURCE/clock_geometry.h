#ifndef FHD_CLOCK_GEOMETRY_H
#define FHD_CLOCK_GEOMETRY_H

#include <stdint.h>

enum { CLOCK_WIDTH = 142, CLOCK_HEIGHT = 38, CLOCK_TOP = 3, CLOCK_MAX_VERTICES = 1200 };

typedef struct {
    float x, y, z, rhw;
    uint32_t color;
} ClockVertex;

typedef struct {
    ClockVertex vertices[CLOCK_MAX_VERTICES];
    unsigned count;
    float left, top, scale;
} ClockMesh;

unsigned clock_mesh(ClockMesh *mesh, unsigned width, unsigned height,
                    unsigned hour, unsigned minute);

#endif
