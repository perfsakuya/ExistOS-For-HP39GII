/* Native two-level Doom rules. Geometry stays immutable; bounded pools own
 * actors, animation clocks and moving sector heights. Consulted GPL Doom
 * p_spec/p_doors/p_floor/p_plats/p_telept for line-number semantics; this is
 * a compact approximation, not copied thinker or demo-compatible code. */
#include "DoomLiteGame.h"
#include <limits.h>
#include <string.h>
#define PLAYER_RADIUS 8
#define PLAYER_HEIGHT 56
#define PICKUP_REACH 24
#define NO_ACTOR 255u
#define MAX_LOS_STEPS 64u
static uint32_t (*game_clock_us)(void);
/* Retain the exact renderer convention rather than a runtime trigonometric
 * library. This corrected table is monotonic and includes all quarter steps. */
static const int16_t sine_table[65] = {
0,402,804,1205,1606,2006,2404,2801,3196,3590,3981,4370,4756,5139,5520,
5897,6270,6639,7005,7366,7723,8076,8423,8765,9102,9434,9760,10080,10394,
10702,11003,11297,11585,11866,12140,12406,12665,12916,13160,13395,13623,
13842,14053,14256,14449,14635,14811,14978,15137,15286,15426,15557,15679,
15791,15893,15986,16069,16143,16207,16261,16305,16340,16364,16379,16384};
static int sine_q14(uint8_t a) {
    unsigned q=a>>6, i=a&63u;
    if(q==0u) return sine_table[i];
    if(q==1u) return sine_table[64u-i];
    if(q==2u) return -sine_table[i];
    return -sine_table[64u-i];
}
unsigned DoomLiteGame_Direction8(int32_t dx,int32_t dy,uint8_t facing) {
    if(!dx&&!dy) return 0u;
    const int32_t cosine=sine_q14((uint8_t)(facing+64u)),sine=sine_q14(facing);
    const int64_t forward=(int64_t)dx*cosine+(int64_t)dy*sine;
    const int64_t side=(int64_t)dy*cosine-(int64_t)dx*sine;
    int64_t x,y;unsigned quadrant;
    if(forward>=0) {
        if(side>=0) {x=forward;y=side;quadrant=0u;}
        else {x=-side;y=forward;quadrant=6u;}
    } else {
        if(side>=0) {x=side;y=-forward;quadrant=2u;}
        else {x=-forward;y=-side;quadrant=4u;}
    }
    /* sin/cos(22.5 degrees), Q14. Comparing cross products avoids atan,
     * floating point, division, and a per-actor lookup table. */
    const unsigned octant=y*15137 < x*6270 ? 0u :
                          y*6270 < x*15137 ? 1u : 2u;
    return (quadrant+octant)&7u;
}
static int32_t fdiv(int32_t n,int32_t d) { return n>=0?n/d:(n+1)/d-1; }
static int32_t absi(int32_t n) { return n<0?-n:n; }
static int64_t sq(int32_t n) { return (int64_t)n*n; }
static uint32_t stamp(void) { return game_clock_us?game_clock_us():0u; }
static void prof(DoomLiteGame *g,unsigned kind,uint32_t started) {
    ++g->profile.calls[kind];
    if(game_clock_us) {
        uint32_t elapsed=stamp()-started;
        g->profile.total_us[kind]+=elapsed;
        if(elapsed>g->profile.max_us[kind]) g->profile.max_us[kind]=elapsed;
    }
}
void DoomLiteGame_SetClock(uint32_t (*clock_us)(void)) { game_clock_us=clock_us; }
void DoomLiteGame_TakeProfile(DoomLiteGame *g,DoomLiteGameProfile *out) {
    if(!g||!out) return;
    *out=g->profile; memset(&g->profile,0,sizeof(g->profile));
}
static int medium(const DoomMapThing *t) { return (t->options&2u)&&!(t->options&16u); }
static unsigned hp_for_type(unsigned type) {
    switch(type) { case 3004:return 20;case 9:return 30;case 3001:return 60;
        case 3002:case 58:return 150;default:return 0; }
}
static int bit(const uint8_t *bits,unsigned index) { return (bits[index>>3]>>(index&7u))&1u; }
static void setbit(uint8_t *bits,unsigned index) { bits[index>>3]|=(uint8_t)(1u<<(index&7u)); }
static int pickup_type(unsigned t) {
    switch(t) { case 5:case 6:case 13:case 38:case 39:case 40:
        case 2001:case 2002:case 2003:case 2004:case 2005:case 2006:
        case 2007:case 2008:case 2010:case 2011:case 2012:case 2013:
        case 2014:case 2015:case 2018:case 2019:case 2022:case 2023:
        case 2024:case 2025:case 2026:case 2046:case 2047:case 2048:
        case 2049:case 8:case 17:case 82:case 83:return 1;default:return 0; }
}
uint16_t DoomLiteGame_PlayerSector(const DoomLiteGame *g) {
    return g&&g->map?DoomMap_SectorAt(g->map,fdiv(g->x_q8,256),fdiv(g->y_q8,256)):DOOM_MAP_NO_SECTOR;
}
int DoomLiteGame_FloorHeight(const DoomLiteGame *g,uint16_t s) {
    return g&&g->map&&s<g->map->sector_count?g->sector_state[s].floor:0;
}
int DoomLiteGame_CeilingHeight(const DoomLiteGame *g,uint16_t s) {
    return g&&g->map&&s<g->map->sector_count?g->sector_state[s].ceiling:0;
}
unsigned DoomLiteGame_DoorLiftQ8(const DoomLiteGame *g,uint16_t s) {
    if(!g||!g->map||s>=g->map->sector_count) return 0;
    const DoomMapSector *sec=&g->map->sectors[s];
    int closed=sec->floor, open=sec->lowest_ceiling-4;
    if(open<=closed) return 0;
    int lift=g->sector_state[s].ceiling-closed;
    if(lift<=0) return 0;
    if(lift>=open-closed) return 256;
    return (unsigned)(lift*256/(open-closed));
}
static void closest(int32_t x,int32_t y,const DoomMapLine *l,int32_t *cx,int32_t *cy) {
    int64_t dx=l->x2-l->x1,dy=l->y2-l->y1;
    int64_t len=dx*dx+dy*dy, p=(int64_t)(x-l->x1)*dx+(int64_t)(y-l->y1)*dy;
    if(p<0) p=0;
    if(p>len) p=len;
    *cx=l->x1+(int32_t)(len?dx*p/len:0);
    *cy=l->y1+(int32_t)(len?dy*p/len:0);
}
static int near_line(int32_t x,int32_t y,const DoomMapLine *l,int radius) {
    if(x<(int32_t)(l->x1<l->x2?l->x1:l->x2)-radius ||
       x>(int32_t)(l->x1>l->x2?l->x1:l->x2)+radius ||
       y<(int32_t)(l->y1<l->y2?l->y1:l->y2)-radius ||
       y>(int32_t)(l->y1>l->y2?l->y1:l->y2)+radius) return 0;
    int32_t cx,cy; closest(x,y,l,&cx,&cy);
    /* Tangency is contact, not overlap: a diameter-wide passage stays
     * traversable while a position even one unit into either wall is solid. */
    return sq(x-cx)+sq(y-cy)<(int64_t)radius*radius;
}
static int64_t side_of(int32_t x,int32_t y,const DoomMapLine *l) {
    return (int64_t)(l->x2-l->x1)*(y-l->y1)-(int64_t)(l->y2-l->y1)*(x-l->x1);
}
static int crosses(int32_t ax,int32_t ay,int32_t bx,int32_t by,const DoomMapLine *l) {
    if((ax<l->x1&&bx<l->x1&&ax<l->x2&&bx<l->x2)||
       (ax>l->x1&&bx>l->x1&&ax>l->x2&&bx>l->x2)||
       (ay<l->y1&&by<l->y1&&ay<l->y2&&by<l->y2)||
       (ay>l->y1&&by>l->y1&&ay>l->y2&&by>l->y2)) return 0;
    int64_t a=side_of(ax,ay,l),b=side_of(bx,by,l);
    if((a<0&&b<0)||(a>0&&b>0)) return 0;
    int64_t dx=bx-ax,dy=by-ay;
    int64_t c=dx*(l->y1-ay)-dy*(l->x1-ax),d=dx*(l->y2-ay)-dy*(l->x2-ax);
    return !((c<0&&d<0)||(c>0&&d>0));
}
static int line_blocked(const DoomLiteGame *g,const DoomMapLine *l,int monster) {
    if((l->flags&1u)||(monster&&(l->flags&2u))||l->front_sector>=g->map->sector_count||l->back_sector>=g->map->sector_count) return 1;
    int fa=g->sector_state[l->front_sector].floor, fb=g->sector_state[l->back_sector].floor;
    int ca=g->sector_state[l->front_sector].ceiling,cb=g->sector_state[l->back_sector].ceiling;
    int low=fa>fb?fa:fb,high=ca<cb?ca:cb;
    /* Step-up is checked against the destination sector in solid_at(). A
     * high floor on the OTHER side must not trap a player just after a drop. */
    return high-low<PLAYER_HEIGHT;
}
static int solid_at(const DoomLiteGame *g,int32_t xq,int32_t yq,int radius,int floor,int monster,uint32_t *line_tests) {
    if(!g||!g->map) return 1;
    const DoomMap *m=g->map;
    int32_t x=fdiv(xq,256),y=fdiv(yq,256);
    int32_t gx0=fdiv(x-radius-m->grid_x0,DOOM_MAP_CELL_SIZE),gx1=fdiv(x+radius-m->grid_x0,DOOM_MAP_CELL_SIZE);
    int32_t gy0=fdiv(y-radius-m->grid_y0,DOOM_MAP_CELL_SIZE),gy1=fdiv(y+radius-m->grid_y0,DOOM_MAP_CELL_SIZE);
    if(gx0<0||gy0<0||gx1>=m->grid_width||gy1>=m->grid_height) return 1;
    uint16_t sector=DoomMap_SectorAt(m,x,y);
    if(sector>=m->sector_count||g->sector_state[sector].ceiling-g->sector_state[sector].floor<PLAYER_HEIGHT||g->sector_state[sector].floor-floor>24) return 1;
    for(int32_t gy=gy0;gy<=gy1;++gy) for(int32_t gx=gx0;gx<=gx1;++gx) {
        const DoomMapCell *c=DoomMap_Cell(m,gx,gy);
        if(!c) continue;
        for(unsigned j=0;j<c->count;++j) {
            const DoomMapLine *l=&m->lines[m->cell_refs[c->first_ref+j]];
            if(line_tests) ++*line_tests;
            if(line_blocked(g,l,monster)&&near_line(x,y,l,radius)) return 1;
        }
    }
    return 0;
}
static int solid_actor_at(const DoomLiteGame *g,int32_t xq,int32_t yq,int radius,int floor) {
    int reach=(radius+12)*256;
    for(unsigned i=0;i<g->actor_count;++i) {
        const DoomLiteActor *a=&g->actors[i];
        if(a->health<=0||!(a->flags&DL_ACTOR_SOLID)) continue;
        int32_t dx=xq-a->x_q8,dy=yq-a->y_q8;
        if(dx<=-reach||dx>=reach||dy<=-reach||dy>=reach) continue;
        if(absi(DoomLiteGame_FloorHeight(g,a->sector)-floor)>=PLAYER_HEIGHT) continue;
        if(sq(dx)+sq(dy)<(int64_t)reach*reach) return 1;
    }
    return 0;
}
int DoomLiteGame_IsSolid(const DoomLiteGame *g,int32_t xq,int32_t yq) {
    uint16_t sector=DoomLiteGame_PlayerSector(g);
    int floor=DoomLiteGame_FloorHeight(g,sector);
    return solid_at(g,xq,yq,PLAYER_RADIUS,floor,0,NULL)||solid_actor_at(g,xq,yq,PLAYER_RADIUS,floor);
}
static int collide(DoomLiteGame *g,int32_t xq,int32_t yq,int radius,int floor,int monster) {
    uint32_t started=stamp(); ++g->metrics.collision_queries;
    int result=solid_at(g,xq,yq,radius,floor,monster,&g->metrics.collision_line_tests);
    if(!result&&!monster) result=solid_actor_at(g,xq,yq,radius,floor);
    prof(g,DL_PROFILE_COLLISION,started);return result;
}
static int sight_cell(DoomLiteGame *g,int gx,int gy,int32_t ax,int32_t ay,int32_t bx,int32_t by,int eye) {
    const DoomMapCell *cell=DoomMap_Cell(g->map,gx,gy);
    if(!cell) return 1;
    for(unsigned j=0;j<cell->count;++j) {
        const DoomMapLine *l=&g->map->lines[g->map->cell_refs[cell->first_ref+j]];
        ++g->metrics.los_line_tests;
        if(!crosses(ax,ay,bx,by,l)) continue;
        if(l->front_sector>=g->map->sector_count||l->back_sector>=g->map->sector_count) return 0;
        int fa=g->sector_state[l->front_sector].floor,fb=g->sector_state[l->back_sector].floor;
        int ca=g->sector_state[l->front_sector].ceiling,cb=g->sector_state[l->back_sector].ceiling;
        int low=fa>fb?fa:fb,high=ca<cb?ca:cb;
        /* Doom's use trace accepts a positive two-sided opening, even a
         * slot too short for the player. Sight/hitscan tests an eye height. */
        if(eye==INT_MIN) {if(high<=low) return 0;}
        else if(eye<=low||eye>=high) return 0;
    }
    return 1;
}
static int sight(DoomLiteGame *g,int32_t ax,int32_t ay,int32_t bx,int32_t by,int eye) {
    uint32_t started=stamp(); ++g->metrics.los_queries;
    int32_t dx=bx-ax,dy=by-ay;
    int span=absi(dx)>absi(dy)?absi(dx):absi(dy);
    int visible=span<=1024;
    int gx=fdiv(ax-g->map->grid_x0,32),gy=fdiv(ay-g->map->grid_y0,32);
    int stepx=dx<0?-1:1,stepy=dy<0?-1:1;
    uint32_t adx=(uint32_t)absi(dx),ady=(uint32_t)absi(dy);
    uint32_t nx=UINT32_MAX,ny=UINT32_MAX;
    if(dx) nx=(uint32_t)((g->map->grid_x0+(gx+(dx>0))*32-ax)*stepx);
    if(dy) ny=(uint32_t)((g->map->grid_y0+(gy+(dy>0))*32-ay)*stepy);
    unsigned visited=0;
    /* Rational supercover DDA compares exact crossing fractions; no rounded
     * delta accumulates and no 64-bit division occurs along the sight ray. */
    while(visible) {
        if(++visited>MAX_LOS_STEPS||gx<0||gy<0||gx>=g->map->grid_width||gy>=g->map->grid_height) {visible=0;break;}
        if(!sight_cell(g,gx,gy,ax,ay,bx,by,eye)) {visible=0;break;}
        if((!dx||nx>adx)&&(!dy||ny>ady)) break;
        uint32_t cross_x=dx?nx*ady:UINT32_MAX,cross_y=dy?ny*adx:UINT32_MAX;
        if(dx&&dy&&cross_x==cross_y) {
            if(!sight_cell(g,gx+stepx,gy,ax,ay,bx,by,eye)||!sight_cell(g,gx,gy+stepy,ax,ay,bx,by,eye)) {visible=0;break;}
            gx+=stepx;gy+=stepy;nx+=32u;ny+=32u;
        } else if(dx&&(!dy||cross_x<cross_y)) {gx+=stepx;nx+=32u;}
        else {gy+=stepy;ny+=32u;}
    }
    prof(g,DL_PROFILE_LOS,started);return visible;
}
int DoomLiteGame_ThingActive(const DoomLiteGame *g,unsigned i) {
    if(!g||!g->map||i>=g->map->thing_count) return 0;
    const DoomMapThing *t=&g->map->things[i];
    if(!medium(t)||t->type<=4u||t->type==14u) return 0;
    if(g->thing_actor[i]!=NO_ACTOR) return g->actors[g->thing_actor[i]].health>0;
    return !bit(g->collected,i);
}
unsigned DoomLiteGame_VisualCount(const DoomLiteGame *g) { return g?g->actor_count:0u; }
int DoomLiteGame_GetVisual(const DoomLiteGame *g,unsigned i,DoomLiteVisual *v) {
    if(!g||!v||i>=g->actor_count) return 0;
    const DoomLiteActor *a=&g->actors[i];
    *v=(DoomLiteVisual){a->x_q8,a->y_q8,g->map->things[a->thing_index].type,a->sector,a->state,a->frame,a->facing};return 1;
}
int DoomLiteGame_InitMap(DoomLiteGame *g,unsigned index) {
    if(!g) return 0;
    const DoomMap *m=DoomMap_Get(index);
    if(!m||m->thing_count>DOOM_MAP_MAX_THINGS||m->sector_count>DOOM_MAP_MAX_SECTORS||m->line_count>DOOM_MAP_MAX_LINES) return 0;
    memset(g,0,sizeof(*g)); memset(g->thing_actor,NO_ACTOR,sizeof(g->thing_actor));
    g->map=m;g->map_index=(uint8_t)index;g->contrast=1;g->health=100;g->ammo=50;g->weapons=3;
    g->x_q8=(int32_t)m->start_x*256;g->y_q8=(int32_t)m->start_y*256;
    g->facing=(uint8_t)(m->start_angle*256u/360u);g->player_sector=m->start_sector;
    for(unsigned s=0;s<m->sector_count;++s) g->sector_state[s]=(DoomLiteSectorState){m->sectors[s].floor,m->sectors[s].ceiling,m->sectors[s].special};
    for(unsigned i=0;i<m->thing_count;++i) {
        const DoomMapThing *t=&m->things[i];
        if(t->type==14u) { if(g->teleport_count<16u) g->teleport_refs[g->teleport_count++]=(uint16_t)i;else ++g->metrics.pool_overflows; }
        if(!medium(t)) continue;
        unsigned hp=hp_for_type(t->type);
        if(hp) {
            if(g->actor_count>=DOOM_LITE_GAME_ACTORS) { ++g->metrics.pool_overflows;continue; }
            unsigned n=g->actor_count++;g->thing_actor[i]=(uint8_t)n;
            g->actors[n]=(DoomLiteActor){(int32_t)t->x*256,(int32_t)t->y*256,(uint16_t)i,t->sector,(int16_t)hp,DL_ACTOR_WALK,0u,8u,0u,DL_ACTOR_SOLID,(uint8_t)(t->angle*256u/360u)};
            ++g->total_enemies;
        } else if(pickup_type(t->type)) g->pickup_refs[g->pickup_count++]=(uint16_t)i;
    }
    g->metrics.actor_high_water=g->actor_count;return g->metrics.pool_overflows==0;
}
void DoomLiteGame_Init(DoomLiteGame *g) { (void)DoomLiteGame_InitMap(g,0u); }
int DoomLiteGame_NextMap(DoomLiteGame *g) {
    if(!g||!g->map) return 0;
    unsigned next=g->map_index+1u;uint16_t hp=g->health,ammo=g->ammo,armor=g->armor;
    uint16_t shells=g->shells,rockets=g->rockets,cells=g->cells;
    uint8_t armor_class=g->armor_class,contrast=g->contrast,weapons=g->weapons,backpack=g->backpack;
    if(next>=DOOM_MAP_COUNT) return DoomLiteGame_InitMap(g,0);
    if(!DoomLiteGame_InitMap(g,next)) return 0;
    g->health=hp?hp:100;g->ammo=ammo;g->armor=armor;g->armor_class=armor_class;g->contrast=contrast;
    g->shells=shells;g->rockets=rockets;g->cells=cells;g->weapons=weapons;g->backpack=backpack;return 1;
}
static DoomLiteMover *mover_for(DoomLiteGame *g,uint16_t sector) {
    for(unsigned i=0;i<DOOM_LITE_GAME_MOVERS;++i) if(g->movers[i].kind&&g->movers[i].sector==sector) return &g->movers[i];
    return NULL;
}
static DoomLiteMover *new_mover(DoomLiteGame *g,uint16_t sector) {
    DoomLiteMover *existing=mover_for(g,sector); if(existing) return existing;
    for(unsigned i=0;i<DOOM_LITE_GAME_MOVERS;++i) if(!g->movers[i].kind) {
        memset(&g->movers[i],0,sizeof(g->movers[i]));g->movers[i].sector=sector;
        ++g->metrics.active_movers;
        if(g->metrics.active_movers>g->metrics.mover_high_water) g->metrics.mover_high_water=g->metrics.active_movers;
        return &g->movers[i];
    }
    ++g->metrics.pool_overflows;return NULL;
}
static void finish_mover(DoomLiteGame *g,DoomLiteMover *m) {
    m->kind=DL_MOVER_NONE;if(g->metrics.active_movers) --g->metrics.active_movers;
}
static int body_ceiling(const DoomLiteGame *g,int32_t xq,int32_t yq,int radius,unsigned sector) {
    int limit=g->sector_state[sector].ceiling;
    int x=fdiv(xq,256),y=fdiv(yq,256);
    int gx0=fdiv(x-radius-g->map->grid_x0,32),gx1=fdiv(x+radius-g->map->grid_x0,32);
    int gy0=fdiv(y-radius-g->map->grid_y0,32),gy1=fdiv(y+radius-g->map->grid_y0,32);
    for(int gy=gy0;gy<=gy1;++gy) for(int gx=gx0;gx<=gx1;++gx) {
        const DoomMapCell *c=DoomMap_Cell(g->map,gx,gy);if(!c) continue;
        for(unsigned j=0;j<c->count;++j) {
            const DoomMapLine *l=&g->map->lines[g->map->cell_refs[c->first_ref+j]];
            if(l->front_sector>=g->map->sector_count||l->back_sector>=g->map->sector_count||!near_line(x,y,l,radius)) continue;
            if(l->front_sector==sector&&g->sector_state[l->back_sector].ceiling<limit) limit=g->sector_state[l->back_sector].ceiling;
            if(l->back_sector==sector&&g->sector_state[l->front_sector].ceiling<limit) limit=g->sector_state[l->front_sector].ceiling;
        }
    }
    return limit;
}
static int floor_would_trap_body(const DoomLiteGame *g,unsigned sector,int next) {
    if(DoomLiteGame_PlayerSector(g)==sector&&next+PLAYER_HEIGHT>body_ceiling(g,g->x_q8,g->y_q8,PLAYER_RADIUS,sector)) return 1;
    for(unsigned i=0;i<g->actor_count;++i) {
        const DoomLiteActor *a=&g->actors[i];
        if(a->health>0&&a->sector==sector&&next+PLAYER_HEIGHT>body_ceiling(g,a->x_q8,a->y_q8,12,sector)) return 1;
    }
    return 0;
}
static int touches_sector(const DoomLiteGame *g,int32_t xq,int32_t yq,int radius,unsigned current,unsigned sector) {
    if(current==sector) return 1;
    const DoomMapSector *s=&g->map->sectors[sector];
    int x=fdiv(xq,256),y=fdiv(yq,256);
    if(x<s->x0-radius||x>s->x1+radius||y<s->y0-radius||y>s->y1+radius) return 0;
    int gx0=fdiv(x-radius-g->map->grid_x0,32),gx1=fdiv(x+radius-g->map->grid_x0,32);
    int gy0=fdiv(y-radius-g->map->grid_y0,32),gy1=fdiv(y+radius-g->map->grid_y0,32);
    for(int gy=gy0;gy<=gy1;++gy) for(int gx=gx0;gx<=gx1;++gx) {
        const DoomMapCell *c=DoomMap_Cell(g->map,gx,gy);if(!c) continue;
        for(unsigned j=0;j<c->count;++j) {
            const DoomMapLine *l=&g->map->lines[g->map->cell_refs[c->first_ref+j]];
            if((l->front_sector==sector||l->back_sector==sector)&&near_line(x,y,l,radius)) return 1;
        }
    }
    return 0;
}
static int occupied_sector(const DoomLiteGame *g,unsigned sector) {
    if(touches_sector(g,g->x_q8,g->y_q8,PLAYER_RADIUS,DoomLiteGame_PlayerSector(g),sector)) return 1;
    for(unsigned i=0;i<g->actor_count;++i) {
        const DoomLiteActor *a=&g->actors[i];
        if(a->health>0&&touches_sector(g,a->x_q8,a->y_q8,12,a->sector,sector)) return 1;
    }
    return 0;
}
static uint32_t start_door(DoomLiteGame *g,uint16_t sector,unsigned speed,int stays_open,int manual) {
    if(sector>=g->map->sector_count) return 0;
    DoomLiteMover *m=mover_for(g,sector);
    if(m) {
        if(m->kind!=DL_MOVER_DOOR) return 0;
        /* A second use reverses a closing door. Opening doors continue;
         * safety waits prevent an actor or player being crushed. */
        if(m->phase==2u) { m->phase=0;m->wait=0;return DL_EVENT_DOOR_OPEN; }
        if(manual&&m->phase==1u&&!occupied_sector(g,sector)) { m->phase=2;m->wait=0;return DL_EVENT_DOOR_CLOSE; }
        return 0;
    }
    int target=g->map->sectors[sector].lowest_ceiling-4;
    if(target<=g->sector_state[sector].floor) return 0;
    if(g->sector_state[sector].ceiling>=target) return 0;
    m=new_mover(g,sector);if(!m) return DL_EVENT_LIMIT;
    m->kind=DL_MOVER_DOOR;m->target=(int16_t)target;m->home=g->sector_state[sector].floor;
    m->speed=(uint8_t)speed;m->return_after=(uint8_t)!stays_open;
    return DL_EVENT_DOOR_OPEN;
}
static uint32_t start_floor(DoomLiteGame *g,uint16_t sector,int target,unsigned speed,int platform) {
    if(sector>=g->map->sector_count||mover_for(g,sector)||target==g->sector_state[sector].floor) return 0;
    DoomLiteMover *m=new_mover(g,sector);if(!m) return DL_EVENT_LIMIT;
    m->kind=(uint8_t)(platform?DL_MOVER_PLATFORM:DL_MOVER_FLOOR);
    m->target=(int16_t)target;m->home=g->sector_state[sector].floor;m->speed=(uint8_t)speed;
    return DL_EVENT_MOVER;
}
static void step_movers(DoomLiteGame *g) {
    uint32_t started=stamp();
    for(unsigned i=0;i<DOOM_LITE_GAME_MOVERS;++i) {
        DoomLiteMover *m=&g->movers[i];if(!m->kind) continue;
        ++g->metrics.mover_steps;
        if(m->wait) { --m->wait;continue; }
        DoomLiteSectorState *s=&g->sector_state[m->sector];
        int16_t *height=m->kind==DL_MOVER_DOOR?&s->ceiling:&s->floor;
        int target=m->phase==2u?m->home:m->target;
        int next=*height;
        if(next<target) { next+=m->speed;if(next>target) next=target; }
        else if(next>target) { next-=m->speed;if(next<target) next=target; }
        if(m->kind==DL_MOVER_DOOR&&m->phase==2u&&next-s->floor<PLAYER_HEIGHT&&occupied_sector(g,m->sector)) {
            m->phase=0u;m->wait=0u;continue;
        }
        /* A rider straddling a low-ceiling room must board fully before the
         * platform rises. Pausing keeps the body able to move off the edge. */
        if(m->kind!=DL_MOVER_DOOR&&next>*height&&floor_would_trap_body(g,m->sector,next)) continue;
        *height=(int16_t)next;
        if(next!=target) continue;
        if(m->kind==DL_MOVER_DOOR) {
            if(m->phase==0u&&m->return_after) { m->phase=1u;m->wait=150u; }
            else if(m->phase==1u) m->phase=2u;
            else finish_mover(g,m);
        } else if(m->kind==DL_MOVER_PLATFORM&&m->phase==0u) { m->phase=1u;m->wait=105u; }
        else if(m->kind==DL_MOVER_PLATFORM&&m->phase==1u) m->phase=2u;
        else finish_mover(g,m);
    }
    prof(g,DL_PROFILE_MOVER,started);
}
static int walk_special(unsigned n) {
    switch(n) { case 2:case 19:case 38:case 58:case 88:case 97:case 109:case 120:return 1;default:return 0; }
}
static int use_special(unsigned n) {
    switch(n) { case 1:case 11:case 23:case 26:case 27:case 28:case 31:
        case 32:case 33:case 34:case 62:case 63:case 71:case 102:case 103:
        case 117:case 118:case 123:return 1;default:return 0; }
}
static int one_shot(unsigned n) {
    switch(n) { case 2:case 19:case 23:case 31:case 32:case 33:case 34:
        case 38:case 58:case 71:case 102:case 103:case 109:case 118:return 1;default:return 0; }
}
static uint32_t key_needed(const DoomLiteGame *g,unsigned n) {
    if((n==26u||n==32u)&&!g->blue_key) return DL_EVENT_NEED_BLUE;
    if((n==27u||n==34u)&&!g->yellow_key) return DL_EVENT_NEED_YELLOW;
    if((n==28u||n==33u)&&!g->red_key) return DL_EVENT_NEED_RED;
    return 0;
}
static uint32_t teleport_player(DoomLiteGame *g,const DoomMapLine *line,int32_t oldx,int32_t oldy) {
    if(g->teleport_cooldown||side_of(oldx,oldy,line)>0) return 0;
    for(unsigned i=0;i<g->teleport_count;++i) {
        const DoomMapThing *dest=&g->map->things[g->teleport_refs[i]];
        if(dest->sector>=g->map->sector_count||g->map->sectors[dest->sector].tag!=line->tag) continue;
        int32_t x=(int32_t)dest->x*256,y=(int32_t)dest->y*256;
        if(collide(g,x,y,PLAYER_RADIUS,g->sector_state[dest->sector].floor,0)) return 0;
        g->x_q8=x;g->y_q8=y;g->facing=(uint8_t)(dest->angle*256u/360u);
        g->player_sector=dest->sector;g->teleport_cooldown=18u;
        ++g->metrics.teleports;return DL_EVENT_TELEPORT;
    }
    return 0;
}
static uint32_t activate_line(DoomLiteGame *g,unsigned index,int walking,int32_t oldx,int32_t oldy) {
    const DoomMapLine *l=&g->map->lines[index];unsigned n=l->special;
    if(bit(g->triggered,index)||(walking?!walk_special(n):!use_special(n))) return 0;
    uint32_t events=key_needed(g,n);if(events) return events;
    if(n==11u) { g->completed=1;return DL_EVENT_EXIT; }
    if(n==97u) return teleport_player(g,l,oldx,oldy);
    uint16_t count=0;const uint16_t *targets=NULL;
    uint16_t manual_sector=l->back_sector;
    int manual=n==1u||n==26u||n==27u||n==28u||n==31u||n==32u||n==33u||n==34u||n==117u||n==118u;
    if(manual) { targets=&manual_sector;count=1; }
    else targets=DoomMap_TagSectors(g->map,l->tag,&count);
    for(unsigned i=0;i<count;++i) {
        unsigned sector=targets[i];if(sector>=g->map->sector_count) continue;
        const DoomMapSector *s=&g->map->sectors[sector];
        switch(n) {
            case 1:case 26:case 27:case 28:case 63:
                events|=start_door(g,(uint16_t)sector,2u,0,manual);break;
            case 117:events|=start_door(g,(uint16_t)sector,8u,0,1);break;
            case 2:case 31:case 32:case 33:case 34:case 103:
                events|=start_door(g,(uint16_t)sector,2u,1,manual);break;
            case 109:case 118:events|=start_door(g,(uint16_t)sector,8u,1,manual);break;
            case 19:case 102:events|=start_floor(g,(uint16_t)sector,s->highest_floor,1u,0);break;
            case 23:case 38:events|=start_floor(g,(uint16_t)sector,s->lowest_floor,1u,0);break;
            case 58:events|=start_floor(g,(uint16_t)sector,g->sector_state[sector].floor+24,1u,0);break;
            case 71:events|=start_floor(g,(uint16_t)sector,s->highest_floor+(s->highest_floor!=g->sector_state[sector].floor?8:0),4u,0);break;
            case 62:case 88:events|=start_floor(g,(uint16_t)sector,s->lowest_floor,4u,1);break;
            case 120:case 123:events|=start_floor(g,(uint16_t)sector,s->lowest_floor,8u,1);break;
            default:break;
        }
    }
    if(events&&one_shot(n)&&!(events&DL_EVENT_LIMIT)) setbit(g->triggered,index);
    return events;
}
static uint32_t use_nearest(DoomLiteGame *g) {
    int32_t x=fdiv(g->x_q8,256),y=fdiv(g->y_q8,256);
    int32_t dx=sine_q14((uint8_t)(g->facing+64u)),dy=sine_q14(g->facing);
    int best=-1;int64_t distance=(int64_t)DOOM_LITE_GAME_USE_REACH*DOOM_LITE_GAME_USE_REACH+1;
    /* This only runs on F2's edge: 42/63 immutable specials, not all lines. */
    for(unsigned i=0;i<g->map->special_line_count;++i) {
        unsigned ref=g->map->special_line_refs[i];const DoomMapLine *l=&g->map->lines[ref];
        if(!use_special(l->special)||bit(g->triggered,ref)||side_of(x,y,l)>0) continue;
        int32_t cx,cy;closest(x,y,l,&cx,&cy);
        int64_t d=sq(x-cx)+sq(y-cy);
        if(d<distance&&((int64_t)(cx-x)*dx+(int64_t)(cy-y)*dy>0||d<64)) {
            int span=absi(cx-x)>absi(cy-y)?absi(cx-x):absi(cy-y);
            int ex=cx,ey=cy;
            if(span>2) {ex-=(cx-x)*2/span;ey-=(cy-y)*2/span;}
            if(sight(g,x,y,ex,ey,INT_MIN)) {best=(int)ref;distance=d;}
        }
    }
    return best<0?0:activate_line(g,(unsigned)best,0,x,y);
}
static uint32_t walk_lines(DoomLiteGame *g,int32_t oldxq,int32_t oldyq) {
    int32_t ax=fdiv(oldxq,256),ay=fdiv(oldyq,256),bx=fdiv(g->x_q8,256),by=fdiv(g->y_q8,256);
    if(ax==bx&&ay==by) return 0;
    uint32_t events=0;
    /* A four-unit step can briefly cross a third cell at a grid corner.
     * Its endpoint AABB covers at most four cells, including that cell.
     * Duplicate references are harmless for repeaters and one-shot bits. */
    int gx0=fdiv((ax<bx?ax:bx)-g->map->grid_x0,32),gx1=fdiv((ax>bx?ax:bx)-g->map->grid_x0,32);
    int gy0=fdiv((ay<by?ay:by)-g->map->grid_y0,32),gy1=fdiv((ay>by?ay:by)-g->map->grid_y0,32);
    for(int gy=gy0;gy<=gy1;++gy) for(int gx=gx0;gx<=gx1;++gx) {
        const DoomMapCell *cell=DoomMap_Cell(g->map,gx,gy);if(!cell) continue;
        for(unsigned i=0;i<cell->count;++i) {
            unsigned ref=g->map->cell_refs[cell->first_ref+i];const DoomMapLine *l=&g->map->lines[ref];
            if(walk_special(l->special)&&crosses(ax,ay,bx,by,l)) {
                events|=activate_line(g,ref,1,ax,ay);
                if(events&DL_EVENT_TELEPORT) return events;
            }
        }
    }
    return events;
}
static uint16_t add_cap(uint16_t value,unsigned add,unsigned cap) {
    unsigned result=value+add;return (uint16_t)(result>cap?cap:result);
}
static uint32_t pickup(DoomLiteGame *g) {
    int32_t px=fdiv(g->x_q8,256),py=fdiv(g->y_q8,256);uint32_t events=0;
    for(unsigned n=0;n<g->pickup_count;++n) {
        unsigned i=g->pickup_refs[n];if(bit(g->collected,i)) continue;
        const DoomMapThing *t=&g->map->things[i];++g->metrics.pickup_tests;
        if(sq(t->x-px)+sq(t->y-py)>PICKUP_REACH*PICKUP_REACH) continue;
        if(t->sector>=g->map->sector_count) continue;
        /* Pickup contact uses the player's full body height, unlike the
         * 24-unit step-up limit. E1M2's red key is on a 32-unit ledge. */
        int delta=g->sector_state[t->sector].floor-DoomLiteGame_FloorHeight(g,g->player_sector);
        if(delta>PLAYER_HEIGHT||delta<-8) continue;
        uint16_t before;unsigned cap=g->backpack?400u:200u;int accepted=1;
        switch(t->type) {
            case 5:case 40:g->blue_key=1;events|=DL_EVENT_BLUE_KEY;break;
            case 13:case 38:g->red_key=1;events|=DL_EVENT_RED_KEY;break;
            case 6:case 39:g->yellow_key=1;events|=DL_EVENT_YELLOW_KEY;break;
            case 2007:case 2048:before=g->ammo;g->ammo=add_cap(g->ammo,t->type==2007?10:50,cap);accepted=g->ammo!=before;break;
            case 2008:case 2049:before=g->shells;g->shells=add_cap(g->shells,t->type==2008?4:20,g->backpack?100:50);accepted=g->shells!=before;break;
            case 2010:case 2046:before=g->rockets;g->rockets=add_cap(g->rockets,t->type==2010?1:5,g->backpack?100:50);accepted=g->rockets!=before;break;
            case 2047:case 17:before=g->cells;g->cells=add_cap(g->cells,t->type==2047?20:100,g->backpack?600:300);accepted=g->cells!=before;break;
            case 2011:case 2012:if(g->health>=100) accepted=0;else g->health=add_cap(g->health,t->type==2011?10:25,100);break;
            case 2013:g->health=add_cap(g->health,100,200);break;
            case 2014:g->health=add_cap(g->health,1,200);break;
            case 2015:g->armor=add_cap(g->armor,1,200);if(!g->armor_class) g->armor_class=1;break;
            case 2018:if(g->armor>=100) accepted=0;else {g->armor=100;g->armor_class=1;}break;
            case 2019:if(g->armor>=200) accepted=0;else {g->armor=200;g->armor_class=2;}break;
            case 2022:g->invulnerable_tics=30u*DOOM_LITE_GAME_HZ;break;
            case 2023:if(g->health<100) g->health=100;break;
            case 2025:g->suit_tics=60u*DOOM_LITE_GAME_HZ;break;
            case 83:g->health=200;g->armor=200;g->armor_class=2;break;
            case 8:g->backpack=1;g->ammo=add_cap(g->ammo,10,400);g->shells=add_cap(g->shells,4,100);g->rockets=add_cap(g->rockets,1,100);g->cells=add_cap(g->cells,20,600);break;
            case 2001:g->weapons|=4u;g->shells=add_cap(g->shells,8,g->backpack?100:50);break;
            case 2002:g->weapons|=8u;g->ammo=add_cap(g->ammo,20,cap);break;
            case 2003:g->weapons|=16u;g->rockets=add_cap(g->rockets,2,g->backpack?100:50);break;
            case 2004:g->weapons|=32u;g->cells=add_cap(g->cells,40,g->backpack?600:300);break;
            case 2005:g->weapons|=1u;break;
            case 2006:g->weapons|=64u;g->cells=add_cap(g->cells,40,g->backpack?600:300);break;
            case 82:g->weapons|=128u;g->shells=add_cap(g->shells,8,g->backpack?100:50);break;
            /* Invisibility and light amp do not alter this monochrome MVP's
             * rules, so leave them present rather than silently consume. */
            default:accepted=0;break;
        }
        if(accepted) {setbit(g->collected,i);events|=DL_EVENT_PICKUP;}
    }
    return events;
}
static uint32_t hurt_player(DoomLiteGame *g,unsigned damage) {
    if(!g->health||g->invulnerable_tics) return 0;
    if(g->armor) {
        unsigned saved=g->armor_class==2u?damage/2u:damage/3u;
        if(saved>g->armor) saved=g->armor;
        g->armor=(uint16_t)(g->armor-saved);damage-=saved;
        if(!g->armor) g->armor_class=0;
    }
    g->health=(uint16_t)(g->health>damage?g->health-damage:0u);
    return DL_EVENT_HURT;
}
static void actor_state(DoomLiteGame *g,DoomLiteActor *a,uint8_t state) {
    static const uint8_t state_tics[5]={8u,7u,6u,4u,0u};
    a->state=state;a->frame=0u;a->tics=state_tics[state];++g->metrics.state_transitions;
}
static void animate_actors(DoomLiteGame *g) {
    static const uint8_t frames[5]={4u,3u,1u,7u,1u};
    static const uint8_t durations[5]={8u,7u,6u,4u,0u};
    uint32_t started=stamp();
    for(unsigned i=0;i<g->actor_count;++i) {
        DoomLiteActor *a=&g->actors[i];
        if(a->cooldown) --a->cooldown;
        if(a->state==DL_ACTOR_CORPSE||(!(a->flags&DL_ACTOR_AWAKE)&&a->state==DL_ACTOR_WALK)) continue;
        if(a->tics) --a->tics;
        if(a->tics) continue;
        if(++a->frame<frames[a->state]) {a->tics=durations[a->state];continue;}
        if(a->state==DL_ACTOR_DEATH) {actor_state(g,a,DL_ACTOR_CORPSE);a->frame=6u;}
        else actor_state(g,a,DL_ACTOR_WALK);
    }
    prof(g,DL_PROFILE_ANIM,started);
}
static int actor_position_blocked(DoomLiteGame *g,unsigned index,int32_t xq,int32_t yq) {
    DoomLiteActor *a=&g->actors[index];
    if(collide(g,xq,yq,12,DoomLiteGame_FloorHeight(g,a->sector),1)) return 1;
    if(sq(fdiv(xq-g->x_q8,256))+sq(fdiv(yq-g->y_q8,256))<24*24) return 1;
    for(unsigned i=0;i<g->actor_count;++i) {
        const DoomLiteActor *other=&g->actors[i];if(i==index||!(other->flags&DL_ACTOR_SOLID)) continue;
        int32_t dx=fdiv(xq-other->x_q8,256),dy=fdiv(yq-other->y_q8,256);
        if(absi(dx)<24&&absi(dy)<24&&sq(dx)+sq(dy)<24*24) return 1;
    }
    return 0;
}
static uint32_t actor_use_door(DoomLiteGame *g,const DoomLiteActor *a) {
    int x=fdiv(a->x_q8,256),y=fdiv(a->y_q8,256);
    int gx0=fdiv(x-24-g->map->grid_x0,32),gx1=fdiv(x+24-g->map->grid_x0,32);
    int gy0=fdiv(y-24-g->map->grid_y0,32),gy1=fdiv(y+24-g->map->grid_y0,32);
    for(int gy=gy0;gy<=gy1;++gy) for(int gx=gx0;gx<=gx1;++gx) {
        const DoomMapCell *c=DoomMap_Cell(g->map,gx,gy);if(!c) continue;
        for(unsigned j=0;j<c->count;++j) {
            const DoomMapLine *l=&g->map->lines[g->map->cell_refs[c->first_ref+j]];
            if((l->special==1u||l->special==117u)&&near_line(x,y,l,24))
                return start_door(g,l->back_sector,l->special==117u?8u:2u,0,0);
        }
    }
    return 0;
}
static uint32_t ai_step(DoomLiteGame *g) {
    uint32_t started=stamp(),events=0;
    if(!g->actor_count) {prof(g,DL_PROFILE_AI,started);return 0;}
    int32_t px=fdiv(g->x_q8,256),py=fdiv(g->y_q8,256);
    unsigned visits=g->actor_count<DOOM_LITE_GAME_AI_BUDGET?g->actor_count:DOOM_LITE_GAME_AI_BUDGET;
    for(unsigned n=0;n<visits;++n) {
        unsigned index=g->ai_cursor++;if(g->ai_cursor>=g->actor_count) g->ai_cursor=0;
        DoomLiteActor *a=&g->actors[index];++g->metrics.ai_visits;
        if(a->health<=0||a->state!=DL_ACTOR_WALK) continue;
        int32_t ax=fdiv(a->x_q8,256),ay=fdiv(a->y_q8,256),dx=px-ax,dy=py-ay;
        int span=absi(dx)>absi(dy)?absi(dx):absi(dy);
        if(span>640) continue;
        int eye=DoomLiteGame_FloorHeight(g,a->sector)+32;
        if(!(a->flags&DL_ACTOR_AWAKE)) {
            if(span>512||!sight(g,ax,ay,px,py,eye)) continue;
            a->flags|=DL_ACTOR_AWAKE;++g->metrics.awake_actors;
        }
        unsigned type=g->map->things[a->thing_index].type;
        unsigned reach=(type==3002u||type==58u)?48u:256u;
        if(!a->cooldown&&sq(dx)+sq(dy)<(int64_t)reach*reach&&sight(g,ax,ay,px,py,eye)) {
            if(dx||dy) a->facing=(uint8_t)(DoomLiteGame_Direction8(dx,dy,0u)*32u);
            actor_state(g,a,DL_ACTOR_ATTACK);
            a->cooldown=(uint8_t)(type==3001u?49u:type==3002u||type==58u?28u:35u+index%9u);
            events|=hurt_player(g,type==3002u||type==58u?7u:type==3001u?6u:4u);
            continue;
        }
        if(span<25) continue;
        /* Eight agents receive navigation work per tic. Displacement is
         * scaled by their bounded round-robin interval, not render FPS. */
        unsigned stride=(g->actor_count+visits-1u)/visits;
        int units=(type==3002u||type==58u)?(int)stride*2:(int)stride;
        if(units>16) units=16;
        int32_t mx=(int32_t)((int64_t)dx*units*256/span),my=(int32_t)((int64_t)dy*units*256/span);
        const int32_t old_x=a->x_q8,old_y=a->y_q8;
        int moved=0;
        if(!actor_position_blocked(g,index,a->x_q8+mx,a->y_q8)) {a->x_q8+=mx;moved=1;}
        if(!actor_position_blocked(g,index,a->x_q8,a->y_q8+my)) {a->y_q8+=my;moved=1;}
        if(!moved) {
            events|=actor_use_door(g,a);
            /* A bounded perpendicular probe lets a pursuing actor slide
             * around a corner instead of retrying the same blocked vector. */
            int32_t side=(index&1u)?units*256:-units*256;
            if(absi(dx)>absi(dy)) {
                if(!actor_position_blocked(g,index,a->x_q8,a->y_q8+side)) {a->y_q8+=side;moved=1;}
            } else if(!actor_position_blocked(g,index,a->x_q8+side,a->y_q8)) {a->x_q8+=side;moved=1;}
        }
        if(moved) {
            a->facing=(uint8_t)(DoomLiteGame_Direction8(a->x_q8-old_x,a->y_q8-old_y,0u)*32u);
            a->sector=DoomMap_SectorAt(g->map,fdiv(a->x_q8,256),fdiv(a->y_q8,256));++g->metrics.actor_moves;
        }
        /* Simple wall sliding, not A* or original eight-direction chase.
         * No route search is allowed to consume an unbounded frame. */
    }
    prof(g,DL_PROFILE_AI,started);return events;
}
static uint32_t shoot(DoomLiteGame *g) {
    if(!g->ammo) return DL_EVENT_SHOT_MISS;
    --g->ammo;g->pistol_tics=15u;++g->metrics.shots;
    int32_t px=fdiv(g->x_q8,256),py=fdiv(g->y_q8,256);
    int32_t fx=sine_q14((uint8_t)(g->facing+64u)),fy=sine_q14(g->facing);
    int best=-1;int64_t best_forward=(int64_t)640*16384+1;
    for(unsigned i=0;i<g->actor_count;++i) {
        DoomLiteActor *a=&g->actors[i];if(a->health<=0) continue;
        int32_t dx=fdiv(a->x_q8,256)-px,dy=fdiv(a->y_q8,256)-py;
        int64_t forward=(int64_t)dx*fx+(int64_t)dy*fy;
        int64_t side=(int64_t)dx*fy-(int64_t)dy*fx,tolerance=(int64_t)12*16384+forward/12;
        if(forward<=0||forward>=best_forward||side<-tolerance||side>tolerance) continue;
        if(!sight(g,px,py,px+dx,py+dy,DoomLiteGame_FloorHeight(g,g->player_sector)+32)) continue;
        best=(int)i;best_forward=forward;
    }
    if(best<0) return DL_EVENT_SHOT_MISS;
    DoomLiteActor *a=&g->actors[best];
    if(!(a->flags&DL_ACTOR_AWAKE)) {a->flags|=DL_ACTOR_AWAKE;++g->metrics.awake_actors;}
    a->health=(int16_t)(a->health>25?a->health-25:0);
    if(!a->health) {
        a->flags&=(uint8_t)~DL_ACTOR_SOLID;if(g->metrics.awake_actors) --g->metrics.awake_actors;
        actor_state(g,a,DL_ACTOR_DEATH);++g->kills;
        return DL_EVENT_SHOT_HIT|DL_EVENT_KILL;
    }
    actor_state(g,a,DL_ACTOR_PAIN);a->cooldown=12u;return DL_EVENT_SHOT_HIT;
}
static uint32_t sector_effects(DoomLiteGame *g) {
    if(g->player_sector>=g->map->sector_count) return 0;
    DoomLiteSectorState *s=&g->sector_state[g->player_sector];uint32_t events=0;
    if(s->special==9u) {s->special=0u;++g->secrets;events|=DL_EVENT_SECRET;}
    if((g->ticks&31u)==0u&&!g->suit_tics) {
        if(s->special==5u) events|=hurt_player(g,10u);
        else if(s->special==7u) events|=hurt_player(g,5u);
        else if(s->special==4u||s->special==16u||s->special==11u) events|=hurt_player(g,20u);
        if(s->special==11u&&g->health<=10u) {g->completed=1u;events|=DL_EVENT_EXIT;}
    }
    return events;
}
uint32_t DoomLiteGame_Step(DoomLiteGame *g,uint16_t buttons) {
    if(!g||!g->map) return 0;
    ++g->ticks;if(g->pistol_tics) --g->pistol_tics;
    if(g->suit_tics) --g->suit_tics;
    if(g->invulnerable_tics) --g->invulnerable_tics;
    if(g->teleport_cooldown) --g->teleport_cooldown;
    uint16_t pressed=buttons&~g->previous_buttons;g->previous_buttons=(uint8_t)buttons;
    if(g->completed||!g->health) return 0;
    step_movers(g);animate_actors(g);
    if(!!(buttons&DL_GAME_LEFT)!=!!(buttons&DL_GAME_RIGHT)) {
        int half=g->turn_remainder+((buttons&DL_GAME_LEFT)?-3:3),whole=half/2;
        g->facing=(uint8_t)(g->facing+whole);g->turn_remainder=(int8_t)(half-whole*2);
    }
    uint32_t events=0;
    g->player_sector=DoomLiteGame_PlayerSector(g);
    if(!g->teleport_cooldown&&!!(buttons&DL_GAME_UP)!=!!(buttons&DL_GAME_DOWN)) {
        int direction=buttons&DL_GAME_UP?1:-1;
        int32_t ox=g->x_q8,oy=g->y_q8;
        int32_t dx=sine_q14((uint8_t)(g->facing+64u))*4*256*direction/16384;
        int32_t dy=sine_q14(g->facing)*4*256*direction/16384;
        int floor=DoomLiteGame_FloorHeight(g,g->player_sector);
        if(!collide(g,g->x_q8+dx,g->y_q8,PLAYER_RADIUS,floor,0)) g->x_q8+=dx;
        if(!collide(g,g->x_q8,g->y_q8+dy,PLAYER_RADIUS,floor,0)) g->y_q8+=dy;
        events|=walk_lines(g,ox,oy);g->player_sector=DoomLiteGame_PlayerSector(g);
    }
    if((buttons&(DL_GAME_UP|DL_GAME_DOWN))||(g->ticks&3u)==1u) events|=pickup(g);
    if(pressed&DL_GAME_USE) events|=use_nearest(g);
    if(pressed&DL_GAME_FIRE) events|=shoot(g);
    if(!g->completed) {events|=ai_step(g);events|=sector_effects(g);}
    return events;
}
