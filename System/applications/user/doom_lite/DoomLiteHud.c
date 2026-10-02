/* Portable Game HUD; shares no RTOS or display queue state. */
#include <string.h>
#include "DoomLiteHud.h"

static const uint8_t glyph_digits[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7},
    {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7},
    {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7},
    {7, 5, 7, 1, 7},
};

static const uint8_t glyph_letters[26][5] = {
    {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3},
    {6, 5, 5, 5, 6}, {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4},
    {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5}, {7, 2, 2, 2, 7},
    {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7},
    {5, 7, 7, 5, 5}, {5, 7, 7, 7, 5}, {7, 5, 5, 5, 7},
    {6, 5, 6, 4, 4}, {7, 5, 5, 7, 1}, {6, 5, 6, 5, 5},
    {3, 4, 7, 1, 6}, {7, 2, 2, 2, 2}, {5, 5, 5, 5, 7},
    {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5},
    {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7},
};

static uint8_t glyph_row(char character, unsigned row) {
    if (character >= '0' && character <= '9')
        return glyph_digits[character - '0'][row];
    if (character >= 'A' && character <= 'Z')
        return glyph_letters[character - 'A'][row];
    if (character == ':') return row == 1u || row == 3u ? 2u : 0u;
    return 0u;
}

static void draw_text(uint8_t *pixels, unsigned x, unsigned y,
                      const char *message, unsigned scale) {
    for (; *message; ++message, x += 4u * scale) {
        if (x + 3u * scale > DOOM_GAME_LCD_W || y + 5u * scale > DOOM_GAME_LCD_H)
            break;
        for (unsigned row = 0; row < 5u; ++row) {
            const uint8_t bits = glyph_row(*message, row);
            for (unsigned col = 0; col < 3u; ++col) {
                if (!(bits & (4u >> col))) continue;
                for (unsigned sy = 0; sy < scale; ++sy)
                    for (unsigned sx = 0; sx < scale; ++sx)
                        pixels[(y + row * scale + sy) * DOOM_GAME_LCD_W +
                               x + col * scale + sx] = 16u;
            }
        }
    }
}

static void draw_number(uint8_t *pixels, unsigned x, unsigned y,
                        unsigned number, unsigned places, unsigned scale) {
    unsigned divisor = places == 3u ? 100u : places == 2u ? 10u : 1u;
    while (places--) {
        const char digit[2] = {(char)('0' + (number / divisor) % 10u), 0};
        draw_text(pixels, x, y, digit, scale);
        x += 4u * scale;
        divisor /= 10u;
    }
}

static void draw_end_panel(uint8_t *pixels, const char *title,
                           unsigned title_x) {
    const unsigned left = 64u, top = 43u, width = 128u, height = 42u;
    for (unsigned y = top; y < top + height; ++y) {
        uint8_t *row = pixels + y * DOOM_GAME_LCD_W + left;
        memset(row, y == top || y == top + height - 1u ? 20 : 233, width);
        row[0] = row[width - 1u] = 20u;
    }
    draw_text(pixels, title_x, 52u, title, 2u);
    draw_text(pixels, 100u, 69u, "F6 EXIT", 2u);
}

void DoomLite_DrawGameHud(uint8_t *pixels, const DoomLiteGame *game,
                          const char *message, unsigned map_zoom) {
    if (!pixels || !game) return;
    memset(pixels, 237, DOOM_GAME_HUD_HEIGHT * DOOM_GAME_LCD_W);
    draw_text(pixels, 2u, 3u, "HP", 2u);
    draw_number(pixels, 22u, 3u, game->health, 3u, 2u);
    draw_text(pixels, 54u, 3u, "AM", 2u);
    draw_number(pixels, 74u, 3u, game->ammo, 3u, 2u);
    draw_text(pixels, 106u, 3u, "K", 2u);
    draw_number(pixels, 118u, 3u,
                game->kills > 99u ? 99u : game->kills, 2u, 2u);
    draw_text(pixels, 142u, 3u, "KEY", 2u);
    draw_number(pixels, 172u, 3u, game->blue_key ? 1u : 0u, 1u, 2u);
    if (map_zoom)
        draw_text(pixels, 194u, 3u, map_zoom == 1u ? "F4 1X" : "F4 2X", 2u);
    if (message) {
        const unsigned y = DOOM_GAME_LCD_H - DOOM_GAME_HUD_HEIGHT;
        memset(pixels + y * DOOM_GAME_LCD_W, 237,
               DOOM_GAME_HUD_HEIGHT * DOOM_GAME_LCD_W);
        draw_text(pixels, 4u, y + 3u, message, 2u);
    }
    if (game->completed) draw_end_panel(pixels, "E1M1 CLEAR", 88u);
    else if (!game->health) draw_end_panel(pixels, "GAME OVER", 92u);
}

