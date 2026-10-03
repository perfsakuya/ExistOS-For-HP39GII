#pragma once
#include <stdint.h>
#include "DoomMap.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Bounded native simulation, deliberately not demo-compatible Doom. */
#define DOOM_LITE_GAME_HZ 35u
#define DOOM_LITE_GAME_USE_REACH 64u
#define DOOM_LITE_GAME_THINGS DOOM_MAP_MAX_THINGS
#define DOOM_LITE_GAME_DOORS DOOM_MAP_MAX_SECTORS
#define DOOM_LITE_GAME_ACTORS 128u
#define DOOM_LITE_GAME_MOVERS 64u
#define DOOM_LITE_GAME_AI_BUDGET 8u
#define DL_GAME_UP (1u << 0)
#define DL_GAME_DOWN (1u << 1)
#define DL_GAME_LEFT (1u << 2)
#define DL_GAME_RIGHT (1u << 3)
#define DL_GAME_USE (1u << 4)
#define DL_GAME_FIRE (1u << 5)
#define DL_GAME_SWITCH (1u << 6)
#define DL_EVENT_BLUE_KEY (1u << 0)
#define DL_EVENT_PICKUP (1u << 1)
#define DL_EVENT_DOOR_OPEN (1u << 2)
#define DL_EVENT_DOOR_CLOSE (1u << 3)
#define DL_EVENT_NEED_BLUE (1u << 4)
#define DL_EVENT_EXIT (1u << 5)
#define DL_EVENT_SHOT_HIT (1u << 6)
#define DL_EVENT_SHOT_MISS (1u << 7)
#define DL_EVENT_KILL (1u << 8)
#define DL_EVENT_HURT (1u << 9)
#define DL_EVENT_RED_KEY (1u << 10)
#define DL_EVENT_YELLOW_KEY (1u << 11)
#define DL_EVENT_NEED_RED (1u << 12)
#define DL_EVENT_NEED_YELLOW (1u << 13)
#define DL_EVENT_TELEPORT (1u << 14)
#define DL_EVENT_MOVER (1u << 15)
#define DL_EVENT_SECRET (1u << 16)
#define DL_EVENT_LIMIT (1u << 17)
#define DL_EVENT_WEAPON (1u << 18)
enum { DL_WEAPON_PISTOL = 0, DL_WEAPON_SHOTGUN = 1 };
/* Selection indices differ from the existing Doom inventory bit layout. */
enum { DL_WEAPON_PISTOL_OWNED = 2u, DL_WEAPON_SHOTGUN_OWNED = 4u };
enum { DL_ACTOR_WALK, DL_ACTOR_ATTACK, DL_ACTOR_PAIN, DL_ACTOR_DEATH, DL_ACTOR_CORPSE };
enum { DL_ACTOR_AWAKE = 1u, DL_ACTOR_SOLID = 2u };
enum { DL_MOVER_NONE, DL_MOVER_DOOR, DL_MOVER_FLOOR, DL_MOVER_PLATFORM };
enum { DL_PROFILE_AI, DL_PROFILE_ANIM, DL_PROFILE_MOVER, DL_PROFILE_COLLISION, DL_PROFILE_LOS, DL_PROFILE_COUNT };
typedef struct {
    int32_t x_q8, y_q8;
    uint16_t thing_index, sector;
    int16_t health;
    uint8_t state, frame, tics, cooldown, flags, facing;
} DoomLiteActor;
typedef struct {
    int32_t x_q8, y_q8;
    uint16_t thing_type, sector;
    uint8_t state, frame, facing;
} DoomLiteVisual;
typedef struct { int16_t floor, ceiling; uint16_t special; } DoomLiteSectorState;
typedef struct {
    uint16_t sector, wait;
    int16_t target, home;
    uint8_t kind, speed, phase, return_after;
} DoomLiteMover;
typedef struct {
    /* AI includes its LOS/collision calls; mover may query body geometry.
     * These nested intervals must not be added to estimate total frame time. */
    uint32_t total_us[DL_PROFILE_COUNT], max_us[DL_PROFILE_COUNT], calls[DL_PROFILE_COUNT];
} DoomLiteGameProfile;
typedef struct {
    uint32_t ai_visits, los_queries, los_line_tests, collision_queries;
    uint32_t collision_line_tests, state_transitions, mover_steps;
    uint32_t pickup_tests, actor_moves, shots, teleports, pool_overflows;
    uint16_t actor_high_water, mover_high_water, active_movers, awake_actors;
} DoomLiteGameMetrics;
typedef struct {
    const DoomMap *map;
    int32_t x_q8, y_q8;
    uint32_t ticks;
    uint16_t kills, total_enemies, health, ammo, armor, secrets;
    uint16_t shells, rockets, cells;
    uint16_t actor_count, ai_cursor, pickup_count, teleport_count;
    uint16_t player_sector, suit_tics, invulnerable_tics;
    uint8_t facing, blue_key, red_key, yellow_key, completed;
    uint8_t map_index, contrast, armor_class, weapons, backpack, current_weapon;
    uint8_t pistol_tics; /* Current weapon animation clock; historical name keeps logs compatible. */
    uint8_t shotgun_cooldown; /* Survives weapon switches; firing the pistol cannot shorten it. */
    uint8_t freeze_actors; /* Static Test previews skip actor animation and AI. */
    uint8_t previous_buttons, teleport_cooldown;
    int8_t turn_remainder;
    uint8_t thing_actor[DOOM_LITE_GAME_THINGS]; /* 255 is not an actor. */
    uint8_t collected[(DOOM_LITE_GAME_THINGS + 7u) / 8u];
    uint8_t triggered[(DOOM_MAP_MAX_LINES + 7u) / 8u];
    uint16_t pickup_refs[DOOM_LITE_GAME_THINGS];
    uint16_t teleport_refs[16];
    DoomLiteActor actors[DOOM_LITE_GAME_ACTORS];
    DoomLiteSectorState sector_state[DOOM_MAP_MAX_SECTORS];
    DoomLiteMover movers[DOOM_LITE_GAME_MOVERS];
    DoomLiteGameProfile profile;
    DoomLiteGameMetrics metrics;
} DoomLiteGame;
void DoomLiteGame_Init(DoomLiteGame *game);
int DoomLiteGame_InitMap(DoomLiteGame *game, unsigned map_index);
/* Map and its referenced arrays stay immutable and alive for the game lifetime.
 * Invalid capacities or references are rejected before changing game state. */
int DoomLiteGame_InitData(DoomLiteGame *game, const DoomMap *map);
int DoomLiteGame_NextMap(DoomLiteGame *game);
uint16_t DoomLiteGame_CurrentAmmo(const DoomLiteGame *game);
uint32_t DoomLiteGame_Step(DoomLiteGame *game, uint16_t buttons);
int DoomLiteGame_IsSolid(const DoomLiteGame *game, int32_t x_q8, int32_t y_q8);
int DoomLiteGame_ThingActive(const DoomLiteGame *game, unsigned thing_index);
unsigned DoomLiteGame_VisualCount(const DoomLiteGame *game);
int DoomLiteGame_GetVisual(const DoomLiteGame *game, unsigned index, DoomLiteVisual *visual);
/* Nearest counterclockwise octant of a vector relative to a heading. Heading
 * uses 256 steps per turn; an exact half-octant selects the following sector.
 * The zero vector has no direction and returns the forward octant. */
unsigned DoomLiteGame_Direction8(int32_t dx, int32_t dy, uint8_t facing);
uint16_t DoomLiteGame_PlayerSector(const DoomLiteGame *game);
int DoomLiteGame_FloorHeight(const DoomLiteGame *game, uint16_t sector);
int DoomLiteGame_CeilingHeight(const DoomLiteGame *game, uint16_t sector);
unsigned DoomLiteGame_DoorLiftQ8(const DoomLiteGame *game, uint16_t sector);
void DoomLiteGame_SetClock(uint32_t (*clock_us)(void));
void DoomLiteGame_TakeProfile(DoomLiteGame *game, DoomLiteGameProfile *profile);
#ifdef __cplusplus
}
#endif
