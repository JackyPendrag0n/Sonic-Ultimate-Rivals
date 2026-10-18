// Shim for PPSSPP's Core/HLE/sceKernelThread.h (rcsm utility extraction): runtime/audio's (the HLE
// bridge's MipsCall/PSPAction) plus what the dialogs use. GPL-2.0+.
#pragma once
#include "../../../../audio/shim/Core/HLE/sceKernelThread.h"
#include "Core/HLE/sceKernel.h"
#include "Core/HLE/ErrorCodes.h"
#include "Core/HLE/scePower.h"
int KernelCurThreadPriority();
