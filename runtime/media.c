// Shared by the native media libraries: guest buffer checks and Media Engine timing.
#include <stdlib.h>
#include "media.h"

bool media_valid(u32 addr, u32 size) {
	u32 a = addr & MEM_MASK;
	return a >= 0x08000000 && a < 0x0C000000 && size <= 0x0C000000 - a;
}

static s64 me_busy_until;

int media_schedule(int us) {
	s64 now = (s64)now_us();
	s64 start = now > me_busy_until ? now : me_busy_until;
	me_busy_until = start + us;
	return (int)(me_busy_until - now);
}

void media_wait(int us) {
	static int nodelay = -1;
	if (nodelay < 0) { const char *e = getenv("SR2_MEDIA_NODELAY"); nodelay = e && atoi(e); }
	if (us > 0 && !nodelay && g_cur && !g_in_intr) thread_delay_until(now_us() + us);
}
