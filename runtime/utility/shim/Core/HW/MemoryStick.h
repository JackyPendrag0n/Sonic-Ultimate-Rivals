// Shim for PPSSPP's Core/HW/MemoryStick.h (rcsm utility extraction). GPL-2.0+.
#pragma once
#include <string>
#include "Common/CommonTypes.h"
enum MemStickState { PSP_MEMORYSTICK_STATE_INSERTED = 1, PSP_MEMORYSTICK_STATE_NOT_INSERTED = 2 };
enum MemStickFatState { PSP_FAT_MEMORYSTICK_STATE_UNASSIGNED = 0, PSP_FAT_MEMORYSTICK_STATE_ASSIGNED = 1 };
MemStickState MemoryStick_State();
u64 MemoryStick_SectorSize();
u64 MemoryStick_FreeSpace(std::string gameID);
void MemoryStick_NotifyGameName(std::string gameID);
