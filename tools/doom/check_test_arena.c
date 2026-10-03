/* Flat Test-room geometry, presets, bounds and renderer regression. Compile
 * with DoomTestMap.c, DoomLiteGame.c and DoomMap.c. No device access. */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

#define PIXELS (LCD_PIX_W * LCD_PIX_H)
static DoomLiteGame game;
static uint8_t guarded[PIXELS+32u];
static unsigned positions, walked_tics, render_frames;

static void geometry(void) {
    const DoomMap *map=DoomTestMap_Get();
    assert(map && !strcmp(map->name,"TEST") && map->index==DOOM_MAP_COUNT);
    assert(map->line_count==4u && map->sector_count==2u && map->node_count==4u);
    assert(map->sectors[0].floor==0 && map->sectors[0].ceiling==128);
    assert(map->grid_x0==-32 && map->grid_y0==-288 && map->grid_width==26u && map->grid_height==18u);
    assert(DoomTestMap_InitGame(&game,DOOM_TEST_SPRITES));
    assert(!DoomLiteGame_IsSolid(&game,game.x_q8,game.y_q8));
    for(int y=-240;y<=240;y+=8) for(int x=16;x<=752;x+=8) {
        assert(DoomMap_SectorAt(map,x,y)==0u);
        assert(!DoomLiteGame_IsSolid(&game,x*256,y*256)); ++positions;
    }
    for(int y=-256;y<256;y+=16) {
        assert(DoomLiteGame_IsSolid(&game,0,y*256));
        assert(DoomLiteGame_IsSolid(&game,768*256,y*256)); positions+=2u;
    }
    for(int x=0;x<=768;x+=16) {
        assert(DoomLiteGame_IsSolid(&game,x*256,-256*256));
        assert(DoomLiteGame_IsSolid(&game,x*256,256*256)); positions+=2u;
    }
    const int outside[][2]={{-1,0},{769,0},{384,-257},{384,257},{-32,-288},{799,287}};
    for(unsigned i=0u;i<sizeof(outside)/sizeof(outside[0]);++i) {
        assert(DoomMap_SectorAt(map,outside[i][0],outside[i][1])==1u);
        assert(DoomLiteGame_IsSolid(&game,outside[i][0]*256,outside[i][1]*256)); ++positions;
    }
    assert(DoomMap_SectorAt(map,-33,0)==DOOM_MAP_NO_SECTOR);
    assert(DoomMap_SectorAt(map,800,0)==DOOM_MAP_NO_SECTOR);
    for(unsigned gy=0u;gy<map->grid_height;++gy) {
        unsigned last_x=0u;
        for(unsigned i=map->row_first[gy];i<map->row_first[gy+1u];++i) {
            const DoomMapCell *cell=&map->cells[i];
            assert(cell->x<map->grid_width && (i==map->row_first[gy]||cell->x>last_x));
            assert((unsigned)cell->first_ref+cell->count<=map->cell_ref_count);
            assert(DoomMap_Cell(map,cell->x,gy)==cell); last_x=cell->x;
            for(unsigned j=0u;j<cell->count;++j) assert(map->cell_refs[cell->first_ref+j]<4u);
        }
    }
}

static void walking(void) {
    for(unsigned angle=0u;angle<256u;angle+=64u) {
        assert(DoomTestMap_InitGame(&game,DOOM_TEST_SPRITES));
        game.facing=(uint8_t)angle;
        int32_t last_x=game.x_q8,last_y=game.y_q8;
        for(unsigned tic=0u;tic<256u;++tic) {
            (void)DoomLiteGame_Step(&game,DL_GAME_UP);DoomTestMap_StepPreview(&game);
            assert(game.x_q8>=8*256 && game.x_q8<=760*256);
            assert(game.y_q8>=-248*256 && game.y_q8<=248*256);
            assert(!DoomLiteGame_IsSolid(&game,game.x_q8,game.y_q8));
            if(tic==255u) assert(last_x==game.x_q8&&last_y==game.y_q8);
            last_x=game.x_q8;last_y=game.y_q8;++walked_tics;
        }
    }
}

static void presets(void) {
    assert(DoomTestMap_PresetCount()==3u);
    assert(!strcmp(DoomTestMap_PresetName(DOOM_TEST_SPRITES),"SPRITES"));
    assert(!strcmp(DoomTestMap_PresetName(DOOM_TEST_COMBAT),"COMBAT"));
    assert(!strcmp(DoomTestMap_PresetName(DOOM_TEST_ITEMS),"ITEMS"));
    assert(!DoomTestMap_InitGame(&game,DOOM_TEST_PRESET_COUNT));
    assert(DoomTestMap_InitGame(&game,DOOM_TEST_SPRITES));
    assert(game.actor_count==4u && game.freeze_actors && !game.total_enemies && !game.pickup_count);
    assert(game.weapons&DL_WEAPON_SHOTGUN_OWNED);
    const int32_t x=game.actors[0].x_q8,y=game.actors[0].y_q8;
    unsigned states=0u,headings=0u;
    for(unsigned tic=0u;tic<560u;++tic) {
        (void)DoomLiteGame_Step(&game,0u);DoomTestMap_StepPreview(&game);
        for(unsigned i=0u;i<game.actor_count;++i) {
            const DoomLiteActor *actor=&game.actors[i];
            assert(!actor->health && !actor->flags);
            assert(actor->state<=DL_ACTOR_CORPSE && actor->frame<7u);
            states|=1u<<actor->state;headings|=1u<<(actor->facing/32u);
        }
        assert(game.actors[0].x_q8==x && game.actors[0].y_q8==y);
        assert(!game.metrics.ai_visits && !game.metrics.actor_moves);
    }
    assert(states==31u && headings==255u && game.health==100u);
    assert(DoomTestMap_InitGame(&game,DOOM_TEST_COMBAT));
    assert(game.actor_count==4u && !game.freeze_actors && game.total_enemies==4u && game.pickup_count==12u);
    game.invulnerable_tics=UINT16_MAX;
    for(unsigned tic=0u;tic<210u;++tic) (void)DoomLiteGame_Step(&game,0u);
    assert(game.metrics.ai_visits && game.metrics.actor_moves && game.metrics.awake_actors==4u);
    assert(DoomTestMap_InitGame(&game,DOOM_TEST_ITEMS));
    assert(!game.actor_count && !game.total_enemies && game.pickup_count==12u);
    game.health=50u;game.ammo=0u;game.shells=0u;
    for(unsigned i=0u;i<game.pickup_count;++i) {
        const unsigned index=game.pickup_refs[i];
        const DoomMapThing *thing=&game.map->things[index];
        game.x_q8=thing->x*256;game.y_q8=thing->y*256;
        uint32_t event=0u;
        for(unsigned tic=0u;tic<4u;++tic) event|=DoomLiteGame_Step(&game,0u);
        if(!(event&(DL_EVENT_PICKUP|DL_EVENT_BLUE_KEY|DL_EVENT_RED_KEY|DL_EVENT_YELLOW_KEY)))
            fprintf(stderr,"Arena pickup index=%u type=%u event=%lu health=%u ammo=%u shells=%u\n",
                    index,thing->type,(unsigned long)event,game.health,game.ammo,game.shells);
        assert(event&(DL_EVENT_PICKUP|DL_EVENT_BLUE_KEY|DL_EVENT_RED_KEY|DL_EVENT_YELLOW_KEY));
        assert(!DoomLiteGame_ThingActive(&game,index));
    }
    assert(game.blue_key&&game.red_key&&game.yellow_key);
    assert(game.weapons&DL_WEAPON_SHOTGUN_OWNED);
    assert(game.shells>=32u && game.ammo>=60u && game.armor>0u && game.health>50u);
    assert(!DoomLiteGame_InitMap(&game,DOOM_MAP_COUNT));
    assert(DoomLiteGame_InitMap(&game,0u) && !game.freeze_actors);
}

static void preview(const char *directory,const char *name,const uint8_t *pixels) {
    if(!directory) return;
    char path[1024];const int length=snprintf(path,sizeof(path),"%s/%s.pgm",directory,name);
    assert(length>0&&(unsigned)length<sizeof(path));
    FILE *file=fopen(path,"wb");assert(file);
    fprintf(file,"P5\n256 127\n255\n");assert(fwrite(pixels,1u,PIXELS,file)==PIXELS);assert(!fclose(file));
}
static void render(const char *directory) {
    for(unsigned preset=0u;preset<DOOM_TEST_PRESET_COUNT;++preset) {
        assert(DoomTestMap_InitGame(&game,preset));
        assert(game_world_map(game.map)==&doom_test_world);
        assert(doom_test_world.line_count==game.map->line_count && doom_test_world.sector_count==2u);
        assert(doom_test_world_sides[0].middle==173u && doom_test_world_sectors[0].floor==62u);
        memset(guarded,0xa5,sizeof(guarded));uint8_t *pixels=guarded+16u;
        DoomLite_RenderGameScene(pixels,&game);
        assert(game_render_stats.rays==LITE_RAYS && !game_render_stats.surface_limit_hits);
        assert(game_render_stats.world_texture_pixels && game_render_stats.plane_texture_pixels);
        DoomLite_RenderGameThings(pixels,&game);assert(game_render_stats.sprite_pixels>0u);
        for(unsigned i=0u;i<16u;++i) {assert(guarded[i]==0xa5u);assert(guarded[PIXELS+16u+i]==0xa5u);}
        for(unsigned i=DOOM_GAME_VIEW_H*LITE_W;i<PIXELS;++i) assert(pixels[i]==0xa5u);
        DoomLite_DrawGameWeapon(pixels,&game);DoomLite_DrawGameHud(pixels,&game,DoomTestMap_PresetName(preset),0u);
        preview(directory,DoomTestMap_PresetName(preset),pixels);++render_frames;
    }
    assert(DoomTestMap_InitGame(&game,DOOM_TEST_SPRITES));
    for(unsigned angle=0u;angle<256u;++angle) {
        game.facing=(uint8_t)angle;
        memset(guarded,0xa5,sizeof(guarded));DoomLite_RenderGameScene(guarded+16u,&game);
        if(game_render_stats.surface_limit_hits||game_render_stats.surfaces<LITE_RAYS)
            fprintf(stderr,"Arena angle=%u surfaces=%lu rays=%u limits=%lu\n",angle,
                    (unsigned long)game_render_stats.surfaces,LITE_RAYS,
                    (unsigned long)game_render_stats.surface_limit_hits);
        assert(!game_render_stats.surface_limit_hits && game_render_stats.surfaces>=LITE_RAYS);
        for(unsigned i=0u;i<16u;++i) {assert(guarded[i]==0xa5u);assert(guarded[PIXELS+16u+i]==0xa5u);}
        ++render_frames;
    }
    puts("ARENA_GRID padded supercover retains boundary walls after DDA interval rounding; outside BSP sector is solid");
}
int main(int argc,char **argv) {
    geometry();walking();presets();render(argc>1?argv[1]:NULL);
    printf("ARENA_OK positions=%u walked_tics=%u render_frames=%u map_bytes=%u world_bytes=%u\n",
           positions,walked_tics,render_frames,DoomTestMap_ReadonlyBytes(),(unsigned)DOOM_TEST_WORLD_BYTES);
    return 0;
}
