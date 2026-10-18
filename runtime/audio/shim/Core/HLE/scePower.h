// Shim for PPSSPP's Core/HLE/scePower.h (rcsm GPU extraction): the PSP runs at default clocks. GPL-2.0+.
#pragma once
inline int PowerScaleFromDefaultClock(int us) { return us; }
