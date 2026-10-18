// Internal glue between the PPSSPP stubs and sce_ge.cpp (rcsm GPU extraction). GPL-2.0+.
#pragma once
// lvl: 0 error, 1 info, 2 debug, 3 verbose (the runtime's logf_ levels).
void gpu_log(int lvl, const char *msg);
