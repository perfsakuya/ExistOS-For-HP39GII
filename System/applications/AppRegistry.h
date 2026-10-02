#ifndef EXISTOS_APP_REGISTRY_H
#define EXISTOS_APP_REGISTRY_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AppEntry {
    const char *name;
    const uint8_t *icon; /* 48 x 48, 8-bit grayscale */
    void (*launch)(void);
} AppEntry;

size_t AppRegistry_Count(void);
const AppEntry *AppRegistry_Get(size_t index);

#ifdef __cplusplus
}
#endif

#endif
