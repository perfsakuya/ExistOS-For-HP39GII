/* Portable gameplay, sector-special, actor and bounded-work replays.
 * gcc -std=c11 -O2 -Wall -Wextra -Werror tools/doom/check_lite_game.c
 * System/applications/user/doom_lite/DoomLiteGame.c
 * System/applications/user/doom_lite/DoomMap.c -o check_lite_game.exe */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../../System/applications/user/doom_lite/DoomLiteGame.h"
static DoomLiteGame game;
static void locate(DoomLiteGame *g,int x,int y,uint8_t angle) {
    g->x_q8=x*256;g->y_q8=y*256;g->facing=angle;g->player_sector=DoomLiteGame_PlayerSector(g);g->previous_buttons=0;
}
static void quiet(DoomLiteGame *g) {g->invulnerable_tics=UINT16_MAX;g->actor_count=0;}
static void ticks(DoomLiteGame *g,unsigned n) {while(n--) (void)DoomLiteGame_Step(g,0);}
static void approach_line(DoomLiteGame *g,unsigned index,unsigned distance) {
    const DoomMapLine *l=&g->map->lines[index];int x=(l->x1+l->x2)/2,y=(l->y1+l->y2)/2;
    int dx=l->x2-l->x1,dy=l->y2-l->y1,span=dx<0?-dx:dx;
    int absdy=dy<0?-dy:dy;if(absdy>span) span=absdy;if(!span) span=1;
    int nx=dy*(int)distance/span,ny=-dx*(int)distance/span;
    uint8_t angle=(uint8_t)(nx>0?128:nx<0?0:ny>0?192:64);
    locate(g,x+nx,y+ny,angle);
}
static uint32_t use_line(DoomLiteGame *g,unsigned index) {
    approach_line(g,index,16);return DoomLiteGame_Step(g,DL_GAME_USE);
}
static unsigned bit_value(const uint8_t *b,unsigned i) {return (b[i>>3]>>(i&7u))&1u;}
static void map_counts_and_reset(void) {
    for(unsigned n=0;n<40;++n) for(unsigned map=0;map<2;++map) {
        assert(DoomLiteGame_InitMap(&game,map));
        assert(game.actor_count==(map?93u:29u)&&game.total_enemies==game.actor_count);
        assert(game.health==100&&game.ammo==50&&game.contrast==1&&!game.metrics.pool_overflows);
        assert(game.player_sector==game.map->start_sector);
        assert(!DoomLiteGame_IsSolid(&game,game.x_q8,game.y_q8));
        assert(game.map->sector_count==(map?380u:182u));
        unsigned spectres=0;
        for(unsigned i=0;i<game.actor_count;++i) {
            DoomLiteVisual v;assert(DoomLiteGame_GetVisual(&game,i,&v));
            assert(v.state==DL_ACTOR_WALK&&v.frame==0&&v.sector<game.map->sector_count);
            if(v.thing_type==58) ++spectres;
        }
        assert(map?spectres>0:spectres==0);
        assert(!DoomLiteGame_GetVisual(&game,game.actor_count,NULL));
        game.health=73;game.ammo=112;game.armor=51;game.armor_class=1;
        game.shells=29;game.weapons=15;game.blue_key=game.red_key=game.yellow_key=1;
        if(!map) {assert(DoomLiteGame_NextMap(&game));assert(game.map_index==1&&game.health==73&&game.ammo==112&&game.armor==51&&game.shells==29&&game.weapons==15);assert(!game.blue_key&&!game.red_key&&!game.yellow_key&&!game.kills);}
    }
    assert(!DoomLiteGame_InitMap(&game,2));
}
static void turn_and_buttons(void) {
    assert(DoomLiteGame_InitMap(&game,0));quiet(&game);
    uint8_t start=game.facing;int32_t x=game.x_q8,y=game.y_q8;
    for(unsigned n=0;n<70;++n) DoomLiteGame_Step(&game,DL_GAME_RIGHT);
    assert(game.facing==(uint8_t)(start+105));
    for(unsigned n=0;n<70;++n) DoomLiteGame_Step(&game,DL_GAME_LEFT);
    assert(game.facing==start&&game.turn_remainder==0&&game.x_q8==x&&game.y_q8==y&&game.ammo==50);
    DoomLiteGame_Step(&game,DL_GAME_FIRE);assert(game.ammo==49&&game.pistol_tics==15);
    for(unsigned n=0;n<15;++n) DoomLiteGame_Step(&game,DL_GAME_FIRE);
    assert(game.ammo==49&&!game.pistol_tics);
    DoomLiteGame_Step(&game,0);game.ammo=0;DoomLiteGame_Step(&game,DL_GAME_FIRE);assert(!game.pistol_tics);
}
static void keys_and_doors(void) {
    for(unsigned map=0;map<2;++map) {
        assert(DoomLiteGame_InitMap(&game,map));quiet(&game);
        for(unsigned color=0;color<3;++color) {
            unsigned special=color==0?26:color==1?28:27;
            int ref=-1;
            for(unsigned i=0;i<game.map->special_line_count;++i) if(game.map->lines[game.map->special_line_refs[i]].special==special) {ref=game.map->special_line_refs[i];break;}
            if(ref<0) {assert(!map&&color>0);continue;}
            uint32_t need=color==0?DL_EVENT_NEED_BLUE:color==1?DL_EVENT_NEED_RED:DL_EVENT_NEED_YELLOW;
            assert(use_line(&game,(unsigned)ref)&need);
            unsigned sector=game.map->lines[ref].back_sector;
            assert(game.sector_state[sector].ceiling==game.map->sectors[sector].ceiling);
            if(color==0) game.blue_key=1;else if(color==1) game.red_key=1;else game.yellow_key=1;
            assert(use_line(&game,(unsigned)ref)&DL_EVENT_DOOR_OPEN);
            int closed=game.sector_state[sector].ceiling;
            ticks(&game,10);assert(game.sector_state[sector].ceiling>closed&&game.sector_state[sector].ceiling<=game.map->sectors[sector].lowest_ceiling-4);
            ticks(&game,70);assert(DoomLiteGame_DoorLiftQ8(&game,(uint16_t)sector)==256u);
        }
    }
    assert(DoomLiteGame_InitMap(&game,0));quiet(&game);
    locate(&game,2192,576,0);assert(DoomLiteGame_Step(&game,0)&DL_EVENT_BLUE_KEY);assert(game.blue_key&&!DoomLiteGame_ThingActive(&game,87));
    assert(DoomLiteGame_InitMap(&game,1));quiet(&game);
    const unsigned key_refs[3]={15,104,164};
    for(unsigned n=0;n<3;++n) {
        const DoomMapThing *key=&game.map->things[key_refs[n]];locate(&game,key->x,key->y,0);
        uint32_t expected=n==0?DL_EVENT_BLUE_KEY:n==1?DL_EVENT_RED_KEY:DL_EVENT_YELLOW_KEY;
        game.ticks=0;assert(DoomLiteGame_Step(&game,0)&expected);assert(!DoomLiteGame_ThingActive(&game,key_refs[n]));
    }
    assert(game.blue_key&&game.red_key&&game.yellow_key);
    assert(DoomLiteGame_InitMap(&game,1));quiet(&game);
    locate(&game,672,-2936,192);assert(game.player_sector==141u);
    assert(game.sector_state[99].floor-game.sector_state[141].floor==32);
    assert(DoomLiteGame_Step(&game,0)&DL_EVENT_RED_KEY);
    assert(game.red_key); /* Reach the ledge without violating step height. */
}
static void specials_and_exit(void) {
    assert(DoomLiteGame_InitMap(&game,1));quiet(&game);
    int before=game.sector_state[45].floor;
    assert(use_line(&game,34)&DL_EVENT_MOVER);ticks(&game,80);assert(before==304&&game.sector_state[45].floor==240);
    assert(bit_value(game.triggered,34));assert(!(use_line(&game,34)&DL_EVENT_MOVER));
    assert(use_line(&game,1904)&DL_EVENT_MOVER);ticks(&game,40);assert(game.sector_state[301].floor==-40);
    for(unsigned i=0;i<game.map->special_line_count;++i) {
        unsigned ref=game.map->special_line_refs[i];if(game.map->lines[ref].special!=123u) continue;
        uint16_t count=0;const uint16_t *sectors=DoomMap_TagSectors(game.map,game.map->lines[ref].tag,&count);
        if(!count||game.map->sectors[sectors[0]].lowest_floor>=game.sector_state[sectors[0]].floor) continue;
        unsigned s=sectors[0];int original=game.sector_state[s].floor;
        assert(use_line(&game,ref)&DL_EVENT_MOVER);ticks(&game,20);assert(game.sector_state[s].floor<original);
        ticks(&game,250);assert(game.sector_state[s].floor==original);break;
    }
    assert(use_line(&game,737)&DL_EVENT_EXIT);assert(game.completed);
    int32_t x=game.x_q8;assert(!DoomLiteGame_Step(&game,DL_GAME_UP)&&game.x_q8==x);
    assert(DoomLiteGame_InitMap(&game,0));quiet(&game);assert(use_line(&game,407)&DL_EVENT_EXIT);
}
static void combat_and_animation(void) {
    assert(DoomLiteGame_InitMap(&game,0));game.invulnerable_tics=UINT16_MAX;
    locate(&game,320,1392,0);unsigned actor=game.thing_actor[88];assert(actor<game.actor_count);
    uint32_t hit=DoomLiteGame_Step(&game,DL_GAME_FIRE);
    assert((hit&(DL_EVENT_SHOT_HIT|DL_EVENT_KILL))==(DL_EVENT_SHOT_HIT|DL_EVENT_KILL));
    assert(game.kills==1&&game.actors[actor].state==DL_ACTOR_DEATH&&!game.actors[actor].health&&!(game.actors[actor].flags&DL_ACTOR_SOLID));
    ticks(&game,30);assert(game.actors[actor].state==DL_ACTOR_CORPSE&&game.actors[actor].frame==6);
    DoomLiteVisual corpse;assert(DoomLiteGame_GetVisual(&game,actor,&corpse)&&corpse.state==DL_ACTOR_CORPSE);
    unsigned kills=game.kills;for(unsigned n=0;n<4;++n) {DoomLiteGame_Step(&game,DL_GAME_FIRE);DoomLiteGame_Step(&game,0);}assert(game.kills>=kills&&game.kills<=game.total_enemies);
    assert(DoomLiteGame_InitMap(&game,0));locate(&game,320,1392,0);
    int32_t ax=game.actors[actor].x_q8,ay=game.actors[actor].y_q8;ticks(&game,70);
    assert(game.metrics.actor_moves>0&&game.metrics.awake_actors>0&&game.health<100);
    assert(ax!=game.actors[actor].x_q8||ay!=game.actors[actor].y_q8||game.actors[actor].state==DL_ACTOR_ATTACK);
    assert(game.metrics.ai_visits<=game.ticks*DOOM_LITE_GAME_AI_BUDGET);
    /* Public solid queries now include living actors, including an actor
     * queried at its own center, so geometry checks use the route replay. */
}
static void player_actor_collision(void) {
    assert(DoomLiteGame_InitMap(&game,0));game.actor_count=1;
    DoomLiteActor *a=&game.actors[0];a->x_q8=game.x_q8+16*256;a->y_q8=game.y_q8;
    a->health=20;a->flags=DL_ACTOR_SOLID;a->sector=game.player_sector;
    int32_t x=game.x_q8;DoomLiteGame_Step(&game,DL_GAME_UP);assert(game.x_q8==x);
    a->health=0;a->flags=0;a->state=DL_ACTOR_CORPSE;
    DoomLiteGame_Step(&game,DL_GAME_UP);assert(game.x_q8==x+4*256);
}
static void narrow_passage_and_corner_trigger(void) {
    static const DoomMapSector sector={.floor=0,.ceiling=128,.highest_floor=-8};
    static const uint16_t subsector=0;
    static const DoomMapLine walls[2]={
        {24,0,24,64,1,0,0,0,DOOM_MAP_NO_SECTOR},
        {40,0,40,64,1,0,0,0,DOOM_MAP_NO_SECTOR}};
    static const uint16_t wall_rows[4]={0,2,4,6},wall_refs[2]={0,1};
    static const DoomMapCell wall_cells[6]={
        {0,0,2},{1,0,2},{0,0,2},{1,0,2},{0,0,2},{1,0,2}};
    DoomMap map={.name="narrow",.grid_width=3,.grid_height=3,.sector_count=1,
        .line_count=2,.subsector_count=1,.subsector_sector=&subsector,
        .sectors=&sector,.lines=walls,.row_first=wall_rows,.cells=wall_cells,.cell_refs=wall_refs};
    memset(&game,0,sizeof(game));game.map=&map;game.health=100;
    game.sector_state[0]=(DoomLiteSectorState){0,128,0};locate(&game,32,32,64);
    assert(!DoomLiteGame_IsSolid(&game,32*256,32*256));
    assert(DoomLiteGame_IsSolid(&game,31*256,32*256));
    assert(DoomLiteGame_IsSolid(&game,33*256,32*256));
    DoomLiteGame_Step(&game,DL_GAME_UP);assert(game.y_q8==36*256);

    static const DoomMapLine trigger={31,32,31,33,4,19,1,0,0};
    static const uint16_t trigger_rows[4]={0,0,1,1},ref=0;
    static const DoomMapCell trigger_cell={0,0,1};
    static const DoomMapTag tag={1,0,1};
    map.name="corner";map.lines=&trigger;map.line_count=1;map.row_first=trigger_rows;
    map.cells=&trigger_cell;map.cell_refs=&ref;map.tag_count=1;map.tags=&tag;map.tag_sector_refs=&ref;
    memset(&game,0,sizeof(game));game.map=&map;game.health=100;
    game.sector_state[0]=(DoomLiteSectorState){0,128,0};locate(&game,30,31,32);
    uint32_t event=DoomLiteGame_Step(&game,DL_GAME_UP);
    assert(game.x_q8/256==32&&game.y_q8/256==33);
    assert(event&DL_EVENT_MOVER);assert(bit_value(game.triggered,0));
    ticks(&game,10);assert(game.sector_state[0].floor==-8);
}
static void wall_occlusion_and_platform_edge(void) {
    assert(DoomLiteGame_InitMap(&game,0));
    locate(&game,832,1450,64);game.actor_count=1;
    DoomLiteActor *a=&game.actors[0];
    *a=(DoomLiteActor){832*256,1536*256,88,DoomMap_SectorAt(game.map,832,1536),20,DL_ACTOR_WALK,0,8,0,DL_ACTOR_SOLID,192};
    ticks(&game,100);assert(game.health==100&&!game.metrics.awake_actors);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);assert(a->health==20&&!game.kills);
    game.blue_key=1;game.previous_buttons=0;
    assert(DoomLiteGame_Step(&game,DL_GAME_USE)&DL_EVENT_DOOR_OPEN);ticks(&game,100);
    assert(game.health<100&&game.metrics.awake_actors);
    assert(DoomLiteGame_InitMap(&game,0));quiet(&game);game.health=200;
    locate(&game,608,480,0);DoomLiteGame_Step(&game,0);assert(game.health==200); /* berserk never lowers HP */
    assert(DoomLiteGame_InitMap(&game,1));quiet(&game);
    assert(use_line(&game,219)&DL_EVENT_MOVER);ticks(&game,30);
    assert(game.sector_state[44].floor==136);locate(&game,1348,-1356,0);
    ticks(&game,160);assert(game.sector_state[44].floor==208); /* neighbor ceiling 264 minus body 56 */
    for(unsigned i=0;i<4;++i) DoomLiteGame_Step(&game,DL_GAME_UP);
    assert(game.x_q8>=1360*256);ticks(&game,30);assert(game.sector_state[44].floor==240);
}
static void teleport_and_high_sector(void) {
    assert(DoomLiteGame_InitMap(&game,1));quiet(&game);
    assert(use_line(&game,34)&DL_EVENT_MOVER);ticks(&game,80);
    locate(&game,1628,-1680,128);
    int32_t ox=game.x_q8,oy=game.y_q8;
    uint32_t event=DoomLiteGame_Step(&game,DL_GAME_UP);
    assert(event&DL_EVENT_TELEPORT);assert(game.metrics.teleports==1&&game.teleport_cooldown==18);
    assert(game.x_q8!=ox||game.y_q8!=oy);
    ox=game.x_q8;oy=game.y_q8;
    for(unsigned i=0;i<10;++i) DoomLiteGame_Step(&game,DL_GAME_UP);
    assert(game.x_q8==ox&&game.y_q8==oy); /* reaction time prevents instant recross */
    assert(DoomLiteGame_InitMap(&game,1));quiet(&game);
    locate(&game,1840,1164,64);event=DoomLiteGame_Step(&game,DL_GAME_UP);
    assert(event&DL_EVENT_MOVER);ticks(&game,90);
    assert(game.sector_state[379].floor==-24); /* no uint8 truncation */
    assert(DoomLiteGame_InitMap(&game,0));quiet(&game);game.invulnerable_tics=0;
    game.sector_state[game.player_sector].special=7u;game.ticks=31;
    assert(DoomLiteGame_Step(&game,0)&DL_EVENT_HURT);assert(game.health==95);
    game.suit_tics=50;game.ticks=63;DoomLiteGame_Step(&game,0);assert(game.health==95);
    game.sector_state[game.player_sector].special=9u;
    assert(DoomLiteGame_Step(&game,0)&DL_EVENT_SECRET);assert(game.secrets==1);
    assert(!(DoomLiteGame_Step(&game,0)&DL_EVENT_SECRET));
}
static void soak_and_timing(void) {
    for(unsigned map=0;map<2;++map) {
        assert(DoomLiteGame_InitMap(&game,map));game.invulnerable_tics=UINT16_MAX;
        clock_t start=clock();
        for(unsigned n=0;n<7000;++n) {
            uint16_t buttons=n%105<35?DL_GAME_RIGHT:n%105<70?DL_GAME_LEFT:0;
            if(n%37==0) buttons|=DL_GAME_FIRE;
            if(n%101==0) buttons|=DL_GAME_USE;
            DoomLiteGame_Step(&game,buttons);
        }
        double ms=1000.0*(clock()-start)/CLOCKS_PER_SEC;
        assert(game.metrics.ai_visits<=7000u*DOOM_LITE_GAME_AI_BUDGET&&!game.metrics.pool_overflows&&game.kills<=game.total_enemies);
        DoomLiteGameProfile profile;DoomLiteGame_TakeProfile(&game,&profile);
        assert(profile.calls[DL_PROFILE_AI]==7000&&profile.calls[DL_PROFILE_ANIM]==7000);
        assert(!profile.total_us[DL_PROFILE_AI]&&!game.profile.calls[DL_PROFILE_AI]);
        printf("%s host-only 7000-tic soak %.3f ms: actors=%u ai=%lu los=%lu line_tests=%lu moves=%lu max_movers=%u game_bytes=%zu\n",game.map->name,ms,game.actor_count,(unsigned long)game.metrics.ai_visits,(unsigned long)game.metrics.los_queries,(unsigned long)game.metrics.los_line_tests,(unsigned long)game.metrics.actor_moves,game.metrics.mover_high_water,sizeof(game));
    }
}
int main(void) {
    map_counts_and_reset();turn_and_buttons();keys_and_doors();specials_and_exit();combat_and_animation();player_actor_collision();narrow_passage_and_corner_trigger();wall_occlusion_and_platform_edge();teleport_and_high_sector();soak_and_timing();
    puts("E1M1/E1M2 native gameplay replay: keys, height movers, exits, actors, bounded AI and reset OK");return 0;
}
