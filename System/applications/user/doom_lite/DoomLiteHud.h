#pragma once
#include <stdint.h>
#include "DoomLiteGame.h"

#define DOOM_GAME_LCD_W 256u
#define DOOM_GAME_LCD_H 127u
#define DOOM_GAME_HUD_HEIGHT 16u

/* Draw the large status row and optional event/end panel into a full LCD
 * frame. map_zoom=0 is 3D; 1 and 2 display the map's F4 zoom hint. */
void DoomLite_DrawGameHud(uint8_t *pixels, const DoomLiteGame *game,
                          const char *message, unsigned map_zoom);
