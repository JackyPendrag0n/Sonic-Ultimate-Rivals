// Shim for PPSSPP's Core/SaveState.h (rcsm utility extraction): no emulator savestates. GPL-2.0+.
#pragma once
namespace SaveState {
	inline void NotifySaveData() {}
}
