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

static unsigned region_hash(const uint8_t *pixels, unsigned x, unsigned y,
                             unsigned width, unsigned height) {
    unsigned hash = 2166136261u;
    for (unsigned row = y; row < y + height; ++row)
        for (unsigned col = x; col < x + width; ++col)
            hash = (hash ^ pixels[row * DOOM_GAME_LCD_W + col]) * 16777619u;
    return hash;
}

static void write_preview(const char *directory, const char *name) {
    char path[512];
    int length = snprintf(path, sizeof(path), "%s/%s.pgm", directory, name);
    assert(length > 0 && (unsigned)length < sizeof(path));
    FILE *output = fopen(path, "wb");
    assert(output);
    fprintf(output, "P5\n%u %u\n255\n", DOOM_GAME_LCD_W, DOOM_GAME_LCD_H);
    assert(fwrite(frame + 8u, 1u, PIXELS, output) == PIXELS);
    assert(!fclose(output));
}

int main(int argc, char **argv) {
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
        for (unsigned y = 0; y < DOOM_GAME_VIEW_H; ++y)
            for (unsigned x = 0; x < DOOM_GAME_LCD_W; ++x)
                assert(comparison[y * DOOM_GAME_LCD_W + x] ==
                       frame[8u + y * DOOM_GAME_LCD_W + x]);
        check_guards();
    }
    assert(hashes[15] == hashes[10]); /* B + flash, six tics. */
    assert(hashes[9] != hashes[10]);  /* C + last flash tic. */
    assert(hashes[8] != hashes[9]);   /* C without flash. */
    assert(hashes[5] != hashes[8]);   /* B recoil return. */
    assert(hashes[0] != hashes[5]);   /* Idle A. */

    unsigned shotgun[36];
    game.current_weapon = DL_WEAPON_SHOTGUN;
    game.weapons |= DL_WEAPON_SHOTGUN_OWNED;
    for (unsigned tic = 0; tic <= 35u; ++tic) {
        clear_frame();
        game.pistol_tics = (uint8_t)tic;
        DoomLite_DrawGameWeapon(frame + 8u, &game);
        for (unsigned i = DOOM_GAME_VIEW_H * DOOM_GAME_LCD_W; i < PIXELS; ++i)
            assert(frame[8u + i] == 147u);
        shotgun[tic] = image_hash(frame + 8u, PIXELS);
        memcpy(comparison, frame + 8u, PIXELS);
        DoomLite_DrawGameHud(frame + 8u, &game, "SHOTGUN F4 SWAP", 0u);
        assert(!memcmp(comparison, frame + 8u, DOOM_GAME_VIEW_H * DOOM_GAME_LCD_W));
        check_guards();
    }
    assert(shotgun[35] == shotgun[32]); /* A + first flash. */
    assert(shotgun[31] == shotgun[29] && shotgun[31] != shotgun[32]);
    assert(shotgun[28] == shotgun[24] && shotgun[28] == shotgun[9]); /* B. */
    assert(shotgun[23] == shotgun[19] && shotgun[23] == shotgun[14]); /* C. */
    assert(shotgun[18] == shotgun[15]); /* D, pump fully open. */
    assert(shotgun[0] == shotgun[4]); /* A ready again. */
    const unsigned poses[] = {0u, 5u, 10u, 15u, 29u, 32u};
    for (unsigned i = 0; i < sizeof(poses) / sizeof(poses[0]); ++i)
        for (unsigned j = i + 1u; j < sizeof(poses) / sizeof(poses[0]); ++j)
            assert(shotgun[poses[i]] != shotgun[poses[j]]);

    /* Flash lighting touches opaque weapon pixels only; no scene wash. */
    clear_frame();
    memset(comparison, 147, sizeof(comparison));
    draw_patch(comparison, &ui_shtga0, ui_shtga0.x, ui_shtga0.y, DOOM_GAME_VIEW_H);
    draw_patch_palette(frame + 8u, &ui_shtga0, ui_shtga0.x, ui_shtga0.y,
                       DOOM_GAME_VIEW_H, e1m1_ui_flash_gray);
    unsigned brighter = 0u;
    for (unsigned i = 0; i < PIXELS; ++i) {
        if (comparison[i] != frame[8u + i]) {
            assert(frame[8u + i] > comparison[i]);
            ++brighter;
        }
    }
    assert(brighter > 100u);
    draw_patch_palette(frame + 8u, &ui_shtfa0, ui_shtfa0.x, ui_shtfa0.y,
                       DOOM_GAME_VIEW_H, e1m1_ui_flash_gray);
    assert(image_hash(frame + 8u, PIXELS) == shotgun[35]);
    check_guards();

    /* Large ammunition counter follows the selected ammo inventory. */
    game.ammo = 137u;
    game.shells = 7u;
    game.pistol_tics = 0u;
    clear_frame();
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    assert(DoomLiteGame_CurrentAmmo(&game) == 7u);
    const unsigned shell_counter = region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 4u, 38u, 13u);
    const unsigned sg_label = region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 19u, 38u, 7u);
    game.ammo = 400u;
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    assert(region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 4u, 38u, 13u) == shell_counter);
    game.shells = 8u;
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    assert(region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 4u, 38u, 13u) != shell_counter);
    memcpy(comparison, frame + 8u, PIXELS);
    DoomLite_DrawGameHud(frame + 8u, &game, "SPRITES F2 NEXT AND A LONG EVENT", 0u);
    assert(region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 19u, 38u, 7u) == sg_label);
    for (unsigned y = DOOM_GAME_VIEW_H; y < DOOM_GAME_LCD_H; ++y)
        for (unsigned x = 116u; x < DOOM_GAME_LCD_W; ++x)
            assert(comparison[y * DOOM_GAME_LCD_W + x] == frame[8u + y * DOOM_GAME_LCD_W + x]);
    game.current_weapon = DL_WEAPON_PISTOL;
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    assert(DoomLiteGame_CurrentAmmo(&game) == 400u);
    assert(region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 19u, 38u, 7u) != sg_label);
    const unsigned bullet_counter = region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 4u, 38u, 13u);
    game.shells = 100u;
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    assert(region_hash(frame + 8u, 0u, DOOM_GAME_VIEW_H + 4u, 38u, 13u) == bullet_counter);
    check_guards();
    assert(E1M1_UI_PIXEL_BYTES == 17062u);
    assert(DoomLite_UiReadonlyBytes() == E1M1_UI_PIXEL_BYTES +
           E1M1_UI_PATCH_COUNT * sizeof(E1M1UiPatch) + 32u + 26u * sizeof(void *) + 180u);
    assert(DoomLite_UiReadonlyBytes() <= E1M1_UI_PIXEL_BUDGET);

    for (unsigned hp = 0; hp <= 100u; ++hp) {
        clear_frame();
        game.health = (uint8_t)hp;
        game.ammo = 200u;
        game.kills = 292u;
        game.blue_key = (uint8_t)(hp & 1u);
        game.red_key = (uint8_t)(hp & 2u);
        game.yellow_key = (uint8_t)(hp & 4u);
        game.armor = (uint16_t)(hp * 2u);
        DoomLite_DrawGameWeapon(frame + 8u, &game);
        memcpy(comparison, frame + 8u, PIXELS);
        DoomLite_DrawGameHud(frame + 8u, &game, "DOOR OPEN", 0u);
        if (hp) assert(!memcmp(comparison, frame + 8u,
                              DOOM_GAME_VIEW_H * DOOM_GAME_LCD_W));
        check_guards();
    }
    clear_frame();
    game.completed = 1u;
    DoomLite_DrawGameWeapon(frame + 8u, &game);
    for (unsigned i = 0; i < PIXELS; ++i) assert(frame[8u + i] == 147u);
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    check_guards();
    game.map_index = 1u;
    clear_frame();
    DoomLite_DrawGameHud(frame + 8u, &game, NULL, 0u);
    check_guards();
    game.completed = 0;
    game.health = 100u;
    clear_frame();
    DoomLite_DrawGameHud(frame + 8u, &game, "BLUE KEY", 2u);
    check_guards();
    for (unsigned level = 0; level < DOOM_MAP_COUNT; ++level)
        for (unsigned contrast = 0; contrast < 3u; ++contrast) {
            DoomLiteGame_InitMap(&game, level);
            game.contrast = (uint8_t)contrast;
            game.blue_key = game.red_key = game.yellow_key = 1u;
            game.health = game.armor = 200u;
            game.backpack = 1u;
            game.ammo = 400u;
            game.shells = game.rockets = 100u;
            game.cells = 600u;
            clear_frame();
            DoomLite_DrawGameHud(frame + 8u, &game, "NEED YELLOW", 1u);
            for (unsigned i = 0; i < DOOM_GAME_VIEW_H * DOOM_GAME_LCD_W; ++i)
                assert(frame[8u + i] == 147u);
            check_guards();
        }
    clear_frame();
    draw_patch(frame + 8u, &ui_stbar, -100, -10, DOOM_GAME_VIEW_H);
    draw_patch(frame + 8u, &ui_pisgb0, 250, 90, DOOM_GAME_VIEW_H);
    draw_patch(frame + 8u, &ui_shtgc0, -73, -66, DOOM_GAME_VIEW_H);
    draw_patch(frame + 8u, &ui_shtgd0, 255, 100, DOOM_GAME_VIEW_H);
    draw_patch(frame + 8u, &ui_shtfb0, -200, -200, DOOM_GAME_VIEW_H);
    draw_patch(frame + 8u, &ui_shtfa0, 300, 200, DOOM_GAME_VIEW_H);
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

    if (argc > 1) {
        game.weapons |= DL_WEAPON_SHOTGUN_OWNED;
        game.shells = 17u;
        game.ammo = 93u;
        for (unsigned weapon = DL_WEAPON_PISTOL; weapon <= DL_WEAPON_SHOTGUN; ++weapon) {
            game.current_weapon = (uint8_t)weapon;
            const unsigned pistol_poses[] = {0u, 15u, 8u, 5u};
            const unsigned shotgun_poses[] = {0u, 35u, 31u, 28u, 23u, 18u, 14u, 9u, 4u};
            const unsigned *tics = weapon == DL_WEAPON_PISTOL ? pistol_poses : shotgun_poses;
            const unsigned count = weapon == DL_WEAPON_PISTOL ? 4u : 9u;
            for (unsigned pose = 0; pose < count; ++pose) {
                char name[32];
                snprintf(name, sizeof(name), "%s-%02u", weapon == DL_WEAPON_PISTOL ? "pistol" : "shotgun", tics[pose]);
                clear_frame();
                game.pistol_tics = (uint8_t)tics[pose];
                DoomLite_DrawGameWeapon(frame + 8u, &game);
                DoomLite_DrawGameHud(frame + 8u, &game, "SPRITES F2 NEXT", 0u);
                write_preview(argv[1], name);
            }
        }
    }
    printf("Classic UI: weapon poses, local flash, current ammo, clipping and reset passed; %u readonly bytes\n",
           DoomLite_UiReadonlyBytes());
    return 0;
}
