/* Portable LCD composition and animation check. Build with DoomLiteGame.c. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../System/applications/user/doom_lite/DoomLiteHud.c"

#define PIXELS (DOOM_GAME_LCD_W * DOOM_GAME_LCD_H)
static uint8_t frame[PIXELS + 16u], comparison[PIXELS];

static void clear_frame(void) {
    memset(frame, 0xa5, sizeof(frame));
    memset(frame + 8u, 147, PIXELS);
}

static void check_guards(void) {
    for (unsigned i = 0; i < 8u; ++i) {
        assert(frame[i] == 0xa5);
        assert(frame[sizeof(frame) - 1u - i] == 0xa5);
    }
}

static unsigned image_hash(const uint8_t *pixels, unsigned size) {
    unsigned hash = 2166136261u;
    while (size--) hash = (hash ^ *pixels++) * 16777619u;
    return hash;
}

int main(void) {
    DoomLiteGame game;
    DoomLiteGame_Init(&game);
    unsigned hashes[16];
    for (unsigned tic = 0; tic <= 15u; ++tic) {
        clear_frame();
        game.pistol_tics = (uint8_t)tic;
        DoomLite_DrawGameWeapon(frame + 8u, &game);
        for (unsigned i = DOOM_GAME_VIEW_H * DOOM_GAME_LCD_W; i < PIXELS; ++i)
            assert(frame[8u + i] == 147u); /* Weapon cannot overwrite status bar. */
        hashes[tic] = image_hash(frame + 8u, PIXELS);
        memcpy(comparison, frame + 8u, PIXELS);
        DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
        assert(!memcmp(comparison, frame + 8u, DOOM_GAME_VIEW_H * DOOM_GAME_LCD_W));
        check_guards();
    }
    assert(hashes[15] == hashes[10]); /* B + flash, six tics. */
    assert(hashes[9] != hashes[10]);  /* C + last flash tic. */
    assert(hashes[8] != hashes[9]);   /* C without flash. */
    assert(hashes[5] != hashes[8]);   /* B recoil return. */
    assert(hashes[0] != hashes[5]);   /* Idle A. */

    for (unsigned hp = 0; hp <= 100u; ++hp) {
        clear_frame();
        game.health = (uint8_t)hp;
        game.ammo = 200u;
        game.kills = 292u;
        game.blue_key = (uint8_t)(hp & 1u);
        DoomLite_DrawGameWeapon(frame + 8u, &game);
        DoomLite_DrawGameHud(frame + 8u, &game, "DOOR OPEN", 0u);
        check_guards();
    }
    clear_frame();
    game.completed = 1u;
    DoomLite_DrawGameWeapon(frame + 8u, &game);
    for (unsigned i = 0; i < PIXELS; ++i) assert(frame[8u + i] == 147u);
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    check_guards();
    game.completed = 0;
    game.health = 100u;
    clear_frame();
    DoomLite_DrawGameHud(frame + 8u, &game, "BLUE KEY", 2u);
    check_guards();
    clear_frame();
    draw_patch(frame + 8u, &ui_stbar, -100, -10, DOOM_GAME_VIEW_H);
    draw_patch(frame + 8u, &ui_pisgb0, 250, 90, DOOM_GAME_VIEW_H);
    check_guards();

    /* Real shooting owns animation time; holding F1 spends only one bullet. */
    DoomLiteGame_Init(&game);
    DoomLiteGame_Step(&game, DL_GAME_FIRE);
    assert(game.ammo == 49u && game.pistol_tics == 15u);
    for (unsigned i = 0; i < 15u; ++i) DoomLiteGame_Step(&game, DL_GAME_FIRE);
    assert(game.ammo == 49u && game.pistol_tics == 0u);
    DoomLiteGame_Step(&game, 0u);
    game.ammo = 0u;
    DoomLiteGame_Step(&game, DL_GAME_FIRE);
    assert(!game.pistol_tics && !game.ammo);
    DoomLiteGame_Init(&game);
    assert(!game.pistol_tics && game.ammo == 50u);
    puts("Classic UI: frame bounds, bar isolation, recoil/flash, empty ammo, reset passed");
    return 0;
}
