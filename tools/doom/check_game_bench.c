/* Production-code host workload benchmark. Host timings are NOT ARM FPS. */
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif
#include "../../System/applications/user/doom_lite/DoomLiteGame.h"
#include "../../System/applications/user/doom_lite/DoomLiteRender.h"
#include "../../System/applications/user/doom_lite/DoomLiteHud.h"

#define PIXELS (DOOM_GAME_LCD_W * DOOM_GAME_LCD_H)
#define SAMPLES 512u
static DoomLiteGame game;
static uint8_t guarded[PIXELS + 32u];
static uint64_t timer_us(void) {
#ifdef _WIN32
    LARGE_INTEGER value, frequency;
    QueryPerformanceCounter(&value);
    QueryPerformanceFrequency(&frequency);
    return (uint64_t)value.QuadPart * 1000000u / (uint64_t)frequency.QuadPart;
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
#endif
}
static uint32_t clock_us(void) { return (uint32_t)timer_us(); }
static uint32_t hash_frame(void) {
    uint32_t h = 2166136261u;
    for (unsigned i = 16; i < PIXELS + 16; ++i) h = (h ^ guarded[i]) * 16777619u;
    return h;
}
static void guards(void) {
    for (unsigned i = 0; i < 16; ++i) {
        assert(guarded[i] == 0xa5);
        assert(guarded[PIXELS + 16 + i] == 0xa5);
    }
}
static void save_preview(const char *directory, unsigned level, unsigned contrast) {
    if (!directory) return;
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s/game-e1m%u-c%u.pgm",
                          directory, level + 1u, contrast + 1u);
    assert(length > 0 && (unsigned)length < sizeof(path));
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P5\n256 127\n255\n");
    assert(fwrite(guarded + 16u, 1u, PIXELS, file) == PIXELS);
    assert(!fclose(file));
}
int main(int argc, char **argv) {
    DoomLiteGame_SetClock(clock_us);
    DoomLite_SetRenderClock(clock_us);
    memset(guarded, 0xa5, sizeof(guarded));
    const char *directory = argc > 1 ? argv[1] : NULL;
    printf("BENCH_META host_only=1 state_bytes=%zu actor_bytes=%zu mover_bytes=%zu samples=%u render_bytes=%u world_bytes=%u\n",
           sizeof(game), sizeof(DoomLiteActor), sizeof(DoomLiteMover), SAMPLES,
           DoomLite_RenderWorkingSetBytes(), DoomLite_WorldReadonlyBytes());
    for (unsigned level = 0; level < DOOM_MAP_COUNT; ++level) {
        for (unsigned mode = 0; mode < 4u; ++mode) {
            if (mode == 3u && level != 0u) continue;
            assert(DoomLiteGame_InitMap(&game, level));
            if (mode == 1u)
                for (unsigned a = 0; a < game.actor_count; ++a)
                    game.actors[a].flags |= DL_ACTOR_AWAKE;
            uint64_t sums[4] = {0}, maxima[4] = {0};
            uint64_t cells = 0, lines = 0, sprite_pixels = 0;
            uint64_t ray_us = 0, plane_us = 0, wall_us = 0, world_pixels = 0;
            uint64_t active_span_bytes = 0;
            uint32_t peak_span_bytes = 0;
            uint32_t max_cells = 0, max_lines = 0, limits = 0, limit_pixels = 0, checksum = 0;
            for (unsigned frame = 0; frame < SAMPLES; ++frame) {
                if (mode == 1u && game.actor_count) {
                    /* Artificial near-monster stress viewpoints, not a recorded route. */
                    const DoomLiteActor *actor = &game.actors[(frame / 8u) % game.actor_count];
                    game.x_q8 = actor->x_q8 + 48 * 256;
                    game.y_q8 = actor->y_q8;
                    game.player_sector = DoomLiteGame_PlayerSector(&game);
                }
                game.invulnerable_tics = 200u;
                const uint64_t a = timer_us();
                DoomLiteGame_Step(&game, 0u);
                const uint64_t b = timer_us();
                game.facing = (uint8_t)(frame / 2u);
                if (mode == 2u) DoomLite_RenderGameMap(guarded + 16u, &game, (frame & 1u) + 1u);
                else if (mode == 3u) {
                    /* Retained original E1M1 scene, same headings/position,
                     * useful for host algorithm comparison, not ARM prediction. */
                    static const uint8_t doors[E1M1_DOOR_COUNT] = {0};
                    DoomLite_RenderGameFrame(guarded + 16u, game.x_q8, game.y_q8,
                                            game.facing, 0, doors);
                } else DoomLite_RenderGameScene(guarded + 16u, &game);
                const uint64_t c = timer_us();
                if (mode < 2u) DoomLite_RenderGameThings(guarded + 16u, &game);
                const uint64_t d = timer_us();
                if (mode != 2u) DoomLite_DrawGameWeapon(guarded + 16u, &game);
                DoomLite_DrawGameHud(guarded + 16u, &game, NULL, mode == 2u ? 2u : 0u);
                const uint64_t e = timer_us();
                const uint64_t times[4] = {b - a, c - b, d - c, e - d};
                for (unsigned p = 0; p < 4u; ++p) {
                    sums[p] += times[p];
                    if (times[p] > maxima[p]) maxima[p] = times[p];
                }
                const DoomLiteRenderStats *stats = DoomLite_GetRenderStats();
                ray_us += stats->ray_us; plane_us += stats->plane_us;
                wall_us += stats->wall_us; world_pixels += stats->world_texture_pixels;
                active_span_bytes += stats->span_cache_bytes;
                if (stats->span_cache_bytes > peak_span_bytes) peak_span_bytes = stats->span_cache_bytes;
                cells += stats->cells; lines += stats->line_tests;
                sprite_pixels += stats->sprite_pixels; limits += stats->surface_limit_hits;
                limit_pixels += stats->surface_limit_pixels;
                if (stats->cells > max_cells) max_cells = stats->cells;
                if (stats->line_tests > max_lines) max_lines = stats->line_tests;
                checksum = (checksum ^ hash_frame()) * 16777619u;
                guards();
            }
            printf("BENCH level=%u mode=%u actors=%u logic_avg_us=%.3f logic_max_us=%" PRIu64
                   " scene_avg_us=%.3f scene_max_us=%" PRIu64
                   " sprite_avg_us=%.3f sprite_max_us=%" PRIu64
                   " ui_avg_us=%.3f ui_max_us=%" PRIu64
                   " cells_avg=%" PRIu64 " cells_max=%u lines_avg=%" PRIu64 " lines_max=%u"
                   " sprite_pixels_avg=%" PRIu64 " surface_limits=%u surface_limit_pixels=%u pool_overflows=%lu checksum=%u"
                   " ray_avg_us=%.3f plane_avg_us=%.3f wall_avg_us=%.3f world_pixels_avg=%" PRIu64
                   " span_avg_bytes=%" PRIu64 " span_peak_bytes=%u\n",
                   level + 1u, mode, (unsigned)game.actor_count,
                   (double)sums[0]/SAMPLES, maxima[0], (double)sums[1]/SAMPLES, maxima[1],
                   (double)sums[2]/SAMPLES, maxima[2], (double)sums[3]/SAMPLES, maxima[3],
                   cells/SAMPLES, max_cells, lines/SAMPLES, max_lines,
                   sprite_pixels/SAMPLES, limits, limit_pixels,
                   (unsigned long)game.metrics.pool_overflows, checksum,
                   (double)ray_us/SAMPLES, (double)plane_us/SAMPLES,
                   (double)wall_us/SAMPLES, world_pixels/SAMPLES,
                   active_span_bytes/SAMPLES, peak_span_bytes);
            assert(!game.metrics.pool_overflows);
        }
        for (unsigned contrast = 0; contrast < 3u; ++contrast) {
            DoomLiteGame_InitMap(&game, level);
            game.contrast = (uint8_t)contrast;
            DoomLite_RenderGameScene(guarded + 16u, &game);
            DoomLite_RenderGameThings(guarded + 16u, &game);
            DoomLite_DrawGameWeapon(guarded + 16u, &game);
            DoomLite_DrawGameHud(guarded + 16u, &game, NULL, 0u);
            guards();
            save_preview(directory, level, contrast);
        }
    }
    puts("BENCH_OK frame guards and fixed pool bounds passed");
    return 0;
}
