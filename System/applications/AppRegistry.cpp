#include "AppRegistry.h"

extern "C" {
void StartKhiCAS(void);
void Notes_Start(void);
void VaporDemo_Start(void);
void DoomProbe_Start(void);
void DoomPort_Start(void);
extern const unsigned char gImage_khicas_ico[48 * 48];
extern const unsigned char gImage_notes_ico[48 * 48];
}

static const AppEntry apps[] = {
    {"KhiCAS", gImage_khicas_ico, StartKhiCAS},
    {"Notes", gImage_notes_ico, Notes_Start},
    {"Vapor Test", gImage_notes_ico, VaporDemo_Start},
    {"Doom Probe", gImage_notes_ico, DoomProbe_Start},
    {"Freedoom E1M1", gImage_notes_ico, DoomPort_Start},
};

extern "C" size_t AppRegistry_Count(void) {
    return sizeof(apps) / sizeof(apps[0]);
}

extern "C" const AppEntry *AppRegistry_Get(size_t index) {
    return index < AppRegistry_Count() ? &apps[index] : nullptr;
}
