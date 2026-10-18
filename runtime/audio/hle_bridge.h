// The PPSSPP-HLE-on-rcsm bridge in libpspaudio.a, shared with other libraries built the same way
// (libpspmpeg.a): PPSSPP HLE modules compiled against runtime/audio/shim register their tables with
// RegisterHLEModule(), then hle_bridge_register_pending() hands them to the rcsm kernel.
// GPL-2.0+.
#pragma once
void hle_bridge_init(void);
int hle_bridge_register_pending(void);
