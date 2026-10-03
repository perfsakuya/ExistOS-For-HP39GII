#pragma once
#include "DoomLiteGame.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { DOOM_TEST_SPRITES, DOOM_TEST_COMBAT, DOOM_TEST_ITEMS, DOOM_TEST_PRESET_COUNT };
const DoomMap *DoomTestMap_Get(void);
int DoomTestMap_InitGame(DoomLiteGame *game, unsigned preset);
unsigned DoomTestMap_PresetCount(void);
const char *DoomTestMap_PresetName(unsigned preset);
/* Call after a normal gameplay tic. Frozen preview actors are visual-only:
 * moving, pickup, weapon and UI logic keep running, while AI remains disabled. */
void DoomTestMap_StepPreview(DoomLiteGame *game);
unsigned DoomTestMap_ReadonlyBytes(void);
#ifdef __cplusplus
}
#endif
