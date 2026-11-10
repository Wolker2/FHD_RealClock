#include "clock_geometry.h"

typedef struct {
    uint16_t code;
    int advance, left, top;
    unsigned width, height, offset;
} ClockGlyph;

#include "clock_art.h"
#include "menu_font.h"

typedef struct {
    const ClockGlyph *glyphs;
    const uint8_t *alpha;
    unsigned count;
} ClockFont;

static const ClockFont time_font = {time_glyphs, time_alpha, sizeof(time_glyphs)/sizeof(time_glyphs[0])};
static const ClockFont date_font = {date_glyphs, date_alpha, sizeof(date_glyphs)/sizeof(date_glyphs[0])};
static const ClockFont menu_font = {menu_glyphs, menu_alpha, sizeof(menu_glyphs)/sizeof(menu_glyphs[0])};

typedef struct { unsigned width, height; uint32_t *pixels; } Raster;
static uint32_t clock_pixels[CLOCK_PIXELS];
static uint32_t menu_pixels[MENU_WIDTH * MENU_HEIGHT];

static const uint16_t weekday_names[7][2] = {
    {0x412, 0x421}, {0x41F, 0x41D}, {0x412, 0x422}, {0x421, 0x420},
    {0x427, 0x422}, {0x41F, 0x422}, {0x421, 0x411}
};
static const uint32_t palette[COLOR_COUNT] = {
    0, 0x70E684u, 0xFFFFFFu, 0xFF6D70u, 0x7DB4FFu,
    0xFFE16Au, 0xB8C0CAu, 0xFF91C6u, 0xC49CFFu, 0xC48D61u
};
static const uint16_t *const color_names[COLOR_COUNT] = {
    u"По умолчанию", u"Зелёный", u"Белый", u"Красный", u"Синий",
    u"Жёлтый", u"Серый", u"Розовый", u"Фиолетовый", u"Коричневый"
};

static uint32_t text_color(unsigned slot, uint32_t fallback)
{
    return slot ? palette[slot] : fallback;
}

static void two_digits(uint16_t *out, unsigned value)
{
    out[0] = '0' + value / 10 % 10;
    out[1] = '0' + value % 10;
}

static const ClockGlyph *glyph_for(const ClockFont *font, uint16_t code)
{
    for (unsigned i = 0; i < font->count; ++i)
        if (font->glyphs[i].code == code) return &font->glyphs[i];
    return 0;
}

static uint32_t blend(uint32_t under, uint32_t color, unsigned opacity)
{
    if (opacity == 255 || !(under >> 24)) return color | opacity << 24;
    const unsigned preserved = ((under >> 24) * (255u-opacity) + 127u)/255u;
    const unsigned total = opacity + preserved;
    uint32_t result = total << 24;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        const unsigned dst = under >> shift & 255u;
        const unsigned src = color >> shift & 255u;
        result |= ((dst*preserved + src*opacity + total/2)/total) << shift;
    }
    return result;
}

// Align anchors at the left edge (-1), center (0), or right edge (+1).
static void draw_text(Raster raster, const ClockFont *font, const uint16_t *text,
                      int anchor, int align, int top, int height,
                      uint32_t color, uint32_t shadow)
{
    const ClockGlyph *glyphs[32];
    int left = 32767, right = -32767, y_min = 32767, y_max = -32767, x = 0;
    unsigned count = 0;
    for (; count < sizeof(glyphs)/sizeof(glyphs[0]) && text[count]; ++count) {
        const ClockGlyph *g = glyph_for(font, text[count]);
        if (!g) return;
        glyphs[count] = g;
        if (g->width && g->height) {
            const int x0 = x + g->left;
            const int x1 = x0 + (int)g->width;
            const int y1 = g->top + (int)g->height;
            if (x0 < left) left = x0;
            if (x1 > right) right = x1;
            if (g->top < y_min) y_min = g->top;
            if (y1 > y_max) y_max = y1;
        }
        x += g->advance;
    }
    if (right <= left) return;
    int start = anchor - left;
    if (align == 0) start -= (right-left)/2;
    if (align > 0) start -= right-left;
    const int baseline = top + height/2 - (y_max-y_min)/2 - y_min;
    for (int offset = 1; offset >= 0; --offset) {
        x = start;
        for (unsigned i = 0; i < count; ++i) {
            const ClockGlyph *g = glyphs[i];
            for (unsigned row = 0; row < g->height; ++row) {
                for (unsigned col = 0; col < g->width; ++col) {
                    const unsigned px = (unsigned)(x + g->left + (int)col + offset);
                    const unsigned py = (unsigned)(baseline + g->top + (int)row + offset);
                    const unsigned alpha = font->alpha[g->offset + row*g->width + col];
                    if (alpha && px < raster.width && py < raster.height) {
                        uint32_t *pixel = &raster.pixels[py*raster.width + px];
                        *pixel = blend(*pixel, offset ? shadow : color, alpha);
                    }
                }
            }
            x += g->advance;
        }
    }
}

static void fill(Raster raster, unsigned x, unsigned y, unsigned width, unsigned height,
                 uint32_t color)
{
    for (unsigned row = y; row < y+height; ++row)
        for (unsigned col = x; col < x+width; ++col) raster.pixels[row*raster.width + col] = color;
}

static void raster_clock(Raster raster, const ClockReading *reading, const ClockOptions *options)
{
    const unsigned opacity = options->show_frame ? 100-options->transparency : 0;
    for (unsigned i = 0; i < CLOCK_PIXELS; ++i) {
        const unsigned alpha = ((background[i] >> 24) * opacity + 50u)/100u;
        raster.pixels[i] = alpha << 24 | (background[i] & 0x00FFFFFFu);
    }
    uint16_t time[6] = {0, 0, ':', 0, 0, 0};
    two_digits(time, reading->hour);
    two_digits(time + 3, reading->minute);
    draw_text(raster, &time_font, time, CLOCK_WIDTH/2, 0, options->show_date ? 1 : 0,
              options->show_date ? 23 : CLOCK_HEIGHT, text_color(options->time_color, 0xDCF8FFu), 0x001426u);
    if (!options->show_date) return;
    uint16_t date[16] = {weekday_names[reading->weekday][0], weekday_names[reading->weekday][1],
                         ' ', '-', ' ', 0, 0, '.', 0, 0, '.', 0, 0, 0, 0, 0};
    two_digits(date + 5, reading->day);
    two_digits(date + 8, reading->month);
    two_digits(date + 11, reading->year / 100);
    two_digits(date + 13, reading->year);
    draw_text(raster, &date_font, date, CLOCK_WIDTH/2, 0, 21, 14,
              text_color(options->date_color, 0x82D2F0u), 0x000F1Eu);
}

static void menu_text(Raster r, const uint16_t *text, int x, int align, int top,
                      unsigned color)
{
    draw_text(r, &menu_font, text, x, align, top, 16, color, 0x07121Bu);
}

static void toggle(Raster r, unsigned y, unsigned enabled)
{
    fill(r, 237, y, 37, 16, 0xFF456375u);
    fill(r, 238, y+1, 35, 14, enabled ? 0xFF185364u : 0xFF1A2A36u);
    fill(r, enabled ? 257 : 239, y+2, 14, 12, enabled ? 0xFF84E2E8u : 0xFF8196A3u);
}

static void swatches(Raster r, unsigned y, unsigned chosen, uint32_t fallback)
{
    for (unsigned i = 0; i < COLOR_COUNT; ++i) {
        const unsigned x = PALETTE_LEFT + PALETTE_STEP*i;
        fill(r, x, y, PALETTE_WIDTH, PALETTE_HEIGHT, i == chosen ? 0xFFECFBFFu : 0xFF405B70u);
        fill(r, x+2, y+2, PALETTE_WIDTH-4, PALETTE_HEIGHT-4, 0xFF111C27u);
        fill(r, x+3, y+3, PALETTE_WIDTH-6, PALETTE_HEIGHT-6, 0xFF000000u | text_color(i,fallback));
    }
}

static void raster_menu(Raster r, const ClockOptions *o)
{
    fill(r, 0, 0, MENU_WIDTH, MENU_HEIGHT, 0xFF426275u);
    fill(r, 2, 2, MENU_WIDTH-4, MENU_HEIGHT-4, 0xFF101D2Au);
    fill(r, 2, 2, MENU_WIDTH-4, 27, 0xFF1E3A4Au);
    fill(r, 2, 28, MENU_WIDTH-4, 1, 0xFF527C91u);
    fill(r, 261, 6, 21, 18, 0xFF36596Au);
    menu_text(r, u"НАСТРОЙКИ ЧАСОВ", 16, -1, 7, 0xE6F7FFu);
    menu_text(r, u"X", 271, 0, 7, 0xE6F7FFu);
    menu_text(r, u"Показывать дату", 16, -1, 34, 0xD2E7F0u);
    menu_text(r, u"Показывать рамку", 16, -1, 57, 0xD2E7F0u);
    toggle(r, 34, o->show_date);
    toggle(r, 57, o->show_frame);
    menu_text(r, u"Прозрачность рамки", 16, -1, 83, 0xD2E7F0u);
    uint16_t percent[5] = {0,0,0,'%',0};
    two_digits(percent+1,o->transparency);
    percent[0] = '0' + o->transparency/100;
    menu_text(r, percent+(o->transparency < 10 ? 2 : o->transparency < 100 ? 1 : 0),
              274, 1, 83, 0x82DCE8u);
    const unsigned knob = SLIDER_LEFT + ((SLIDER_RIGHT-SLIDER_LEFT)*o->transparency+50)/100;
    fill(r, SLIDER_LEFT, 111, SLIDER_RIGHT-SLIDER_LEFT+1, 4, 0xFF304B5Du);
    fill(r, SLIDER_LEFT, 111, knob-SLIDER_LEFT+1, 4, 0xFF5799ADu);
    fill(r, knob-3, 106, 7, 14, 0xFF9CE6EBu);
    menu_text(r, u"Цвет цифр", 16, -1, 123, 0xD2E7F0u);
    menu_text(r, color_names[o->time_color], 274, 1, 123, 0x82DCE8u);
    swatches(r,TIME_PALETTE_Y,o->time_color,0xDCF8FFu);
    fill(r,16,171,MENU_WIDTH-32,1,0xFF263D4Eu);
    menu_text(r, u"Цвет даты", 16, -1, 174, 0xD2E7F0u);
    menu_text(r, color_names[o->date_color], 274, 1, 174, 0x82DCE8u);
    swatches(r,DATE_PALETTE_Y,o->date_color,0x82D2F0u);
}

ClockLayout clock_layout(unsigned width, unsigned height)
{
    if (width < CLOCK_WIDTH || height < CLOCK_TOP + CLOCK_HEIGHT) return (ClockLayout){0};
    float scale = height / 1080.0f;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;
    if (CLOCK_WIDTH * scale > width) scale = width / (float)CLOCK_WIDTH;
    return (ClockLayout){(width-CLOCK_WIDTH*scale)*0.5f, CLOCK_TOP, scale};
}

ClockLayout clock_menu_layout(unsigned width, unsigned height)
{
    const ClockLayout clock = clock_layout(width,height);
    if (!clock.scale) return (ClockLayout){0};
    const float top = CLOCK_TOP + CLOCK_HEIGHT*clock.scale + 5;
    float scale = clock.scale;
    if (MENU_WIDTH*scale > width-8) scale = (width-8)/(float)MENU_WIDTH;
    if (top+MENU_HEIGHT*scale > height-4) scale = (height-4-top)/MENU_HEIGHT;
    if (scale < 0.5f) return (ClockLayout){0};
    return (ClockLayout){(width-MENU_WIDTH*scale)*0.5f,top,scale};
}

static void vertex(ClockMesh *mesh, ClockLayout place, unsigned x, unsigned y, uint32_t color)
{
    ClockVertex *v = &mesh->vertices[mesh->count++];
    v->x = place.left + x * place.scale - 0.5f;
    v->y = place.top + y * place.scale - 0.5f;
    v->z = 0;
    v->rhw = 1;
    v->color = color;
}

static void mesh_raster(ClockMesh *mesh, ClockLayout place, Raster r)
{
    // Merge equal pixels into disjoint rectangles, preserving straight alpha.
    for (unsigned y = 0; y < r.height; ++y) {
        for (unsigned x = 0; x < r.width;) {
            const uint32_t color = r.pixels[y*r.width + x];
            if (!(color >> 24)) {
                ++x;
                continue;
            }
            unsigned right = x + 1, bottom = y + 1;
            while (right < r.width && r.pixels[y*r.width + right] == color) ++right;
            while (bottom < r.height) {
                unsigned col = x;
                while (col < right && r.pixels[bottom*r.width + col] == color) ++col;
                if (col < right) break;
                ++bottom;
            }
            for (unsigned row = y+1; row < bottom; ++row)
                for (unsigned col = x; col < right; ++col) r.pixels[row*r.width + col] = 0;
            if (mesh->count > CLOCK_MAX_VERTICES-6) return;
            vertex(mesh, place, x, y, color);
            vertex(mesh, place, right, y, color);
            vertex(mesh, place, right, bottom, color);
            vertex(mesh, place, x, y, color);
            vertex(mesh, place, right, bottom, color);
            vertex(mesh, place, x, bottom, color);
            x = right;
        }
    }
}

void clock_mesh(ClockMesh *mesh, unsigned width, unsigned height,
                const ClockReading *reading, const ClockOptions *options, int menu_open)
{
    mesh->count = 0;
    const ClockLayout clock = clock_layout(width,height);
    if (!clock.scale || reading->hour >= 24 || reading->minute >= 60 ||
        reading->weekday >= 7 || reading->month < 1 || reading->month > 12 ||
        reading->day < 1 || reading->day > 31 || reading->year > 9999) return;
    const Raster face = {CLOCK_WIDTH,CLOCK_HEIGHT,clock_pixels};
    raster_clock(face,reading,options);
    mesh_raster(mesh,clock,face);
    if (!menu_open) return;
    const ClockLayout place = clock_menu_layout(width,height);
    if (!place.scale) return;
    const Raster menu = {MENU_WIDTH,MENU_HEIGHT,menu_pixels};
    raster_menu(menu,options);
    mesh_raster(mesh,place,menu);
}
