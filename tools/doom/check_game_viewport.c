/* Scene/map composition bounds and full-view map coverage, without a device. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../System/applications/user/doom_lite/DoomLiteGame.h"
#include "../../System/applications/user/doom_lite/DoomLiteRender.h"
#include "../../System/applications/user/doom_lite/DoomLiteHud.h"

#define PIXELS (DOOM_GAME_LCD_W * DOOM_GAME_LCD_H)
#define VIEW_PIXELS (DOOM_GAME_LCD_W * DOOM_GAME_VIEW_H)
static uint8_t guarded[PIXELS + 32u], view[VIEW_PIXELS];
static uint8_t *const pixels = guarded + 16u;

static void clear(void) { memset(guarded, 0xa5, sizeof(guarded)); }
static void guards(void) {
    for (unsigned i = 0u; i < 16u; ++i) {
        assert(guarded[i] == 0xa5u);
        assert(guarded[PIXELS + 16u + i] == 0xa5u);
    }
}
static void bar_untouched(void) {
    for (unsigned i = VIEW_PIXELS; i < PIXELS; ++i) assert(pixels[i] == 0xa5u);
}
static void preview(const char *directory, unsigned level, unsigned zoom) {
    if (!directory) return;
    char path[1024];
    const int length = snprintf(path, sizeof(path), "%s/map-e1m%u-%ux.pgm",
                                directory, level + 1u, zoom);
    assert(length > 0 && (unsigned)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P5\n256 127\n255\n");
    assert(fwrite(pixels, 1u, PIXELS, file) == PIXELS);
    assert(!fclose(file));
}

int main(int argc, char **argv) {
    DoomLiteGame game;
    const char *directory = argc > 1 ? argv[1] : NULL;
    for (unsigned level = 0u; level < DOOM_MAP_COUNT; ++level) {
        assert(DoomLiteGame_InitMap(&game, level));
        for (unsigned zoom = 1u; zoom <= 2u; ++zoom) {
            clear();
            DoomLite_RenderGameMap(pixels, &game, zoom);
            bar_untouched();
            memcpy(view, pixels, VIEW_PIXELS);
            DoomLite_DrawGameHud(pixels, &game, NULL, zoom);
            assert(!memcmp(view, pixels, VIEW_PIXELS));
            preview(directory, level, zoom);
            DoomLite_DrawGameHud(pixels, &game, "NEED YELLOW", zoom);
            assert(!memcmp(view, pixels, VIEW_PIXELS));
            guards();
        }
    }

    /* A tall synthetic boundary must cross both scene edges in follow mode.
     * This directly detects a remaining top HUD clip or bottom bar overwrite. */
    const DoomMapLine boundary = {
        .x1 = 0, .y1 = 0, .x2 = 0, .y2 = 6400,
        .front_sector = DOOM_MAP_NO_SECTOR, .back_sector = DOOM_MAP_NO_SECTOR,
    };
    const DoomMap tall = {
        .name = "TEST", .grid_width = 20u, .grid_height = 200u,
        .line_count = 1u, .lines = &boundary,
    };
    memset(&game, 0, sizeof(game));
    game.map = &tall;
    game.x_q8 = 320 * 256;
    game.y_q8 = 1600 * 256;
    clear();
    DoomLite_RenderGameMap(pixels, &game, 2u);
    assert(pixels[108u] == 20u);
    assert(pixels[(DOOM_GAME_VIEW_H - 1u) * DOOM_GAME_LCD_W + 108u] == 20u);
    bar_untouched();
    guards();

    /* The complete tall level fits within the overview, with marker room. */
    clear();
    DoomLite_RenderGameMap(pixels, &game, 1u);
    unsigned first = DOOM_GAME_VIEW_H, last = 0u, rows = 0u;
    for (unsigned y = 0u; y < DOOM_GAME_VIEW_H; ++y)
        for (unsigned x = 0u; x < DOOM_GAME_LCD_W; ++x)
            if (pixels[y * DOOM_GAME_LCD_W + x] == 20u) {
                if (first > y) first = y;
                if (last < y) last = y;
                ++rows;
            }
    assert(first >= 8u && last + 8u < DOOM_GAME_VIEW_H && rows == last - first + 1u);
    bar_untouched();
    guards();
    puts("Game viewport: both maps, top/bottom map edges, full overview, bottom bar and event isolation passed");
    return 0;
}
