/* Controlled diagnostics live in their own tiny flat room. They do not
 * masquerade as an E1M1/E1M2 map or borrow either map's sidedef indices. */
#include "DoomTestMap.h"
#include "DoomTestMapData.h"
#include <limits.h>
#include <string.h>

static const char preset_names[DOOM_TEST_PRESET_COUNT][8] = {"SPRITES","COMBAT","ITEMS"};

const DoomMap *DoomTestMap_Get(void) { return &doom_test_map; }
unsigned DoomTestMap_PresetCount(void) { return DOOM_TEST_PRESET_COUNT; }
const char *DoomTestMap_PresetName(unsigned preset) {
    return preset<DOOM_TEST_PRESET_COUNT?preset_names[preset]:"INVALID";
}
unsigned DoomTestMap_ReadonlyBytes(void) {
    return (unsigned)(DOOM_TEST_MAP_DATA_BYTES+sizeof(preset_names)+sizeof("INVALID"));
}
int DoomTestMap_InitGame(DoomLiteGame *game,unsigned preset) {
    if(preset>=DOOM_TEST_PRESET_COUNT||!DoomLiteGame_InitData(game,&doom_test_map)) return 0;
    if(preset==DOOM_TEST_SPRITES) {
        game->freeze_actors=1u;game->invulnerable_tics=UINT16_MAX;
        game->total_enemies=0u;game->weapons|=4u;game->ammo=200u;game->shells=50u;
        /* Actors remain in the bounded visual array, but neither obstruct
         * movement nor become combat targets. Their poses are explicit. */
        for(unsigned i=0u;i<game->actor_count;++i) {game->actors[i].health=0;game->actors[i].flags=0u;}
        for(unsigned i=0u;i<game->pickup_count;++i) {
            const unsigned index=game->pickup_refs[i];
            game->collected[index>>3]|=(uint8_t)(1u<<(index&7u));
        }
        game->pickup_count=0u;
        DoomTestMap_StepPreview(game);
    } else if(preset==DOOM_TEST_COMBAT) {
        /* Start far enough away to see all four types before their first
         * hitscan attack. This is ordinary movement/combat, not preview AI. */
        for(unsigned i=0u;i<game->actor_count;++i) game->actors[i].x_q8=544*256;
    } else {
        for(unsigned i=0u;i<game->actor_count;++i) {
            const unsigned index=game->actors[i].thing_index;
            game->thing_actor[index]=255u;
            game->collected[index>>3]|=(uint8_t)(1u<<(index&7u));
        }
        game->actor_count=0u;game->total_enemies=0u;game->metrics.actor_high_water=0u;
    }
    return 1;
}
void DoomTestMap_StepPreview(DoomLiteGame *game) {
    if(!game||game->map!=&doom_test_map||!game->freeze_actors) return;
    const unsigned phase=game->ticks%128u;
    for(unsigned i=0u;i<game->actor_count;++i) {
        DoomLiteActor *actor=&game->actors[i];
        actor->health=0;actor->flags=0u;
        actor->facing=(uint8_t)(128u+((game->ticks/70u)&7u)*32u);
        if(phase<32u) {actor->state=DL_ACTOR_WALK;actor->frame=(uint8_t)(phase/8u);}
        else if(phase<53u) {actor->state=DL_ACTOR_ATTACK;actor->frame=(uint8_t)((phase-32u)/7u);}
        else if(phase<59u) {actor->state=DL_ACTOR_PAIN;actor->frame=0u;}
        else if(phase<83u) {actor->state=DL_ACTOR_DEATH;actor->frame=(uint8_t)((phase-59u)/4u);}
        else {actor->state=DL_ACTOR_CORPSE;actor->frame=6u;}
        actor->tics=0u;
    }
}
