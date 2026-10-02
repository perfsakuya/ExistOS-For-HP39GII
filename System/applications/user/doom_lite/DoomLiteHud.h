#pragma once
#include <stdint.h>
#include "DoomLiteGame.h"

#define DOOM_GAME_LCD_W 256u
#define DOOM_GAME_LCD_H 127u
#define DOOM_GAME_HUD_HEIGHT 16u
#define DOOM_GAME_STATUS_HEIGHT 26u
#define DOOM_GAME_VIEW_H (DOOM_GAME_LCD_H - DOOM_GAME_STATUS_HEIGHT)

/* The 3D view uses a classic bottom status bar. The map retains the large
 * top row so its overview and objective markers stay readable. */
void DoomLite_DrawGameHud(uint8_t *pixels, const DoomLiteGame *game,
                          const char *message, unsigned map_zoom);

/* Flash-backed, pre-sized pistol and muzzle flash, clipped to the 3D view.
 * No game state changes or dynamic allocation. Do not call for the map. */
void DoomLite_DrawGameWeapon(uint8_t *pixels, const DoomLiteGame *game);
