#pragma once

#include <stdint.h>

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

#ifdef __cplusplus
}
#endif
