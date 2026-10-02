#ifndef SKYOS_DOOM_PROBE_RENDER_H
#define SKYOS_DOOM_PROBE_RENDER_H

#include <stdint.h>

#define DOOM_PROBE_W 160
#define DOOM_PROBE_H 120

void DoomProbe_Render(uint8_t *pixels, uint8_t view_angle);

#endif
