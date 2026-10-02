#ifndef SKYOS_VAPOR_DISPLAY_H
#define SKYOS_VAPOR_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The caller owns a persistent 256x127 8-bit grayscale framebuffer. */
uint32_t SkyVapor_Init(uint8_t *pixels);
/* Returns a dirty logical-row mask; zero means no visual change. */
uint32_t SkyVapor_Press(uint8_t button);

#ifdef __cplusplus
}
#endif

#endif
