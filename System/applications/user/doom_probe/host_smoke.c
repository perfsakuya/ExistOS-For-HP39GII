#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "DoomProbeRender.h"

static uint32_t hash_scene(const uint8_t *pixels) {
    uint32_t h = 2166136261u;
    unsigned i;
    for (i = 0; i < DOOM_PROBE_W * DOOM_PROBE_H; ++i) {
        h ^= pixels[i];
        h *= 16777619u;
    }
    return h;
}

int main(int argc, char **argv) {
    uint8_t guarded[8 + DOOM_PROBE_W * DOOM_PROBE_H + 8];
    uint8_t *scene = guarded + 8;
    uint8_t seen[256] = {0};
    uint32_t first_hash;
    uint32_t second_hash;
    unsigned colors = 0;
    unsigned i;
    memset(guarded, 0xa5, sizeof guarded);
    DoomProbe_Render(scene, 0);
    first_hash = hash_scene(scene);
    for (i = 0; i < DOOM_PROBE_W * DOOM_PROBE_H; ++i)
        seen[scene[i]] = 1;
    for (i = 0; i < 256; ++i)
        colors += seen[i];
    DoomProbe_Render(scene, 64);
    second_hash = hash_scene(scene);
    for (i = 0; i < 8; ++i) {
        if (guarded[i] != 0xa5 ||
            guarded[8 + DOOM_PROBE_W * DOOM_PROBE_H + i] != 0xa5) {
            fputs("framebuffer guard changed\n", stderr);
            return 1;
        }
    }
    if (first_hash == second_hash || colors < 8) {
        fputs("scene did not vary as expected\n", stderr);
        return 1;
    }
    if (argc > 1) {
        FILE *f = fopen(argv[1], "wb");
        if (!f) return 1;
        fprintf(f, "P5\n%d %d\n255\n", DOOM_PROBE_W, DOOM_PROBE_H);
        if (fwrite(scene, 1, DOOM_PROBE_W * DOOM_PROBE_H, f) !=
            DOOM_PROBE_W * DOOM_PROBE_H) {
            fclose(f);
            return 1;
        }
        fclose(f);
    }
    printf("Doom Probe render ok: colors=%u hash0=%08x hash64=%08x\n",
           colors, first_hash, second_hash);
    return 0;
}
