#include "AppRegistry.h"

extern "C" {
void StartKhiCAS(void);
void Notes_Start(void);
#ifdef SKYOS_BUILD_LEGACY_DOOM
void DoomPort_Start(void);
void DoomPort_StartFlat(void);
void DoomPort_StartFast(void);
#endif
void DoomGame_Start(void);
void DoomTest_Start(void);
void RayDemo_Start(void);
extern const unsigned char gImage_khicas_ico[48 * 48];
extern const unsigned char gImage_notes_ico[48 * 48];
}

static const AppEntry apps[] = {
    {"KhiCAS", gImage_khicas_ico, StartKhiCAS},
    {"Notes", gImage_notes_ico, Notes_Start},
    {"Game", gImage_notes_ico, DoomGame_Start},
    {"Test", gImage_notes_ico, DoomTest_Start},
    {"Ray", gImage_notes_ico, RayDemo_Start},
#ifdef SKYOS_BUILD_LEGACY_DOOM
    {"E1M1", gImage_notes_ico, DoomPort_Start},
    {"Flat", gImage_notes_ico, DoomPort_StartFlat},
    {"Hybrid", gImage_notes_ico, DoomPort_StartFast},
#endif
};

extern "C" size_t AppRegistry_Count(void) {
    return sizeof(apps) / sizeof(apps[0]);
}

extern "C" const AppEntry *AppRegistry_Get(size_t index) {
    return index < AppRegistry_Count() ? &apps[index] : nullptr;
}
