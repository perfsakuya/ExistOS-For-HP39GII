#include "DoomMap.h"

#include <stddef.h>

#include "E1M1MapData.h"
#include "E1M2MapData.h"

_Static_assert(sizeof(DoomMapThing) == 12u, "DoomMapThing layout changed");
_Static_assert(sizeof(DoomMapLine) == 18u, "DoomMapLine layout changed");
_Static_assert(sizeof(DoomMapSector) == 32u, "DoomMapSector layout changed");
_Static_assert(sizeof(DoomMapNode) == 12u, "DoomMapNode layout changed");
_Static_assert(sizeof(DoomMapCell) == 6u, "DoomMapCell layout changed");

const DoomMap *DoomMap_Get(unsigned index) {
    if (index == 0u) return &e1m1_map;
    if (index == 1u) return &e1m2_map;
    return NULL;
}

static int32_t floor_cell(int32_t value) {
    return value >= 0 ? value / 32 : (value + 1) / 32 - 1;
}

const DoomMapCell *DoomMap_Cell(const DoomMap *map, int32_t gx, int32_t gy) {
    if (!map || gx < 0 || gy < 0 || gx >= map->grid_width ||
        gy >= map->grid_height) return NULL;
    unsigned first = map->row_first[gy];
    unsigned end = map->row_first[gy + 1];
    while (first < end) {
        const unsigned middle = first + (end - first) / 2u;
        const DoomMapCell *cell = &map->cells[middle];
        if (cell->x < (unsigned)gx) first = middle + 1u;
        else if (cell->x > (unsigned)gx) end = middle;
        else return cell;
    }
    return NULL;
}

const uint16_t *DoomMap_CellLines(const DoomMap *map, int32_t x, int32_t y,
                                uint16_t *count) {
    if (count) *count = 0;
    if (!map || x < map->grid_x0 || y < map->grid_y0 ||
        x >= map->grid_x0 + (int32_t)map->grid_width * 32 ||
        y >= map->grid_y0 + (int32_t)map->grid_height * 32) return NULL;
    const DoomMapCell *cell = DoomMap_Cell(map, floor_cell(x - map->grid_x0),
                                          floor_cell(y - map->grid_y0));
    if (!cell) return NULL;
    if (count) *count = cell->count;
    return map->cell_refs + cell->first_ref;
}

uint16_t DoomMap_SectorAt(const DoomMap *map, int32_t x, int32_t y) {
    if (!map || !map->subsector_count || x < map->grid_x0 ||
        y < map->grid_y0 || x >= map->grid_x0 + (int32_t)map->grid_width * 32 ||
        y >= map->grid_y0 + (int32_t)map->grid_height * 32)
        return DOOM_MAP_NO_SECTOR;
    if (!map->node_count) return map->subsector_sector[0];
    uint16_t child = (uint16_t)(map->node_count - 1u);
    for (unsigned steps = 0; steps <= map->node_count; ++steps) {
        if (child & 0x8000u) {
            const unsigned subsector = child & 0x7fffu;
            return subsector < map->subsector_count ?
                   map->subsector_sector[subsector] : DOOM_MAP_NO_SECTOR;
        }
        if (child >= map->node_count) return DOOM_MAP_NO_SECTOR;
        const DoomMapNode *node = &map->nodes[child];
        unsigned side;
        /* Match Doom's axis-aligned tie convention exactly. A cross product
         * uses 64 bits so full int16 coordinates cannot overflow on ARM. */
        if (!node->dx) side = x <= node->x ? node->dy > 0 : node->dy < 0;
        else if (!node->dy) side = y <= node->y ? node->dx < 0 : node->dx > 0;
        else side = ((int64_t)(y - node->y) * node->dx >=
                     (int64_t)(x - node->x) * node->dy);
        child = node->child[side];
    }
    return DOOM_MAP_NO_SECTOR;
}

const uint16_t *DoomMap_TagSectors(const DoomMap *map, uint16_t tag,
                                 uint16_t *count) {
    if (count) *count = 0;
    if (!map || !tag) return NULL;
    unsigned first = 0, end = map->tag_count;
    while (first < end) {
        const unsigned middle = first + (end - first) / 2u;
        const DoomMapTag *entry = &map->tags[middle];
        if (entry->tag < tag) first = middle + 1u;
        else if (entry->tag > tag) end = middle;
        else {
            if (count) *count = entry->count;
            return map->tag_sector_refs + entry->first_ref;
        }
    }
    return NULL;
}
