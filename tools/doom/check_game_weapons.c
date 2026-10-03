/* Portable weapon and custom-map checks; no calculator access.
 * gcc -std=c11 -O2 -Wall -Wextra -Werror tools/doom/check_game_weapons.c
 * System/applications/user/doom_lite/DoomLiteGame.c
 * System/applications/user/doom_lite/DoomMap.c -o check_game_weapons.exe */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../System/applications/user/doom_lite/DoomLiteGame.h"

static DoomLiteGame game, saved;
static const DoomMapSector sector={.floor=0,.ceiling=128,.light=160,
    .lowest_ceiling=128,.highest_ceiling=128,.x0=0,.y0=0,.x1=768,.y1=640};
static const uint16_t subsector=0, empty_rows[21]={0};
static DoomMap map_for(const DoomMapThing *things,unsigned count) {
    return (DoomMap){.name="WEAPONS",.index=DOOM_MAP_COUNT,.start_x=128,.start_y=320,
        .start_angle=0,.start_sector=0,.grid_width=24,.grid_height=20,
        .thing_count=(uint16_t)count,.sector_count=1,.subsector_count=1,
        .things=things,.sectors=&sector,.subsector_sector=&subsector,.row_first=empty_rows};
}
static void init(const DoomMap *map) {
    assert(DoomLiteGame_InitData(&game,map));
    game.freeze_actors=1;
}
static void ticks(unsigned count) {while(count--) (void)DoomLiteGame_Step(&game,0);}
static void locate(int x,int y) {
    game.x_q8=x*256;game.y_q8=y*256;game.player_sector=DoomLiteGame_PlayerSector(&game);
    game.previous_buttons=0;game.ticks=0;
}
static void own_shotgun(void) {
    game.weapons|=DL_WEAPON_SHOTGUN_OWNED;game.current_weapon=DL_WEAPON_SHOTGUN;
}
static void reject(const DoomMap *map) {
    saved=game;
    assert(!DoomLiteGame_InitData(&game,map));
    assert(!memcmp(&saved,&game,sizeof(game)));
}
static void custom_initialization(void) {
    static const DoomMapThing things[]={{192,320,180,3002,2,0},{256,320,0,2001,2,0}};
    DoomMap map=map_for(things,2),before=map;
    assert(!DoomLiteGame_InitData(NULL,&map));
    assert(DoomLiteGame_InitData(&game,&map));
    assert(!memcmp(&map,&before,sizeof(map)));
    assert(game.map==&map&&game.map_index==DOOM_MAP_COUNT&&game.actor_count==1&&game.pickup_count==1);
    assert(game.current_weapon==DL_WEAPON_PISTOL&&game.weapons==3&&!game.freeze_actors);
    assert(game.health==100&&game.ammo==50&&!game.shells&&DoomLiteGame_CurrentAmmo(&game)==50);
    assert(game.actors[0].health==150&&game.actors[0].sector==0&&game.thing_actor[0]==0);
    assert(game.thing_actor[1]==255&&game.player_sector==0&&!game.metrics.pool_overflows);
    assert(!DoomLiteGame_IsSolid(&game,game.x_q8,game.y_q8));
    reject(NULL);
    DoomMap bad=map;bad.index=256;reject(&bad);
    bad=map;bad.thing_count=DOOM_MAP_MAX_THINGS+1;reject(&bad);
    bad=map;bad.sector_count=DOOM_MAP_MAX_SECTORS+1;reject(&bad);
    bad=map;bad.line_count=DOOM_MAP_MAX_LINES+1;reject(&bad);
    bad=map;bad.start_sector=1;reject(&bad);
    bad=map;bad.start_x=-1;reject(&bad);
    bad=map;bad.things=NULL;reject(&bad);
    bad=map;bad.row_first=NULL;reject(&bad);
    static const DoomMapThing invalid_sector={192,320,0,3002,2,1};
    bad=map_for(&invalid_sector,1);reject(&bad);
    static const DoomMapNode invalid_node={.child={0x8001,0x8000}};
    bad=map;bad.node_count=1;bad.nodes=&invalid_node;reject(&bad);
    static const DoomMapLine wall={192,0,192,640,1,0,0,0,DOOM_MAP_NO_SECTOR};
    static const uint16_t invalid_ref=1;
    bad=map;bad.line_count=1;bad.lines=&wall;bad.cell_ref_count=1;bad.cell_refs=&invalid_ref;reject(&bad);
    DoomMapThing crowded[DOOM_LITE_GAME_ACTORS+1];
    for(unsigned i=0;i<DOOM_LITE_GAME_ACTORS+1;++i) crowded[i]=(DoomMapThing){192,320,0,3004,2,0};
    bad=map_for(crowded,DOOM_LITE_GAME_ACTORS+1);reject(&bad);
    DoomMapThing teleports[17];
    for(unsigned i=0;i<17;++i) teleports[i]=(DoomMapThing){192,320,0,14,2,0};
    bad=map_for(teleports,17);reject(&bad);
    saved=game;
    assert(!DoomLiteGame_InitMap(&game,DOOM_MAP_COUNT)&&!memcmp(&saved,&game,sizeof(game)));
    assert(!DoomLiteGame_InitMap(&game,UINT32_MAX)&&!memcmp(&saved,&game,sizeof(game)));
}
static void ownership_pickups_and_switch_edges(void) {
    DoomMap empty=map_for(NULL,0);init(&empty);
    assert(!(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON));
    assert(game.current_weapon==DL_WEAPON_PISTOL);
    game.current_weapon=DL_WEAPON_SHOTGUN;game.shells=10;
    assert(DoomLiteGame_CurrentAmmo(&game)==0);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);
    assert(game.shells==10&&game.ammo==50&&!game.metrics.shots&&!game.pistol_tics);
    game.current_weapon=255;assert(!DoomLiteGame_CurrentAmmo(&game));
    assert(!DoomLiteGame_CurrentAmmo(NULL));
    static const DoomMapThing things[]={
        {256,320,0,2001,2,0},{320,320,0,2008,2,0},
        {384,320,0,2002,2,0},{448,320,0,82,2,0}};
    DoomMap map=map_for(things,4);init(&map);
    locate(256,320);
    assert(DoomLiteGame_Step(&game,0)&DL_EVENT_PICKUP);
    assert(game.weapons&DL_WEAPON_SHOTGUN_OWNED);
    assert(game.shells==8&&game.current_weapon==DL_WEAPON_PISTOL&&!DoomLiteGame_ThingActive(&game,0));
    assert(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON);
    assert(game.current_weapon==DL_WEAPON_SHOTGUN&&DoomLiteGame_CurrentAmmo(&game)==8);
    for(unsigned i=0;i<50;++i) assert(!(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON));
    assert(game.current_weapon==DL_WEAPON_SHOTGUN);
    DoomLiteGame_Step(&game,0);
    assert(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON);
    assert(game.current_weapon==DL_WEAPON_PISTOL&&DoomLiteGame_CurrentAmmo(&game)==50);
    locate(320,320);assert(DoomLiteGame_Step(&game,0)&DL_EVENT_PICKUP);assert(game.shells==12);
    locate(384,320);assert(DoomLiteGame_Step(&game,0)&DL_EVENT_PICKUP);assert(game.weapons&8u);
    locate(448,320);assert(DoomLiteGame_Step(&game,0)&DL_EVENT_PICKUP);assert(game.weapons&128u);
    for(unsigned i=0;i<4;++i) {
        DoomLiteGame_Step(&game,0);
        assert(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON);
        assert(game.current_weapon==(i&1u?DL_WEAPON_PISTOL:DL_WEAPON_SHOTGUN));
    }
    init(&empty);game.weapons|=8u|128u;
    assert(!(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON));
    assert(game.current_weapon==DL_WEAPON_PISTOL); /* Unimplemented pickups do not grant a usable shotgun. */
}
static void ammunition_cooldown_and_pistol(void) {
    static const DoomMapThing monster={192,320,180,3002,2,0};
    DoomMap map=map_for(&monster,1);init(&map);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_HIT);
    assert(game.ammo==49&&game.actors[0].health==125&&game.pistol_tics==15);
    DoomLiteGame_Step(&game,0);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_HIT);
    assert(game.ammo==48&&game.actors[0].health==100&&game.pistol_tics==15);
    init(&map);own_shotgun();game.shells=4;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_HIT);
    assert(game.shells==3&&game.ammo==50&&game.actors[0].health==80&&game.pistol_tics==35&&game.shotgun_cooldown==35&&game.metrics.shots==1);
    assert(game.actors[0].state==DL_ACTOR_PAIN&&game.actors[0].cooldown==12);
    DoomLiteGame_Step(&game,0);
    assert(!(DoomLiteGame_Step(&game,DL_GAME_FIRE)&(DL_EVENT_SHOT_HIT|DL_EVENT_SHOT_MISS)));
    assert(game.shells==3&&game.actors[0].health==80&&game.metrics.shots==1);
    ticks(32);assert(game.pistol_tics==1);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_HIT);
    assert(game.shells==2&&game.actors[0].health==10&&game.pistol_tics==35&&game.metrics.shots==2);
    for(unsigned i=0;i<70;++i) DoomLiteGame_Step(&game,DL_GAME_FIRE);
    assert(!game.pistol_tics&&game.shells==2&&game.actors[0].health==10);
    DoomLiteGame_Step(&game,0);
    assert((DoomLiteGame_Step(&game,DL_GAME_FIRE)&(DL_EVENT_SHOT_HIT|DL_EVENT_KILL))==(DL_EVENT_SHOT_HIT|DL_EVENT_KILL));
    assert(game.kills==1&&!game.actors[0].health&&game.actors[0].state==DL_ACTOR_DEATH);
    assert(!(game.actors[0].flags&DL_ACTOR_SOLID)&&!game.metrics.awake_actors);
    ticks(35);game.shells=0;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);
    assert(!game.pistol_tics&&game.metrics.shots==3&&game.ammo==50);
    game.previous_buttons=0;game.health=0;game.shells=2;
    assert(!DoomLiteGame_Step(&game,DL_GAME_FIRE)&&game.shells==2);
    /* A switch edge during fire/reload is ignored until a new edge at idle. */
    DoomMap empty=map_for(NULL,0);init(&empty);own_shotgun();game.shells=3;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);
    assert(game.shotgun_cooldown==35&&game.shells==2);
    assert(!(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON));
    assert(game.current_weapon==DL_WEAPON_SHOTGUN&&game.pistol_tics==34&&game.shotgun_cooldown==34);
    for(unsigned i=0;i<40;++i) assert(!(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON));
    assert(game.current_weapon==DL_WEAPON_SHOTGUN&&!game.pistol_tics&&!game.shotgun_cooldown);
    DoomLiteGame_Step(&game,0);
    assert(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON);
    assert(game.current_weapon==DL_WEAPON_PISTOL);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);
    assert(game.pistol_tics==15&&game.ammo==49&&!game.shotgun_cooldown);
    assert(!(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON));
    assert(game.current_weapon==DL_WEAPON_PISTOL&&game.pistol_tics==14);
    ticks(14);
    assert(DoomLiteGame_Step(&game,DL_GAME_SWITCH)&DL_EVENT_WEAPON);
    assert(game.current_weapon==DL_WEAPON_SHOTGUN&&game.shells==2);
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);
    assert(game.shells==1&&game.shotgun_cooldown==35&&game.metrics.shots==3);
}
static const uint16_t wall_rows[21]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20};
static const uint16_t wall_ref=0;
#define WALL_CELL {6,0,1}
static const DoomMapCell wall_cells[20]={
    WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,
    WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL,WALL_CELL};
#undef WALL_CELL
static void walls_nearest_target_and_spread(void) {
    static const DoomMapThing nearest[]={
        {192,320,180,3002,2,0},{512,320,180,3002,2,0}};
    DoomMap map=map_for(nearest,2);init(&map);own_shotgun();game.shells=2;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_HIT);
    assert(game.actors[0].health==80&&game.actors[1].health==150);
    static const DoomMapThing spread[]={
        {512,296,180,3004,2,0},{512,320,180,3004,2,0},{512,344,180,3004,2,0}};
    map=map_for(spread,3);init(&map);own_shotgun();game.shells=2;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_KILL);
    assert(game.kills==3&&game.shells==1&&game.metrics.shots==1);
    for(unsigned i=0;i<3;++i) assert(!game.actors[i].health&&game.actors[i].state==DL_ACTOR_DEATH);
    static const DoomMapLine full_wall={192,0,192,640,1,0,0,0,DOOM_MAP_NO_SECTOR};
    map.lines=&full_wall;map.line_count=1;map.cells=wall_cells;map.cell_count=20;
    map.row_first=wall_rows;map.cell_refs=&wall_ref;map.cell_ref_count=1;
    init(&map);own_shotgun();game.shells=2;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_MISS);
    assert(!game.kills&&game.shells==1);
    for(unsigned i=0;i<3;++i) assert(game.actors[i].health==20);
    static const DoomMapLine half_wall={192,320,192,640,1,0,0,0,DOOM_MAP_NO_SECTOR};
    map.lines=&half_wall;init(&map);own_shotgun();game.shells=2;
    assert(DoomLiteGame_Step(&game,DL_GAME_FIRE)&DL_EVENT_SHOT_HIT);
    assert(!game.actors[0].health&&game.actors[1].health==10&&game.actors[2].health==20&&game.kills==1);
    /* This also proves pellet LOS follows each heading around a wall edge. */
}
static void next_map_and_static_preview(void) {
    assert(DoomLiteGame_InitMap(&game,0));
    own_shotgun();game.health=73;game.armor=52;game.armor_class=2;game.ammo=77;
    game.shells=23;game.rockets=7;game.cells=49;game.backpack=1;game.contrast=2;
    game.weapons=255;game.blue_key=game.red_key=game.yellow_key=1;game.pistol_tics=35;
    assert(DoomLiteGame_NextMap(&game));
    assert(game.map_index==1&&game.current_weapon==DL_WEAPON_SHOTGUN&&game.weapons==255);
    assert(game.health==73&&game.armor==52&&game.armor_class==2&&game.ammo==77);
    assert(game.shells==23&&game.rockets==7&&game.cells==49&&game.backpack&&game.contrast==2);
    assert(!game.blue_key&&!game.red_key&&!game.yellow_key&&!game.pistol_tics&&!game.kills);
    assert(DoomLiteGame_CurrentAmmo(&game)==23);
    assert(DoomLiteGame_NextMap(&game));
    assert(game.map_index==0&&game.current_weapon==DL_WEAPON_PISTOL&&game.weapons==3&&game.ammo==50&&!game.shells);
    static const DoomMapThing things[]={{256,320,180,3002,2,0},{132,320,0,2001,2,0}};
    DoomMap map=map_for(things,2);init(&map);
    DoomLiteActor actor=game.actors[0];
    assert(DoomLiteGame_Step(&game,DL_GAME_UP)&DL_EVENT_PICKUP);
    assert(game.x_q8==132*256&&game.shells==8&&!memcmp(&actor,&game.actors[0],sizeof(actor)));
    ticks(100);
    assert(!memcmp(&actor,&game.actors[0],sizeof(actor))&&!game.metrics.ai_visits&&!game.metrics.actor_moves&&game.health==100);
    game.freeze_actors=0;ticks(1);assert(game.metrics.ai_visits>0);
}
int main(void) {
    custom_initialization();ownership_pickups_and_switch_edges();ammunition_cooldown_and_pistol();
    walls_nearest_target_and_spread();next_map_and_static_preview();
    puts("Game weapons: custom map bounds, ownership, switch edges, seven pellets, cooldown, wall occlusion and cross-map inventory OK");
    return 0;
}
