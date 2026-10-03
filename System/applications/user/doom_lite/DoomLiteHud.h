#pragma once
#include <stdint.h>
#include "DoomLiteGame.h"

#define DOOM_GAME_LCD_W 256u
#define DOOM_GAME_LCD_H 127u
#define DOOM_GAME_STATUS_HEIGHT 26u
#define DOOM_GAME_VIEW_H (DOOM_GAME_LCD_H - DOOM_GAME_STATUS_HEIGHT)

/* The scene and map share the classic bottom status bar. Normal HUD and
 * event messages never cover view rows; end-of-level/death panels do. */
void DoomLite_DrawGameHud(uint8_t *pixels, const DoomLiteGame *game,
                          const char *message, unsigned map_zoom);

/* Flash-backed, pre-sized pistol/shotgun and flash/pump poses, clipped to the view.
 * No game state changes or dynamic allocation. Do not call for the map. */
void DoomLite_DrawGameWeapon(uint8_t *pixels, const DoomLiteGame *game);
/* Asset pixels, patch descriptors, palette/pointer tables and compact text
 * glyphs. Uses target sizeof; excludes code and linker placement padding. */
unsigned DoomLite_UiReadonlyBytes(void);
