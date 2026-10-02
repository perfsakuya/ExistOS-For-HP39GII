#pragma once

#include <stdint.h>
#include "E1M1Gameplay.h"
#include "DoomLiteGame.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Render one 8-bit grayscale LCD frame into caller-owned memory.
 * The buffer must hold LCD_PIX_W * LCD_PIX_H bytes. Coordinates are Doom map
 * units in Q8; facing spans a full turn in 256 steps (0 points along +X).
 * map_mode selects the overhead map when nonzero. This does not queue an LCD
 * transfer or change the player's/game's state. */
void DoomLite_RenderFrame(uint8_t *pixels, int32_t x_q8, int32_t y_q8,
                          uint8_t facing, int map_mode);

/* Gameplay renderer: a zero door_open[] entry blocks rays at that door's
 * exact WAD boundary; any nonzero entry lets rays pass through. This leaves
 * the original DoomLite_RenderFrame() geometry and appearance unchanged.
 * The 3D Game entry renders the top 101 rows; DrawGameHud owns the bottom 26. */
void DoomLite_RenderGameFrame(uint8_t *pixels, int32_t x_q8, int32_t y_q8,
                              uint8_t facing, int map_mode,
                              const uint8_t door_open[E1M1_DOOR_COUNT]);

/* Game map: zoom=1 shows the full map, zoom=2 follows the player at 2x.
 * The top 16 rows are reserved for the HUD. Includes large objective marks. */
void DoomLite_RenderGameMap(uint8_t *pixels, const DoomLiteGame *game,
                            unsigned zoom);

/* Draw packed Freedoom enemies and the blue key, clipped against walls, over the most
 * recently rendered game scene. Call immediately after RenderGameFrame with
 * map_mode=0 and before drawing the HUD; inactive things are omitted. */
void DoomLite_RenderGameThings(uint8_t *pixels, const DoomLiteGame *game);

#ifdef __cplusplus
}
#endif
