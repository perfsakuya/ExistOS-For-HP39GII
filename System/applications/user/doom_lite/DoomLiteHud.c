/* Portable Game HUD; shares no RTOS or display queue state. */
#include <string.h>
#include "DoomLiteHud.h"
#include "E1M1Ui.h"

#if DOOM_GAME_VIEW_H != E1M1_UI_VIEW_H
#error "Regenerate the pre-sized UI assets after changing the Game viewport"
#endif

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

static void draw_text_color(uint8_t *pixels, unsigned x, unsigned y,
                            const char *message, unsigned scale, uint8_t ink) {
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
                               x + col * scale + sx] = ink;
            }
        }
    }
}

static void draw_text(uint8_t *pixels, unsigned x, unsigned y,
                      const char *message, unsigned scale) {
    draw_text_color(pixels, x, y, message, scale, 16u);
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
                           unsigned title_x, const char *action) {
    const unsigned left = 48u, top = 32u, width = 160u, height = 60u;
    for (unsigned y = top; y < top + height; ++y) {
        uint8_t *row = pixels + y * DOOM_GAME_LCD_W + left;
        memset(row, y == top || y == top + height - 1u ? 20 : 233, width);
        row[0] = row[width - 1u] = 20u;
    }
    draw_text(pixels, title_x, 41u, title, 2u);
    draw_text(pixels, 96u, 59u, action, 2u);
    draw_text(pixels, 100u, 77u, "F6 EXIT", 2u);
}

static void draw_map_hud(uint8_t *pixels, const DoomLiteGame *game,
                         unsigned map_zoom) {
    memset(pixels, 237, DOOM_GAME_HUD_HEIGHT * DOOM_GAME_LCD_W);
    draw_text(pixels, 2u, 3u, game->map ? game->map->name : "E1M1", 2u);
    draw_text(pixels, 42u, 3u, "HP", 2u);
    draw_number(pixels, 62u, 3u, game->health, 3u, 2u);
    draw_text(pixels, 92u, 3u, "AM", 2u);
    draw_number(pixels, 112u, 3u, game->ammo, 3u, 2u);
    draw_text(pixels, 142u, 3u, "K", 2u);
    draw_number(pixels, 154u, 3u,
                game->kills > 99u ? 99u : game->kills, 2u, 2u);
    draw_text_color(pixels, 180u, 3u, "B", 2u, game->blue_key ? 16u : 180u);
    draw_text_color(pixels, 192u, 3u, "R", 2u, game->red_key ? 16u : 180u);
    draw_text_color(pixels, 204u, 3u, "Y", 2u, game->yellow_key ? 16u : 180u);
    if (map_zoom)
        draw_text(pixels, 224u, 3u, map_zoom == 1u ? "1X" : "2X", 2u);
}

static void draw_patch(uint8_t *pixels, const E1M1UiPatch *patch,
                       int x, int y, unsigned clip_bottom) {
    const int x0 = x < 0 ? 0 : x;
    const int x1 = x + patch->width > (int)DOOM_GAME_LCD_W
        ? (int)DOOM_GAME_LCD_W : x + patch->width;
    const int y0 = y < 0 ? 0 : y;
    const int y1 = y + patch->height > (int)clip_bottom
        ? (int)clip_bottom : y + patch->height;
    for (int sy = y0; sy < y1; ++sy) {
        unsigned source = (unsigned)(sy - y) * patch->width + (unsigned)(x0 - x);
        uint8_t *row = pixels + sy * DOOM_GAME_LCD_W;
        for (int sx = x0; sx < x1; ++sx, ++source) {
            const unsigned code = (patch->pixels[source >> 1] >>
                                   ((source & 1u) * 4u)) & 15u;
            if (code) row[sx] = e1m1_ui_gray[code];
        }
    }
}

/* Right-aligned original digit patches; omit unnecessary leading zeroes. */
static void draw_patch_number(uint8_t *pixels, unsigned right, unsigned y,
                              unsigned value, int small) {
    const E1M1UiPatch *const *digits = small ? ui_small_digits : ui_digits;
    do {
        const E1M1UiPatch *digit = digits[value % 10u];
        right -= digit->width;
        draw_patch(pixels, digit, (int)right, (int)y, DOOM_GAME_LCD_H);
        value /= 10u;
    } while (value);
}

static void draw_status_bar(uint8_t *pixels, const DoomLiteGame *game) {
    const unsigned top = DOOM_GAME_VIEW_H;
    memset(pixels + top * DOOM_GAME_LCD_W, 237,
           DOOM_GAME_STATUS_HEIGHT * DOOM_GAME_LCD_W);
    draw_patch(pixels, &ui_stbar, 0, (int)top, DOOM_GAME_LCD_H);
    draw_patch_number(pixels, 34u, top + 4u, game->ammo, 0);
    draw_patch_number(pixels, 71u, top + 4u, game->health, 0);
    draw_patch(pixels, &ui_sttprcnt, 72, (int)top + 4, DOOM_GAME_LCD_H);
    draw_patch_number(pixels, 111u, top + 4u,
                       game->kills > 99u ? 99u : game->kills, 0);
    const unsigned pain = (100u - (game->health > 100u ? 100u : game->health)) * 5u / 101u;
    const E1M1UiPatch *face = ui_faces[game->health ? pain : 5u];
    draw_patch(pixels, face, 117, (int)top + 1, DOOM_GAME_LCD_H);
    draw_patch_number(pixels, 170u, top + 4u, game->armor, 0);
    draw_patch(pixels, &ui_sttprcnt, 171, (int)top + 4, DOOM_GAME_LCD_H);
    const uint8_t keys[3] = {game->blue_key, game->red_key, game->yellow_key};
    const char *key_names[3] = {"B", "R", "Y"};
    for (unsigned k = 0u; k < 3u; ++k) {
        const unsigned ky = top + 1u + k * 8u;
        if (keys[k]) draw_patch(pixels, &ui_stkeys0, 194, (int)ky, DOOM_GAME_LCD_H);
        /* Letters distinguish key colors on the monochrome LCD. */
        draw_text_color(pixels, 207u, ky + 1u, key_names[k], 1u,
                        keys[k] ? 16u : 168u);
    }
    /* The pistol is the playable weapon. Other pickups retain their real
     * inventory for the next level; the small rows display that inventory. */
    draw_patch_number(pixels, 230u, top + 1u, game->ammo, 1);
    draw_patch_number(pixels, 250u, top + 1u, game->backpack ? 400u : 200u, 1);
    const uint16_t ammunition[3] = {game->shells, game->rockets, game->cells};
    const uint16_t capacities[3] = {game->backpack ? 100u : 50u,
                                  game->backpack ? 100u : 50u,
                                  game->backpack ? 600u : 300u};
    for (unsigned row = 0; row < 3u; ++row) {
        const unsigned y = top + 8u + row * 6u;
        draw_patch_number(pixels, 230u, y, ammunition[row], 1);
        draw_patch_number(pixels, 250u, y, capacities[row], 1);
    }
}

void DoomLite_DrawGameWeapon(uint8_t *pixels, const DoomLiteGame *game) {
    if (!pixels || !game || !game->health || game->completed) return;
    const E1M1UiPatch *gun = &ui_pisga0;
    if (game->pistol_tics > 9u || (game->pistol_tics && game->pistol_tics <= 5u))
        gun = &ui_pisgb0;
    else if (game->pistol_tics) gun = &ui_pisgc0;
    draw_patch(pixels, gun, gun->x, gun->y, DOOM_GAME_VIEW_H);
    if (game->pistol_tics > 8u)
        draw_patch(pixels, &ui_pisfa0, ui_pisfa0.x, ui_pisfa0.y, DOOM_GAME_VIEW_H);
}

void DoomLite_DrawGameHud(uint8_t *pixels, const DoomLiteGame *game,
                          const char *message, unsigned map_zoom) {
    if (!pixels || !game) return;
    if (map_zoom) draw_map_hud(pixels, game, map_zoom);
    else {
        draw_status_bar(pixels, game);
        const char *map_name = game->map ? game->map->name : "E1M1";
        for (unsigned y = 0u; y < 8u; ++y)
            memset(pixels + y * DOOM_GAME_LCD_W + 216u, 237u, 40u);
        draw_text(pixels, 218u, 1u, map_name, 1u);
        const char contrast[3] = {'C', (char)('1' + game->contrast), 0};
        draw_text(pixels, 242u, 1u, contrast, 1u);
    }
    if (message) {
        const unsigned y = map_zoom ? DOOM_GAME_LCD_H - DOOM_GAME_HUD_HEIGHT : 0u;
        memset(pixels + y * DOOM_GAME_LCD_W, 237,
               DOOM_GAME_HUD_HEIGHT * DOOM_GAME_LCD_W);
        draw_text(pixels, 4u, y + 3u, message, 2u);
    }
    if (game->completed) {
        const char *title = game->map_index ? "E1M2 CLEAR" : "E1M1 CLEAR";
        draw_end_panel(pixels, title, 88u, game->map_index ? "F2 REPLAY" : "F2 NEXT");
    } else if (!game->health) draw_end_panel(pixels, "GAME OVER", 92u, "F2 RETRY");
}

