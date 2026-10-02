#include "AppRegistry.h"

extern "C" {
void StartKhiCAS(void);
void Notes_Start(void);
extern const unsigned char gImage_khicas_ico[48 * 48];
extern const unsigned char gImage_notes_ico[48 * 48];
}

static const AppEntry apps[] = {
    {"KhiCAS", gImage_khicas_ico, StartKhiCAS},
    {"Notes", gImage_notes_ico, Notes_Start},
};

extern "C" size_t AppRegistry_Count(void) {
    return sizeof(apps) / sizeof(apps[0]);
}

extern "C" const AppEntry *AppRegistry_Get(size_t index) {
    return index < AppRegistry_Count() ? &apps[index] : nullptr;
}
