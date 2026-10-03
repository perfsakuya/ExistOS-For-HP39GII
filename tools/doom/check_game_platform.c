/* Real E1M1 platform occlusion; native rendering only, no device or AI route. */
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
static uint8_t guarded[PIXELS + 32u], scene[VIEW_PIXELS], no_plane[VIEW_PIXELS];
static GameWallSpan saved_spans[LITE_RAYS * GAME_RAY_SPANS];
static uint16_t saved_first[LITE_RAYS];
static uint8_t saved_count[LITE_RAYS];
static uint8_t *const pixels = guarded + 16u;

static void guards(void) {
    for (unsigned i = 0u; i < 16u; ++i) {
        assert(guarded[i] == 0xa5u);
        assert(guarded[PIXELS + 16u + i] == 0xa5u);
    }
    for (unsigned i = VIEW_PIXELS; i < PIXELS; ++i) assert(pixels[i] == 0xa5u);
}
static unsigned changed(const uint8_t *a, const uint8_t *b) {
    unsigned count = 0u;
    for (unsigned i = 0u; i < VIEW_PIXELS; ++i) count += a[i] != b[i];
    return count;
}
static void preview(const char *directory, const char *name, const uint8_t *view) {
    if (!directory) return;
    char path[1024];
    const int length = snprintf(path, sizeof(path), "%s/%s.pgm", directory, name);
    assert(length > 0 && (unsigned)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P5\n256 101\n255\n");
    assert(fwrite(view, 1u, VIEW_PIXELS, file) == VIEW_PIXELS);
    assert(!fclose(file));
}
static void isolate(DoomLiteGame *game) {
    assert(DoomLiteGame_InitMap(game, 0u));
    assert(game->map->things[265u].type == 3004u);
    memset(game->collected, 255, sizeof(game->collected));
    memset(game->thing_actor, 255, sizeof(game->thing_actor));
    game->actor_count = 1u;
    game->actors[0] = (DoomLiteActor){.x_q8=944 * 256, .y_q8=-224 * 256,
        .thing_index=265u, .sector=57u, .health=20, .state=DL_ACTOR_WALK};
    game->y_q8 = -224 * 256;
    game->facing = 128u;
    assert(game->sector_state[102u].floor == 136);
    assert(game->sector_state[57u].floor == 0);
}

static void check_view(DoomLiteGame *game, int player_x, int expect_visible,
                       const char *directory) {
    game->x_q8 = player_x * 256;
    assert(DoomLiteGame_PlayerSector(game) == 102u);
    assert(!DoomLiteGame_IsSolid(game, game->x_q8, game->y_q8));
    memset(guarded, 0xa5, sizeof(guarded));
    DoomLite_RenderGameScene(pixels, game);
    const unsigned limits = game_render_stats.surface_limit_hits;
    const unsigned limit_pixels = game_render_stats.surface_limit_pixels;
    assert(limits == 0u && limit_pixels == 0u);
    memcpy(scene, pixels, sizeof(scene));
    memcpy(saved_spans, game_spans, sizeof(saved_spans));
    memcpy(saved_count, game_span_count, sizeof(saved_count));
    memcpy(saved_first, game_span_first, sizeof(saved_first));
    DoomLite_RenderGameThings(pixels, game);
    const unsigned actual = changed(scene, pixels);
    if (expect_visible) assert(actual > 0u);
    else assert(actual == 0u);
    guards();
    preview(directory, expect_visible ? "platform-edge-visible" : "platform-floor-hidden", pixels);

    /* Remove only depth-only planes to reproduce the original fault while
     * retaining walls, railing alpha, sprite positioning and pixel sampling. */
    for (unsigned ray = 0u; ray < LITE_RAYS; ++ray) {
        unsigned keep = 0u;
        for (unsigned i = 0u; i < game_span_count[ray]; ++i)
            if (game_spans[game_span_first[ray] + (i)].color != GAME_WALL_PLANE)
                game_spans[game_span_first[ray] + (keep++)] = game_spans[game_span_first[ray] + (i)];
        game_span_count[ray] = (uint8_t)keep;
    }
    memcpy(pixels, scene, sizeof(scene));
    DoomLite_RenderGameThings(pixels, game);
    memcpy(no_plane, pixels, sizeof(no_plane));
    const unsigned unoccluded = changed(scene, no_plane);
    assert(unoccluded > 0u);
    if (expect_visible) assert(actual == unoccluded);
    else preview(directory, "platform-without-floor-cover", no_plane);
    memcpy(game_spans, saved_spans, sizeof(saved_spans));
    memcpy(game_span_count, saved_count, sizeof(saved_count));
    memcpy(game_span_first, saved_first, sizeof(saved_first));
    guards();
    printf("Platform E1M1 player=(%d,-224) actor=(944,-224) hidden=%u visible=%u without_plane=%u limits=%u limit_pixels=%u\n",
           player_x, !expect_visible, actual, unoccluded, limits, limit_pixels);
}

static void check_spawn_headings(DoomLiteGame *game) {
    for (unsigned level = 0u; level < DOOM_MAP_COUNT; ++level) {
        assert(DoomLiteGame_InitMap(game, level));
        assert(!DoomLiteGame_IsSolid(game, game->x_q8, game->y_q8));
        unsigned limits = 0u, area = 0u;
        for (unsigned facing = 0u; facing < 256u; ++facing) {
            game->facing = (uint8_t)facing;
            memset(guarded, 0xa5, sizeof(guarded));
            DoomLite_RenderGameScene(pixels, game);
            limits += game_render_stats.surface_limit_hits;
            area += game_render_stats.surface_limit_pixels;
            guards();
        }
        printf("Platform spawn E1M%u legal_headings=256 limits=%u limit_pixels=%u\n",level + 1u,limits,area);
        assert(limits == 0u && area == 0u);
    }
}

int main(int argc, char **argv) {
    DoomLiteGame game;
    isolate(&game);
    const char *directory = argc > 1 ? argv[1] : NULL;
    /* Line 611 is 192 units away; its high floor edge is at screen y=77.
     * The low monster at depth 512 projects wholly below it (y=81..94). */
    check_view(&game, 1456, 0, directory);
    /* At 48 units from the same edge its projection is below the viewport.
     * The same low monster's head at y=93..100 remains visible through the
     * original railing's holes; the railing is retained in both renders. */
    check_view(&game, 1312, 1, directory);
    check_spawn_headings(&game);
    puts("PLATFORM_OK real platform hides lower monster; visible head over edge retained; frame/bar bounds passed");
    return 0;
}
