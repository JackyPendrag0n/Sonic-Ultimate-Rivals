// Shared by the native media libraries (sas.c, atrac.c): guest buffer checks and Media Engine timing.
#pragma once
#include "kernel.h"

// True if [addr, addr+size) lies in the PSP's RAM (any of its mirrors).
bool media_valid(u32 addr, u32 size);
// Queues a Media Engine job of `us` microseconds behind the ones still running and returns how long
// from now until it is done (PPSSPP's MEScheduleJob).
int media_schedule(int us);
// Blocks the calling thread for `us` microseconds, as the firmware call would (no-op in interrupts
// or with SR2_MEDIA_NODELAY=1).
void media_wait(int us);
