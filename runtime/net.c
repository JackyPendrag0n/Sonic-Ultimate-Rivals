// The PSP's ad hoc wireless stack (sceNetAdhocctl, sceNetAdhoc PDP, sceNetAdhocMatching) carried over UDP, so
// copies of the game on one PC (local split screen, see splitscreen.c) or on a LAN find and race each other the
// way PSPs did in Wireless Play.
//
// Every copy is a "PSP" with its own MAC, 02:53:52:32:00:<n>. Copies on this machine bind UDP 127.0.0.1:<base+n-1>
// (the first free of NET_MAX ports) and reach each other by port; SR2_NET_PEERS="host:port,..." adds remote
// copies (LAN). Ad hoc group membership, PDP datagrams and the Matching (lobby) protocol are small messages on
// that socket. Matching events are delivered to the game's callback from a runtime-owned thread, like the
// real library's event thread.
//
// With no other copy running it behaves like a PSP alone: you can host a game and nobody turns up.
#define _GNU_SOURCE
#include "kernel.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define ADHOC_INVALID_SOCKET 0x80410701u
#define ADHOC_INVALID_ADDR   0x80410702u
#define ADHOC_NOT_ENOUGH     0x80400706u
#define ADHOC_WOULD_BLOCK    0x80410709u
#define ADHOC_PORT_IN_USE    0x8041070Au
#define ADHOC_TIMEOUT        0x80410715u
#define ADHOC_NO_ENTRY       0x80410716u
#define ADHOC_ID_NOT_AVAIL   0x8041070Fu
#define MATCH_INVALID_ID     0x80410807u
#define MATCH_UNKNOWN_TARGET 0x8041080Cu
#define MATCH_IS_RUNNING     0x8041080Au
#define MATCH_NOT_RUNNING    0x8041080Bu
#define CTL_NOT_FOUND        0x80410B06u

enum { EV_HELLO = 1, EV_REQUEST, EV_LEAVE, EV_DENY, EV_CANCEL, EV_ACCEPT, EV_ESTABLISHED, EV_TIMEOUT, EV_ERROR, EV_BYE };
enum { MODE_PARENT = 1, MODE_CHILD = 2, MODE_P2P = 3 };
// wire messages
enum { M_BEACON = 1, M_PDP, M_HELLO, M_JOIN, M_ACCEPT, M_CANCEL, M_BYE, M_KEEPALIVE };

#define NET_MAX 4
#define NET_BASE_PORT 47320
#define MAX_PDP 16
#define MAX_PEERS 16
#define MAX_MATCH 4
#define PDP_QUEUE 64

static int sock = -1;
static int self_n;                  // 1..NET_MAX
static u8 self_mac[6];
static char self_nick[128];
static struct sockaddr_in remote[8]; static int nremote;
static const u8 BCAST[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

// ---------------------------------------------------------------- adhocctl state
static int ctl_state;                // 0 disconnected, 1 connected to a group
static char ctl_group[8];
static struct { u32 fn, arg; } ctl_handler[4];
static int ctl_event_pending;        // adhocctl event waiting for the event thread
static HleFn netconf_get_status;
// other copies seen in our group (from beacons)
static struct { bool used; u8 mac[6]; char nick[128]; char group[8]; u64 seen; } peers[MAX_PEERS];

// ---------------------------------------------------------------- PDP
typedef struct { u8 mac[6]; u16 port; u16 len; u8 *data; } Datagram;
static struct {
	bool used; u16 port;
	Datagram q[PDP_QUEUE]; int qh, qn;
} pdp[MAX_PDP];

// ---------------------------------------------------------------- matching
enum { PS_NONE, PS_SEEN, PS_OUT_REQ, PS_IN_REQ, PS_ESTABLISHED };
typedef struct { bool used; u8 mac[6]; int state; u64 last; } MPeer;
typedef struct Ev { int event; u8 mac[6]; int optlen; u8 opt[512]; } Ev;
typedef struct {
	bool used, running;
	int mode, maxnum; u16 port;
	u32 hello_int, keepalive_int, init_count, callback;
	u8 hello_opt[512]; int hello_optlen;
	u64 next_hello, next_keepalive;
	MPeer peer[MAX_PEERS];
	Ev ev[64]; int evh, evn;
} Match;
static Match match[MAX_MATCH];

static Thread *ev_thread;
static u32 ev_scratch;              // guest memory for callback arguments

// ---------------------------------------------------------------- transport
static void net_open(void) {
	if (sock >= 0) return;
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0) { LOGE("net: socket: %s", strerror(errno)); return; }
	const char *e = getenv("SR2_NET_ID");
	int want = e ? atoi(e) : 0;
	for (int n = 1; n <= NET_MAX; n++) {
		if (want && n != want) continue;
		struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NET_BASE_PORT + n - 1)};
		// bound on all interfaces only when LAN peers are configured; local copies use loopback
		a.sin_addr.s_addr = htonl(getenv("SR2_NET_PEERS") ? INADDR_ANY : INADDR_LOOPBACK);
		if (bind(sock, (struct sockaddr *)&a, sizeof a) == 0) { self_n = n; break; }
	}
	if (!self_n) { LOGE("net: no free UDP port %d-%d: wireless play disabled", NET_BASE_PORT, NET_BASE_PORT + NET_MAX - 1); close(sock); sock = -1; return; }
	fcntl(sock, F_SETFL, O_NONBLOCK);
	u8 mac[6] = {0x02, 0x53, 0x52, 0x32, 0x00, (u8)self_n};
	memcpy(self_mac, mac, 6);
	if ((e = getenv("SR2_NET_PEERS"))) {
		char tmp[512]; snprintf(tmp, sizeof tmp, "%s", e);
		for (char *sv, *t = strtok_r(tmp, ",", &sv); t && nremote < 8; t = strtok_r(NULL, ",", &sv)) {
			char *colon = strrchr(t, ':');
			int port = colon ? atoi(colon + 1) : NET_BASE_PORT;
			if (colon) *colon = 0;
			struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM}, *res;
			if (getaddrinfo(t, NULL, &hints, &res) == 0) {
				remote[nremote] = *(struct sockaddr_in *)res->ai_addr;
				remote[nremote++].sin_port = htons(port);
				freeaddrinfo(res);
			}
		}
	}
	LOGI("net: wireless play as %02x:%02x:%02x:%02x:%02x:%02x on UDP %d", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
	     NET_BASE_PORT + self_n - 1);
}

// message: type u8, src mac[6], dst mac[6], then payload
static void send_raw(const u8 *dst, int type, const void *payload, int len) {
	if (sock < 0) return;
	u8 buf[2048];
	if (len > (int)sizeof buf - 13) return;
	buf[0] = type; memcpy(buf + 1, self_mac, 6); memcpy(buf + 7, dst, 6);
	if (len) memcpy(buf + 13, payload, len);
	for (int n = 1; n <= NET_MAX; n++) {
		if (n == self_n) continue;
		// local copies: unicast goes straight to the owner of that MAC
		if (memcmp(dst, BCAST, 6) && !(dst[0] == 0x02 && dst[1] == 0x53 && dst[5] == n)) continue;
		struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(NET_BASE_PORT + n - 1), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
		sendto(sock, buf, 13 + len, 0, (struct sockaddr *)&a, sizeof a);
	}
	for (int i = 0; i < nremote; i++) sendto(sock, buf, 13 + len, 0, (struct sockaddr *)&remote[i], sizeof remote[i]);
}

static Match *match_by_port(u16 port) {
	for (int i = 0; i < MAX_MATCH; i++) if (match[i].used && match[i].running && match[i].port == port) return &match[i];
	return NULL;
}
static MPeer *mpeer(Match *m, const u8 *mac, bool add) {
	for (int i = 0; i < MAX_PEERS; i++) if (m->peer[i].used && !memcmp(m->peer[i].mac, mac, 6)) return &m->peer[i];
	if (!add) return NULL;
	for (int i = 0; i < MAX_PEERS; i++)
		if (!m->peer[i].used) { memset(&m->peer[i], 0, sizeof m->peer[i]); m->peer[i].used = true; memcpy(m->peer[i].mac, mac, 6); return &m->peer[i]; }
	return NULL;
}
static void queue_ev(Match *m, int event, const u8 *mac, const void *opt, int optlen) {
	if (m->evn >= 64) { LOGE("net: matching event queue full"); return; }
	Ev *e = &m->ev[(m->evh + m->evn++) % 64];
	e->event = event; memcpy(e->mac, mac, 6);
	if (optlen < 0 || optlen > 512) optlen = 0;
	e->optlen = optlen; if (optlen) memcpy(e->opt, opt, optlen);
}
static int established_count(Match *m) {
	int n = 0;
	for (int i = 0; i < MAX_PEERS; i++) n += m->peer[i].used && m->peer[i].state == PS_ESTABLISHED;
	return n;
}

// matching payload: u16 port, then opt
static void msend(Match *m, const u8 *dst, int type, const void *opt, int optlen) {
	u8 b[520];
	b[0] = m->port & 0xff; b[1] = m->port >> 8;
	if (optlen < 0 || optlen > 512) optlen = 0;
	if (optlen) memcpy(b + 2, opt, optlen);
	send_raw(dst, type, b, 2 + optlen);
}

static void on_matching(int type, const u8 *src, const u8 *p, int len) {
	if (len < 2) return;
	Match *m = match_by_port(p[0] | p[1] << 8);
	if (!m) return;
	const u8 *opt = p + 2; int optlen = len - 2;
	MPeer *pe = mpeer(m, src, type == M_HELLO || type == M_JOIN);
	if (!pe) return;
	pe->last = now_us();
	switch (type) {
	case M_HELLO:   // a parent (or p2p peer) advertising
		if (m->mode == MODE_PARENT) break;
		if (pe->state == PS_NONE || pe->state == PS_SEEN) { pe->state = PS_SEEN; queue_ev(m, EV_HELLO, src, opt, optlen); }
		break;
	case M_JOIN:    // someone asks to join us
		if (m->mode == MODE_CHILD) break;
		if (pe->state == PS_ESTABLISHED) break;
		if (pe->state == PS_OUT_REQ && m->mode == MODE_P2P) {   // both asked each other: accept
			pe->state = PS_ESTABLISHED; msend(m, src, M_ACCEPT, NULL, 0);
			queue_ev(m, EV_ESTABLISHED, src, NULL, 0);
			break;
		}
		if (m->maxnum && established_count(m) + 1 >= m->maxnum) { msend(m, src, M_CANCEL, NULL, 0); break; }
		pe->state = PS_IN_REQ;
		queue_ev(m, EV_REQUEST, src, opt, optlen);
		break;
	case M_ACCEPT:
		if (pe->state != PS_OUT_REQ) break;
		pe->state = PS_ESTABLISHED;
		queue_ev(m, EV_ACCEPT, src, opt, optlen);
		queue_ev(m, EV_ESTABLISHED, src, NULL, 0);
		break;
	case M_CANCEL:
		if (pe->state == PS_OUT_REQ) queue_ev(m, EV_DENY, src, opt, optlen);
		else if (pe->state == PS_IN_REQ) queue_ev(m, EV_CANCEL, src, opt, optlen);
		else if (pe->state == PS_ESTABLISHED) queue_ev(m, EV_LEAVE, src, opt, optlen);
		pe->state = PS_SEEN;
		break;
	case M_BYE:
		if (pe->state == PS_ESTABLISHED || pe->state == PS_IN_REQ || pe->state == PS_OUT_REQ) queue_ev(m, EV_BYE, src, NULL, 0);
		pe->used = false;
		break;
	case M_KEEPALIVE: break;
	}
}

static void net_pump(void) {
	if (sock < 0) return;
	u8 buf[2048];
	int n;
	while ((n = recv(sock, buf, sizeof buf, 0)) >= 13) {
		const u8 *src = buf + 1, *dst = buf + 7, *p = buf + 13;
		int len = n - 13;
		if (!memcmp(src, self_mac, 6)) continue;
		if (memcmp(dst, BCAST, 6) && memcmp(dst, self_mac, 6)) continue;
		switch (buf[0]) {
		case M_BEACON: {   // group[8] nick[...]
			if (len < 8) break;
			int slot = -1;
			for (int i = 0; i < MAX_PEERS; i++) if (peers[i].used && !memcmp(peers[i].mac, src, 6)) slot = i;
			for (int i = 0; i < MAX_PEERS && slot < 0; i++) if (!peers[i].used) slot = i;
			if (slot < 0) break;
			if (!peers[slot].used) LOGI("net: found %.*s in group '%.8s'", len - 8, (const char *)p + 8, (const char *)p);
			peers[slot].used = true; memcpy(peers[slot].mac, src, 6); memcpy(peers[slot].group, p, 8);
			snprintf(peers[slot].nick, sizeof peers[slot].nick, "%.*s", len - 8, (const char *)p + 8);
			peers[slot].seen = now_us();
			break;
		}
		case M_PDP: {      // dport u16, sport u16, data
			if (len < 4 || !ctl_state) break;
			u16 dport = p[0] | p[1] << 8, sport = p[2] | p[3] << 8;
			for (int i = 0; i < MAX_PDP; i++) {
				if (!pdp[i].used || pdp[i].port != dport) continue;
				if (pdp[i].qn >= PDP_QUEUE) { LOGD("net: PDP queue full on port %u, dropping", dport); break; }
				Datagram *d = &pdp[i].q[(pdp[i].qh + pdp[i].qn++) % PDP_QUEUE];
				memcpy(d->mac, src, 6); d->port = sport; d->len = len - 4;
				d->data = malloc(d->len ? d->len : 1); memcpy(d->data, p + 4, d->len);
				break;
			}
			break;
		}
		default:
			if (ctl_state) on_matching(buf[0], src, p, len);
		}
	}
}

static void beacon(void) {
	static u64 next;
	u64 t = now_us();
	if (!ctl_state || t < next) return;
	next = t + 500000;
	u8 b[8 + 128];
	memcpy(b, ctl_group, 8);
	int nl = strlen(self_nick);
	memcpy(b + 8, self_nick, nl);
	send_raw(BCAST, M_BEACON, b, 8 + nl);
}

// timers: hello broadcasts, keepalives, peers that went quiet
static void match_tick(void) {
	u64 t = now_us();
	for (int i = 0; i < MAX_MATCH; i++) {
		Match *m = &match[i];
		if (!m->used || !m->running) continue;
		if (m->mode != MODE_CHILD && t >= m->next_hello && (m->mode == MODE_P2P || !m->maxnum || established_count(m) + 1 < m->maxnum)) {
			msend(m, BCAST, M_HELLO, m->hello_opt, m->hello_optlen);
			m->next_hello = t + (m->hello_int ? m->hello_int : 200000);
		}
		u64 ka = m->keepalive_int ? m->keepalive_int : 200000;
		if (t >= m->next_keepalive) {
			for (int k = 0; k < MAX_PEERS; k++)
				if (m->peer[k].used && m->peer[k].state >= PS_OUT_REQ) msend(m, m->peer[k].mac, M_KEEPALIVE, NULL, 0);
			m->next_keepalive = t + ka;
		}
		u64 limit = ka * (m->init_count ? m->init_count : 10);
		if (limit < 3000000) limit = 3000000;
		for (int k = 0; k < MAX_PEERS; k++) {
			MPeer *pe = &m->peer[k];
			if (!pe->used || !pe->last || t - pe->last < limit) continue;
			if (pe->state == PS_ESTABLISHED || pe->state == PS_OUT_REQ || pe->state == PS_IN_REQ) queue_ev(m, EV_TIMEOUT, pe->mac, NULL, 0);
			pe->used = false;
		}
	}
}

void net_poll(void) { net_pump(); beacon(); match_tick(); }

// ---------------------------------------------------------------- the event thread
static void call_guest5(Thread *t, u32 fn, u32 a0, u32 a1, u32 a2, u32 a3, u32 a4) {
	Ctx save = t->ctx;
	t->ctx.r[4] = a0; t->ctx.r[5] = a1; t->ctx.r[6] = a2; t->ctx.r[7] = a3; t->ctx.r[8] = a4;
	t->ctx.r[29] = (t->ctx.r[29] - 0x100) & ~15u;
	t->ctx.r[31] = 0;
	rt_call(&t->ctx, fn);
	t->ctx = save;
}

static void ev_main(Thread *t) {
	for (;;) {
		net_poll();
		bool any = false;
		if (ctl_event_pending) {
			int e = ctl_event_pending; ctl_event_pending = 0;
			for (int i = 0; i < 4; i++)
				if (ctl_handler[i].fn) call_guest5(t, ctl_handler[i].fn, e, 0, ctl_handler[i].arg, 0, 0);
			any = true;
		}
		for (int i = 0; i < MAX_MATCH; i++) {
			Match *m = &match[i];
			while (m->used && m->evn) {
				Ev e = m->ev[m->evh];
				m->evh = (m->evh + 1) % 64; m->evn--;
				memcpy(PTR(ev_scratch), e.mac, 6);
				if (e.optlen) memcpy(PTR(ev_scratch + 16), e.opt, e.optlen);
				if (e.event == EV_HELLO) LOGD("net: matching %d event %d (peer %02x)", i + 1, e.event, e.mac[5]);
				else LOGI("net: matching %d event %d (peer %02x)", i + 1, e.event, e.mac[5]);
				if (m->callback) call_guest5(t, m->callback, i + 1, e.event, ev_scratch, e.optlen, e.optlen ? ev_scratch + 16 : 0);
				any = true;
			}
		}
		if (!any) thread_delay_until(now_us() + 2000);
	}
}

static void ensure_ev_thread(u32 prio) {
	if (ev_thread) return;
	ev_scratch = kalloc(0x220);
	ev_thread = thread_create("SceNetAdhocEvent", 0, prio ? prio : 0x20, 0x4000, 0, cur_module_gp());
	ev_thread->host_entry = ev_main;
	thread_start(ev_thread, 0, 0);
}

// ---------------------------------------------------------------- HLE: sceNet / adhocctl
static void h_Ok(Ctx *c) { RET(0); }
static void h_WlanSwitch(Ctx *c) { RET(1); }        // WLAN switch on
static void h_AdhocctlInit(Ctx *c) {
	net_open();
	ctl_state = 0;
	extern const char *utility_nickname(void);
	snprintf(self_nick, sizeof self_nick, "%s", utility_nickname());
	ensure_ev_thread(0x30);
	RET(0);
}
static void h_AdhocctlAddHandler(Ctx *c) {
	for (int i = 0; i < 4; i++)
		if (!ctl_handler[i].fn) { ctl_handler[i].fn = A(0); ctl_handler[i].arg = A(1); RET(i); return; }
	RET(0x80410b07);
}
static void h_AdhocctlDelHandler(Ctx *c) { if (A(0) < 4) ctl_handler[A(0)].fn = 0; RET(0); }
static void h_AdhocctlConnect(Ctx *c) {
	memset(ctl_group, 0, 8);
	if (A(0)) memcpy(ctl_group, PTR(A(0)), 8);
	ctl_state = 1;
	ctl_event_pending = 1;   // PSP_ADHOCCTL_EVENT_CONNECT
	LOGI("net: joined ad hoc group '%.8s'", ctl_group);
	RET(0);
}
// The ad hoc network dialog reports the connection through the adhocctl handlers before it finishes.
static void h_NetconfGetStatus(Ctx *c) { netconf_get_status(c); }
static void h_AdhocctlGetState(Ctx *c) {
	if (A(0)) SW(A(0), ctl_state);
	RET(0);
}
static void h_AdhocctlDisconnect(Ctx *c) {
	bool was = ctl_state;
	ctl_state = 0;
	memset(peers, 0, sizeof peers);
	if (was) ctl_event_pending = 2;   // PSP_ADHOCCTL_EVENT_DISCONNECT
	RET(0);
}
static void h_AdhocctlGetParameter(Ctx *c) {
	// {s32 channel; char group[8]; u8 bssid[6]; char nickname[128]}
	u32 p = A(0);
	if (!p) { RET(0x80410B04); return; }
	memset(PTR(p), 0, 4 + 8 + 6 + 128);
	SW(p, 1);
	memcpy(PTR(p + 4), ctl_group, 8);
	memcpy(PTR(p + 12), self_mac, 6);
	snprintf((char *)PTR(p + 18), 128, "%s", self_nick);
	RET(0);
}
static void h_AdhocctlGetNameByAddr(Ctx *c) {
	u32 macp = A(0), out = A(1);
	if (!macp || !out) { RET(0x80410B04); return; }
	const u8 *mac = PTR(macp);
	if (!memcmp(mac, self_mac, 6)) { snprintf((char *)PTR(out), 128, "%s", self_nick); RET(0); return; }
	for (int i = 0; i < MAX_PEERS; i++)
		if (peers[i].used && !memcmp(peers[i].mac, mac, 6)) { snprintf((char *)PTR(out), 128, "%s", peers[i].nick); RET(0); return; }
	RET(CTL_NOT_FOUND);
}
static void h_GetLocalEtherAddr(Ctx *c) { net_open(); if (A(0)) memcpy(PTR(A(0)), self_mac, 6); RET(0); }
static void h_EtherNtostr(Ctx *c) {
	if (A(0) && A(1)) {
		const u8 *m = PTR(A(0));
		snprintf((char *)PTR(A(1)), 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	}
	RET(0);
}

// ---------------------------------------------------------------- HLE: PDP
static void h_PdpCreate(Ctx *c) {
	// (mac*, port, bufsize, flag)
	net_open();
	u16 port = A(1);
	for (int i = 0; i < MAX_PDP; i++) if (pdp[i].used && pdp[i].port == port && port) { RET(ADHOC_PORT_IN_USE); return; }
	for (int i = 0; i < MAX_PDP; i++)
		if (!pdp[i].used) {
			memset(&pdp[i], 0, sizeof pdp[i]);
			pdp[i].used = true; pdp[i].port = port;
			LOGI("net: PDP socket %d on port %u", i + 1, port);
			RET(i + 1); return;
		}
	RET(ADHOC_ID_NOT_AVAIL);
}
static void pdp_flush(int i) {
	while (pdp[i].qn) { free(pdp[i].q[pdp[i].qh].data); pdp[i].qh = (pdp[i].qh + 1) % PDP_QUEUE; pdp[i].qn--; }
}
static void h_PdpDelete(Ctx *c) {
	u32 id = A(0);
	if (id >= 1 && id <= MAX_PDP && pdp[id - 1].used) { pdp_flush(id - 1); pdp[id - 1].used = false; RET(0); return; }
	RET(ADHOC_INVALID_SOCKET);
}
static void h_PdpSend(Ctx *c) {
	// (id, dstmac*, dport, data*, len, timeout, nonblock)
	u32 id = A(0), macp = A(1), dport = A(2), data = A(3), len = c->r[8];
	if (!(id >= 1 && id <= MAX_PDP && pdp[id - 1].used)) { RET(ADHOC_INVALID_SOCKET); return; }
	if (!macp) { RET(ADHOC_INVALID_ADDR); return; }
	if (len > 2000) { RET(0x80410705); return; }
	u8 b[2048];
	b[0] = dport & 0xff; b[1] = dport >> 8; b[2] = pdp[id - 1].port & 0xff; b[3] = pdp[id - 1].port >> 8;
	if (len) memcpy(b + 4, PTR(data), len);
	if (ctl_state) send_raw(PTR(macp), M_PDP, b, 4 + len);
	RET(0);
}
static void h_PdpRecv(Ctx *c) {
	// (id, srcmac*, port*, buf*, len*, timeout, nonblock)
	u32 id = A(0), macp = A(1), portp = A(2), buf = A(3), lenp = c->r[8], timeout = c->r[9], nonblock = c->r[10];
	if (!(id >= 1 && id <= MAX_PDP && pdp[id - 1].used)) { RET(ADHOC_INVALID_SOCKET); return; }
	int i = id - 1;
	u64 until = now_us() + (timeout ? timeout : 0);
	for (;;) {
		net_poll();
		if (pdp[i].qn) break;
		if (nonblock) { RET(ADHOC_WOULD_BLOCK); return; }
		if (timeout && now_us() >= until) { RET(ADHOC_TIMEOUT); return; }
		thread_delay_until(now_us() + 1000);
		if (!pdp[i].used) { RET(0x80410707); return; }
	}
	Datagram *d = &pdp[i].q[pdp[i].qh];
	u32 room = lenp ? LW(lenp) : 0;
	if (d->len > room) { if (lenp) SW(lenp, d->len); RET(ADHOC_NOT_ENOUGH); return; }
	if (d->len) memcpy(PTR(buf), d->data, d->len);
	if (lenp) SW(lenp, d->len);
	if (macp) memcpy(PTR(macp), d->mac, 6);
	if (portp) SH(portp, d->port);
	free(d->data);
	pdp[i].qh = (pdp[i].qh + 1) % PDP_QUEUE; pdp[i].qn--;
	RET(0);
}
static void h_GetPdpStat(Ctx *c) {
	// (int *size, SceNetAdhocPdpStat *buf): {next, id, mac[6], port u16, rcv_sb_cc u32} = 20 bytes each
	u32 sizep = A(0), buf = A(1), n = 0;
	net_poll();
	for (int i = 0; i < MAX_PDP; i++) n += pdp[i].used;
	if (!sizep) { RET(ADHOC_NO_ENTRY); return; }
	if (!buf) { SW(sizep, n * 20); RET(0); return; }
	u32 room = LW(sizep) / 20, k = 0;
	for (int i = 0; i < MAX_PDP && k < room; i++) {
		if (!pdp[i].used) continue;
		u32 e = buf + k * 20, pending = 0;
		for (int q = 0; q < pdp[i].qn; q++) pending += pdp[i].q[(pdp[i].qh + q) % PDP_QUEUE].len;
		SW(e, 0); SW(e + 4, i + 1); memcpy(PTR(e + 8), self_mac, 6); SH(e + 14, pdp[i].port); SW(e + 16, pending);
		if (k) SW(e - 20, e);
		k++;
	}
	SW(sizep, k * 20);
	RET(0);
}
static void h_ResolverFail(Ctx *c) { RET(0x80410408); }   // no DNS: infrastructure mode can't reach anything

// ---------------------------------------------------------------- HLE: matching
static Match *mget(u32 id) { return id >= 1 && id <= MAX_MATCH && match[id - 1].used ? &match[id - 1] : NULL; }
static void h_MatchingCreate(Ctx *c) {
	// (mode, maxnum, port, rxbuflen, hello_int, keepalive_int, init_count, rexmt_int, callback)
	net_open();
	for (int i = 0; i < MAX_MATCH; i++) {
		if (match[i].used) continue;
		Match *m = &match[i];
		memset(m, 0, sizeof *m);
		m->used = true; m->mode = A(0); m->maxnum = A(1); m->port = A(2);
		m->hello_int = c->r[8]; m->keepalive_int = c->r[9]; m->init_count = c->r[10]; m->callback = LW(c->r[29]);   // 9th argument: on the stack
		LOGI("net: matching %d: mode %d, max %d, port %u, callback %08x", i + 1, m->mode, m->maxnum, m->port, m->callback);
		RET(i + 1); return;
	}
	RET(0x80410808);
}
static void h_MatchingStart(Ctx *c) {
	// (id, evthpri, evthstack, inthpri, inthstack, optlen, optdata)
	Match *m = mget(A(0));
	if (!m) { RET(MATCH_INVALID_ID); return; }
	if (m->running) { RET(MATCH_IS_RUNNING); return; }
	int optlen = c->r[9]; u32 opt = c->r[10];
	m->hello_optlen = (optlen > 0 && optlen <= 512 && opt) ? optlen : 0;
	if (m->hello_optlen) memcpy(m->hello_opt, PTR(opt), m->hello_optlen);
	m->running = true;
	m->next_hello = m->next_keepalive = 0;
	ensure_ev_thread(A(1));
	RET(0);
}
static void h_MatchingStop(Ctx *c) {
	Match *m = mget(A(0));
	if (!m) { RET(MATCH_INVALID_ID); return; }
	if (m->running)
		for (int k = 0; k < MAX_PEERS; k++)
			if (m->peer[k].used && m->peer[k].state >= PS_OUT_REQ) msend(m, m->peer[k].mac, M_BYE, NULL, 0);
	m->running = false;
	memset(m->peer, 0, sizeof m->peer);
	m->evn = 0;
	RET(0);
}
static void h_MatchingDelete(Ctx *c) {
	Match *m = mget(A(0));
	if (!m) { RET(MATCH_INVALID_ID); return; }
	if (m->running) h_MatchingStop(c);
	m->used = false;
	RET(0);
}
static void h_MatchingSelectTarget(Ctx *c) {
	// (id, mac*, optlen, opt*)
	Match *m = mget(A(0));
	if (!m) { RET(MATCH_INVALID_ID); return; }
	if (!m->running) { RET(MATCH_NOT_RUNNING); return; }
	const u8 *mac = PTR(A(1));
	int optlen = A(2); const void *opt = optlen > 0 && A(3) ? PTR(A(3)) : NULL;
	if (!opt) optlen = 0;
	MPeer *pe = mpeer(m, mac, false);
	if (!pe) { RET(MATCH_UNKNOWN_TARGET); return; }
	if (pe->state == PS_IN_REQ) {          // accept an incoming request
		pe->state = PS_ESTABLISHED;
		msend(m, mac, M_ACCEPT, opt, optlen);
		queue_ev(m, EV_ESTABLISHED, mac, NULL, 0);
	} else if (pe->state == PS_SEEN && m->mode != MODE_PARENT) {   // ask to join
		pe->state = PS_OUT_REQ;
		msend(m, mac, M_JOIN, opt, optlen);
	} else if (pe->state == PS_ESTABLISHED) { RET(0x80410810); return; }
	else if (pe->state == PS_OUT_REQ) { RET(0x8041080F); return; }
	else { RET(0x8041080D); return; }
	RET(0);
}
static void h_MatchingCancelTarget(Ctx *c) {
	Match *m = mget(A(0));
	if (!m) { RET(MATCH_INVALID_ID); return; }
	MPeer *pe = A(1) ? mpeer(m, PTR(A(1)), false) : NULL;
	if (!pe) { RET(MATCH_UNKNOWN_TARGET); return; }
	if (pe->state >= PS_OUT_REQ) msend(m, pe->mac, M_CANCEL, NULL, 0);
	pe->state = PS_SEEN;
	RET(0);
}

static const HleEnt net_ents[] = {
	{0xD7763699, "sceWlanGetSwitchState", h_WlanSwitch},
	{0x93440B11, "sceWlanDevIsPowerOn", h_WlanSwitch},
	{0x39AF39A6, "sceNetInit", h_Ok},
	{0x281928A9, "sceNetTerm", h_Ok},
	{0x0BF0A3AE, "sceNetGetLocalEtherAddr", h_GetLocalEtherAddr},
	{0x89360950, "sceNetEtherNtostr", h_EtherNtostr},
	{0xFD8585E1, "sceNetSetDropRate", h_Ok},
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
	{0x0AD043ED, "sceNetAdhocctlConnect", h_AdhocctlConnect},
	{0x75ECD386, "sceNetAdhocctlGetState", h_AdhocctlGetState},
	{0x34401D65, "sceNetAdhocctlDisconnect", h_AdhocctlDisconnect},
	{0xDED9D28E, "sceNetAdhocctlGetParameter", h_AdhocctlGetParameter},
	{0x8916C003, "sceNetAdhocctlGetNameByAddr", h_AdhocctlGetNameByAddr},
	{0x2A2A1E07, "sceNetAdhocMatchingInit", h_Ok},
	{0x7945ECDA, "sceNetAdhocMatchingTerm", h_Ok},
	{0xCA5EDA6F, "sceNetAdhocMatchingCreate", h_MatchingCreate},
	{0x93EF3843, "sceNetAdhocMatchingStart", h_MatchingStart},
	{0x32B156B3, "sceNetAdhocMatchingStop", h_MatchingStop},
	{0xF16EAF4F, "sceNetAdhocMatchingDelete", h_MatchingDelete},
	{0x5E3D4B79, "sceNetAdhocMatchingSelectTarget", h_MatchingSelectTarget},
	{0xEA3C6108, "sceNetAdhocMatchingCancelTarget", h_MatchingCancelTarget},
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
void net_adhoc_connected(void) { if (!ctl_state) { ctl_state = 1; ctl_event_pending = 1; } }

void net_init(void) {
	extern void utility_nickname_from_env(void);
	utility_nickname_from_env();
	hle_register(net_ents, sizeof net_ents / sizeof net_ents[0]);
	netconf_get_status = hle_lookup(0x6332AA39);
	if (netconf_get_status) {
		static const HleEnt wrap[] = {{0x6332AA39, "sceUtilityNetconfGetStatus", h_NetconfGetStatus}};
		hle_register(wrap, 1);
	}
}
