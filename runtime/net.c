// A network stack with nobody else on it: enough of the PSP's WLAN / ad hoc / infrastructure libraries for the
// game's multiplayer code to host an ad hoc game on its own. Everything initialises, the ad hoc connection
// "succeeds", packets go nowhere and nothing ever arrives. That's what lets the multiplayer maps be loaded and
// run around in (there's no one to play against).
#include "kernel.h"
#include <stdio.h>
#include <string.h>

#define ADHOC_WOULD_BLOCK 0x80410709u
#define ADHOC_TIMEOUT     0x80410715u
#define ADHOC_INVALID_ID  0x80410707u

static const u8 MAC[6] = {0x00, 0x16, 0xfe, 0x5a, 0x11, 0x01};
static struct { bool used; u16 port; } pdp[8];
static int ctl_state;   // 0 disconnected, 1 connected
static struct { u32 fn, arg; } ctl_handler[4];
static bool connect_pending;
static HleFn netconf_get_status;

// call a guest function from inside an HLE call, preserving the caller's registers
static void guest_call(Ctx *c, u32 fn, u32 a0, u32 a1, u32 a2) {
	Ctx save;
	memcpy(&save, c, sizeof save);
	c->r[4] = a0; c->r[5] = a1; c->r[6] = a2;
	c->r[29] = (c->r[29] - 0x400) & ~15u;
	rt_call(c, fn);
	memcpy(c, &save, sizeof save);
}

static void h_Ok(Ctx *c) { RET(0); }
static void h_WlanSwitch(Ctx *c) { RET(1); }        // WLAN switch on
static void h_AdhocctlInit(Ctx *c) { ctl_state = 0; connect_pending = false; RET(0); }
static void h_AdhocctlAddHandler(Ctx *c) {
	for (int i = 0; i < 4; i++)
		if (!ctl_handler[i].fn) { ctl_handler[i].fn = A(0); ctl_handler[i].arg = A(1); RET(i); return; }
	RET(0x80410b07);
}
static void h_AdhocctlDelHandler(Ctx *c) { if (A(0) < 4) ctl_handler[A(0)].fn = 0; RET(0); }
static void fire(Ctx *c, u32 event) {
	for (int i = 0; i < 4; i++)
		if (ctl_handler[i].fn) guest_call(c, ctl_handler[i].fn, event, 0, ctl_handler[i].arg);
}
// The ad hoc network dialog reports the connection through the adhocctl handlers before it finishes.
static void h_NetconfGetStatus(Ctx *c) {
	if (connect_pending) {
		connect_pending = false;
		LOGI("net: ad hoc 'connected' (no other players)");
		fire(c, 1);   // PSP_ADHOCCTL_EVENT_CONNECT
	}
	netconf_get_status(c);
}
static void h_AdhocctlGetState(Ctx *c) {
	// the game connects through the (placeholder) network dialog, which always succeeds: connected from then on
	if (A(0)) SW(A(0), ctl_state);
	RET(0);
}
static void h_AdhocctlDisconnect(Ctx *c) {
	bool was = ctl_state;
	ctl_state = 0;
	RET(0);
	if (was) { fire(c, 2); RET(0); }   // PSP_ADHOCCTL_EVENT_DISCONNECT
}
static void h_GetLocalEtherAddr(Ctx *c) { if (A(0)) memcpy(PTR(A(0)), MAC, 6); RET(0); }
static void h_EtherNtostr(Ctx *c) {
	if (A(0) && A(1)) {
		const u8 *m = PTR(A(0));
		snprintf((char *)PTR(A(1)), 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	}
	RET(0);
}
static void h_PdpCreate(Ctx *c) {
	for (int i = 0; i < 8; i++)
		if (!pdp[i].used) { pdp[i].used = true; pdp[i].port = A(1); LOGI("net: PDP socket %d on port %u", i + 1, A(1)); RET(i + 1); return; }
	RET(0x80410711);
}
static void h_PdpDelete(Ctx *c) { u32 id = A(0); if (id >= 1 && id <= 8) pdp[id - 1].used = false; RET(0); }
static void h_PdpSend(Ctx *c) { u32 id = A(0); RET(id >= 1 && id <= 8 && pdp[id - 1].used ? 0 : ADHOC_INVALID_ID); }
static void h_PdpRecv(Ctx *c) {
	// (id, srcmac, port*, buf, len*, timeout, nonblock)
	u32 id = A(0), lenp = A(4), timeout = A(5), nonblock = A(6);
	if (!(id >= 1 && id <= 8 && pdp[id - 1].used)) { RET(ADHOC_INVALID_ID); return; }
	if (lenp) SW(lenp, 0);
	if (nonblock) { RET(ADHOC_WOULD_BLOCK); return; }
	c->r[4] = timeout ? timeout : 1000;
	HLE(c, 0xCEADEB47);   // sceKernelDelayThread: nothing will arrive
	RET(ADHOC_TIMEOUT);
}
static void h_GetPdpStat(Ctx *c) {
	// (int *size, SceNetAdhocPdpStat *buf): {next, id, mac[6], port u16, rcv_sb_cc u32} = 20 bytes each
	u32 sizep = A(0), buf = A(1), n = 0;
	for (int i = 0; i < 8; i++) n += pdp[i].used;
	if (!sizep) { RET(0x80410716); return; }
	if (!buf) { SW(sizep, n * 20); RET(0); return; }
	u32 room = LW(sizep) / 20, k = 0;
	for (int i = 0; i < 8 && k < room; i++) {
		if (!pdp[i].used) continue;
		u32 e = buf + k * 20;
		SW(e, 0); SW(e + 4, i + 1); memcpy(PTR(e + 8), MAC, 6); SH(e + 14, pdp[i].port); SW(e + 16, 0);
		if (k) SW(e - 20, e);
		k++;
	}
	SW(sizep, k * 20);
	RET(0);
}
static void h_ResolverFail(Ctx *c) { RET(0x80410408); }   // no DNS: infrastructure mode can't reach anything

static const HleEnt net_ents[] = {
	{0xD7763699, "sceWlanGetSwitchState", h_WlanSwitch},
	{0x93440B11, "sceWlanDevIsPowerOn", h_WlanSwitch},
	{0x39AF39A6, "sceNetInit", h_Ok},
	{0x281928A9, "sceNetTerm", h_Ok},
	{0x0BF0A3AE, "sceNetGetLocalEtherAddr", h_GetLocalEtherAddr},
	{0x89360950, "sceNetEtherNtostr", h_EtherNtostr},
	{0xE1D621D7, "sceNetAdhocInit", h_Ok},
	{0xA62C6F57, "sceNetAdhocTerm", h_Ok},
	{0x6F92741B, "sceNetAdhocPdpCreate", h_PdpCreate},
	{0x7F27BB5E, "sceNetAdhocPdpDelete", h_PdpDelete},
	{0xABED3790, "sceNetAdhocPdpSend", h_PdpSend},
	{0xDFE53E03, "sceNetAdhocPdpRecv", h_PdpRecv},
	{0xC7C1FC57, "sceNetAdhocGetPdpStat", h_GetPdpStat},
	{0xE26F226E, "sceNetAdhocctlInit", h_AdhocctlInit},
	{0x9D689E13, "sceNetAdhocctlTerm", h_Ok},
	{0x20B317A0, "sceNetAdhocctlAddHandler", h_AdhocctlAddHandler},
	{0x6402490B, "sceNetAdhocctlDelHandler", h_AdhocctlDelHandler},
	{0x75ECD386, "sceNetAdhocctlGetState", h_AdhocctlGetState},
	{0x34401D65, "sceNetAdhocctlDisconnect", h_AdhocctlDisconnect},
	{0x17943399, "sceNetInetInit", h_Ok},
	{0xA9ED66B9, "sceNetInetTerm", h_Ok},
	{0xE2F91F9B, "sceNetApctlInit", h_Ok},
	{0xB3EDD0EC, "sceNetApctlTerm", h_Ok},
	{0x8ABADD51, "sceNetApctlAddHandler", h_Ok},
	{0x5963991B, "sceNetApctlDelHandler", h_Ok},
	{0x24FE91A1, "sceNetApctlDisconnect", h_Ok},
	{0xF3370E61, "sceNetResolverInit", h_Ok},
	{0x6138194A, "sceNetResolverTerm", h_Ok},
	{0x244172AF, "sceNetResolverCreate", h_Ok},
	{0x94523E09, "sceNetResolverDelete", h_Ok},
	{0x224C5F44, "sceNetResolverStartNtoA", h_ResolverFail},
	{0xAB1ABE07, "sceHttpInit", h_Ok},
	{0xD1C8945E, "sceHttpEnd", h_Ok},
	{0xE4D21302, "sceHttpsInit", h_Ok},
	{0xF9D8EB63, "sceHttpsEnd", h_Ok},
	{0x87797BDD, "sceHttpsLoadDefaultCert", h_Ok},
	{0x957ECBE2, "sceSslInit", h_Ok},
	{0x191CDEFF, "sceSslEnd", h_Ok},
};

// the ad hoc network dialog finished (utility layer): the connection is up
void net_adhoc_connected(void) { if (!ctl_state) { ctl_state = 1; connect_pending = true; } }

void net_init(void) {
	hle_register(net_ents, sizeof net_ents / sizeof net_ents[0]);
	netconf_get_status = hle_lookup(0x6332AA39);
	if (netconf_get_status) {
		static const HleEnt wrap[] = {{0x6332AA39, "sceUtilityNetconfGetStatus", h_NetconfGetStatus}};
		hle_register(wrap, 1);
	}
}
