/* Host regression for the exact WAD exit-switch recess.
 * gcc -std=c11 -O2 -Wall -Wextra -Werror tools/doom/check_lite_exit.c
 *     System/applications/user/doom_lite/DoomLiteGame.c -o check_lite_exit.exe
 */
#include <assert.h>
#include <stdio.h>

#include "../../System/applications/user/doom_lite/DoomLiteGame.h"

int main(void) {
    DoomLiteGame game;
    DoomLiteGame_Init(&game);

    /* The 32-unit grid used to mark this whole entry as solid. The player
     * should now pass between its true upper/lower one-sided WAD edges. */
    for (int x = -360; x >= -388; x -= 4)
        assert(!DoomLiteGame_IsSolid(&game, x * 256, 1296 * 256));

    /* Keep the actual switch wall and top/bottom recess boundaries solid. */
    assert(DoomLiteGame_IsSolid(&game, -392 * 256, 1296 * 256));
    assert(DoomLiteGame_IsSolid(&game, -390 * 256, 1287 * 256));
    assert(DoomLiteGame_IsSolid(&game, -390 * 256, 1305 * 256));

    game.x_q8 = -380 * 256;
    game.y_q8 = 1296 * 256;
    game.facing = 128u; /* Face west toward x=-400, twenty map units away. */
    assert((DoomLiteGame_Step(&game, DL_GAME_USE) & DL_EVENT_EXIT) != 0u);
    assert(game.completed);
    puts("E1M1 exit recess: approach, true walls, 20-unit switch use OK");
    return 0;
}
