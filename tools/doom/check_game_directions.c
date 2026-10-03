/* Native directional sprite checks. Compile with DoomLiteGame.c, DoomMap.c
 * and -lm. Floating point is a host-only reference, never firmware code. */
#define DOOM_LITE_RAY_TEST
#define LCD_PIX_W 256
#define LCD_PIX_H 127
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../../System/applications/user/doom_lite/DoomLite.c"
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

static const double tau = 6.28318530717958647693;
static unsigned direction_checks, frame_checks, raster_checks;
static uint8_t guarded[LCD_PIX_W * LCD_PIX_H + 32u];

static void vector_at(double heading, int32_t *dx, int32_t *dy) {
    const double angle = heading * tau / 256.0;
    *dx = (int32_t)llround(cos(angle) * 1048576.0);
    *dy = (int32_t)llround(sin(angle) * 1048576.0);
}

static void check_directions(void) {
    /* Every original map heading is allowed, including non-45-degree angles.
     * Probe either side of each 22.5-degree boundary with a 0.1-degree margin;
     * table quantization is much smaller than this margin. */
    for (unsigned facing = 0u; facing < 256u; ++facing) {
        for (unsigned direction = 0u; direction < 8u; ++direction) {
            int32_t dx, dy;
            vector_at(facing + direction * 32.0, &dx, &dy);
            assert(DoomLiteGame_Direction8(dx, dy, (uint8_t)facing) == direction);
            ++direction_checks;
            const double margin = 0.1 * 256.0 / 360.0;
            vector_at(facing + direction * 32.0 + 16.0 - margin, &dx, &dy);
            assert(DoomLiteGame_Direction8(dx, dy, (uint8_t)facing) == direction);
            vector_at(facing + direction * 32.0 + 16.0 + margin, &dx, &dy);
            assert(DoomLiteGame_Direction8(dx, dy, (uint8_t)facing) == ((direction + 1u) & 7u));
            direction_checks += 2u;
        }
    }
    /* Exact fixed-point boundaries advance counterclockwise, also across
     * negative quadrants and the 7 -> 0 wrap. */
    const int32_t boundary[8][2] = {
        {15137,6270},{6270,15137},{-6270,15137},{-15137,6270},
        {-15137,-6270},{-6270,-15137},{6270,-15137},{15137,-6270}
    };
    for (unsigned i = 0u; i < 8u; ++i) {
        assert(DoomLiteGame_Direction8(boundary[i][0], boundary[i][1], 0u) == ((i + 1u) & 7u));
        ++direction_checks;
    }
    assert(DoomLiteGame_Direction8(0, 0, 73u) == 0u);
    assert(DoomLiteGame_Direction8(INT32_MAX, 0, 0u) == 0u);
    assert(DoomLiteGame_Direction8(INT32_MIN, 0, 0u) == 4u);
    assert(DoomLiteGame_Direction8(INT32_MAX, INT32_MIN, 0u) == 7u);
    assert(DoomLiteGame_Direction8(INT32_MIN, INT32_MIN, 32u) == 4u);
    direction_checks += 5u;
}

static void check_selection(void) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 0u));
    DoomLiteVisual visual = {.x_q8 = game.x_q8 + 128 * 256,
        .y_q8 = game.y_q8, .facing = 13u};
    const unsigned rotation = game_sprite_rotation(&game, &visual);
    for (unsigned facing = 0u; facing < 256u; ++facing) {
        game.facing = (uint8_t)facing;
        assert(game_sprite_rotation(&game, &visual) == rotation);
        ++direction_checks;
    }
    for (unsigned sequence = 0u; sequence < 4u; ++sequence) {
        const GameSpriteSequence *frames = &game_sprite_sequences[sequence];
        for (unsigned state = 0u; state < 9u; ++state)
            for (unsigned frame = 0u; frame < 256u; ++frame)
                for (unsigned direction = 0u; direction < 8u; ++direction) {
                    const int encoded = game_sprite_for_actor(frames->thing_type, state, frame, direction);
                    assert(encoded >= 0 && ((unsigned)encoded & GAME_SPRITE_INDEX_MASK) < GAME_SPRITE_COUNT);
                    if (state >= DL_ACTOR_DEATH) {
                        assert(encoded == game_sprite_for_actor(frames->thing_type, state, frame, 0u));
                        assert(!((unsigned)encoded & GAME_SPRITE_FLIP));
                    }
                    ++frame_checks;
                }
        for (unsigned direction = 0u; direction < 8u; ++direction)
            assert(game_sprite_for_actor(frames->thing_type, DL_ACTOR_WALK, 0u, direction + 8u) ==
                   game_sprite_for_actor(frames->thing_type, DL_ACTOR_WALK, 0u, direction));
    }
    assert(game_sprite_for_actor(65535u, 0u, 0u, 0u) == -1);
    for (unsigned direction = 0u; direction < 8u; ++direction)
        assert(game_sprite_for_actor(58u, DL_ACTOR_WALK, 1u, direction) ==
               game_sprite_for_actor(3002u, DL_ACTOR_WALK, 1u, direction));
}

static void check_mirrors(void) {
    DoomLiteGame game;
    assert(DoomLiteGame_InitMap(&game, 0u));
    game.facing = 0u;
    const unsigned sector = DoomLiteGame_PlayerSector(&game);
    const int eye = DoomLiteGame_FloorHeight(&game, (uint16_t)sector) + 41;
    unsigned pairs = 0u;
    for (unsigned sequence = 0u; sequence < 4u; ++sequence)
        for (unsigned direction = 0u; direction < 8u; ++direction) {
            const int encoded = game_sprite_for_actor(game_sprite_sequences[sequence].thing_type,
                                                      DL_ACTOR_WALK, 0u, direction);
            if (!((unsigned)encoded & GAME_SPRITE_FLIP)) continue;
            const unsigned index = (unsigned)encoded & GAME_SPRITE_INDEX_MASK;
            const GameSpriteAsset *asset = &game_sprites[index];
            AnimatedGameSprite sprites[LITE_SPRITE_LIMIT]; unsigned count = 0u;
            game_queue_sprite(sprites, &count, &game, game.x_q8 + 160 * 256,
                              game.y_q8, (uint16_t)sector, eye, encoded, 0);
            assert(count == 1u && sprites[0].texture == index && sprites[0].flip);
            const AnimatedGameSprite mirrored = sprites[0];
            assert(mirrored.left == LITE_W / 2 -
                   ((int)asset->world_width - asset->left) * mirrored.width / asset->world_width);
            count = 0u;
            game_queue_sprite(sprites, &count, &game, game.x_q8 + 160 * 256,
                              game.y_q8, (uint16_t)sector, eye, (int)index, 0);
            assert(count == 1u && !sprites[0].flip);
            assert(sprites[0].left == LITE_W / 2 - asset->left * sprites[0].width / asset->world_width);
            assert(mirrored.top == sprites[0].top && mirrored.depth == sprites[0].depth);
            for (unsigned x = 0u; x < mirrored.width; ++x) {
                const unsigned source = x * asset->width / mirrored.width;
                assert(game_sprite_source_x(&mirrored, asset, x) == asset->width - 1u - source);
                assert(game_sprite_source_x(&sprites[0], asset, x) == source);
                ++raster_checks;
            }
            /* Visibility must still reject behind-camera actors, even when
             * bit 15 is set in the selected reference. */
            count = 0u;
            game_queue_sprite(sprites, &count, &game, game.x_q8 - 160 * 256,
                              game.y_q8, (uint16_t)sector, eye, encoded, 0);
            assert(!count);
            ++pairs;
        }
    assert(pairs >= 9u); /* Zombie uses distinct patches; the other three share mirrored pairs. */
}

static void check_rendered_mirror(void) {
    DoomLiteGame game; assert(DoomLiteGame_InitMap(&game, 0u));
    game.facing = 0u; game.contrast = 1u;
    const uint16_t sector = DoomLiteGame_PlayerSector(&game);
    DoomMap map = *game.map;
    DoomMapThing thing = {.type = 9u, .options = 2u, .sector = sector};
    map.things = &thing; map.thing_count = 1u; game.map = &map;
    game.actor_count = 1u; game.thing_actor[0] = 0u;
    game.actors[0] = (DoomLiteActor){.x_q8=game.x_q8+160*256, .y_q8=game.y_q8,
        .thing_index=0u, .sector=sector, .health=20, .state=DL_ACTOR_WALK,
        .facing=160u}; /* Viewer lies at 180 degrees, relative direction 8. */
    const int encoded = game_sprite_for_actor(thing.type, DL_ACTOR_WALK, 0u, 7u);
    assert((unsigned)encoded & GAME_SPRITE_FLIP);
    const GameSpriteAsset *asset = &game_sprites[(unsigned)encoded & GAME_SPRITE_INDEX_MASK];
    AnimatedGameSprite queued[LITE_SPRITE_LIMIT]; unsigned count=0u;
    const int eye = DoomLiteGame_FloorHeight(&game, sector) + 41;
    game_queue_sprite(queued, &count, &game, game.actors[0].x_q8, game.actors[0].y_q8,
                      sector, eye, encoded, 0);
    assert(count==1u);
    const AnimatedGameSprite *sprite=&queued[0];
    memset(guarded,0xa5,sizeof(guarded));
    uint8_t *pixels=guarded+16u; memset(pixels,221,LCD_PIX_W*LCD_PIX_H);
    memset(game_span_count,0,sizeof(game_span_count)); game_wall_ready=1u;
    DoomLite_RenderGameThings(pixels,&game);
    unsigned drawn=0u;
    for(unsigned y=0u;y<DOOM_GAME_VIEW_H;++y)
        for(unsigned x=0u;x<LITE_W;++x) {
            unsigned expected=221u;
            if((int)x>=sprite->left&&(int)x<sprite->left+sprite->width&&
               (int)y>=sprite->top&&(int)y<sprite->top+sprite->height) {
                const unsigned sx=asset->width-1u-(x-sprite->left)*asset->width/sprite->width;
                const unsigned step=((unsigned)asset->height<<16)/sprite->height;
                const unsigned sy=((y-sprite->top)*step)>>16;
                const unsigned index=sy*asset->width+sx;
                const unsigned byte=asset->pixels[index/2u];
                const unsigned code=index%2u?byte/16u:byte%16u;
                if(code) {expected=game_sprite_gray[code];++drawn;}
            }
            assert(pixels[y*LITE_W+x]==expected); ++raster_checks;
        }
    assert(drawn>100u);
    for(unsigned i=0u;i<16u;++i) {
        assert(guarded[i]==0xa5u);
        assert(guarded[LCD_PIX_W*LCD_PIX_H+16u+i]==0xa5u);
    }
    for(unsigned i=DOOM_GAME_VIEW_H*LCD_PIX_W;i<LCD_PIX_W*LCD_PIX_H;++i) assert(pixels[i]==221u);
}

static void check_actor_headings(void) {
    DoomLiteGame game; assert(DoomLiteGame_InitMap(&game,0u));
    for(unsigned i=0u;i<game.actor_count;++i) {
        DoomLiteVisual visual; assert(DoomLiteGame_GetVisual(&game,i,&visual));
        assert(visual.facing==game.actors[i].facing);
        assert(visual.facing==(uint8_t)(game.map->things[game.actors[i].thing_index].angle*256u/360u));
    }
    /* Existing AI navigation and attack rules continue unchanged; heading
     * follows actual displacement (including wall sliding), or attack aim. */
    unsigned moves=0u, attacks=0u;
    game.invulnerable_tics=UINT16_MAX;
    game.x_q8=320*256;game.y_q8=1392*256;
    game.player_sector=DoomLiteGame_PlayerSector(&game);
    for(unsigned tic=0u;tic<350u;++tic) {
        DoomLiteActor before[DOOM_LITE_GAME_ACTORS];
        memcpy(before,game.actors,sizeof(before));
        (void)DoomLiteGame_Step(&game,0u);
        for(unsigned i=0u;i<game.actor_count;++i) {
            const DoomLiteActor *actor=&game.actors[i];
            const int32_t dx=actor->x_q8-before[i].x_q8,dy=actor->y_q8-before[i].y_q8;
            if(dx||dy) {
                assert(actor->facing==DoomLiteGame_Direction8(dx,dy,0u)*32u); ++moves;
            } else if(before[i].state==DL_ACTOR_WALK&&actor->state==DL_ACTOR_ATTACK) {
                const int32_t ax=(int32_t)floor(actor->x_q8/256.0),ay=(int32_t)floor(actor->y_q8/256.0);
                const int32_t px=(int32_t)floor(game.x_q8/256.0),py=(int32_t)floor(game.y_q8/256.0);
                assert(actor->facing==DoomLiteGame_Direction8(px-ax,py-ay,0u)*32u); ++attacks;
            }
            DoomLiteVisual visual;assert(DoomLiteGame_GetVisual(&game,i,&visual));
            assert(visual.facing==actor->facing);
        }
    }
    assert(moves>0u && attacks>0u);
    printf("DIRECTIONS_AI moves=%u attacks=%u\n",moves,attacks);
}

int main(void) {
    check_directions();check_selection();check_mirrors();check_rendered_mirror();check_actor_headings();
    printf("DIRECTIONS_OK vectors=%u frames=%u raster=%u queue_bytes=%u\n",
           direction_checks,frame_checks,raster_checks,(unsigned)sizeof(AnimatedGameSprite));
    return 0;
}
