#pragma once

#include <stdint.h>
#if defined(DOOM_LITE_RAY_TEST) || defined(SKYOS_BUILD_LEGACY_DOOM)
#include "E1M1Gameplay.h"
#endif
#include "DoomLiteGame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render one 8-bit grayscale LCD frame into caller-owned memory.
 * The buffer must hold LCD_PIX_W * LCD_PIX_H bytes. Coordinates are Doom map
 * units in Q8; facing spans a full turn in 256 steps (0 points along +X).
 * map_mode selects the overhead map when nonzero. This does not queue an LCD
 * transfer or change the player's/game's state. */
#if defined(DOOM_LITE_RAY_TEST) || defined(SKYOS_BUILD_LEGACY_DOOM)
void DoomLite_RenderFrame(uint8_t *pixels, int32_t x_q8, int32_t y_q8,
                          uint8_t facing, int map_mode);

/* Legacy E1M1 preview renderer: a zero door_open[] entry blocks rays at that door's
 * exact WAD boundary; any nonzero entry lets rays pass through. This leaves
 * the original DoomLite_RenderFrame() geometry and appearance unchanged.
 * The 3D Game entry renders the top 101 rows; DrawGameHud owns the bottom 26. */
void DoomLite_RenderGameFrame(uint8_t *pixels, int32_t x_q8, int32_t y_q8,
                              uint8_t facing, int map_mode,
                              const uint8_t door_open[E1M1_DOOR_COUNT]);
#endif

/* Game map: zoom=1 shows the full map, zoom=2 follows the player at 2x.
 * Uses view rows 0..100 above the shared classic bar, with large objectives. */
void DoomLite_RenderGameMap(uint8_t *pixels, const DoomLiteGame *game,
                            unsigned zoom);

/* Draw packed Freedoom actor animations, grounded corpses, three keys and
 * common pickups against the native scene's wall/plane depth cache. Call immediately
 * after RenderGameScene and before the weapon/HUD; collected things are omitted. */
void DoomLite_RenderGameThings(uint8_t *pixels, const DoomLiteGame *game);

/* Native Game renderer; selects E1M1/E1M2 from game->map, uses exact cell
 * line references and current sector heights. Owns only the top view rows. */
void DoomLite_RenderGameScene(uint8_t *pixels, const DoomLiteGame *game);

typedef struct {
    uint32_t rays, cells, line_tests, boundaries, surfaces;
    uint32_t sprite_candidates, sprite_pixels;
    uint32_t surface_limit_hits;
    uint32_t surface_limit_pixels; /* Remaining ray rows filled by bounded fallback. */
    uint32_t ray_us, plane_us, wall_us; /* Disjoint scene stages, optional clock. */
    uint32_t world_texture_pixels, plane_texture_pixels;
    uint32_t span_cache_bytes; /* This frame's contiguous active span payload. */
} DoomLiteRenderStats;

/* Counters reset by RenderGameScene/Map; Things extends the same frame. */
const DoomLiteRenderStats *DoomLite_GetRenderStats(void);
/* Static writable caches only; excludes the borrowed LCD framebuffer. */
unsigned DoomLite_RenderWorkingSetBytes(void);
unsigned DoomLite_WorldReadonlyBytes(void);
unsigned DoomLite_SpriteReadonlyBytes(void);
/* Supply the same monotonic microsecond clock used by the game task.
 * Only phase boundaries are sampled; no calls inside pixel loops. */
void DoomLite_SetRenderClock(uint32_t (*clock_us)(void));

#ifdef __cplusplus
}
#endif
