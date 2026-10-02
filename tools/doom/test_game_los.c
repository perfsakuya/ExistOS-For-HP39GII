/* Independent full-linedef LOS reference versus the sparse traversal.
 * Include the module only to inspect its static sight() boundary; do not
 * compile DoomLiteGame.c a second time when linking this standalone test. */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "DoomLiteGame.c"

static uint32_t random_state = 0x39e1d002u;
static unsigned checked, mismatches, budget_rejections, natural_clear;

static uint32_t next_random(void) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return random_state;
}

static int exact_cross(int32_t ax,int32_t ay,int32_t bx,int32_t by,
                        const DoomMapLine *line) {
    /* Closed integer segment intersection independent of grid traversal. */
    if (ax > bx) { int32_t t=ax;ax=bx;bx=t;t=ay;ay=by;by=t; }
    const int32_t lx0=line->x1<line->x2?line->x1:line->x2;
    const int32_t lx1=line->x1>line->x2?line->x1:line->x2;
    const int32_t ly0=line->y1<line->y2?line->y1:line->y2;
    const int32_t ly1=line->y1>line->y2?line->y1:line->y2;
    if (bx<lx0 || ax>lx1 || (ay>by?by:ay)>ly1 || (ay>by?ay:by)<ly0)
        return 0;
    const int64_t sx=(int32_t)line->x2-line->x1,sy=(int32_t)line->y2-line->y1;
    const int64_t ab=(int64_t)(bx-ax)*(line->y1-ay)-(int64_t)(by-ay)*(line->x1-ax);
    const int64_t ac=(int64_t)(bx-ax)*(line->y2-ay)-(int64_t)(by-ay)*(line->x2-ax);
    const int64_t cd=sx*(ay-line->y1)-sy*(ax-line->x1);
    const int64_t ce=sx*(by-line->y1)-sy*(bx-line->x1);
    return ((ab<=0&&ac>=0)||(ab>=0&&ac<=0))&&
           ((cd<=0&&ce>=0)||(cd>=0&&ce<=0));
}

static int full_reference(const DoomLiteGame *game,int ax,int ay,int bx,int by,
                           int eye,unsigned *blocked_line) {
    for (unsigned i=0;i<game->map->line_count;++i) {
        const DoomMapLine *line=&game->map->lines[i];
        if (!exact_cross(ax,ay,bx,by,line)) continue;
        int blocked=line->front_sector>=game->map->sector_count ||
                    line->back_sector>=game->map->sector_count;
        if (!blocked) {
            const DoomLiteSectorState *front=&game->sector_state[line->front_sector];
            const DoomLiteSectorState *back=&game->sector_state[line->back_sector];
            const int floor=front->floor>back->floor?front->floor:back->floor;
            const int ceiling=front->ceiling<back->ceiling?front->ceiling:back->ceiling;
            blocked=eye<=floor || eye>=ceiling;
        }
        if (blocked) { if(blocked_line)*blocked_line=i;return 0; }
    }
    if (blocked_line) *blocked_line=UINT_MAX;
    return 1;
}

static int within_traversal_budget(const DoomMap *map,int ax,int ay,int bx,int by) {
    const int64_t dx=(int64_t)bx-ax,dy=(int64_t)by-ay;
    const int64_t adx=dx<0?-dx:dx,ady=dy<0?-dy:dy;
    if(adx>1024 || ady>1024) return 0;
    int gx=(ax-map->grid_x0)/32,gy=(ay-map->grid_y0)/32;
    const int sx=dx<0?-1:1,sy=dy<0?-1:1;
    int64_t nx=dx?(map->grid_x0+(gx+(dx>0))*32-ax)*sx:INT64_MAX;
    int64_t ny=dy?(map->grid_y0+(gy+(dy>0))*32-ay)*sy:INT64_MAX;
    for(unsigned visits=0;;++visits) {
        if(visits>=MAX_LOS_STEPS || gx<0 || gy<0 || gx>=map->grid_width || gy>=map->grid_height)
            return 0;
        if((!dx||nx>adx)&&(!dy||ny>ady)) return 1;
        if(dx&&dy&&nx*ady==ny*adx) {gx+=sx;gy+=sy;nx+=32;ny+=32;}
        else if(dx&&(!dy||nx*ady<ny*adx)) {gx+=sx;nx+=32;}
        else {gy+=sy;ny+=32;}
    }
}

static void compare_ray(DoomLiteGame *game,int ax,int ay,int bx,int by,int eye,
                        const char *label) {
    unsigned blocked_line;
    const int natural=full_reference(game,ax,ay,bx,by,eye,&blocked_line);
    const int budget=within_traversal_budget(game->map,ax,ay,bx,by);
    const int expected=natural&&budget;
    const int actual=sight(game,ax,ay,bx,by,eye);
    ++checked;
    if(natural) ++natural_clear;
    if(!budget) ++budget_rejections;
    if(expected!=actual) {
        if(mismatches<20u)
            printf("LOS mismatch %s map=%s (%d,%d)->(%d,%d) eye=%d expected=%d actual=%d block_line=%u budget=%d\n",
                   label,game->map->name,ax,ay,bx,by,eye,expected,actual,blocked_line,budget);
        ++mismatches;
    }
}

static int clamp_coordinate(int value,int low,int high) {
    return value<low?low:value>high?high:value;
}

static void random_rays(DoomLiteGame *game,unsigned count,int max_span,
                        const char *label) {
    const DoomMap *map=game->map;
    const int lowx=map->grid_x0,lowy=map->grid_y0;
    const int highx=lowx+map->grid_width*32-1,highy=lowy+map->grid_height*32-1;
    for(unsigned i=0;i<count;++i) {
        int ax=lowx+(int)(next_random()%(unsigned)(highx-lowx+1));
        int ay=lowy+(int)(next_random()%(unsigned)(highy-lowy+1));
        int bx=clamp_coordinate(ax+(int)(next_random()%(unsigned)(max_span*2+1))-max_span,lowx,highx);
        int by=clamp_coordinate(ay+(int)(next_random()%(unsigned)(max_span*2+1))-max_span,lowy,highy);
        const int eye=(int)(next_random()%640u)-192;
        compare_ray(game,ax,ay,bx,by,eye,label);
    }
}

static void boundary_rays(DoomLiteGame *game) {
    const DoomMap *map=game->map;
    const int highx=map->grid_x0+map->grid_width*32-1;
    const int highy=map->grid_y0+map->grid_height*32-1;
    for(unsigned i=0;i<512u;++i) {
        const int cx=map->grid_x0+32+(int)(next_random()%(map->grid_width-2u))*32;
        const int cy=map->grid_y0+32+(int)(next_random()%(map->grid_height-2u))*32;
        for(int offset=-1;offset<=1;++offset) {
            const int reach=32*(1+(int)(next_random()%20u));
            const int bx=clamp_coordinate(cx+reach, map->grid_x0,highx);
            const int by=clamp_coordinate(cy+reach+offset,map->grid_y0,highy);
            compare_ray(game,cx,cy,bx,by,32,"corner-forward");
            compare_ray(game,bx,by,cx,cy,32,"corner-reverse");
        }
    }
    for(unsigned i=0;i<game->map->line_count;++i) {
        const DoomMapLine *line=&game->map->lines[i];
        compare_ray(game,line->x1,line->y1,line->x1,line->y1,32,"degenerate-endpoint");
        compare_ray(game,line->x2,line->y2,line->x2,line->y2,32,"degenerate-endpoint");
    }
}

static void corner_regression(void) {
    static const DoomMapLine lines[]={{33,23,33,31,1,0,0,0,65535}};
    static const DoomMapSector sectors[]={{0,128,128,0,0,0,0,0,128,128,0,0,0,0,96,96}};
    static const uint16_t rows[]={0,1,1,1};
    static const DoomMapCell cells[]={{1,0,1}};
    static const uint16_t refs[]={0};
    DoomMap map={.name="CORNER",.grid_width=3,.grid_height=3,.line_count=1,.sector_count=1,
        .lines=lines,.sectors=sectors,.row_first=rows,.cells=cells,.cell_refs=refs};
    DoomLiteGame game={0};game.map=&map;game.sector_state[0].ceiling=128;
    compare_ray(&game,0,0,64,60,32,"original-16-unit-regression");
    compare_ray(&game,64,60,0,0,32,"original-regression-reverse");
    for(int dy=-2;dy<=2;++dy)
        compare_ray(&game,0,0,64,64+dy,32,"near-corner");
}

int main(void) {
    corner_regression();
    for(unsigned level=0;level<2u;++level) {
        DoomLiteGame game;
        if(!DoomLiteGame_InitMap(&game,level)) return 2;
        const unsigned before=checked;
        for(unsigned configuration=0;configuration<3u;++configuration) {
            for(unsigned s=0;s<game.map->sector_count;++s) {
                const DoomMapSector *sector=&game.map->sectors[s];
                game.sector_state[s].floor=configuration==0u?sector->floor:0;
                game.sector_state[s].ceiling=configuration==0u?sector->ceiling:
                    configuration==2u&&sector->ceiling<=sector->floor?0:512;
            }
            random_rays(&game,6000u,640,"random-640");
            random_rays(&game,4000u,1024,"random-1024");
        }
        boundary_rays(&game);
        printf("%s: %u LOS rays checked against full immutable geometry\n",game.map->name,checked-before);
    }
    printf("LOS reference result: checked=%u natural_clear=%u budget_rejections=%u mismatches=%u\n",
           checked,natural_clear,budget_rejections,mismatches);
    return mismatches?1:0;
}
