#include "clock_geometry.h"

typedef struct {
    float x, y;
} Point;

static void vertex(ClockMesh *mesh, Point point, uint32_t color)
{
    ClockVertex *v = &mesh->vertices[mesh->count++];
    // D3D9 XYZRHW positions use a half-pixel offset.
    v->x = mesh->left + point.x * mesh->scale - 0.5f;
    v->y = mesh->top + point.y * mesh->scale - 0.5f;
    v->z = 0;
    v->rhw = 1;
    v->color = color;
}

static void polygon(ClockMesh *mesh, const Point *points, unsigned count,
                    float split_y, uint32_t top, uint32_t bottom)
{
    for (unsigned i = 1; i + 1 < count; ++i) {
        vertex(mesh, points[0], points[0].y <= split_y ? top : bottom);
        vertex(mesh, points[i], points[i].y <= split_y ? top : bottom);
        vertex(mesh, points[i + 1], points[i + 1].y <= split_y ? top : bottom);
    }
}

static void box(ClockMesh *mesh, float x, float y, float width, float height,
                uint32_t top, uint32_t bottom)
{
    const Point points[] = {{x,y}, {x+width,y}, {x+width,y+height}, {x,y+height}};
    polygon(mesh, points, 4, y, top, bottom);
}

static void chassis(ClockMesh *mesh, float inset, float top, float bottom,
                    float bevel, uint32_t high, uint32_t low)
{
    const float left = inset;
    const float right = CLOCK_WIDTH - inset;
    const Point outline[] = {
        {left+bevel,top}, {right-bevel,top}, {right,top+bevel},
        {right,bottom-bevel}, {right-bevel,bottom}, {left+bevel,bottom},
        {left,bottom-bevel}, {left,top+bevel}
    };
    polygon(mesh, outline, 8, (top+bottom)*0.5f, high, low);
}

static void segment(ClockMesh *mesh, float x, float y, int vertical,
                    uint32_t high, uint32_t low)
{
    static const Point bevel[] = {{0,1.25f}, {1.25f,0}, {7.75f,0},
                                  {9,1.25f}, {7.75f,2.5f}, {1.25f,2.5f}};
    Point positions[6];
    for (unsigned i = 0; i < 6; ++i) {
        positions[i].x = x + (vertical ? bevel[i].y : bevel[i].x);
        positions[i].y = y + (vertical ? bevel[i].x : bevel[i].y);
    }
    polygon(mesh, positions, 6, y + (vertical ? 4.5f : 1.25f), high, low);
}

static void digit(ClockMesh *mesh, float x, float y, unsigned value)
{
    static const uint8_t bits[] = {0x3F,0x06,0x5B,0x4F,0x66,0x6D,0x7D,0x07,0x7F,0x6F};
    static const Point origins[] = {
        {1.5f,0}, {9.75f,1.5f}, {9.75f,11.25f}, {1.5f,19.25f},
        {0,11.25f}, {0,1.5f}, {1.5f,9.625f}
    };
    for (unsigned i = 0; i < 7; ++i) {
        if (!(bits[value] & (1u << i))) continue;
        const int vertical = i == 1 || i == 2 || i == 4 || i == 5;
        segment(mesh, x+origins[i].x, y+origins[i].y, vertical, 0xFFFCF5C9u, 0xFFE2AF61u);
    }
}

static void separator(ClockMesh *mesh, float x)
{
    box(mesh, x, 13, 2.5f, 2.5f, 0xFFF3DA96u, 0xFFAF7942u);
    box(mesh, x, 22, 2.5f, 2.5f, 0xFFF3DA96u, 0xFFAF7942u);
}

unsigned clock_mesh(ClockMesh *mesh, unsigned width, unsigned height,
                    unsigned hour, unsigned minute)
{
    mesh->count = 0;
    if (width < CLOCK_WIDTH || height < CLOCK_TOP + CLOCK_HEIGHT ||
        hour >= 24 || minute >= 60) return 0;

    float scale = height / 1080.0f;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    if (CLOCK_WIDTH * scale > width) scale = width / (float)CLOCK_WIDTH;
    mesh->scale = scale;
    mesh->left = (width - CLOCK_WIDTH * scale) * 0.5f;
    mesh->top = CLOCK_TOP;

    chassis(mesh, 0, 0, 38, 9, 0xFF101D28u, 0xFF04090Du);
    chassis(mesh, 1, 1, 36, 8, 0xFF667E8Bu, 0xFF233B49u);
    chassis(mesh, 2, 2, 35, 7, 0xFF304D5Cu, 0xFF101D2Bu);
    chassis(mesh, 6, 5, 32, 6, 0xFF10232Au, 0xFF42626Bu);
    chassis(mesh, 7, 6, 31, 5, 0xFF142A34u, 0xFF080F18u);
    box(mesh, 39, 1, CLOCK_WIDTH-78, 1, 0xFF8D9F9Cu, 0xFF657A80u);
    box(mesh, 35, 34, 72, 1, 0xFF5B4630u, 0xFF312C2Au);

    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t high = i == 0 ? 0xFF83DCE2u : 0xFF54767Cu;
        const uint32_t low = i == 0 ? 0xFF327880u : 0xFF27414Cu;
        box(mesh, 15, 12 + 5*i, 9-2*i, 2, high, low);
        box(mesh, CLOCK_WIDTH-24+2*i, 12 + 5*i, 9-2*i, 2, high, low);
    }

    const unsigned values[] = {hour/10, hour%10, minute/10, minute%10};
    const unsigned x[] = {35, 51, 78, 94};
    for (unsigned i = 0; i < 4; ++i) digit(mesh, (float)x[i], 8, values[i]);
    separator(mesh, 69.5f);
    return mesh->count;
}
