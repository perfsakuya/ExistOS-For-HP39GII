/* Host smoke check of the actual generated code and grayscale adapter. */
#include "VaporDisplay.h"
#include "profile.h"
#include "runtime/vapor.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t pixels[256 * 127];
static uint8_t previous[256 * 127];

static int32_t current_count(void) {
    int32_t count = -1;
    assert(app_debug_state((volatile u8 *)&count) == 4);
    return count;
}

int main(int argc, char **argv) {
    uint32_t changed;
    assert(SkyVapor_Init(pixels) == (1u << VP_GRID_H) - 1u);
    assert(current_count() == 0);
    memcpy(previous, pixels, sizeof(pixels));

    changed = SkyVapor_Press(6); /* Pocket Button.Up */
    assert(changed == ((1u << 3) | (1u << 5)));
    assert(current_count() == 1);
    assert(memcmp(previous, pixels, sizeof(pixels)) != 0);

    if (argc > 1) {
        FILE *out = fopen(argv[1], "wb");
        assert(out != NULL);
        fprintf(out, "P5\n256 127\n255\n");
        assert(fwrite(pixels, 1, sizeof(pixels), out) == sizeof(pixels));
        fclose(out);
    }

    changed = SkyVapor_Press(0); /* Pocket Button.A */
    assert(changed == ((1u << 3) | (1u << 5)));
    assert(current_count() == 0);
    assert(SkyVapor_Press(0) == 0); /* No redundant redraw. */

    puts("Pocket Vapor generated C: input, computed state, dirty rows, and pixels OK");
    return 0;
}
