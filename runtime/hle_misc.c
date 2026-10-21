// Display, GE (display-list walker, no rasteriser yet), controller, audio, SAS, Atrac, MPEG, UMD,
// power and utility HLE.
#define _GNU_SOURCE
#include "kernel.h"
#include <sys/stat.h>
#include <time.h>

// ================================================================= display
static u32 fb_top, fb_width, fb_format;
static u64 frames_flipped;
static void h_DispSetMode(Ctx *c) { LOGD("sceDisplaySetMode(%d, %d, %d)", A(0), A(1), A(2)); RET(0); }
void fe_set_display(u32 top, u32 stride, u32 fmt);
static void h_DispSetFrameBuf(Ctx *c) {
	if (A(0) != fb_top) frames_flipped++;
	fb_top = A(0); fb_width = A(1); fb_format = A(2);
	fe_set_display(A(0), A(1), A(2));
	RET(0);
}
static void h_DispWaitVblankStart(Ctx *c) { RET(0); kernel_wait_vblank(c, false); }
static void h_DispWaitVblankStartCB(Ctx *c) { RET(0); kernel_wait_vblank(c, true); }
static void h_DispGetVcount(Ctx *c) { RET((u32)g_vblank_count); }
static void h_DispGetFramePerSec(Ctx *c) { RETF(59.9400599f); RET(0); }
static void h_DispIsVblank(Ctx *c) { RET(0); }
static void h_DispGetFrameBuf(Ctx *c) { SW(A(0), fb_top); SW(A(1), fb_width); SW(A(2), fb_format); RET(0); }

// ================================================================= GE
typedef struct {
	int id, used;
	u32 start, pc, stall;
	int cbid;
	int state;  // 0 done, 1 queued, 2 drawing, 3 stalled
	u32 stack[32]; int sp;
	u32 base, offset;
} GeList;
static GeList ge_lists[16];
static struct { u32 signal_func, signal_arg, finish_func, finish_arg, gp; int used; } ge_cbs[16];
static u64 ge_prims, ge_prims_frame;
static u32 ge_edram_trans = 0x400;

static void ge_run(GeList *l) {
	l->state = 2;
	int guard = 0;
	while (guard++ < 2000000) {
		if (l->stall && l->pc == l->stall) { l->state = 3; return; }
		u32 op = LW(l->pc);
		u32 cmd = op >> 24, data = op & 0xFFFFFF;
		u32 cur = l->pc;
		l->pc += 4;
		switch (cmd) {
		case 0x04: case 0x05: ge_prims++; break;  // PRIM / BEZIER
		case 0x08: l->pc = (((l->base << 8) & 0x0F000000) | (data & 0x00FFFFFC)) + l->offset; break;  // JUMP
		case 0x0A:  // CALL
			if (l->sp < 32) l->stack[l->sp++] = l->pc;
			l->pc = (((l->base << 8) & 0x0F000000) | (data & 0x00FFFFFC)) + l->offset;
			break;
		case 0x0B: if (l->sp) l->pc = l->stack[--l->sp]; break;  // RET
		case 0x0E: {  // SIGNAL (followed by END)
			u32 behav = (data >> 16) & 0xFF;
			if (behav <= 3 && l->cbid >= 0 && ge_cbs[l->cbid].signal_func)
				intr_queue(ge_cbs[l->cbid].signal_func, data & 0xFFFF, ge_cbs[l->cbid].signal_arg, 0, ge_cbs[l->cbid].gp);
			else if (behav > 3) LOGD("GE signal behaviour %x at %08x", behav, cur);
			break;
		}
		case 0x0F:  // FINISH (followed by END)
			if (l->cbid >= 0 && ge_cbs[l->cbid].finish_func)
				intr_queue(ge_cbs[l->cbid].finish_func, data & 0xFFFF, ge_cbs[l->cbid].finish_arg, 0, ge_cbs[l->cbid].gp);
			break;
		case 0x0C:  // END
			{
				u32 prev = LW(cur - 4) >> 24;
				if (prev == 0x0F || prev == 0x0E) {
					if (prev == 0x0E && ((LW(cur - 4) >> 16) & 0xFF) != 0) break;  // signal: continue after
					l->state = 0;
					return;
				}
			}
			l->state = 0;
			return;
		case 0x10: l->base = data; break;
		case 0x13: l->offset = data << 8; break;
		case 0x14: l->offset = cur; break;
		default: break;
		}
	}
	LOGE("GE list %d ran away (pc %08x)", l->id, l->pc);
	l->state = 0;
}

static void h_GeEdramGetAddr(Ctx *c) { RET(0x04000000); }
static void h_GeEdramGetSize(Ctx *c) { RET(0x00200000); }
static void h_GeEdramSetAddrTranslation(Ctx *c) { u32 o = ge_edram_trans; ge_edram_trans = A(0); RET(o); }
static void enqueue(Ctx *c, bool head) {
	int i;
	for (i = 0; i < 16; i++) if (!ge_lists[i].used) break;
	if (i == 16) {
		for (i = 0; i < 16; i++) if (ge_lists[i].state == 0) break;
		if (i == 16) { RET(0x80000021); return; }
	}
	GeList *l = &ge_lists[i];
	memset(l, 0, sizeof *l);
	l->used = 1; l->id = i;
	l->start = l->pc = A(0) & 0x0FFFFFFF;
	l->stall = A(1) & 0x0FFFFFFF;
	l->cbid = (s32)A(2);
	if (l->cbid >= 16) l->cbid = -1;
	ge_run(l);
	RET(i);
}
static void h_GeListEnQueue(Ctx *c) { enqueue(c, false); }
static void h_GeListEnQueueHead(Ctx *c) { enqueue(c, true); }
static void h_GeListUpdateStallAddr(Ctx *c) {
	if (A(0) >= 16 || !ge_lists[A(0)].used) { RET(0x80000100); return; }
	GeList *l = &ge_lists[A(0)];
	l->stall = A(1) & 0x0FFFFFFF;
	if (l->state == 3) ge_run(l);
	RET(0);
}
static void h_GeListSync(Ctx *c) {
	if (A(0) >= 16) { RET(0x80000100); return; }
	RET(A(1) == 1 ? (u32)ge_lists[A(0)].state : 0);
}
static void h_GeDrawSync(Ctx *c) { RET(0); }
static void h_GeSetCallback(Ctx *c) {
	int i;
	for (i = 0; i < 16; i++) if (!ge_cbs[i].used) break;
	if (i == 16) { RET(0x80000022); return; }
	u32 p = A(0);
	ge_cbs[i].signal_func = LW(p); ge_cbs[i].signal_arg = LW(p + 4);
	ge_cbs[i].finish_func = LW(p + 8); ge_cbs[i].finish_arg = LW(p + 12);
	ge_cbs[i].gp = c->r[28]; ge_cbs[i].used = 1;
	LOGD("sceGeSetCallback signal %08x finish %08x -> %d", ge_cbs[i].signal_func, ge_cbs[i].finish_func, i);
	RET(i);
}
static void h_GeUnsetCallback(Ctx *c) { if (A(0) < 16) ge_cbs[A(0)].used = 0; RET(0); }
static void h_GeSaveContext(Ctx *c) { RET(0); }
static void h_GeRestoreContext(Ctx *c) { RET(0); }
static void h_GeContinue(Ctx *c) { RET(0); }
static void h_GeBreak(Ctx *c) { RET(0); }
static void h_GeGetCmd(Ctx *c) { RET(0); }

void fe_vblank(void);
void ge_vblank(void) {
	static u64 last_report, last_flips, last_vb;
	fe_vblank();
	u64 n = now_us();
	if (!last_report) last_report = n;
	if (n - last_report >= 5000000) {
		LOGI("status: vblank %llu, display flips %llu (last 5s: %llu flips, %llu vblanks)", (unsigned long long)g_vblank_count,
		     (unsigned long long)frames_flipped, (unsigned long long)(frames_flipped - last_flips),
		     (unsigned long long)(g_vblank_count - last_vb));
		last_report = n; last_flips = frames_flipped; last_vb = g_vblank_count;
	}
}

// ================================================================= controller
extern u32 fe_buttons;
extern u8 fe_lx, fe_ly;
extern u32 fe_pbuttons[4]; extern u8 fe_plx[4], fe_ply[4];
int g_ctrl_port;   // 1-3 while the game reads player 2-4's pad (game.c, extra racers)
static void write_ctrl(u32 p, int n) {
	u32 b = g_ctrl_port ? fe_pbuttons[g_ctrl_port] : fe_buttons;
	u8 lx = g_ctrl_port ? fe_plx[g_ctrl_port] : fe_lx, ly = g_ctrl_port ? fe_ply[g_ctrl_port] : fe_ly;
	for (int i = 0; i < n; i++) {
		u32 q = p + i * 16;
		SW(q, (u32)now_us()); SW(q + 4, b); SB(q + 8, lx); SB(q + 9, ly);
		memset(PTR(q + 10), 0, 6);
	}
}
static void h_CtrlSetSamplingCycle(Ctx *c) { RET(0); }
static void h_CtrlSetSamplingMode(Ctx *c) { RET(0); }
static void h_CtrlReadBufferPositive(Ctx *c) { int n = A(1) ? A(1) : 1; write_ctrl(A(0), n); RET(n); kernel_wait_vblank(c, false); c->r[2] = n; }
static void h_CtrlPeekBufferPositive(Ctx *c) { int n = A(1) ? A(1) : 1; write_ctrl(A(0), n); RET(n); }
static void h_CtrlReadLatch(Ctx *c) {
	static u32 prevs[4];
	u32 *prev = &prevs[g_ctrl_port & 3], cur = g_ctrl_port ? fe_pbuttons[g_ctrl_port & 3] : fe_buttons;
	SW(A(0), cur & ~*prev); SW(A(0) + 4, *prev & ~cur); SW(A(0) + 8, cur); SW(A(0) + 12, ~cur);
	*prev = cur;
	RET(1);
}

// ================================================================= audio
static u32 audio2_samples;
static u64 audio2_next;
static void h_AudioOutput2Reserve(Ctx *c) { audio2_samples = A(0); audio2_next = 0; RET(0); }
static void h_AudioOutput2Release(Ctx *c) { RET(0); }
static void h_AudioOutput2GetRestSample(Ctx *c) { RET(0); }
static void h_AudioOutput2OutputBlocking(Ctx *c) {
	u64 n = now_us();
	u64 dur = (u64)audio2_samples * 1000000 / 44100;
	if (!audio2_next || audio2_next + dur * 4 < n) audio2_next = n;
	audio2_next += dur;
	extern void fe_audio_push(const s16 *samples, int frames);
	fe_audio_push((const s16 *)PTR(A(1)), audio2_samples);
	if (audio2_next > n + dur) thread_delay_until(audio2_next - dur);
	RET(audio2_samples);
}

// ================================================================= SAS
static u32 sas_grain = 256;
static void h_SasInit(Ctx *c) { sas_grain = A(1); LOGD("__sceSasInit grain %u voices %u out %u rate %u", A(1), A(2), A(3), A(4)); RET(0); }
static void h_SasCore(Ctx *c) { memset(PTR(A(1)), 0, sas_grain * 4); RET(0); }
static void h_SasCoreWithMix(Ctx *c) { RET(0); }
static void h_SasGetEndFlag(Ctx *c) { RET(0xFFFFFFFF); }
static void h_SasZero(Ctx *c) { RET(0); }

// ================================================================= Atrac (music) - reports "no audio"
static void h_AtracSetDataAndGetID(Ctx *c) { LOGD("sceAtracSetDataAndGetID(%x, %x)", A(0), A(1)); RET(0); }
static void h_AtracGetStreamDataInfo(Ctx *c) { SW(A(1), 0); SW(A(2), 0); SW(A(3), 0); RET(0); }
static void h_AtracDecodeData(Ctx *c) { SW(A(2), 0); SW(A(3), 1); if (A(4)) SW(A(4), 0); RET(0x80630024); }
static void h_AtracGetRemainFrame(Ctx *c) { SW(A(1), 0xFFFFFFFF); RET(0); }
static void h_AtracGetOutputChannel(Ctx *c) { SW(A(1), 2); RET(0); }
static void h_AtracZero(Ctx *c) { RET(0); }

// ================================================================= MPEG (videos) - unsupported for now
static void h_MpegFail(Ctx *c) { LOGI("sceMpeg* video call (unsupported) ra=%08x", c->r[31]); RET(0x80618001); }
static void h_MpegQueryMemSize(Ctx *c) { RET(0x10000); }
static void h_MpegRingbufferQueryMemSize(Ctx *c) { RET(A(0) * 2064); }

// ================================================================= UMD / power
static void h_UmdGetDriveStat(Ctx *c) { RET(0x32); }
static void h_Zero(Ctx *c) { RET(0); }
static void h_VolatileMemLock(Ctx *c) { if (A(1)) SW(A(1), 0x08400000); if (A(2)) SW(A(2), 0x400000); RET(0); }

// ================================================================= utility
static int sd_status;
static u32 sd_params;
static void h_GetSystemParamInt(Ctx *c) {
	u32 v = 0;
	switch (A(0)) {
	case 8: v = 1; break;  // language: English
	case 9: v = 1; break;  // confirm button: cross
	case 4: v = 2; break;  // date format DD/MM/YYYY
	case 5: v = 0; break;  // 24h
	default: v = 0;
	}
	SW(A(1), v);
	RET(0);
}
static void h_SavedataInitStart(Ctx *c) {
	sd_params = A(0);
	u32 mode = LW(sd_params + 48);
	char game[16] = {0}, save[24] = {0};
	memcpy(game, PTR(sd_params + 60), 13); memcpy(save, PTR(sd_params + 76), 20);
	LOGI("sceUtilitySavedataInitStart mode %u game '%s' save '%s'", mode, game, save);
	// Without saves on disk every load reports "no data"; saving is acknowledged but not written yet.
	u32 result = 0;
	if (mode == 0 || mode == 2 || mode == 4 || mode == 6) result = 0x80110307;  // LOAD_NO_DATA
	else if (mode == 11 || mode == 12) result = 0x80110307;
	SW(sd_params + 28, result);
	sd_status = 1;
	RET(0);
}
static void h_SavedataGetStatus(Ctx *c) {
	int s = sd_status;
	if (sd_status == 1) sd_status = 2;
	else if (sd_status == 2) sd_status = 3;
	else if (sd_status == 4) sd_status = 0;
	RET(s);
}
static void h_SavedataShutdownStart(Ctx *c) { sd_status = 4; RET(0); }
static void h_SavedataUpdate(Ctx *c) { RET(0); }
static int osk_status;
static void h_OskInitStart(Ctx *c) { osk_status = 1; RET(0); }
static void h_OskGetStatus(Ctx *c) { int s = osk_status; if (osk_status == 1) osk_status = 2; else if (osk_status == 2) osk_status = 3; else if (osk_status == 4) osk_status = 0; RET(s); }
static void h_OskShutdownStart(Ctx *c) { osk_status = 4; RET(0); }

static const HleEnt misc_ents[] = {
	{0x0E20F177, "sceDisplaySetMode", h_DispSetMode},
	{0x289D82FE, "sceDisplaySetFrameBuf", h_DispSetFrameBuf},
	{0xEEDA2E54, "sceDisplayGetFrameBuf", h_DispGetFrameBuf},
	{0x984C27E7, "sceDisplayWaitVblankStart", h_DispWaitVblankStart},
	{0x46F186C3, "sceDisplayWaitVblankStartCB", h_DispWaitVblankStartCB},
	{0x36CDFADE, "sceDisplayWaitVblank", h_DispWaitVblankStart},
	{0x9C6EAAD7, "sceDisplayGetVcount", h_DispGetVcount},
	{0xDBA6C4C4, "sceDisplayGetFramePerSec", h_DispGetFramePerSec},
	{0x4D4E10EC, "sceDisplayIsVblank", h_DispIsVblank},
	{0x6A2774F3, "sceCtrlSetSamplingCycle", h_CtrlSetSamplingCycle},
	{0x1F4011E6, "sceCtrlSetSamplingMode", h_CtrlSetSamplingMode},
	{0x1F803938, "sceCtrlReadBufferPositive", h_CtrlReadBufferPositive},
	{0x3A622550, "sceCtrlPeekBufferPositive", h_CtrlPeekBufferPositive},
	{0x0B588501, "sceCtrlReadLatch", h_CtrlReadLatch},
	{0x01562BA3, "sceAudioOutput2Reserve", h_AudioOutput2Reserve},
	{0x43196845, "sceAudioOutput2Release", h_AudioOutput2Release},
	{0x2D53F36E, "sceAudioOutput2OutputBlocking", h_AudioOutput2OutputBlocking},
	{0x647CEF33, "sceAudioOutput2GetRestSample", h_AudioOutput2GetRestSample},
	{0x42778A9F, "__sceSasInit", h_SasInit},
	{0xA3589D81, "__sceSasCore", h_SasCore},
	{0x50A14DFC, "__sceSasCoreWithMix", h_SasCoreWithMix},
	{0x68A46B95, "__sceSasGetEndFlag", h_SasGetEndFlag},
	{0x2C8E6AB3, "__sceSasGetPauseFlag", h_SasZero},
	{0x74AE582A, "__sceSasGetEnvelopeHeight", h_SasZero},
	{0x99944089, "__sceSasSetVoice", h_SasZero},
	{0xAD84D37F, "__sceSasSetPitch", h_SasZero},
	{0x440CA7D8, "__sceSasSetVolume", h_SasZero},
	{0x76F01ACA, "__sceSasSetKeyOn", h_SasZero},
	{0xA0CF2FA4, "__sceSasSetKeyOff", h_SasZero},
	{0x787D04D5, "__sceSasSetPause", h_SasZero},
	{0x019B25EB, "__sceSasSetADSR", h_SasZero},
	{0x9EC3676A, "__sceSasSetADSRmode", h_SasZero},
	{0x5F9529F6, "__sceSasSetSL", h_SasZero},
	{0xCBCD4F79, "__sceSasSetSimpleADSR", h_SasZero},
	{0xB7660A23, "__sceSasSetNoise", h_SasZero},
	{0x267A6DD2, "__sceSasRevParam", h_SasZero},
	{0x33D4AB37, "__sceSasRevType", h_SasZero},
	{0xD5A229C9, "__sceSasRevEVOL", h_SasZero},
	{0xF983B186, "__sceSasRevVON", h_SasZero},
	{0x7A20E7AF, "sceAtracSetDataAndGetID", h_AtracSetDataAndGetID},
	{0x5D268707, "sceAtracGetStreamDataInfo", h_AtracGetStreamDataInfo},
	{0x6A8C3CD5, "sceAtracDecodeData", h_AtracDecodeData},
	{0x9AE849A7, "sceAtracGetRemainFrame", h_AtracGetRemainFrame},
	{0xB3B5D042, "sceAtracGetOutputChannel", h_AtracGetOutputChannel},
	{0x7DB31251, "sceAtracAddStreamData", h_AtracZero},
	{0x868120B5, "sceAtracSetLoopNum", h_AtracZero},
	{0x61EB33F5, "sceAtracReleaseAtracID", h_AtracZero},
	{0x682A619B, "sceMpegInit", h_Zero},
	{0xC132E22F, "sceMpegQueryMemSize", h_MpegQueryMemSize},
	{0xD7A29F46, "sceMpegRingbufferQueryMemSize", h_MpegRingbufferQueryMemSize},
	{0xD8C5F121, "sceMpegCreate", h_Zero},
	{0x37295ED8, "sceMpegRingbufferConstruct", h_Zero},
	{0x874624D6, "sceMpegFinish", h_Zero},
	{0x6B4A146C, "sceUmdGetDriveStat", h_UmdGetDriveStat},
	{0x8EF08FCE, "sceUmdWaitDriveStat", h_Zero},
	{0xC6183D47, "sceUmdActivate", h_Zero},
	{0xE83742BA, "sceUmdDeactivate", h_Zero},
	{0x04B7766E, "scePowerRegisterCallback", h_Zero},
	{0xDFA8BAF8, "scePowerUnregisterCallback", h_Zero},
	{0xEFD3C963, "scePowerTick", h_Zero},
	{0xD6D016EF, "scePowerLock", h_Zero},
	{0xCA3D34C1, "scePowerUnlock", h_Zero},
	{0x3E0271D3, "sceKernelVolatileMemLock", h_VolatileMemLock},
	{0xA569E425, "sceKernelVolatileMemUnlock", h_Zero},
	{0xA14F40B2, "sceKernelVolatileMemTryLock", h_VolatileMemLock},
	{0x090CCB3F, "sceKernelPowerTick", h_Zero},
	{0xEADB1BD7, "sceKernelPowerLock", h_Zero},
	{0x3AEE7261, "sceKernelPowerUnlock", h_Zero},
	{0xA5DA2406, "sceUtilityGetSystemParamInt", h_GetSystemParamInt},
	{0x50C4CD57, "sceUtilitySavedataInitStart", h_SavedataInitStart},
	{0x8874DBE0, "sceUtilitySavedataGetStatus", h_SavedataGetStatus},
	{0x9790B33C, "sceUtilitySavedataShutdownStart", h_SavedataShutdownStart},
	{0xD4B95FFB, "sceUtilitySavedataUpdate", h_SavedataUpdate},
	{0xC629AF26, "sceUtilityLoadAvModule", h_Zero},
	{0xF7D8D092, "sceUtilityUnloadAvModule", h_Zero},
	{0xF6269B82, "sceUtilityOskInitStart", h_OskInitStart},
	{0xF3F76017, "sceUtilityOskGetStatus", h_OskGetStatus},
	{0x3DFAEBA9, "sceUtilityOskShutdownStart", h_OskShutdownStart},
	{0x4B85C861, "sceUtilityOskUpdate", h_Zero},
};

static void h_Gettimeofday(Ctx *c) {
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	if (A(0)) { SW(A(0), ts.tv_sec); SW(A(0) + 4, ts.tv_nsec / 1000); }
	RET(0);
}
static void h_LibcTime(Ctx *c) { u32 t = (u32)time(NULL); if (A(0)) SW(A(0), t); RET(t); }
static void h_LibcClock(Ctx *c) { RET((u32)now_us()); }
// PSP RTC ticks: microseconds since 0001-01-01
static void h_RtcGetCurrentTick(Ctx *c) {
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	u64 t = (u64)ts.tv_sec * 1000000u + ts.tv_nsec / 1000 + 62135596800000000ull;
	if (A(0)) { SW(A(0), (u32)t); SW(A(0) + 4, (u32)(t >> 32)); }
	RET(0);
}
static void h_One(Ctx *c) { RET(1); }

void kernel_register_hle(void);
void display_init(void) {}
void ge_init(void) {}
void hle_misc_init(void) {
	kernel_register_hle();
	hle_register(misc_ents, sizeof misc_ents / sizeof misc_ents[0]);
	static const HleEnt libc[] = {
		{0x71EC4271, "sceKernelLibcGettimeofday", h_Gettimeofday},
		{0x27CC57F0, "sceKernelLibcTime", h_LibcTime},
		{0x91E4F6A7, "sceKernelLibcClock", h_LibcClock},
	};
	hle_register(libc, 3);
	static const HleEnt more[] = {
		{0x3F7AD767, "sceRtcGetCurrentTick", h_RtcGetCurrentTick},
		{0x46EBB729, "sceUmdCheckMedium", h_One},
		{0x56202973, "sceUmdWaitDriveStatWithTimer", h_Zero},
		{0x4A9E5E29, "sceUmdWaitDriveStatCB", h_Zero},
		{0xAEE7404D, "sceUmdRegisterUMDCallBack", h_Zero},
		{0xBD2BDE07, "sceUmdUnRegisterUMDCallBack", h_Zero},
		{0xEBD177D6, "scePowerSetClockFrequency350", h_Zero},
		{0x737486F2, "scePowerSetClockFrequency", h_Zero},
		{0x3B84732D, "sceKernelCpuResumeIntrWithSync", h_Zero},   // interrupts are not modelled
		{0x37FB5C42, "sceKernelGetGPI", h_Zero},
		// used around SAS setup to pause the SAS thread for a moment; threads here only run when they
		// would anyway, so these are no-ops for now
		{0x9944F31F, "sceKernelSuspendThread", h_Zero},
		{0x75156E8F, "sceKernelResumeThread", h_Zero},
	};
	hle_register(more, sizeof more / sizeof more[0]);
}
