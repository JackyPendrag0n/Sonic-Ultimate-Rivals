// Shim for PPSSPP's Core/HLE/sceKernelInterrupt.h (rcsm GPU extraction). Implemented in sce_ge.cpp
// on top of the rcsm kernel. GPL-2.0+.
#pragma once
enum PSPInterrupt { PSP_GE_INTR = 25, PSP_VBLANK_INTR = 30 };
enum PSPGeSubInterrupts { PSP_GE_SUBINTR_SIGNAL = 0, PSP_GE_SUBINTR_FINISH = 1 };
bool __InterruptsEnabled();
bool __IsInInterrupt();
int __CancelRaisedInterrupts(PSPInterrupt intno);
