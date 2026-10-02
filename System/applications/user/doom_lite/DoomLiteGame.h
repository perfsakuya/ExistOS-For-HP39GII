#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* E1M1-only simplified rules. This is not GBADoom's game simulation. */
#define DOOM_LITE_GAME_HZ 35u
#define DOOM_LITE_GAME_USE_REACH 48u
#define DOOM_LITE_GAME_THINGS 292u
#define DOOM_LITE_GAME_DOORS 15u

#define DL_GAME_UP    (1u << 0)
#define DL_GAME_DOWN  (1u << 1)
#define DL_GAME_LEFT  (1u << 2)
#define DL_GAME_RIGHT (1u << 3)
#define DL_GAME_USE   (1u << 4) /* F2, rising edge */
#define DL_GAME_FIRE  (1u << 5) /* F1, rising edge */

#define DL_EVENT_BLUE_KEY   (1u << 0)
#define DL_EVENT_PICKUP     (1u << 1)
#define DL_EVENT_DOOR_OPEN  (1u << 2)
#define DL_EVENT_DOOR_CLOSE (1u << 3)
#define DL_EVENT_NEED_BLUE  (1u << 4)
#define DL_EVENT_EXIT       (1u << 5)
#define DL_EVENT_SHOT_HIT   (1u << 6)
#define DL_EVENT_SHOT_MISS  (1u << 7)
#define DL_EVENT_KILL       (1u << 8)
#define DL_EVENT_HURT       (1u << 9)

typedef struct {
    int32_t x_q8, y_q8;
    uint32_t ticks;
    uint16_t kills, total_enemies;
    uint8_t facing; /* 256 steps/turn; 0 points along +X. */
    int8_t turn_remainder; /* Signed half-step retained between turns. */
    uint8_t health, ammo, blue_key, completed;
    uint8_t door_open[DOOM_LITE_GAME_DOORS];
    uint8_t enemy_hp[DOOM_LITE_GAME_THINGS]; /* 0 means dead/non-enemy. */
    uint8_t collected[(DOOM_LITE_GAME_THINGS + 7u) / 8u];
    uint8_t previous_buttons;
} DoomLiteGame;

void DoomLiteGame_Init(DoomLiteGame *game);

/* Call exactly once per 1/35 second with the buttons currently held.
 * Returns DL_EVENT_* flags for this tick. Drawing is caller-owned. */
uint32_t DoomLiteGame_Step(DoomLiteGame *game, uint16_t buttons);

/* Player-center collision query with an eight-map-unit radius. Open doors
 * use precise WAD line segments where the coarse Lite grid is ambiguous. */
int DoomLiteGame_IsSolid(const DoomLiteGame *game, int32_t x_q8, int32_t y_q8);

/* Whether a medium-skill single-player WAD thing is still present. */
int DoomLiteGame_ThingActive(const DoomLiteGame *game, unsigned thing_index);

#ifdef __cplusplus
}
#endif
