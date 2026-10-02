# Upstream source

Engine source in `gbadoom/include` and `gbadoom/source` was copied from
<https://github.com/doomhack/GBADoom> at commit
`89097b3ff31ac1e1b2cdce9854e49726cfa462bf`. `doom_iwad.c` and
`i_main.c` were omitted because SkyOS supplies an embedded asset and native
entry point. Upstream's generated status-bar artwork was also omitted;
SkyOS reads the free `STBAR` lump from Freedoom instead. The copied files retain their original copyright notices and
GPL-2.0-or-later license terms. SkyOS modifications are the screen geometry,
zone sizing, startup, timing, and platform bridge.

The engine descends from PrBoom and Doom. Its source license is separate from
the Freedoom artwork and map data license in `data/COPYING.txt`.
