/* Small, E1M1-specific gameplay approximation driven by Freedoom's WAD
 * THINGS and LINEDEFS. It deliberately does not reproduce GBADoom AI,
 * sector thinkers, damage tables, inventory, or demo-compatible movement. */
#include "DoomLiteGame.h"

#include <string.h>

#include "E1M1Gameplay.h"
#include "E1M1Grid.h"
#include "E1M1Portals.h"

_Static_assert(DOOM_LITE_GAME_THINGS == E1M1_THING_COUNT,
               "E1M1 THINGS count changed");
_Static_assert(DOOM_LITE_GAME_DOORS == E1M1_DOOR_COUNT,
               "E1M1 door count changed");
_Static_assert(DOOM_LITE_GAME_DOORS == E1M1_PORTAL_DOOR_COUNT,
               "E1M1 portal door count changed");
_Static_assert(E1M1_WIDTH == E1M1_PORTAL_WIDTH &&
               E1M1_HEIGHT == E1M1_PORTAL_HEIGHT &&
               E1M1_GX0 == E1M1_PORTAL_GX0 &&
               E1M1_GY0 == E1M1_PORTAL_GY0 &&
               E1M1_CELL == E1M1_PORTAL_CELL_SIZE,
               "E1M1 grid and portal coordinates differ");

#define PLAYER_RADIUS 8
#define PICKUP_REACH 24

/* The same Q14 quarter-wave convention as DoomLite's renderer. */
static const int16_t quarter_sine[65] = {
       0, 402, 804, 1205, 1606, 2006, 2404, 2801, 3196, 3590, 3981,
    4370, 4756, 5139, 5520, 5897, 6270, 6639, 7005, 7366, 7723, 8076,
    8423, 8765, 9102, 9434, 9760, 10080, 10394, 10702, 11003, 11297,
    11585, 11866, 12140, 12406, 12665, 12916, 13160, 13395, 13623,
    13842, 14053, 14256, 14449, 14635, 14811, 14978, 15137, 15286,
    15426, 15557, 15679, 15791, 15893, 15986, 16069, 16143, 16207,
    16261, 16305, 16340, 16364, 16379, 16384,
};

static int sine_q14(uint8_t angle) {
    const unsigned quadrant = angle >> 6;
    const unsigned index = angle & 63u;
    if (quadrant == 0u) return quarter_sine[index];
    if (quadrant == 1u) return quarter_sine[64u - index];
    if (quadrant == 2u) return -quarter_sine[index];
    return -quarter_sine[64u - index];
}

static int32_t floor_div(int32_t number, int32_t divisor) {
    return number >= 0 ? number / divisor : (number + 1) / divisor - 1;
}

static int active_on_medium(const E1M1Thing *thing) {
    return (thing->options & 2u) != 0u && (thing->options & 16u) == 0u;
}

static uint8_t starting_hp(uint16_t type) {
    switch (type) {
        case 3004u: return 20u;  /* former human */
        case 9u: return 30u;     /* shotgun guy */
        case 3001u: return 60u;  /* imp */
        case 3002u: return 150u; /* demon */
        default: return 0u;
    }
}

static int collected(const DoomLiteGame *game, unsigned index) {
    return (game->collected[index >> 3] >> (index & 7u)) & 1u;
}

static void collect(DoomLiteGame *game, unsigned index) {
    game->collected[index >> 3] |= (uint8_t)(1u << (index & 7u));
}

static int door_index_for_sector(uint8_t sector) {
    if (sector >= E1M1_PORTAL_SECTOR_COUNT) return -1;
    const unsigned index = e1m1_door_index_by_sector[sector];
    return index == E1M1_PORTAL_NO_DOOR ? -1 : (int)index;
}

static const E1M1PortalCell *portal_cell(unsigned gx, unsigned gy) {
    const unsigned first = e1m1_portal_row_first[gy];
    const unsigned end = e1m1_portal_row_first[gy + 1u];
    for (unsigned i = first; i < end; ++i) {
        if (e1m1_portal_cells[i].x == gx) return &e1m1_portal_cells[i];
        if (e1m1_portal_cells[i].x > gx) break;
    }
    return NULL;
}

static int64_t square(int64_t value) { return value * value; }

static int point_near_segment(int32_t x, int32_t y,
                              int32_t x1, int32_t y1,
                              int32_t x2, int32_t y2, int32_t radius) {
    const int64_t dx = x2 - x1, dy = y2 - y1;
    const int64_t vx = x - x1, vy = y - y1;
    const int64_t length2 = dx * dx + dy * dy;
    const int64_t projection = vx * dx + vy * dy;
    const int64_t radius2 = (int64_t)radius * radius;
    if (!length2 || projection <= 0)
        return square(vx) + square(vy) <= radius2;
    if (projection >= length2)
        return square(x - x2) + square(y - y2) <= radius2;
    const int64_t cross = vx * dy - vy * dx;
    return square(cross) <= radius2 * length2;
}

int DoomLiteGame_IsSolid(const DoomLiteGame *game, int32_t x_q8, int32_t y_q8) {
    const int32_t x = floor_div(x_q8, 256);
    const int32_t y = floor_div(y_q8, 256);
    for (unsigned i = 0; i < E1M1_DOOR_COUNT; ++i) {
        if (game->door_open[i]) continue;
        const E1M1Door *door = &e1m1_doors[i];
        const int32_t closest_x = x < door->x0 ? door->x0 :
                                  x > door->x1 ? door->x1 : x;
        const int32_t closest_y = y < door->y0 ? door->y0 :
                                  y > door->y1 ? door->y1 : y;
        if (square(x - closest_x) + square(y - closest_y) <=
            PLAYER_RADIUS * PLAYER_RADIUS) return 1;
    }
    const int32_t gx0 = floor_div(x - PLAYER_RADIUS, E1M1_CELL) - E1M1_GX0;
    const int32_t gx1 = floor_div(x + PLAYER_RADIUS, E1M1_CELL) - E1M1_GX0;
    const int32_t gy0 = floor_div(y - PLAYER_RADIUS, E1M1_CELL) - E1M1_GY0;
    const int32_t gy1 = floor_div(y + PLAYER_RADIUS, E1M1_CELL) - E1M1_GY0;
    for (int32_t gy = gy0; gy <= gy1; ++gy) {
        for (int32_t gx = gx0; gx <= gx1; ++gx) {
            if ((uint32_t)gx >= E1M1_WIDTH || (uint32_t)gy >= E1M1_HEIGHT)
                return 1;
            const E1M1PortalCell *portal = portal_cell((unsigned)gx,
                                                        (unsigned)gy);
            if (!portal) {
                if (e1m1_grid[gy * E1M1_WIDTH + gx]) return 1;
                continue;
            }
            /* A portal cell replaces the coarse occupancy bit with exact
             * permanent walls and closed door lines from the original WAD. */
            for (unsigned j = 0; j < portal->ref_count; ++j) {
                const unsigned ref = e1m1_portal_refs[portal->first_ref + j];
                const E1M1PortalSegment *line = &e1m1_portal_segments[ref];
                if (line->door_sector != E1M1_PORTAL_NO_DOOR) {
                    const int door = door_index_for_sector(line->door_sector);
                    if (door >= 0 && game->door_open[door]) continue;
                }
                if (point_near_segment(x, y, line->x1, line->y1,
                                       line->x2, line->y2, PLAYER_RADIUS))
                    return 1;
            }
        }
    }
    return 0;
}

int DoomLiteGame_ThingActive(const DoomLiteGame *game, unsigned thing_index) {
    if (thing_index >= E1M1_THING_COUNT) return 0;
    const E1M1Thing *thing = &e1m1_things[thing_index];
    if (!active_on_medium(thing) || thing->type <= 4u) return 0;
    if (starting_hp(thing->type)) return game->enemy_hp[thing_index] != 0;
    return !collected(game, thing_index);
}

void DoomLiteGame_Init(DoomLiteGame *game) {
    memset(game, 0, sizeof(*game));
    game->health = 100u;
    game->ammo = 50u;
    for (unsigned i = 0; i < E1M1_THING_COUNT; ++i) {
        const E1M1Thing *thing = &e1m1_things[i];
        if (thing->type == 1u) {
            game->x_q8 = (int32_t)thing->x * 256;
            game->y_q8 = (int32_t)thing->y * 256;
            game->facing = (uint8_t)((unsigned)thing->angle * 256u / 360u);
        }
        if (active_on_medium(thing)) {
            game->enemy_hp[i] = starting_hp(thing->type);
            if (game->enemy_hp[i]) ++game->total_enemies;
        }
    }
}

static void closest_on_segment(int32_t x, int32_t y,
                               int32_t x1, int32_t y1,
                               int32_t x2, int32_t y2,
                               int32_t *closest_x, int32_t *closest_y) {
    const int64_t dx = x2 - x1, dy = y2 - y1;
    const int64_t length2 = dx * dx + dy * dy;
    int64_t projection = (int64_t)(x - x1) * dx + (int64_t)(y - y1) * dy;
    if (projection < 0) projection = 0;
    if (projection > length2) projection = length2;
    *closest_x = x1 + (int32_t)(length2 ? dx * projection / length2 : 0);
    *closest_y = y1 + (int32_t)(length2 ? dy * projection / length2 : 0);
}

static int looking_toward(const DoomLiteGame *game, int32_t x, int32_t y) {
    const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
    const int32_t dx = x - px, dy = y - py;
    if (square(dx) + square(dy) <= 64) return 1;
    const int32_t dirx = sine_q14((uint8_t)(game->facing + 64u));
    const int32_t diry = sine_q14(game->facing);
    return (int64_t)dx * dirx + (int64_t)dy * diry > 0;
}

static int door_for_special(const E1M1SpecialLine *line) {
    if (line->special == 2u) {
        /* In this E1M1, tag 5 opens sector 77; tag 6 opens sector 145. */
        return door_index_for_sector(line->tag == 5u ? 77u :
                                     line->tag == 6u ? 145u : 255u);
    }
    return door_index_for_sector(line->back_sector);
}

static uint32_t toggle_door(DoomLiteGame *game, int door) {
    if (door < 0) return 0;
    if (e1m1_doors[door].kind == E1M1_DOOR_BLUE && !game->blue_key)
        return DL_EVENT_NEED_BLUE;
    if (game->door_open[door]) {
        const E1M1Door *rect = &e1m1_doors[door];
        const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
        const int32_t cx = px < rect->x0 ? rect->x0 :
                           px > rect->x1 ? rect->x1 : px;
        const int32_t cy = py < rect->y0 ? rect->y0 :
                           py > rect->y1 ? rect->y1 : py;
        /* Instant-close rules must not seal the player inside the sector. */
        if (square(px - cx) + square(py - cy) <=
            PLAYER_RADIUS * PLAYER_RADIUS) return 0;
    }
    game->door_open[door] ^= 1u;
    return game->door_open[door] ? DL_EVENT_DOOR_OPEN : DL_EVENT_DOOR_CLOSE;
}

static uint32_t use_nearest(DoomLiteGame *game) {
    const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
    int best_line = -1;
    int64_t best_distance2 = (int64_t)DOOM_LITE_GAME_USE_REACH *
                             DOOM_LITE_GAME_USE_REACH + 1;
    for (unsigned i = 0; i < E1M1_SPECIAL_LINE_COUNT; ++i) {
        const E1M1SpecialLine *line = &e1m1_special_lines[i];
        if (line->special != 1u && line->special != 2u &&
            line->special != 11u && line->special != 26u &&
            line->special != 117u) continue;
        int32_t cx, cy;
        closest_on_segment(px, py, line->x1, line->y1, line->x2, line->y2,
                           &cx, &cy);
        const int64_t distance2 = square(cx - px) + square(cy - py);
        if (distance2 < best_distance2 && looking_toward(game, cx, cy)) {
            best_distance2 = distance2;
            best_line = (int)i;
        }
    }
    if (best_line >= 0) {
        const E1M1SpecialLine *line = &e1m1_special_lines[best_line];
        if (line->special == 11u) {
            game->completed = 1u;
            return DL_EVENT_EXIT;
        }
        return toggle_door(game, door_for_special(line));
    }
    /* The two walk-triggered doors have no nearby use line. F2 can operate
     * their actual door rectangle to keep all 15 doors accessible. */
    int best_door = -1;
    best_distance2 = (int64_t)DOOM_LITE_GAME_USE_REACH *
                     DOOM_LITE_GAME_USE_REACH + 1;
    for (unsigned i = 0; i < E1M1_DOOR_COUNT; ++i) {
        const E1M1Door *door = &e1m1_doors[i];
        const int32_t cx = px < door->x0 ? door->x0 :
                           px > door->x1 ? door->x1 : px;
        const int32_t cy = py < door->y0 ? door->y0 :
                           py > door->y1 ? door->y1 : py;
        const int64_t distance2 = square(cx - px) + square(cy - py);
        if (distance2 < best_distance2 && looking_toward(game, cx, cy)) {
            best_distance2 = distance2;
            best_door = (int)i;
        }
    }
    return toggle_door(game, best_door);
}

static int segments_cross(int32_t ax, int32_t ay, int32_t bx, int32_t by,
                          int32_t cx, int32_t cy, int32_t dx, int32_t dy) {
    if ((ax < cx && bx < cx && ax < dx && bx < dx) ||
        (ax > cx && bx > cx && ax > dx && bx > dx) ||
        (ay < cy && by < cy && ay < dy && by < dy) ||
        (ay > cy && by > cy && ay > dy && by > dy)) return 0;
    const int64_t vx = bx - ax, vy = by - ay;
    const int64_t wx = dx - cx, wy = dy - cy;
    const int64_t a = vx * (cy - ay) - vy * (cx - ax);
    const int64_t b = vx * (dy - ay) - vy * (dx - ax);
    const int64_t c = wx * (ay - cy) - wy * (ax - cx);
    const int64_t d = wx * (by - cy) - wy * (bx - cx);
    return ((a <= 0 && b >= 0) || (a >= 0 && b <= 0)) &&
           ((c <= 0 && d >= 0) || (c >= 0 && d <= 0));
}

static uint32_t trigger_walk_lines(DoomLiteGame *game,
                                   int32_t old_x_q8, int32_t old_y_q8) {
    const int32_t ax = old_x_q8 / 256, ay = old_y_q8 / 256;
    const int32_t bx = game->x_q8 / 256, by = game->y_q8 / 256;
    if (ax == bx && ay == by) return 0;
    uint32_t events = 0;
    for (unsigned i = 0; i < E1M1_SPECIAL_LINE_COUNT; ++i) {
        const E1M1SpecialLine *line = &e1m1_special_lines[i];
        if (line->special != 2u) continue;
        if (segments_cross(ax, ay, bx, by, line->x1, line->y1,
                           line->x2, line->y2)) {
            const int door = door_for_special(line);
            if (door >= 0 && !game->door_open[door]) {
                game->door_open[door] = 1u;
                events |= DL_EVENT_DOOR_OPEN;
            }
        }
    }
    return events;
}

static uint32_t pick_up_things(DoomLiteGame *game) {
    const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
    uint32_t events = 0;
    for (unsigned i = 0; i < E1M1_THING_COUNT; ++i) {
        const E1M1Thing *thing = &e1m1_things[i];
        if (!active_on_medium(thing) || collected(game, i)) continue;
        const int32_t dx = thing->x - px, dy = thing->y - py;
        if (square(dx) + square(dy) > PICKUP_REACH * PICKUP_REACH) continue;
        switch (thing->type) {
            case 5u: /* blue key */
                game->blue_key = 1u;
                events |= DL_EVENT_BLUE_KEY;
                break;
            case 2007u: /* ammo clip */
            case 2048u: { /* box of bullets */
                unsigned ammo = game->ammo + (thing->type == 2007u ? 10u : 50u);
                game->ammo = (uint8_t)(ammo > 200u ? 200u : ammo);
                events |= DL_EVENT_PICKUP;
                break;
            }
            case 2011u: /* stimpack */
            case 2012u: { /* medikit */
                unsigned health = game->health + (thing->type == 2011u ? 10u : 25u);
                game->health = (uint8_t)(health > 100u ? 100u : health);
                events |= DL_EVENT_PICKUP;
                break;
            }
            default:
                continue;
        }
        collect(game, i);
    }
    return events;
}

static int line_of_sight(const DoomLiteGame *game, int32_t tx, int32_t ty) {
    const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
    const int32_t dx = tx - px, dy = ty - py;
    const int32_t span = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy)
        ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    const int32_t steps = span / 16;
    for (int32_t step = 1; step < steps; ++step) {
        const int32_t x = (px + dx * step / steps) * 256;
        const int32_t y = (py + dy * step / steps) * 256;
        if (DoomLiteGame_IsSolid(game, x, y)) return 0;
    }
    return 1;
}

static uint32_t fire_pistol(DoomLiteGame *game) {
    if (!game->ammo) return DL_EVENT_SHOT_MISS;
    --game->ammo;
    game->pistol_tics = 15u;
    const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
    const int32_t dirx = sine_q14((uint8_t)(game->facing + 64u));
    const int32_t diry = sine_q14(game->facing);
    int best = -1;
    int64_t best_forward = (int64_t)640 * 16384 + 1;
    for (unsigned i = 0; i < E1M1_THING_COUNT; ++i) {
        if (!game->enemy_hp[i]) continue;
        const int32_t dx = e1m1_things[i].x - px;
        const int32_t dy = e1m1_things[i].y - py;
        const int64_t forward = (int64_t)dx * dirx + (int64_t)dy * diry;
        if (forward <= 0 || forward >= best_forward) continue;
        const int64_t sideways = (int64_t)dx * diry - (int64_t)dy * dirx;
        const int64_t tolerance = (int64_t)12 * 16384 + forward / 12;
        if (sideways < -tolerance || sideways > tolerance) continue;
        if (!line_of_sight(game, e1m1_things[i].x, e1m1_things[i].y)) continue;
        best_forward = forward;
        best = (int)i;
    }
    if (best < 0) return DL_EVENT_SHOT_MISS;
    if (game->enemy_hp[best] <= 25u) {
        game->enemy_hp[best] = 0;
        ++game->kills;
        return DL_EVENT_SHOT_HIT | DL_EVENT_KILL;
    }
    game->enemy_hp[best] -= 25u;
    return DL_EVENT_SHOT_HIT;
}

static uint32_t enemy_attack(DoomLiteGame *game) {
    if (game->ticks % DOOM_LITE_GAME_HZ || !game->health) return 0;
    const int32_t px = game->x_q8 / 256, py = game->y_q8 / 256;
    int nearest = -1;
    int64_t best_distance2 = (int64_t)256 * 256;
    for (unsigned i = 0; i < E1M1_THING_COUNT; ++i) {
        if (!game->enemy_hp[i]) continue;
        const int32_t dx = e1m1_things[i].x - px;
        const int32_t dy = e1m1_things[i].y - py;
        const int64_t distance2 = square(dx) + square(dy);
        if (distance2 >= best_distance2) continue;
        if (!line_of_sight(game, e1m1_things[i].x, e1m1_things[i].y)) continue;
        best_distance2 = distance2;
        nearest = (int)i;
    }
    if (nearest < 0) return 0;
    const unsigned damage = e1m1_things[nearest].type == 3002u ? 7u : 4u;
    game->health = (uint8_t)(game->health > damage ? game->health - damage : 0u);
    return DL_EVENT_HURT;
}

uint32_t DoomLiteGame_Step(DoomLiteGame *game, uint16_t buttons) {
    ++game->ticks;
    if (game->pistol_tics) --game->pistol_tics;
    const uint16_t pressed = buttons & ~game->previous_buttons;
    game->previous_buttons = (uint8_t)buttons;
    if (game->completed || !game->health) return 0;
    if (!!(buttons & DL_GAME_LEFT) != !!(buttons & DL_GAME_RIGHT)) {
        /* 1.5 angle steps/tic is 25% slower than the original two. Keep
         * signed half-steps so opposite turns cancel without angle drift. */
        const int half_steps = game->turn_remainder +
                               ((buttons & DL_GAME_LEFT) ? -3 : 3);
        const int whole_steps = half_steps / 2;
        game->facing = (uint8_t)(game->facing + whole_steps);
        game->turn_remainder = (int8_t)(half_steps - whole_steps * 2);
    }

    uint32_t events = 0;
    if (!!(buttons & DL_GAME_UP) != !!(buttons & DL_GAME_DOWN)) {
        const int direction = buttons & DL_GAME_UP ? 1 : -1;
        const int32_t old_x = game->x_q8, old_y = game->y_q8;
        const int32_t dx = (sine_q14((uint8_t)(game->facing + 64u)) *
                            4 * 256 * direction) / 16384;
        const int32_t dy = (sine_q14(game->facing) *
                            4 * 256 * direction) / 16384;
        if (!DoomLiteGame_IsSolid(game, game->x_q8 + dx, game->y_q8))
            game->x_q8 += dx;
        if (!DoomLiteGame_IsSolid(game, game->x_q8, game->y_q8 + dy))
            game->y_q8 += dy;
        events |= trigger_walk_lines(game, old_x, old_y);
    }
    events |= pick_up_things(game);
    if (pressed & DL_GAME_USE) events |= use_nearest(game);
    if (pressed & DL_GAME_FIRE) events |= fire_pistol(game);
    if (!game->completed) events |= enemy_attack(game);
    return events;
}
