#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>

#include "DoomMap.h"

static void test_map(unsigned index) {
    const DoomMap *map = DoomMap_Get(index);
    assert(map);
    assert(DoomMap_SectorAt(map, map->start_x, map->start_y) == map->start_sector);
    assert(map->thing_count == (index == 0 ? 292u : 356u));
    assert(map->line_count == (index == 0 ? 1175u : 2290u));
    assert(map->sector_count == (index == 0 ? 182u : 380u));
    for (unsigned i = 0; i < map->thing_count; ++i) {
        const DoomMapThing *thing = &map->things[i];
        assert(DoomMap_SectorAt(map, thing->x, thing->y) == thing->sector);
    }
    unsigned line_samples = 0;
    for (unsigned i = 0; i < map->line_count; ++i) {
        const DoomMapLine *line = &map->lines[i];
        const int32_t xs[3] = {line->x1, line->x2, (line->x1 + line->x2) / 2};
        const int32_t ys[3] = {line->y1, line->y2, (line->y1 + line->y2) / 2};
        for (unsigned sample = 0; sample < 3; ++sample) {
            uint16_t count = 0;
            const uint16_t *refs = DoomMap_CellLines(map, xs[sample], ys[sample], &count);
            assert(refs && count);
            int found = 0;
            for (unsigned j = 0; j < count; ++j) {
                assert(refs[j] < map->line_count);
                if (refs[j] == i) found = 1;
            }
            assert(found);
            ++line_samples;
        }
    }
    for (unsigned y = 0; y < map->grid_height; ++y) {
        assert(map->row_first[y] <= map->row_first[y + 1]);
        for (unsigned c = map->row_first[y]; c < map->row_first[y + 1]; ++c) {
            const DoomMapCell *cell = &map->cells[c];
            assert(DoomMap_Cell(map, cell->x, y) == cell);
            assert(cell->first_ref + cell->count <= map->cell_ref_count);
        }
    }
    for (unsigned i = 0; i < map->tag_count; ++i) {
        uint16_t count = 0;
        const uint16_t *refs = DoomMap_TagSectors(map, map->tags[i].tag, &count);
        assert(count == map->tags[i].count);
        for (unsigned j = 0; j < count; ++j)
            assert(map->sectors[refs[j]].tag == map->tags[i].tag);
    }
    assert(DoomMap_SectorAt(map, INT32_MIN, INT32_MAX) == DOOM_MAP_NO_SECTOR);
    assert(DoomMap_Cell(map, -1, 0) == NULL);
    assert(DoomMap_Cell(map, map->grid_width, 0) == NULL);
    uint16_t count = 123;
    assert(DoomMap_CellLines(map, INT32_MIN, INT32_MAX, &count) == NULL && count == 0);
    assert(DoomMap_TagSectors(map, 0, &count) == NULL && count == 0);
    printf("%s: things=%u lines=%u sectors=%u sparse cells=%u refs=%u C line samples=%u\n",
           map->name, map->thing_count, map->line_count, map->sector_count,
           map->cell_count, map->cell_ref_count, line_samples);
}

int main(void) {
    test_map(0);
    test_map(1);
    assert(DoomMap_Get(2) == NULL);
    assert(DoomMap_SectorAt(NULL, 0, 0) == DOOM_MAP_NO_SECTOR);
    assert(DoomMap_Cell(NULL, 0, 0) == NULL);
    puts("DoomMap runtime guards and geometry checks passed");
    return 0;
}
