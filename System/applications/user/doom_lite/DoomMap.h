#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DOOM_MAP_COUNT 2u
#define DOOM_MAP_CELL_SIZE 32u
#define DOOM_MAP_NO_SECTOR UINT16_MAX
#define DOOM_MAP_NO_LINE UINT16_MAX
#define DOOM_MAP_MAX_THINGS 356u
#define DOOM_MAP_MAX_SECTORS 380u
#define DOOM_MAP_MAX_LINES 2290u

/* Immutable WAD records. References, tags, and sector IDs never truncate to
 * eight bits. Coordinates and heights are map units, not fixed point. */
typedef struct {
    int16_t x, y;
    uint16_t angle, type, options, sector;
} DoomMapThing;

typedef struct {
    int16_t x1, y1, x2, y2;
    uint16_t flags, special, tag, front_sector, back_sector;
} DoomMapLine;

typedef struct {
    int16_t floor, ceiling, light;
    uint16_t special, tag;
    int16_t lowest_floor, highest_floor, next_floor; /* Lowest includes self. */
    int16_t lowest_ceiling, highest_ceiling;
    uint16_t first_neighbor, neighbor_count;
    int16_t x0, y0, x1, y1;
} DoomMapSector;

typedef struct {
    int16_t x, y, dx, dy;
    uint16_t child[2]; /* 0x8000 marks a subsector index. */
} DoomMapNode;

typedef struct {
    uint16_t x, first_ref, count;
} DoomMapCell;

typedef struct {
    uint16_t tag, first_ref, count;
} DoomMapTag;

typedef struct {
    const char *name;
    uint16_t index;
    int16_t start_x, start_y;
    uint16_t start_angle, start_sector;
    int16_t grid_x0, grid_y0; /* World coordinate of cell (0,0). */
    uint16_t grid_width, grid_height;
    uint16_t thing_count, line_count, sector_count, node_count;
    uint16_t subsector_count, cell_count, cell_ref_count;
    uint16_t special_line_count, tag_count, neighbor_ref_count;
    const DoomMapThing *things;
    const DoomMapLine *lines;
    const DoomMapSector *sectors;
    const DoomMapNode *nodes;
    const uint16_t *subsector_sector;
    const uint16_t *row_first;
    const DoomMapCell *cells;
    const uint16_t *cell_refs;
    const uint16_t *special_line_refs;
    const DoomMapTag *tags;
    const uint16_t *tag_sector_refs;
    const uint16_t *neighbor_refs;
} DoomMap;

const DoomMap *DoomMap_Get(unsigned index);
uint16_t DoomMap_SectorAt(const DoomMap *map, int32_t x, int32_t y);
const DoomMapCell *DoomMap_Cell(const DoomMap *map, int32_t gx, int32_t gy);
const uint16_t *DoomMap_CellLines(const DoomMap *map, int32_t x, int32_t y,
                                uint16_t *count);
const uint16_t *DoomMap_TagSectors(const DoomMap *map, uint16_t tag,
                                 uint16_t *count);

#ifdef __cplusplus
}
#endif
