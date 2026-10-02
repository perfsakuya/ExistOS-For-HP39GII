#include "profile.h"
#include "runtime/vapor.h"
#include "VaporDisplay.h"

#include <string.h>

#define SKYOS_LCD_W 256
#define SKYOS_LCD_H 127
#define SKYOS_CELL_W 8
#define SKYOS_CELL_H 8
#define SKYOS_TOP ((SKYOS_LCD_H - VP_GRID_H * SKYOS_CELL_H) / 2)

#if VP_GRID_W * SKYOS_CELL_W != SKYOS_LCD_W
#error Pocket Vapor grid must exactly fill the SkyOS display width
#endif
#if VP_GRID_H * SKYOS_CELL_H > SKYOS_LCD_H || VP_GRID_H > 32
#error Pocket Vapor grid exceeds the SkyOS display
#endif

/* Pocket Vapor owns these grid cells; SkyOS owns the 32 KiB pixel buffer. */
u8 vp_grid_ch[VP_GRID_H][VP_GRID_W];
u8 vp_grid_pal[VP_GRID_H][VP_GRID_W];

static uint8_t *framebuffer;
static u8 ink_gray[SKYOS_VP_STYLE_COUNT];
static u8 paper_gray[SKYOS_VP_STYLE_COUNT];

static u8 gray_from_rgb565(u16 color) {
    const u32 red = (color >> 11) & 31;
    const u32 green = (color >> 5) & 63;
    const u32 blue = color & 31;
    /* Integer luma conversion, rounded to the 8-bit display range. */
    return (u8)((77 * red * 255 / 31 + 150 * green * 255 / 63 +
                 29 * blue * 255 / 31) >> 8);
}

static void render_rows(u32 rows) {
    u8 row, col, py, px;
    for (row = 0; row < VP_GRID_H; ++row) {
        if (!(rows & vp_bit32[row])) continue;
        for (col = 0; col < VP_GRID_W; ++col) {
            u8 ch = vp_grid_ch[row][col];
            u8 style = vp_grid_pal[row][col];
            if (ch < 0x20 || ch > 0x7e) ch = ' ';
            if (style >= SKYOS_VP_STYLE_COUNT) style = 0;
            for (py = 0; py < SKYOS_CELL_H; ++py) {
                const u8 bits = vp_font_tiles[(u16)(ch - 0x20) * 8 + py];
                u8 *dst = framebuffer + (SKYOS_TOP + row * 8 + py) * SKYOS_LCD_W + col * 8;
                for (px = 0; px < SKYOS_CELL_W; ++px) {
                    dst[px] = (bits & (0x80u >> px)) ? ink_gray[style] : paper_gray[style];
                }
            }
        }
    }
}

uint32_t SkyVapor_Init(uint8_t *pixels) {
    u8 style;
    framebuffer = pixels;
    if (!framebuffer) return 0;
    for (style = 0; style < SKYOS_VP_STYLE_COUNT; ++style) {
        const u8 mapped = vp_pal_style[style];
        ink_gray[style] = gray_from_rgb565(vp_ink565[mapped]);
        paper_gray[style] = gray_from_rgb565(vp_paper565[mapped]);
    }
    memset(framebuffer, gray_from_rgb565(vp_backdrop), SKYOS_LCD_W * SKYOS_LCD_H);
    memset(vp_grid_ch, ' ', sizeof(vp_grid_ch));
    memset(vp_grid_pal, 0, sizeof(vp_grid_pal));
    vp_rows_dirty = 0;
    vp_tripwires = 0;
    app_init();
    render_rows((1u << VP_GRID_H) - 1u);
    vp_rows_dirty = 0;
    return (1u << VP_GRID_H) - 1u;
}

uint32_t SkyVapor_Press(uint8_t button) {
    u32 changed;
    if (!framebuffer) return 0;
    app_on_button(button);
    app_flush();
    changed = vp_rows_dirty;
    render_rows(changed);
    vp_rows_dirty = 0;
    return changed;
}
