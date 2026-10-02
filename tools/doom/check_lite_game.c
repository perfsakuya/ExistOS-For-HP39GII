/* Host test: gcc -std=c11 -O2 -Wall -Wextra -Werror
 * tools/doom/check_lite_game.c
 * System/applications/user/doom_lite/DoomLiteGame.c -o check_lite_game.exe */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../../System/applications/user/doom_lite/DoomLiteGame.h"
#include "../../System/applications/user/doom_lite/E1M1Gameplay.h"
#include "../../System/applications/user/doom_lite/E1M1Grid.h"

#define WALK_STEP 8
#define EXIT_WALK_REACH 32
#define WALK_WIDTH (E1M1_WIDTH * E1M1_CELL / WALK_STEP)
#define WALK_HEIGHT (E1M1_HEIGHT * E1M1_CELL / WALK_STEP)

/* An eight-map-unit lattice catches narrow door passages without assuming
 * that the coarse 32-unit wall cell is wholly open or wholly closed. */
static void check_reachability(void) {
    DoomLiteGame game;
    DoomLiteGame_Init(&game);
    for (unsigned i = 0; i < E1M1_DOOR_COUNT; ++i)
        game.door_open[i] = 1u;
    const int x_origin = E1M1_GX0 * E1M1_CELL;
    const int y_origin = E1M1_GY0 * E1M1_CELL;
    const unsigned count = WALK_WIDTH * WALK_HEIGHT;
    uint8_t *seen = calloc(count, 1u);
    uint32_t *queue = malloc(count * sizeof(*queue));
    assert(seen && queue);
    const unsigned sx = (unsigned)((game.x_q8 / 256 - x_origin) / WALK_STEP);
    const unsigned sy = (unsigned)((game.y_q8 / 256 - y_origin) / WALK_STEP);
    const unsigned start = sy * WALK_WIDTH + sx;
    assert(start < count && !DoomLiteGame_IsSolid(&game, game.x_q8,
                                                  game.y_q8));
    unsigned head = 0, tail = 0;
    queue[tail++] = start;
    seen[start] = 1u;
    int key_reached = 0, exit_reached = 0;
    int nearest_exit_distance2 = 0x7fffffff;
    int nearest_exit_x = 0, nearest_exit_y = 0;
    const int offsets[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    while (head < tail) {
        const unsigned node = queue[head++];
        const int cx = (int)(node % WALK_WIDTH);
        const int cy = (int)(node / WALK_WIDTH);
        const int x = x_origin + cx * WALK_STEP;
        const int y = y_origin + cy * WALK_STEP;
        const int kx = x - 2192, ky = y - 576;
        if (kx * kx + ky * ky <= 24 * 24) key_reached = 1;
        const int ey = y < 1280 ? 1280 : y > 1312 ? 1312 : y;
        const int distance2 = (x + 400) * (x + 400) + (y - ey) * (y - ey);
        if (distance2 < nearest_exit_distance2) {
            nearest_exit_distance2 = distance2;
            nearest_exit_x = x;
            nearest_exit_y = y;
        }
        if (distance2 <= EXIT_WALK_REACH * EXIT_WALK_REACH) {
            DoomLiteGame at_exit = game;
            at_exit.x_q8 = x * 256;
            at_exit.y_q8 = y * 256;
            at_exit.facing = x >= -400 ? 128u : 0u;
            if (DoomLiteGame_Step(&at_exit, DL_GAME_USE) & DL_EVENT_EXIT)
                exit_reached = 1;
        }
        for (unsigned direction = 0; direction < 4u; ++direction) {
            const int nx = cx + offsets[direction][0];
            const int ny = cy + offsets[direction][1];
            if ((unsigned)nx >= WALK_WIDTH || (unsigned)ny >= WALK_HEIGHT)
                continue;
            const unsigned neighbor = (unsigned)ny * WALK_WIDTH +
                                      (unsigned)nx;
            if (seen[neighbor]) continue;
            seen[neighbor] = 1u;
            if (DoomLiteGame_IsSolid(&game,
                    (x_origin + nx * WALK_STEP) * 256,
                    (y_origin + ny * WALK_STEP) * 256)) continue;
            queue[tail++] = neighbor;
        }
    }
    fprintf(stderr, "all-door-open reachability: visited=%u key=%d exit=%d nearest_exit=(%d,%d) dist2=%d\n",
            tail, key_reached, exit_reached, nearest_exit_x,
            nearest_exit_y, nearest_exit_distance2);
    free(queue);
    free(seen);
    assert(key_reached && exit_reached);
}

static int door_index(uint8_t sector) {
    for (unsigned i = 0; i < E1M1_DOOR_COUNT; ++i)
        if (e1m1_doors[i].sector == sector) return (int)i;
    return -1;
}

static void locate(DoomLiteGame *game, int32_t x, int32_t y, uint8_t facing) {
    game->x_q8 = x * 256;
    game->y_q8 = y * 256;
    game->facing = facing;
}

static void check_all_door_use(void) {
    const int vx[4] = {-1, 1, 0, 0};
    const int vy[4] = {0, 0, -1, 1};
    const uint8_t toward[4] = {0u, 128u, 64u, 192u};
    for (unsigned target = 0; target < E1M1_DOOR_COUNT; ++target) {
        const E1M1Door *door = &e1m1_doors[target];
        const int cx = (door->x0 + door->x1) / 2;
        const int cy = (door->y0 + door->y1) / 2;
        int opened_by_f2 = 0;
        for (int distance = 16; distance <= 56 && !opened_by_f2;
             distance += 8) {
            for (unsigned side = 0; side < 4u; ++side) {
                DoomLiteGame game;
                DoomLiteGame_Init(&game);
                game.blue_key = 1u;
                for (unsigned i = 0; i < E1M1_DOOR_COUNT; ++i)
                    game.door_open[i] = i != target;
                locate(&game, cx + vx[side] * distance,
                       cy + vy[side] * distance, toward[side]);
                if (DoomLiteGame_IsSolid(&game, game.x_q8, game.y_q8))
                    continue;
                DoomLiteGame_Step(&game, DL_GAME_USE);
                if (game.door_open[target]) {
                    opened_by_f2 = 1;
                    break;
                }
            }
        }
        if (!opened_by_f2) {
            fprintf(stderr, "F2 cannot open door sector %u\n", door->sector);
            exit(1);
        }
    }
}

static void check_turn_speed(void) {
    DoomLiteGame game;
    DoomLiteGame_Init(&game);
    const int32_t x = game.x_q8, y = game.y_q8;
    const uint8_t start = game.facing;
    for (unsigned tick = 0; tick < 2u * DOOM_LITE_GAME_HZ; ++tick)
        DoomLiteGame_Step(&game, DL_GAME_RIGHT);
    /* Two seconds at 73.828125 degrees/s advances 105 of 256 steps. */
    assert(game.facing == (uint8_t)(start + 105u));
    for (unsigned tick = 0; tick < 2u * DOOM_LITE_GAME_HZ; ++tick)
        DoomLiteGame_Step(&game, DL_GAME_LEFT);
    assert(game.facing == start && game.turn_remainder == 0);
    for (unsigned tick = 0; tick < 17u; ++tick) {
        DoomLiteGame_Step(&game, DL_GAME_LEFT);
        DoomLiteGame_Step(&game, 0);
        DoomLiteGame_Step(&game, DL_GAME_RIGHT);
    }
    assert(game.facing == start && game.turn_remainder == 0);
    assert(game.x_q8 == x && game.y_q8 == y && game.ammo == 50u);
}

int main(void) {
    DoomLiteGame game;
    DoomLiteGame_Init(&game);
    assert(game.x_q8 == -416 * 256 && game.y_q8 == 256 * 256);
    assert(game.health == 100 && game.ammo == 50 && game.total_enemies == 29);
    assert(DoomLiteGame_ThingActive(&game, 87)); /* WAD blue key */
    assert(!DoomLiteGame_ThingActive(&game, 83)); /* hard-only imp */
    assert(DoomLiteGame_ThingActive(&game, 88)); /* medium zombieman */
    check_reachability();
    check_all_door_use();
    check_turn_speed();

    for (unsigned i = 0; i < E1M1_DOOR_COUNT; ++i) {
        const E1M1Door *door = &e1m1_doors[i];
        const int32_t center_x = (door->x0 + door->x1) * 128;
        const int32_t center_y = (door->y0 + door->y1) * 128;
        const int closed_solid = DoomLiteGame_IsSolid(&game, center_x, center_y);
        game.door_open[i] = 1u;
        const int open_solid = DoomLiteGame_IsSolid(&game, center_x, center_y);
        /* Sector 84 is an eight-unit-deep niche with a permanent WAD back
         * wall at y=-1000; an eight-unit-radius player cannot stand inside. */
        const int expected_open_solid = door->sector == 84u;
        if (!closed_solid || open_solid != expected_open_solid) {
            fprintf(stderr, "door %u sector %u center collision closed=%d open=%d\n",
                    i, door->sector, closed_solid, open_solid);
            return 1;
        }
        game.door_open[i] = 0u;
    }

    const int blue = door_index(71);
    assert(blue >= 0 && e1m1_doors[blue].kind == E1M1_DOOR_BLUE);
    locate(&game, 832, 1450, 64); /* Face blue door sector 71. */
    assert(DoomLiteGame_IsSolid(&game, 832 * 256, 1488 * 256));
    assert(DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_NEED_BLUE);
    assert(!game.door_open[blue]);
    for (unsigned tick = 0; tick < 16; ++tick)
        DoomLiteGame_Step(&game, DL_GAME_UP);
    assert(game.y_q8 < 1480 * 256); /* Closed door stops movement. */
    DoomLiteGame_Step(&game, 0);

    locate(&game, 2192, 576, 0); /* WAD blue key THING 87. */
    assert(DoomLiteGame_Step(&game, 0) & DL_EVENT_BLUE_KEY);
    assert(game.blue_key && !DoomLiteGame_ThingActive(&game, 87));

    locate(&game, 832, 1450, 64);
    assert(DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_DOOR_OPEN);
    assert(game.door_open[blue]);
    assert(!DoomLiteGame_IsSolid(&game, 832 * 256, 1488 * 256));
    /* Holding F2 does not repeatedly toggle the door. */
    assert(!(DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_DOOR_CLOSE));
    locate(&game, 832, 1488, 64);
    DoomLiteGame_Step(&game, 0);
    assert(!(DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_DOOR_CLOSE));
    assert(game.door_open[blue]); /* Cannot seal the player in the door. */
    locate(&game, 832, 1450, 64);
    DoomLiteGame_Step(&game, 0);
    for (unsigned tick = 0; tick < 16; ++tick)
        DoomLiteGame_Step(&game, DL_GAME_UP);
    assert(game.y_q8 > 1500 * 256); /* Open door is traversable. */
    locate(&game, 832, 1450, 64);
    DoomLiteGame_Step(&game, 0);
    assert(DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_DOOR_CLOSE);
    assert(!game.door_open[blue]);
    assert(DoomLiteGame_IsSolid(&game, 832 * 256, 1488 * 256));

    /* The original E1M1 exit is a use switch on LINEDEF 407. */
    DoomLiteGame_Step(&game, 0);
    locate(&game, -430, 1296, 0);
    assert(DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_EXIT);
    assert(game.completed);
    assert(DoomLiteGame_Step(&game, DL_GAME_UP) == 0);

    /* One simplified 25-HP pistol hit kills a WAD medium zombieman. */
    DoomLiteGame combat;
    DoomLiteGame_Init(&combat);
    locate(&combat, 320, 1392, 0);
    const uint32_t shot = DoomLiteGame_Step(&combat, DL_GAME_FIRE);
    assert((shot & (DL_EVENT_SHOT_HIT | DL_EVENT_KILL)) ==
           (DL_EVENT_SHOT_HIT | DL_EVENT_KILL));
    assert(combat.kills == 1 && combat.ammo == 49);
    assert(!DoomLiteGame_ThingActive(&combat, 88));

    DoomLiteGame_Init(&combat);
    locate(&combat, 320, 1392, 0);
    uint32_t damage_event = 0;
    for (unsigned tick = 0; tick < DOOM_LITE_GAME_HZ; ++tick)
        damage_event |= DoomLiteGame_Step(&combat, 0);
    assert(damage_event & DL_EVENT_HURT);
    assert(combat.health < 100);

    printf("E1M1 game: medium THINGS, key, doors, exit, shot and HP OK\n");
    return 0;
}
