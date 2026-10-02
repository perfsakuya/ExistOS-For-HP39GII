/* Host-only walking replay using the real public simulation API. The route
 * planner may predict key-authorized manual doors in a COPY of game state;
 * the replay itself must open them with F2 and can never edit heights,
 * collected bits or completion. Invulnerability isolates traversal from
 * combat, which has its own correctness tests. Heading is assigned by the
 * host path follower; every displacement, shot, pickup and special uses Step.
 * Live actors remain solid and nearby obstructions are fought with real F1
 * edges and actual ammunition. No doors, heights or pickups are edited in the
 * actual game. The four-unit lattice and malloc arrays are host-only. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "../../System/applications/user/doom_lite/DoomLiteGame.h"
#define LATTICE 4
static DoomLiteGame game, planner;
static DoomLiteSectorState snapshot[DOOM_MAP_MAX_SECTORS];
static uint32_t steps,uses,replans;
static int abs_i(int n) {return n<0?-n:n;}
static int is_manual(unsigned n) {return n==1||n==26||n==27||n==28||n==31||n==32||n==33||n==34||n==117||n==118;}
static int has_key(const DoomLiteGame *g,unsigned n) {
    return !(((n==26||n==32)&&!g->blue_key)||((n==27||n==34)&&!g->yellow_key)||((n==28||n==33)&&!g->red_key));
}
static void planner_doors(void) {
    planner=game;
    for(unsigned i=0;i<planner.map->special_line_count;++i) {
        const DoomMapLine *l=&planner.map->lines[planner.map->special_line_refs[i]];
        if(planner.map_index==0u&&(l->special==2u||l->special==109u)) {
            uint16_t count=0;const uint16_t *refs=DoomMap_TagSectors(planner.map,l->tag,&count);
            for(unsigned s=0;s<count;++s) planner.sector_state[refs[s]].ceiling=planner.map->sectors[refs[s]].lowest_ceiling-4;
            continue;
        }
        if(!is_manual(l->special)||l->special==31u||!has_key(&planner,l->special)||l->back_sector>=planner.map->sector_count) continue;
        const DoomMapSector *s=&planner.map->sectors[l->back_sector];
        planner.sector_state[l->back_sector].ceiling=s->lowest_ceiling-4;
    }
}
static int near_goal(int x,int y,unsigned ref,int is_thing) {
    int dx,dy;
    if(is_thing==2) return DoomMap_SectorAt(game.map,x,y)==ref&&
        DoomMap_SectorAt(game.map,x-12,y)==ref&&DoomMap_SectorAt(game.map,x+12,y)==ref&&
        DoomMap_SectorAt(game.map,x,y-12)==ref&&DoomMap_SectorAt(game.map,x,y+12)==ref;
    if(is_thing==1) {
        const DoomMapThing *t=&game.map->things[ref];dx=x-t->x;dy=y-t->y;
        unsigned sector=DoomMap_SectorAt(game.map,x,y);
        if(sector>=game.map->sector_count||t->sector>=game.map->sector_count) return 0;
        int delta=game.sector_state[t->sector].floor-game.sector_state[sector].floor;
        return dx*dx+dy*dy<=24*24&&delta<=56&&delta>=-8;
    }
    const DoomMapLine *l=&game.map->lines[ref];
    unsigned sector=DoomMap_SectorAt(game.map,x,y);
    if(sector!=l->front_sector&&sector!=l->back_sector) return 0;
    int lx=l->x2-l->x1,ly=l->y2-l->y1;
    if(is_thing==0&&(int64_t)lx*(y-l->y1)-(int64_t)ly*(x-l->x1)>0) return 0;
    int64_t length=(int64_t)lx*lx+(int64_t)ly*ly;
    int64_t p=(int64_t)(x-l->x1)*lx+(int64_t)(y-l->y1)*ly;
    if(p<0) p=0;
    if(p>length) p=length;
    dx=x-l->x1-(int)(length?lx*p/length:0);dy=y-l->y1-(int)(length?ly*p/length:0);
    return dx*dx+dy*dy<=(is_thing==3?4*4:32*32);
}
static uint8_t angle_toward(int dx,int dy) {
    return (uint8_t)(abs_i(dx)>=abs_i(dy)?(dx>=0?0:128):(dy>=0?64:192));
}
static uint32_t action_use(void) {
    DoomLiteGame_Step(&game,0);uint32_t event=DoomLiteGame_Step(&game,DL_GAME_USE);++uses;steps+=2;
    return event;
}
static int fight_near(void) {
    int best=-1,nearest=80*80;
    int px=game.x_q8/256,py=game.y_q8/256;
    for(unsigned i=0;i<game.actor_count;++i) {
        const DoomLiteActor *a=&game.actors[i];if(a->health<=0) continue;
        int dx=a->x_q8/256-px,dy=a->y_q8/256-py,d=dx*dx+dy*dy;
        if(d<nearest) {nearest=d;best=(int)i;}
    }
    if(best<0||!game.ammo) return 0;
    const DoomLiteActor *a=&game.actors[best];
    game.facing=(uint8_t)lround(atan2((double)(a->y_q8-game.y_q8),(double)(a->x_q8-game.x_q8))*256.0/6.283185307179586);
    DoomLiteGame_Step(&game,0);uint32_t event=DoomLiteGame_Step(&game,DL_GAME_FIRE);steps+=2;
    return !!(event&DL_EVENT_SHOT_HIT);
}
static int ride_mover(void) {
    unsigned sector=DoomLiteGame_PlayerSector(&game);int found=0;
    for(unsigned i=0;i<DOOM_LITE_GAME_MOVERS;++i) if(game.movers[i].kind&&game.movers[i].sector==sector) found=1;
    if(!found) return 0;
    for(unsigned i=0;i<400;++i) {DoomLiteGame_Step(&game,0);++steps;}
    return 1;
}
static int walk_target(unsigned target,int is_thing) {
    const DoomMap *m=game.map;unsigned width=m->grid_width*(32u/LATTICE),height=m->grid_height*(32u/LATTICE),count=width*height;
    int32_t *parent=malloc(count*sizeof(*parent));uint32_t *queue=malloc(count*sizeof(*queue));assert(parent&&queue);
    for(unsigned retry=0;retry<80;++retry) {
        ++replans;memcpy(snapshot,game.sector_state,sizeof(snapshot));planner_doors();for(unsigned i=0;i<count;++i) parent[i]=-1;
        int sx=(game.x_q8/256-m->grid_x0)/LATTICE,sy=(game.y_q8/256-m->grid_y0)/LATTICE;
        assert(sx>=0&&sy>=0&&(unsigned)sx<width&&(unsigned)sy<height);
        unsigned start=(unsigned)sy*width+(unsigned)sx,head=0,tail=0;int goal=-1;
        parent[start]=(int32_t)start;queue[tail++]=start;
        while(head<tail) {
            unsigned node=queue[head++];int cx=(int)(node%width),cy=(int)(node/width);
            int x=m->grid_x0+cx*LATTICE,y=m->grid_y0+cy*LATTICE;
            if(near_goal(x,y,target,is_thing)) {goal=(int)node;break;}
            planner.x_q8=x*256;planner.y_q8=y*256;
            static const int dirs[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
            for(unsigned d=0;d<4;++d) {
                int nx=cx+dirs[d][0],ny=cy+dirs[d][1];if(nx<0||ny<0||(unsigned)nx>=width||(unsigned)ny>=height) continue;
                unsigned next=(unsigned)ny*width+(unsigned)nx;if(parent[next]>=0) continue;
                int wx=m->grid_x0+nx*LATTICE,wy=m->grid_y0+ny*LATTICE;
                if(DoomLiteGame_IsSolid(&planner,wx*256,wy*256)) continue;
                parent[next]=(int32_t)node;queue[tail++]=next;
            }
        }
        if(goal<0) {
            int nearest=INT32_MAX,nx=0,ny=0;
            for(unsigned p=0;p<tail;++p) {
                int x=m->grid_x0+(int)(queue[p]%width)*LATTICE,y=m->grid_y0+(int)(queue[p]/width)*LATTICE;
                int gx=is_thing==1?m->things[target].x:m->lines[target].x1,gy=is_thing==1?m->things[target].y:m->lines[target].y1;
                int d=(x-gx)*(x-gx)+(y-gy)*(y-gy);if(d<nearest) {nearest=d;nx=x;ny=y;}
            }
            fprintf(stderr,"%s no lattice path to %s %u after %u replans (visited %u, player %ld,%ld keys%u%u%u nearest%d,%d sector%u d2%d)\n",m->name,is_thing?"thing":"line",target,retry,tail,(long)(game.x_q8/256),(long)(game.y_q8/256),game.blue_key,game.red_key,game.yellow_key,nx,ny,DoomMap_SectorAt(m,nx,ny),nearest);free(parent);free(queue);return 0;
        }
        unsigned path_count=0,node=(unsigned)goal;
        while(node!=start) {queue[path_count++]=node;node=(unsigned)parent[node];assert(path_count<count);}
        int interrupted=0;
        while(path_count) {
            node=queue[--path_count];int tx=m->grid_x0+(int)(node%width)*LATTICE,ty=m->grid_y0+(int)(node/width)*LATTICE;
            int px=game.x_q8/256,py=game.y_q8/256;game.facing=angle_toward(tx-px,ty-py);
            for(unsigned tic=0;tic<LATTICE/4u;++tic) {
                int32_t before_x=game.x_q8,before_y=game.y_q8;
                uint32_t event=DoomLiteGame_Step(&game,DL_GAME_UP);++steps;
                if(event&DL_EVENT_TELEPORT) {interrupted=1;break;}
                if(game.x_q8==before_x&&game.y_q8==before_y) {
                    uint32_t opened=action_use();
                    if(!(opened&(DL_EVENT_DOOR_OPEN|DL_EVENT_MOVER))) {
                        if(fight_near()) {interrupted=1;break;}
                        if(ride_mover()) {interrupted=1;break;}
                        if(memcmp(snapshot,game.sector_state,sizeof(snapshot))) {interrupted=1;break;}
                        unsigned ps=DoomLiteGame_PlayerSector(&game),ts=DoomMap_SectorAt(m,tx,ty);
                        uint16_t count=0;const uint16_t *refs=DoomMap_CellLines(m,tx,ty,&count);
                        for(unsigned q=0;q<count;++q) {const DoomMapLine *l=&m->lines[refs[q]];fprintf(stderr,"nearline%u (%d,%d>%d,%d) special%u flags%u sectors%u,%u ceilings%d,%d\n",refs[q],l->x1,l->y1,l->x2,l->y2,l->special,l->flags,l->front_sector,l->back_sector,DoomLiteGame_CeilingHeight(&game,l->front_sector),DoomLiteGame_CeilingHeight(&game,l->back_sector));}
                        for(unsigned q=0;q<game.actor_count;++q) {const DoomLiteActor *a=&game.actors[q];int dx=a->x_q8/256-px,dy=a->y_q8/256-py;if(a->health>0&&dx*dx+dy*dy<80*80) fprintf(stderr,"nearactor%u pos%ld,%ld hp%d\n",q,(long)(a->x_q8/256),(long)(a->y_q8/256),a->health);}
                        fprintf(stderr,"%s stuck at (%d,%d) toward (%d,%d) target%u event%lx sectors%u,%u floors%d,%d ammo%u\n",m->name,px,py,tx,ty,target,(unsigned long)opened,ps,ts,DoomLiteGame_FloorHeight(&game,(uint16_t)ps),DoomLiteGame_FloorHeight(&game,(uint16_t)ts),game.ammo);free(parent);free(queue);return 0;
                    }
                    for(unsigned wait=0;wait<45;++wait) {DoomLiteGame_Step(&game,0);++steps;}
                    interrupted=1;break;
                }
            }
            if(interrupted) break;
            if(game.x_q8/256!=tx||game.y_q8/256!=ty) {interrupted=1;break;}
        }
        if(interrupted) continue;
        if(is_thing==1) {
            for(unsigned wait=0;wait<4;++wait) {DoomLiteGame_Step(&game,0);++steps;}
            if(DoomLiteGame_ThingActive(&game,target)) {fprintf(stderr,"%s pickup %u not collected at %ld,%ld sector%u\n",m->name,target,(long)(game.x_q8/256),(long)(game.y_q8/256),game.player_sector);free(parent);free(queue);return 0;}
        } else if(is_thing==0||is_thing==3) {
            const DoomMapLine *l=&m->lines[target];int px=game.x_q8/256,py=game.y_q8/256;
            int lx=l->x2-l->x1,ly=l->y2-l->y1;int64_t len=(int64_t)lx*lx+(int64_t)ly*ly;
            int64_t p=(int64_t)(px-l->x1)*lx+(int64_t)(py-l->y1)*ly;
            if(p<0) p=0;
            if(p>len) p=len;
            int dx=l->x1+(int)(len?lx*p/len:0)-px,dy=l->y1+(int)(len?ly*p/len:0)-py;
            game.facing=(uint8_t)lround(atan2((double)dy,(double)dx)*256.0/6.283185307179586);
            uint32_t event=0;
            if(is_thing==3) {
                if((game.triggered[target>>3]>>(target&7u))&1u) event=DL_EVENT_DOOR_OPEN;
                else for(unsigned n=0;n<3;++n) {event|=DoomLiteGame_Step(&game,DL_GAME_UP);++steps;}
            } else event=action_use();
            if(!(event&(DL_EVENT_MOVER|DL_EVENT_DOOR_OPEN|DL_EVENT_EXIT))) {fprintf(stderr,"%s use line %u failed at %ld,%ld event%lx\n",m->name,target,(long)(game.x_q8/256),(long)(game.y_q8/256),(unsigned long)event);free(parent);free(queue);return 0;}
        }
        printf("%s reached %s %u via Step: pos=(%ld,%ld) sector=%u ticks=%lu uses=%lu keys=%u%u%u\n",m->name,is_thing==2?"sector":is_thing==3?"walk-line":is_thing?"thing":"line",target,(long)(game.x_q8/256),(long)(game.y_q8/256),game.player_sector,(unsigned long)game.ticks,(unsigned long)uses,game.blue_key,game.red_key,game.yellow_key);
        free(parent);free(queue);return 1;
    }
    free(parent);free(queue);return 0;
}
int main(void) {
    assert(DoomLiteGame_InitMap(&game,0));game.invulnerable_tics=UINT16_MAX;
    assert(walk_target(620,0));for(unsigned i=0;i<35;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[103].floor==8);assert(walk_target(103,2));
    for(unsigned i=0;i<160;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[103].floor==136);
    assert(walk_target(87,1));assert(game.blue_key);assert(walk_target(407,0));assert(game.completed);
    printf("E1M1 clear: health=%u ammo=%u armor=%u kills=%u/%u shots=%lu ticks=%lu\n",game.health,game.ammo,game.armor,game.kills,game.total_enemies,(unsigned long)game.metrics.shots,(unsigned long)game.ticks);
    assert(DoomLiteGame_NextMap(&game));assert(game.map_index==1);
    assert(!game.blue_key&&!game.red_key&&!game.yellow_key);game.invulnerable_tics=UINT16_MAX;
    assert(walk_target(219,0));for(unsigned i=0;i<30;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[44].floor==136);assert(walk_target(44,2));
    for(unsigned i=0;i<160;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[44].floor==240);
    assert(walk_target(34,0));for(unsigned i=0;i<80;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[45].floor==240);assert(walk_target(15,1));assert(game.blue_key);
    assert(walk_target(44,2));for(unsigned i=0;i<35;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[44].floor==136);
    assert(walk_target(1556,3));for(unsigned i=0;i<30;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(walk_target(104,1));assert(game.red_key);
    assert(walk_target(93,0));for(unsigned i=0;i<40;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(walk_target(1904,0));for(unsigned i=0;i<160;++i) {DoomLiteGame_Step(&game,0);++steps;}
    assert(game.sector_state[301].floor==-40);assert(walk_target(164,1));assert(game.yellow_key);
    assert(walk_target(737,0));assert(game.completed);
    printf("E1M2 clear: health=%u ammo=%u armor=%u kills=%u/%u shots=%lu ticks=%lu\n",game.health,game.ammo,game.armor,game.kills,game.total_enemies,(unsigned long)game.metrics.shots,(unsigned long)game.ticks);
    printf("Both native levels walking+F2 replay complete: Step=%lu F2=%lu planner_replans=%lu (host topology test, invulnerable player)\n",(unsigned long)steps,(unsigned long)uses,(unsigned long)replans);return 0;
}
