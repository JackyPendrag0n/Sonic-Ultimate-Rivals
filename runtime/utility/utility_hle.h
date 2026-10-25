// rcsm runtime: PPSSPP's sceUtility dialogs (savedata, message, OSK) + PPGe, as a static library.
// GPL-2.0+ (see LICENSE.TXT).
#pragma once
#ifdef __cplusplus
extern "C" {
#endif
// Registers every sceUtility NID PPSSPP defines (overriding earlier registrations) and sets up PPGe.
// Call after kernel_init/io_init, ge_soft_init and audio init.
void utility_hle_init(void);
#ifdef __cplusplus
}
#endif
