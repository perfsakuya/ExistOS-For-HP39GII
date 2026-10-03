/* Test arena integration: shared scene, actors, weapons and HUD. No device I/O. */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

#define PIXELS (LCD_PIX_W * LCD_PIX_H)
#define VIEW_PIXELS (LCD_PIX_W * DOOM_GAME_VIEW_H)
static uint8_t guarded[PIXELS + 32u];
static uint8_t *const pixels = guarded + 16u;

static void guards(void) {
    for (unsigned i = 0u; i < 16u; ++i) {
        assert(guarded[i] == 0xa5u);
        assert(guarded[PIXELS + 16u + i] == 0xa5u);
    }
}
static unsigned changes(const uint8_t *a, const uint8_t *b, unsigned size) {
    unsigned count = 0u;
    for (unsigned i = 0u; i < size; ++i) count += a[i] != b[i];
    return count;
}
static void save(const char *directory, const char *name) {
    if (!directory) return;
    char path[1024];
    const int size = snprintf(path, sizeof(path), "%s/%s.pgm", directory, name);
    assert(size > 0 && (unsigned)size < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P5\n256 127\n255\n");
    assert(fwrite(pixels, 1u, PIXELS, file) == PIXELS);
    assert(!fclose(file));
}
static void compose(DoomLiteGame *game, const char *message, int map_mode) {
    memset(guarded, 0xa5, sizeof(guarded));
    if (map_mode) DoomLite_RenderGameMap(pixels, game, 2u);
    else {
        DoomLite_RenderGameScene(pixels, game);
        DoomLite_RenderGameThings(pixels, game);
        DoomLite_DrawGameWeapon(pixels, game);
    }
    guards();
    for (unsigned i = VIEW_PIXELS; i < PIXELS; ++i) assert(pixels[i] == 0xa5u);
    DoomLite_DrawGameHud(pixels, game, message, map_mode ? 2u : 0u);
    guards();
    assert(DoomLite_GetRenderStats()->surface_limit_hits == 0u);
}

int main(int argc, char **argv) {
    const char *directory = argc > 1 ? argv[1] : NULL;
    DoomLiteGame game;
    assert(DoomTestMap_InitGame(&game, DOOM_TEST_SPRITES));
    assert(game_world_map(game.map) == &doom_test_world);
    assert(game_world_map(NULL) == NULL);
    DoomMap fake = *game.map;
    assert(game_world_map(&fake) == NULL); /* An index is not asset identity. */
    fake.index = UINT16_MAX;
    assert(game_world_map(&fake) == NULL);
    const unsigned material = game_world_side(game.map, 0u, 1u)->middle;
    assert(material > 0u && material <= GAME_WORLD_TEXTURE_COUNT);
    assert(!game_world_side(game.map, game.map->line_count, 1u));
    assert(!game_world_side(game.map, 0u, 0u));

    static uint8_t before[PIXELS];
    DoomLite_RenderGameScene(pixels, &game);
    memcpy(before, pixels, VIEW_PIXELS);
    DoomLite_RenderGameThings(pixels, &game);
    assert(changes(before, pixels, VIEW_PIXELS) > 0u);
    compose(&game, "SPRITES F2 NEXT", 0);
    save(directory, "test-sprites-pistol");
    game.current_weapon = DL_WEAPON_SHOTGUN;
    game.pistol_tics = 35u;
    compose(&game, "SPRITES F2 NEXT", 0);
    save(directory, "test-sprites-shotgun-flash");
    game.pistol_tics = 0u;
    compose(&game, "SPRITES F2 NEXT", 1);
    save(directory, "test-sprites-map");

    unsigned frames = 0u;
    for (unsigned preset = 0u; preset < DoomTestMap_PresetCount(); ++preset) {
        assert(DoomTestMap_InitGame(&game, preset));
        for (unsigned contrast = 0u; contrast < 3u; ++contrast) {
            game.contrast = (uint8_t)contrast;
            for (unsigned angle = 0u; angle < 256u; angle += 8u) {
                game.facing = (uint8_t)angle;
                game.ticks = angle;
                DoomTestMap_StepPreview(&game);
                for (unsigned weapon = 0u; weapon < 2u; ++weapon) {
                    game.current_weapon = (uint8_t)weapon;
                    game.pistol_tics = (uint8_t)(angle % 36u);
                    compose(&game, DoomTestMap_PresetName(preset), 0);
                    ++frames;
                }
            }
        }
        game.facing = 0u; game.contrast = 1u; game.pistol_tics = 0u;
        compose(&game, DoomTestMap_PresetName(preset), 0);
        save(directory, preset == DOOM_TEST_COMBAT ? "test-combat" :
                        preset == DOOM_TEST_ITEMS ? "test-items" : "test-sprites");
    }
    /* Sweep every pump/flash pose with a close foreground scene. */
    assert(DoomTestMap_InitGame(&game, DOOM_TEST_SPRITES));
    game.current_weapon = DL_WEAPON_SHOTGUN;
    for (unsigned tic = 0u; tic <= 35u; ++tic) {
        game.pistol_tics = (uint8_t)tic;
        compose(&game, "SG F4 SWITCH", 0);
        ++frames;
    }
    printf("TEST_VISUALS_OK frames=%u test_world_bytes=%u ui_bytes=%u test_map_bytes=%u\n",
           frames, (unsigned)DOOM_TEST_WORLD_BYTES, DoomLite_UiReadonlyBytes(), DoomTestMap_ReadonlyBytes());
    return 0;
}
