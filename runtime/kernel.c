// PSP kernel HLE: objects, memory partitions, threads + scheduler, sync primitives, time, callbacks,
// interrupts, module manager.
#define _GNU_SOURCE
#include "kernel.h"
#include <time.h>
#include <errno.h>

void vfpu_reset_ctx(Ctx *c);

// ================================================================= time
static struct timespec t0;
// SR2_DETERMINISTIC=1: virtual clock that only advances when every guest thread is waiting (plus a
// tiny amount per context switch), so runs are reproducible and as fast as the host allows.
static bool g_virtual;
static u64 g_vtime = 1000000, g_pause_us;
u64 now_us(void) {
	if (g_virtual) return g_vtime;
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (u64)(t.tv_sec - t0.tv_sec) * 1000000 + (t.tv_nsec - t0.tv_nsec) / 1000 + 1000000 - g_pause_us;
}
void kernel_add_pause_time(u64 us) { if (!g_virtual) g_pause_us += us; }
bool kernel_is_virtual_time(void) { return g_virtual; }

// ================================================================= objects
#define MAX_OBJ 4096
static struct { int type; void *p; } objs[MAX_OBJ];
u32 obj_new(int type, void *p) {
	for (int i = 1; i < MAX_OBJ; i++)
		if (!objs[i].type) { objs[i].type = type; objs[i].p = p; return 0x1000 + i * 2 + 1; }
	fatal("out of kernel objects");
}
void *obj_get(u32 uid, int type) {
	u32 i = (uid - 0x1001) / 2;
	if (uid < 0x1001 || i >= MAX_OBJ || objs[i].type != type) return NULL;
	return objs[i].p;
}
void obj_free(u32 uid) {
	u32 i = (uid - 0x1001) / 2;
	if (i < MAX_OBJ) { objs[i].type = 0; objs[i].p = NULL; }
}

// ================================================================= memory partitions
typedef struct Block { u32 addr, size, uid; int used; char name[32]; struct Block *next; } Block;
static Block *user_blocks;
#define USER_START 0x08800000u
#define USER_END   0x0A000000u
static u32 kheap = 0x08000000u + 0x100000u;  // kernel partition scratch

u32 kalloc(u32 size) {
	u32 a = (kheap + 15) & ~15u;
	kheap = a + size;
	if (kheap > 0x08400000u) fatal("kernel scratch exhausted");
	memset(PTR(a), 0, size);
	return a;
}

static void mem_init(void) {
	user_blocks = calloc(1, sizeof(Block));
	user_blocks->addr = USER_START;
	user_blocks->size = USER_END - USER_START;
}

u32 mem_alloc(int part, const char *name, int type, u32 size, u32 addr, u32 align, u32 *uid_out) {
	if (align < 256) align = 256;
	size = (size + 255) & ~255u;
	Block *best = NULL;
	u32 baddr = 0;
	for (Block *b = user_blocks; b; b = b->next) {
		if (b->used) continue;
		u32 a;
		if (type == 2) {  // fixed address
			if (addr < b->addr || addr + size > b->addr + b->size) continue;
			a = addr;
		} else if (type == 1) {  // high
			if (b->size < size) continue;
			a = (b->addr + b->size - size) & ~(align - 1);
			if (a < b->addr) continue;
		} else {
			a = (b->addr + align - 1) & ~(align - 1);
			if (a + size > b->addr + b->size || a < b->addr) continue;
		}
		if (type == 1) { if (!best || a > baddr) { best = b; baddr = a; } }
		else { best = b; baddr = a; break; }
	}
	if (!best) return 0;
	Block *b = best;
	// split: [b->addr, baddr) free, [baddr, baddr+size) used, rest free
	if (baddr > b->addr) {
		Block *n = calloc(1, sizeof *n);
		n->addr = baddr; n->size = b->addr + b->size - baddr; n->next = b->next;
		b->size = baddr - b->addr; b->next = n; b = n;
	}
	if (b->size > size) {
		Block *n = calloc(1, sizeof *n);
		n->addr = b->addr + size; n->size = b->size - size; n->next = b->next;
		b->size = size; b->next = n;
	}
	b->used = 1;
	snprintf(b->name, sizeof b->name, "%s", name ? name : "");
	b->uid = obj_new(OBJ_MEMBLOCK, b);
	if (uid_out) *uid_out = b->uid;
	LOGD("mem_alloc '%s' type %d size %x -> %08x", b->name, type, size, b->addr);
	return b->addr;
}

void mem_free_uid(u32 uid) {
	Block *b = obj_get(uid, OBJ_MEMBLOCK);
	if (!b) return;
	obj_free(uid);
	b->used = 0;
	for (Block *p = user_blocks; p && p->next;) {
		if (!p->used && !p->next->used) { Block *n = p->next; p->size += n->size; p->next = n->next; free(n); }
		else p = p->next;
	}
}
u32 mem_block_addr(u32 uid) { Block *b = obj_get(uid, OBJ_MEMBLOCK); return b ? b->addr : 0; }
u32 mem_block_by_name(const char *name, u32 *size) {
	for (Block *b = user_blocks; b; b = b->next)
		if (b->used && !strcmp(b->name, name)) { if (size) *size = b->size; return b->addr; }
	return 0;
}
u32 mem_max_free(void) { u32 m = 0; for (Block *b = user_blocks; b; b = b->next) if (!b->used && b->size > m) m = b->size; return m; }
u32 mem_total_free(void) { u32 m = 0; for (Block *b = user_blocks; b; b = b->next) if (!b->used) m += b->size; return m; }

// ================================================================= threads
Thread *g_cur;
static Thread *all_threads;
static host_ctx sched_uc;
static u64 seq_counter;
bool g_in_intr;
u64 g_vblank_count;
static u64 next_vblank;
static u64 vblank_rephase;   // set by the frontend to line the emulated vblank up with the display's
// The frontend's presents are V-Synced: it moves the next emulated vblank to just before the display's next
// refresh, so the swap doesn't stall the game for a varying part of every frame (see fe_vblank).
void kernel_vblank_rephase(u64 at) { vblank_rephase = at; }
u32 g_vblank_us = 16683;            // emulated vblank period (the races can ask for more than 60 Hz, see game.c)

static void thread_tramp(void) {
	Thread *t = g_cur;
	if (t->host_entry) t->host_entry(t);
	else rt_call(&t->ctx, t->entry);
	// returned from entry: exit with v0
	t->exit_status = t->ctx.r[2];
	LOGD("thread '%s' returned %x", t->name, t->exit_status);
	t->status = TS_DORMANT;
	for (Thread *w = NULL; (w = thread_find_waiting(W_THREADEND, t->uid, w));) thread_wake(w, t->exit_status);
	if (t->attr & 0x80000000u) {}  // main thread: keep going (game may have other threads)
	host_ctx_swap(&t->uc, &sched_uc);
	fatal("dead thread resumed");
}

Thread *thread_create(const char *name, u32 entry, u32 prio, u32 stacksize, u32 attr, u32 gp) {
	Thread *t = calloc(1, sizeof *t);
	snprintf(t->name, sizeof t->name, "%s", name);
	t->entry = entry; t->prio = t->init_prio = prio; t->attr = attr; t->gp = gp;
	if (stacksize < 0x200) stacksize = 0x200;
	stacksize = (stacksize + 255) & ~255u;
	t->stack_size = stacksize;
	t->stack_addr = mem_alloc(2, "stack", 1, stacksize, 0, 256, &t->stack_uid);
	if (!t->stack_addr) fatal("no memory for thread stack (%s, %x)", name, stacksize);
	memset(PTR(t->stack_addr), 0xFF, stacksize);
	t->status = TS_DORMANT;
	t->uid = obj_new(OBJ_THREAD, t);
	t->next_all = all_threads;
	all_threads = t;
	LOGD("create thread '%s' uid %x entry %08x prio %x stack %x@%08x", name, t->uid, entry, prio, stacksize, t->stack_addr);
	return t;
}

void thread_start(Thread *t, u32 argsize, u32 argp) {
	memset(&t->ctx, 0, sizeof t->ctx);
	vfpu_reset_ctx(&t->ctx);
	t->ctx.thread = t;
	u32 sp = t->stack_addr + t->stack_size;
	// the thread's id is written at the stack bottom like the real kernel does
	SW(t->stack_addr, t->uid);
	sp -= 64;
	if (argsize && argp) {
		sp -= (argsize + 15) & ~15u;
		memmove(PTR(sp), PTR(argp), argsize);
		t->ctx.r[5] = sp;
	} else t->ctx.r[5] = 0;
	t->ctx.r[4] = argsize;
	t->ctx.r[29] = sp - 64;
	t->ctx.r[28] = t->gp;
	t->ctx.r[31] = 0;
	t->ctx.fcr31 = 0;
	if (!t->hoststack) t->hoststack = malloc(1 << 20);
	host_ctx_make(&t->uc, t->hoststack, 1 << 20, thread_tramp);
	t->status = TS_READY;
	t->seq = ++seq_counter;
	t->wakeup_count = 0;
}

Thread *thread_find_waiting(int wait_type, u32 id, Thread *after) {
	// highest priority first, FIFO within priority
	Thread *best = NULL;
	for (Thread *t = all_threads; t; t = t->next_all) {
		if (t == after) continue;
		if (t->status == TS_WAITING && t->wait_type == wait_type && t->wait_id == id) {
			if (after && (t->prio < after->prio || (t->prio == after->prio && t->seq < after->seq))) continue;
			if (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq)) best = t;
		}
	}
	return best;
}

static void write_timeout(Thread *t, bool expired) {
	if (!t->timeout_ptr) return;
	if (expired) SW(t->timeout_ptr, 0);
	else if (t->wake_time) {
		u64 n = now_us();
		SW(t->timeout_ptr, t->wake_time > n ? (u32)(t->wake_time - n) : 0);
	}
}

void thread_wake(Thread *t, s32 result) {
	if (t->status != TS_WAITING) return;
	write_timeout(t, false);
	t->wait_result = result;
	t->wait_type = W_NONE;
	t->status = TS_READY;
	t->seq = ++seq_counter;
}

static void switch_to_sched(void) {
	Thread *t = g_cur;
	host_ctx_swap(&t->uc, &sched_uc);
}

// Block until really woken. A thread in a CB-enabled wait may be resumed just to run notified
// callbacks on its own stack; it then goes back to waiting (same wait object and timeout).
static void wait_loop(Thread *t) {
	for (;;) {
		switch_to_sched();
		if (!t->cb_wake) break;
		t->cb_wake = false;
		callbacks_check();
		if (t->status == TS_RUNNING && t->wait_type != W_NONE) { t->status = TS_WAITING; continue; }
		break;
	}
}

s32 thread_wait(int wait_type, u32 id, u32 timeout_ptr, bool cb) {
	Thread *t = g_cur;
	if (!t) fatal("blocking wait outside a thread (wait %d)", wait_type);
	if (g_in_intr) fatal("blocking wait in interrupt context");
	if (cb) callbacks_check();
	t->status = TS_WAITING;
	t->wait_type = wait_type;
	t->wait_id = id;
	t->timeout_ptr = timeout_ptr;
	t->wake_time = 0;
	if (timeout_ptr) {
		u32 us = LW(timeout_ptr);
		t->wake_time = now_us() + (us ? us : 1);
	}
	t->cb_wait = cb;
	t->wait_result = 0;
	wait_loop(t);
	t->cb_wait = false;
	return t->wait_result;
}

static Thread *pick_ready(void) {
	Thread *best = NULL;
	for (Thread *t = all_threads; t; t = t->next_all)
		if (t->status == TS_READY && (!best || t->prio < best->prio || (t->prio == best->prio && t->seq < best->seq))) best = t;
	return best;
}

void thread_resched(void) {
	if (!g_cur || g_in_intr) return;
	Thread *b = pick_ready();
	if (b && b->prio < g_cur->prio) {
		g_cur->status = TS_READY;  // keeps its seq: stays at the head of its priority
		switch_to_sched();
	}
}

void thread_delay_until(u64 when) {
	Thread *t = g_cur;
	t->status = TS_WAITING; t->wait_type = W_DELAY; t->wait_id = 0; t->timeout_ptr = 0;
	t->wake_time = when;
	switch_to_sched();
}

static void thread_yield(void) {  // go to the back of the ready queue
	if (!g_cur) return;
	g_cur->status = TS_READY;
	g_cur->seq = ++seq_counter;
	switch_to_sched();
}

// ---------------------------------------------------------------- interrupts
typedef struct { u32 handler, a0, a1, a2, gp; void (*done)(void *, u32); void *ud; } IntrReq;
static IntrReq intrq[256];
static int intrq_n;
static Ctx intr_ctx;
static u32 intr_stack;

void intr_queue_ex(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp, void (*done)(void *, u32), void *ud) {
	if (intrq_n >= 256) { LOGE("interrupt queue overflow"); return; }
	intrq[intrq_n++] = (IntrReq){handler, a0, a1, a2, gp, done, ud};
}
void intr_queue(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp) { intr_queue_ex(handler, a0, a1, a2, gp, NULL, NULL); }

u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2) {
	Ctx tmp = *tmpl;
	tmp.r[4] = a0; tmp.r[5] = a1; tmp.r[6] = a2;
	tmp.r[29] = (tmp.r[29] - 0x200) & ~15u;
	tmp.r[31] = 0;
	rt_call(&tmp, addr);
	return tmp.r[2];
}

static void run_interrupts(void) {
	// handlers may queue further interrupts (e.g. the GE resuming after a signal), so loop by index
	for (int i = 0; i < intrq_n; i++) {
		IntrReq q = intrq[i];
		u32 r = 0;
		g_in_intr = true;
		if (q.handler) {
			intr_ctx.r[28] = q.gp;
			intr_ctx.r[29] = intr_stack;
			r = run_guest(&intr_ctx, q.handler, q.a0, q.a1, q.a2);
		}
		g_in_intr = false;
		if (q.done) q.done(q.ud, r);
	}
	intrq_n = 0;
}

typedef struct { u32 handler, arg, gp; bool enabled, registered; } SubIntr;
static SubIntr vblank_subintr[32];

// ---------------------------------------------------------------- callbacks
typedef struct { u32 uid; char name[32]; u32 func, arg; Thread *owner; u32 notify_count, notify_arg; } Callback;
static Callback *cbs[64];
static int ncbs;

void callbacks_check(void) {
	Thread *t = g_cur;
	if (!t) return;
	for (int i = 0; i < ncbs; i++) {
		Callback *cb = cbs[i];
		if (cb && cb->owner == t && cb->notify_count) {
			u32 cnt = cb->notify_count, arg = cb->notify_arg;
			cb->notify_count = 0;
			cb->notify_arg = 0;
			LOGD("running callback '%s'", cb->name);
			u32 r = run_guest(&t->ctx, cb->func, cnt, arg, cb->arg);
			if (r) { /* non-zero return deletes the callback */ }
		}
	}
}

void callback_notify(u32 uid, u32 arg) {
	Callback *cb = obj_get(uid, OBJ_CB);
	if (!cb) return;
	cb->notify_count++;
	cb->notify_arg = arg;
	LOGD("notify callback '%s' arg %x", cb->name, arg);
	Thread *o = cb->owner;
	if (o->status == TS_WAITING && o->cb_wait && !o->cb_wake) {
		o->cb_wake = true;
		o->status = TS_READY;
		o->seq = ++seq_counter;
	}
}

// ---------------------------------------------------------------- scheduler main loop
static void process_timers(u64 n) {
	for (Thread *t = all_threads; t; t = t->next_all) {
		if (t->status == TS_WAITING && t->wake_time && n >= t->wake_time) {
			int wt = t->wait_type;
			write_timeout(t, true);
			t->timeout_ptr = 0;
			t->wake_time = 0;
			t->wait_type = W_NONE;
			t->status = TS_READY;
			t->seq = ++seq_counter;
			t->wait_result = (wt == W_DELAY) ? 0 : (s32)SCE_KERNEL_ERROR_WAIT_TIMEOUT;
		}
	}
}

static u64 next_event(u64 n) {
	u64 e = next_vblank;
	for (Thread *t = all_threads; t; t = t->next_all)
		if (t->status == TS_WAITING && t->wake_time && t->wake_time < e) e = t->wake_time;
	return e > n ? e : n;
}

static void do_vblank(void) {
	g_vblank_count++;
	for (Thread *t = NULL; (t = thread_find_waiting(W_VBLANK, 0, NULL));) thread_wake(t, 0);
	for (int i = 0; i < 32; i++)
		if (vblank_subintr[i].registered && vblank_subintr[i].enabled)
			intr_queue(vblank_subintr[i].handler, i, vblank_subintr[i].arg, 0, vblank_subintr[i].gp);
	ge_vblank();
}

void sched_run(void) {
	next_vblank = now_us() + g_vblank_us;
	u64 idle_since = 0;
	for (;;) {
		u64 n = now_us();
		if (n >= next_vblank) {
			do_vblank();
			next_vblank += g_vblank_us;
			if (next_vblank < n) next_vblank = n + g_vblank_us;
			if (vblank_rephase) { next_vblank = vblank_rephase; vblank_rephase = 0; }
		}
		run_interrupts();
		process_timers(n);
		Thread *t = pick_ready();
		if (!t) {
			bool any = false;
			for (Thread *x = all_threads; x; x = x->next_all) if (x->status == TS_WAITING) any = true;
			if (!any) { LOGI("no threads left, exiting"); exit(0); }
			if (!idle_since) idle_since = n;
			u64 e = next_event(n);
			if (g_virtual) { if (e > g_vtime) g_vtime = e; continue; }
			if (e > n) {
				struct timespec ts = {(e - n) / 1000000, ((e - n) % 1000000) * 1000};
				nanosleep(&ts, NULL);
			}
			continue;
		}
		idle_since = 0;
		if (g_virtual) g_vtime += 2;
		g_cur = t;
		t->status = TS_RUNNING;
		host_ctx_swap(&sched_uc, &t->uc);
		if (t->status == TS_RUNNING) t->status = TS_READY;
		g_cur = NULL;
	}
}

void kernel_wait_vblank(Ctx *c, bool cb) {
	thread_wait(W_VBLANK, 0, 0, cb);
}

u32 cur_module_gp(void) { return g_cur ? g_cur->ctx.r[28] : 0; }

void kernel_init(void) {
	clock_gettime(CLOCK_MONOTONIC, &t0);
	g_virtual = getenv("SR2_DETERMINISTIC") && atoi(getenv("SR2_DETERMINISTIC"));
	mem_init();
	intr_stack = kalloc(0x10000) + 0x10000 - 64;
	vfpu_reset_ctx(&intr_ctx);
}

// ================================================================= HLE: ThreadManForUser
static Thread *thr(u32 uid) { return uid == 0 ? g_cur : obj_get(uid, OBJ_THREAD); }

static void h_CreateThread(Ctx *c) {
	Thread *t = thread_create(Pstr(A(0)), A(1), A(2), A(3), A(4), c->r[28]);
	RET(t->uid);
}
static void h_StartThread(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status != TS_DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	LOGD("start thread '%s' (%x) args %x %x", t->name, t->uid, A(1), A(2));
	thread_start(t, A(1), A(2));
	RET(0);
	thread_resched();
}
static void exit_cur(Ctx *c, s32 status, bool del) {
	Thread *t = g_cur;
	t->exit_status = status;
	t->status = del ? TS_DEAD : TS_DORMANT;
	LOGD("thread '%s' exits (%x)%s", t->name, status, del ? " and deletes itself" : "");
	for (Thread *w = NULL; (w = thread_find_waiting(W_THREADEND, t->uid, w));) thread_wake(w, status);
	if (del) { mem_free_uid(t->stack_uid); obj_free(t->uid); }
	switch_to_sched();
	fatal("exited thread resumed");
}
static void h_ExitThread(Ctx *c) { exit_cur(c, A(0), false); }
static void h_ExitDeleteThread(Ctx *c) { exit_cur(c, A(0), true); }
static void h_DeleteThread(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status != TS_DORMANT) { RET(SCE_KERNEL_ERROR_NOT_DORMANT); return; }
	t->status = TS_DEAD;
	mem_free_uid(t->stack_uid);
	obj_free(t->uid);
	RET(0);
}
static void h_TerminateThread(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	t->status = TS_DORMANT;
	for (Thread *w = NULL; (w = thread_find_waiting(W_THREADEND, t->uid, w));) thread_wake(w, 0x800201AC);
	RET(0);
}
static void h_TerminateDeleteThread(Ctx *c) { h_TerminateThread(c); if (!c->r[2]) { Thread *t = thr(A(0)); t->status = TS_DEAD; mem_free_uid(t->stack_uid); obj_free(t->uid); } }
static void h_GetThreadId(Ctx *c) { RET(g_cur->uid); }
static void h_WaitThreadEnd(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status == TS_DORMANT || t->status == TS_DEAD) { RET(t->exit_status); return; }
	RET(thread_wait(W_THREADEND, t->uid, A(1), false));
}
static void h_ChangeThreadPriority(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	t->prio = A(1) ? A(1) : t->prio;
	RET(0);
	thread_resched();
}
static void h_ChangeCurrentThreadAttr(Ctx *c) { g_cur->attr = (g_cur->attr & ~A(0)) | A(1); RET(0); }
static void h_GetThreadCurrentPriority(Ctx *c) { RET(g_cur->prio); }
static void h_RotateThreadReadyQueue(Ctx *c) { RET(0); thread_yield(); }
static void delay(Ctx *c, u32 us, bool cb) {
	Thread *t = g_cur;
	if (cb) callbacks_check();
	t->status = TS_WAITING;
	t->wait_type = W_DELAY;
	t->wait_id = 0;
	t->timeout_ptr = 0;
	t->wake_time = now_us() + (us < 200 ? 200 : us);
	t->cb_wait = cb;
	wait_loop(t);
	t->cb_wait = false;
	if (cb) callbacks_check();
	RET(0);
}
static void h_DelayThread(Ctx *c) { delay(c, A(0), false); }
static void h_DelayThreadCB(Ctx *c) { delay(c, A(0), true); }
static void sleep_thread(Ctx *c, bool cb) {
	Thread *t = g_cur;
	if (t->wakeup_count) { t->wakeup_count--; RET(0); return; }
	RET(thread_wait(W_SLEEP, 0, 0, cb));
}
static void h_SleepThread(Ctx *c) { sleep_thread(c, false); }
static void h_SleepThreadCB(Ctx *c) { sleep_thread(c, true); }
static void h_WakeupThread(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	if (t->status == TS_WAITING && t->wait_type == W_SLEEP) thread_wake(t, 0);
	else t->wakeup_count++;
	RET(0);
	thread_resched();
}
static void h_ReferThreadStatus(Ctx *c) {
	Thread *t = thr(A(0));
	if (!t) { RET(SCE_KERNEL_ERROR_UNKNOWN_THID); return; }
	u32 p = A(1);
	if (p) {
		u32 sz = LW(p);
		u8 tmp[0x68] = {0};
		memcpy(tmp + 4, t->name, 31);
		u32 st = t->status == TS_RUNNING ? 1 : t->status == TS_READY ? 2 : t->status == TS_WAITING ? 4 : 16;
		u32 vals[] = {t->attr, st, t->entry, t->stack_addr, t->stack_size, t->gp, t->init_prio, t->prio, t->wait_type, t->wait_id, t->wakeup_count, (u32)t->exit_status};
		memcpy(tmp + 0x24, vals, sizeof vals);
		memcpy(PTR(p) + 4, tmp + 4, sz > 0x68 ? 0x64 : sz - 4);
	}
	RET(0);
}
static void h_GetSystemTime(Ctx *c) { u64 n = now_us(); SW(A(0), (u32)n); SW(A(0) + 4, (u32)(n >> 32)); RET(0); }
static void h_GetSystemTimeLow(Ctx *c) { RET((u32)now_us()); }
static void h_GetSystemTimeWide(Ctx *c) { RET64(now_us()); }
static void h_SysClock2USec(Ctx *c) {
	u64 t = A(0) ? ((u64)LW(A(0) + 4) << 32 | LW(A(0))) : 0;
	if (A(1)) SW(A(1), (u32)(t / 1000000));
	if (A(2)) SW(A(2), (u32)(t % 1000000));
	RET(0);
}
static void h_SysClock2USecWide(Ctx *c) {
	u64 t = (u64)A(1) << 32 | A(0);
	if (A(2)) SW(A(2), (u32)(t / 1000000));
	if (A(3)) SW(A(3), (u32)(t % 1000000));
	RET(0);
}
static void h_USec2SysClock(Ctx *c) { if (A(1)) { SW(A(1), A(0)); SW(A(1) + 4, 0); } RET(0); }
static void h_CheckThreadStack(Ctx *c) { RET(c->r[29] - g_cur->stack_addr); }
static void h_GetThreadStackFreeSize(Ctx *c) { RET(g_cur->stack_size / 2); }

// ---- callbacks
static void h_CreateCallback(Ctx *c) {
	Callback *cb = calloc(1, sizeof *cb);
	snprintf(cb->name, sizeof cb->name, "%s", Pstr(A(0)));
	cb->func = A(1); cb->arg = A(2); cb->owner = g_cur;
	cb->uid = obj_new(OBJ_CB, cb);
	if (ncbs < 64) cbs[ncbs++] = cb;
	LOGD("create callback '%s' func %08x", cb->name, cb->func);
	RET(cb->uid);
}
static void h_DeleteCallback(Ctx *c) {
	Callback *cb = obj_get(A(0), OBJ_CB);
	if (!cb) { RET(SCE_KERNEL_ERROR_UNKNOWN_CBID); return; }
	for (int i = 0; i < ncbs; i++) if (cbs[i] == cb) cbs[i] = NULL;
	obj_free(A(0));
	RET(0);
}
static void h_NotifyCallback(Ctx *c) { callback_notify(A(0), A(1)); RET(0); }
static void h_CheckCallback(Ctx *c) { callbacks_check(); RET(0); }

// ---- semaphores
typedef struct { u32 uid; char name[32]; u32 attr; s32 count, max; } Sema;
static void sema_wake(Sema *s) {
	for (;;) {
		Thread *w = thread_find_waiting(W_SEMA, s->uid, NULL);
		if (!w || (s32)w->wait_val > s->count) break;
		s->count -= w->wait_val;
		thread_wake(w, 0);
	}
}
static void h_CreateSema(Ctx *c) {
	Sema *s = calloc(1, sizeof *s);
	snprintf(s->name, sizeof s->name, "%s", Pstr(A(0)));
	s->attr = A(1); s->count = A(2); s->max = A(3);
	s->uid = obj_new(OBJ_SEMA, s);
	LOGD("create sema '%s' %d/%d -> %x", s->name, s->count, s->max, s->uid);
	RET(s->uid);
}
static void h_DeleteSema(Ctx *c) {
	Sema *s = obj_get(A(0), OBJ_SEMA);
	if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	for (Thread *w; (w = thread_find_waiting(W_SEMA, s->uid, NULL));) thread_wake(w, SCE_KERNEL_ERROR_WAIT_DELETE);
	obj_free(A(0)); free(s);
	RET(0);
	thread_resched();
}
static void h_SignalSema(Ctx *c) {
	Sema *s = obj_get(A(0), OBJ_SEMA);
	if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if (s->count + (s32)A(1) > s->max) { RET(SCE_KERNEL_ERROR_SEMA_OVF); return; }
	s->count += A(1);
	sema_wake(s);
	RET(0);
	thread_resched();
}
static void wait_sema(Ctx *c, bool cb) {
	Sema *s = obj_get(A(0), OBJ_SEMA);
	if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	s32 n = A(1);
	if (n <= 0 || n > s->max) { RET(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if (s->count >= n && !thread_find_waiting(W_SEMA, s->uid, NULL)) { s->count -= n; RET(0); if (cb) callbacks_check(); return; }
	g_cur->wait_val = n;
	RET(thread_wait(W_SEMA, s->uid, A(2), cb));
}
static void h_WaitSema(Ctx *c) { wait_sema(c, false); }
static void h_WaitSemaCB(Ctx *c) { wait_sema(c, true); }
static void h_PollSema(Ctx *c) {
	Sema *s = obj_get(A(0), OBJ_SEMA);
	if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	if ((s32)A(1) <= 0) { RET(SCE_KERNEL_ERROR_ILLEGAL_COUNT); return; }
	if (s->count >= (s32)A(1)) { s->count -= A(1); RET(0); } else RET(SCE_KERNEL_ERROR_SEMA_ZERO);
}
static void h_ReferSemaStatus(Ctx *c) {
	Sema *s = obj_get(A(0), OBJ_SEMA);
	if (!s) { RET(SCE_KERNEL_ERROR_UNKNOWN_SEMID); return; }
	u32 p = A(1);
	memcpy(PTR(p + 4), s->name, 32);
	int nw = 0; for (Thread *w = NULL; (w = thread_find_waiting(W_SEMA, s->uid, w));) nw++;
	SW(p + 36, s->attr); SW(p + 40, s->count); SW(p + 44, s->count); SW(p + 48, s->max); SW(p + 52, nw);
	RET(0);
}

// ---- event flags
typedef struct { u32 uid; char name[32]; u32 attr, pattern; } Evf;
static bool evf_match(Evf *e, u32 bits, u32 mode) {
	return (mode & 1) ? (e->pattern & bits) != 0 : (e->pattern & bits) == bits;
}
static void evf_wake(Evf *e) {
	for (Thread *w = NULL; (w = thread_find_waiting(W_EVF, e->uid, w));) {
		if (evf_match(e, w->wait_val, w->wait_val2)) {
			if (w->wait_ptr) SW(w->wait_ptr, e->pattern);
			if (w->wait_val2 & 0x20) e->pattern = 0;
			else if (w->wait_val2 & 0x10) e->pattern &= ~w->wait_val;
			thread_wake(w, 0);
			w = NULL;  // restart scan
		}
	}
}
static void h_CreateEventFlag(Ctx *c) {
	Evf *e = calloc(1, sizeof *e);
	snprintf(e->name, sizeof e->name, "%s", Pstr(A(0)));
	e->attr = A(1); e->pattern = A(2);
	e->uid = obj_new(OBJ_EVF, e);
	RET(e->uid);
}
static void h_DeleteEventFlag(Ctx *c) {
	Evf *e = obj_get(A(0), OBJ_EVF);
	if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	for (Thread *w; (w = thread_find_waiting(W_EVF, e->uid, NULL));) thread_wake(w, SCE_KERNEL_ERROR_WAIT_DELETE);
	obj_free(A(0)); free(e);
	RET(0);
}
static void h_SetEventFlag(Ctx *c) {
	Evf *e = obj_get(A(0), OBJ_EVF);
	if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	e->pattern |= A(1);
	evf_wake(e);
	RET(0);
	thread_resched();
}
static void h_ClearEventFlag(Ctx *c) {
	Evf *e = obj_get(A(0), OBJ_EVF);
	if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	e->pattern &= A(1);
	RET(0);
}
static void wait_evf(Ctx *c, bool cb, bool poll) {
	Evf *e = obj_get(A(0), OBJ_EVF);
	if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	u32 bits = A(1), mode = A(2), outp = A(3);
	if (!bits) { RET(SCE_KERNEL_ERROR_EVF_ILPAT); return; }
	if (evf_match(e, bits, mode)) {
		if (outp) SW(outp, e->pattern);
		if (mode & 0x20) e->pattern = 0;
		else if (mode & 0x10) e->pattern &= ~bits;
		RET(0);
		return;
	}
	if (poll) { if (outp) SW(outp, e->pattern); RET(SCE_KERNEL_ERROR_EVF_COND); return; }
	g_cur->wait_val = bits; g_cur->wait_val2 = mode; g_cur->wait_ptr = outp;
	RET(thread_wait(W_EVF, e->uid, A(4), cb));
}
static void h_WaitEventFlag(Ctx *c) { wait_evf(c, false, false); }
static void h_WaitEventFlagCB(Ctx *c) { wait_evf(c, true, false); }
static void h_PollEventFlag(Ctx *c) { wait_evf(c, false, true); }
static void h_ReferEventFlagStatus(Ctx *c) {
	Evf *e = obj_get(A(0), OBJ_EVF);
	if (!e) { RET(SCE_KERNEL_ERROR_UNKNOWN_EVFID); return; }
	u32 p = A(1);
	memcpy(PTR(p + 4), e->name, 32);
	int nw = 0; for (Thread *w = NULL; (w = thread_find_waiting(W_EVF, e->uid, w));) nw++;
	SW(p + 36, e->attr); SW(p + 40, e->pattern); SW(p + 44, e->pattern); SW(p + 48, nw);
	RET(0);
}

// ---- message boxes
// The queue is kept on the host side. (The real kernel links messages through their first word; the game
// reuses overlapping static message buffers, so a link can be overwritten while the message is still queued.
// Walking guest links then loops forever, e.g. when a level exits. A host queue is immune to that.)
typedef struct { u32 uid; char name[32]; u32 attr; u32 *q; int count, cap; } Mbx;
static void h_CreateMbx(Ctx *c) {
	Mbx *m = calloc(1, sizeof *m);
	snprintf(m->name, sizeof m->name, "%s", Pstr(A(0)));
	m->attr = A(1);
	m->uid = obj_new(OBJ_MBX, m);
	LOGD("create mbx '%s' -> %x", m->name, m->uid);
	RET(m->uid);
}
static void h_DeleteMbx(Ctx *c) {
	Mbx *m = obj_get(A(0), OBJ_MBX);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
	for (Thread *w; (w = thread_find_waiting(W_MBX, m->uid, NULL));) thread_wake(w, SCE_KERNEL_ERROR_WAIT_DELETE);
	obj_free(A(0)); free(m->q); free(m);
	RET(0);
}
static void h_SendMbx(Ctx *c) {
	Mbx *m = obj_get(A(0), OBJ_MBX);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
	u32 msg = A(1);
	Thread *w = thread_find_waiting(W_MBX, m->uid, NULL);
	if (w) {
		if (w->wait_ptr) SW(w->wait_ptr, msg);
		thread_wake(w, 0);
	} else {
		SW(msg, 0);
		if (m->count == m->cap) { m->cap = m->cap ? m->cap * 2 : 16; m->q = realloc(m->q, m->cap * sizeof *m->q); }
		int at = m->count;
		if (m->attr & 0x400)   // priority ordered: byte 4 is the priority (lower first, FIFO among equals)
			while (at > 0 && LBU(m->q[at - 1] + 4) > LBU(msg + 4)) at--;
		memmove(m->q + at + 1, m->q + at, (m->count - at) * sizeof *m->q);
		m->q[at] = msg;
		m->count++;
	}
	RET(0);
	thread_resched();
}
static void recv_mbx(Ctx *c, bool cb, bool poll) {
	Mbx *m = obj_get(A(0), OBJ_MBX);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MBXID); return; }
	if (m->count) {
		u32 msg = m->q[0];
		m->count--;
		memmove(m->q, m->q + 1, m->count * sizeof *m->q);
		SW(A(1), msg);
		RET(0);
		return;
	}
	if (poll) { RET(SCE_KERNEL_ERROR_MBOX_NOMSG); return; }
	g_cur->wait_ptr = A(1);
	RET(thread_wait(W_MBX, m->uid, A(2), cb));
}
static void h_ReceiveMbx(Ctx *c) { recv_mbx(c, false, false); }
static void h_ReceiveMbxCB(Ctx *c) { recv_mbx(c, true, false); }
static void h_PollMbx(Ctx *c) { recv_mbx(c, false, true); }

// ---- mutexes
typedef struct { u32 uid; char name[32]; u32 attr; s32 count; Thread *owner; } Mutex;
static void h_CreateMutex(Ctx *c) {
	Mutex *m = calloc(1, sizeof *m);
	snprintf(m->name, sizeof m->name, "%s", Pstr(A(0)));
	m->attr = A(1); m->count = A(2);
	if (m->count) m->owner = g_cur;
	m->uid = obj_new(OBJ_MUTEX, m);
	RET(m->uid);
}
static void h_DeleteMutex(Ctx *c) {
	Mutex *m = obj_get(A(0), OBJ_MUTEX);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MUTEXID); return; }
	for (Thread *w; (w = thread_find_waiting(W_MUTEX, m->uid, NULL));) thread_wake(w, SCE_KERNEL_ERROR_WAIT_DELETE);
	obj_free(A(0)); free(m);
	RET(0);
}
static void lock_mutex(Ctx *c, bool cb, bool try_) {
	Mutex *m = obj_get(A(0), OBJ_MUTEX);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MUTEXID); return; }
	s32 n = A(1);
	if (m->count == 0) { m->count = n; m->owner = g_cur; RET(0); return; }
	if (m->owner == g_cur) {
		if (!(m->attr & 0x200)) { RET(SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED); return; }
		m->count += n; RET(0); return;
	}
	if (try_) { RET(SCE_KERNEL_ERROR_MUTEX_LOCKED); return; }
	g_cur->wait_val = n;
	RET(thread_wait(W_MUTEX, m->uid, A(2), cb));
}
static void h_LockMutex(Ctx *c) { lock_mutex(c, false, false); }
static void h_LockMutexCB(Ctx *c) { lock_mutex(c, true, false); }
static void h_TryLockMutex(Ctx *c) { lock_mutex(c, false, true); }
static void h_UnlockMutex(Ctx *c) {
	Mutex *m = obj_get(A(0), OBJ_MUTEX);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MUTEXID); return; }
	if (m->count == 0 || m->owner != g_cur) { RET(SCE_KERNEL_ERROR_MUTEX_UNLOCKED); return; }
	m->count -= A(1);
	if (m->count < 0) { m->count = 0; RET(SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW); return; }
	if (m->count == 0) {
		m->owner = NULL;
		Thread *w = thread_find_waiting(W_MUTEX, m->uid, NULL);
		if (w) { m->owner = w; m->count = w->wait_val; thread_wake(w, 0); }
	}
	RET(0);
	thread_resched();
}

// ---- fixed-size pools
typedef struct { u32 uid; char name[32]; u32 attr, blocksize, nblocks, base, mem_uid; u8 *used; } Fpl;
static void h_CreateFpl(Ctx *c) {
	Fpl *f = calloc(1, sizeof *f);
	snprintf(f->name, sizeof f->name, "%s", Pstr(A(0)));
	f->attr = A(2); f->blocksize = (A(3) + 3) & ~3u; f->nblocks = A(4);
	u32 align = 4;
	if (A(5) && LW(A(5)) >= 8) align = LW(A(5) + 4);
	f->blocksize = (A(3) + align - 1) & ~(align - 1);
	f->base = mem_alloc(A(1), f->name, (f->attr & 0x4000) ? 1 : 0, f->blocksize * f->nblocks, 0, align, &f->mem_uid);
	if (!f->base) { free(f); RET(SCE_KERNEL_ERROR_NO_MEMORY); return; }
	f->used = calloc(f->nblocks, 1);
	f->uid = obj_new(OBJ_FPL, f);
	LOGD("create fpl '%s' %u x %u at %08x", f->name, f->nblocks, f->blocksize, f->base);
	if (getenv("SR2_MEMMAP") && !strcmp(f->name, "HeapFPL1")) {
		for (Block *b = user_blocks; b; b = b->next)
			LOGI("memmap %08x-%08x %7x %s %s", b->addr, b->addr + b->size, b->size, b->used ? "USED" : "free", b->name);
	}
	RET(f->uid);
}
static void h_DeleteFpl(Ctx *c) {
	Fpl *f = obj_get(A(0), OBJ_FPL);
	if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	mem_free_uid(f->mem_uid);
	obj_free(A(0)); free(f->used); free(f);
	RET(0);
}
static int fpl_take(Fpl *f) {
	for (u32 i = 0; i < f->nblocks; i++) if (!f->used[i]) { f->used[i] = 1; return i; }
	return -1;
}
static void alloc_fpl(Ctx *c, bool cb, bool try_) {
	Fpl *f = obj_get(A(0), OBJ_FPL);
	if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	int i = fpl_take(f);
	if (i >= 0) { SW(A(1), f->base + i * f->blocksize); RET(0); return; }
	if (try_) { RET(0x800201AD + 0x9); return; }  // SCE_KERNEL_ERROR_NO_MEMORY-ish for FPL
	g_cur->wait_ptr = A(1);
	RET(thread_wait(W_FPL, f->uid, A(2), cb));
}
static void h_AllocateFpl(Ctx *c) { alloc_fpl(c, false, false); }
static void h_AllocateFplCB(Ctx *c) { alloc_fpl(c, true, false); }
static void h_TryAllocateFpl(Ctx *c) { alloc_fpl(c, false, true); }
static void h_FreeFpl(Ctx *c) {
	Fpl *f = obj_get(A(0), OBJ_FPL);
	if (!f) { RET(SCE_KERNEL_ERROR_UNKNOWN_FPLID); return; }
	u32 i = (A(1) - f->base) / f->blocksize;
	if (A(1) < f->base || i >= f->nblocks || !f->used[i]) { RET(SCE_KERNEL_ERROR_ILLEGAL_ADDR); return; }
	f->used[i] = 0;
	Thread *w = thread_find_waiting(W_FPL, f->uid, NULL);
	if (w) { int k = fpl_take(f); SW(w->wait_ptr, f->base + k * f->blocksize); thread_wake(w, 0); }
	RET(0);
	thread_resched();
}

// ---- interrupts
static void h_CpuSuspendIntr(Ctx *c) { RET(c->intr); c->intr = 0; }
static void h_CpuResumeIntr(Ctx *c) { c->intr = A(0); RET(0); }
static void h_RegisterSubIntrHandler(Ctx *c) {
	u32 intno = A(0), sub = A(1);
	LOGD("register sub intr %u/%u handler %08x arg %x", intno, sub, A(2), A(3));
	if (intno == 30 && sub < 32) {
		vblank_subintr[sub].handler = A(2); vblank_subintr[sub].arg = A(3);
		vblank_subintr[sub].gp = c->r[28]; vblank_subintr[sub].registered = true;
	}
	RET(0);
}
static void h_ReleaseSubIntrHandler(Ctx *c) { if (A(0) == 30 && A(1) < 32) vblank_subintr[A(1)].registered = false; RET(0); }
static void h_EnableSubIntr(Ctx *c) { if (A(0) == 30 && A(1) < 32) vblank_subintr[A(1)].enabled = true; RET(0); }
static void h_DisableSubIntr(Ctx *c) { if (A(0) == 30 && A(1) < 32) vblank_subintr[A(1)].enabled = false; RET(0); }

// ---- SysMemUserForUser
static void h_AllocPartitionMemory(Ctx *c) {
	u32 uid;
	u32 a = mem_alloc(A(0), Pstr(A(1)), A(2), A(3), A(4), 256, &uid);
	LOGD("AllocPartitionMemory(%d, '%s', type %d, %x) -> %08x", A(0), Pstr(A(1)), A(2), A(3), a);
	RET(a ? uid : SCE_KERNEL_ERROR_NO_MEMORY);
}
static void h_FreePartitionMemory(Ctx *c) { mem_free_uid(A(0)); RET(0); }
static void h_GetBlockHeadAddr(Ctx *c) { RET(mem_block_addr(A(0))); }
static void h_MaxFreeMemSize(Ctx *c) { RET(mem_max_free()); }
static void h_TotalFreeMemSize(Ctx *c) { RET(mem_total_free()); }
static void h_Ok(Ctx *c) { RET(0); }
u32 g_sdk_version;   // the SDK the game was built with (the save data encryption depends on it)
static void h_SetCompiledSdkVersion(Ctx *c) { g_sdk_version = A(0); RET(0); }
// sceDmacMemcpy(dst, src, size): Sonic Rivals (1) copies its menu textures with it
static void h_DmacMemcpy(Ctx *c) { if (A(2)) memmove(PTR(A(0)), PTR(A(1)), A(2)); RET(0); }
static void h_DevkitVersion(Ctx *c) { RET(0x02070110); }

// ---- module manager
static void h_LoadModule(Ctx *c) {
	s32 err = 0;
	LOGI("sceKernelLoadModule(%s)", Pstr(A(0)));
	LoadedModule *m = module_load(Pstr(A(0)), &err);
	RET(m ? m->uid : (u32)err);
}
static void h_LoadModuleByID(Ctx *c) {
	s32 err = SCE_KERNEL_ERROR_UNKNOWN_MODULE;
	const char *path = io_fd_path(A(0));
	LOGI("sceKernelLoadModuleByID(fd %d: %s)", A(0), path ? path : "?");
	LoadedModule *m = path ? module_load(path, &err) : NULL;
	RET(m ? m->uid : (u32)err);
}
// The kernel deletes the threads it runs module_start/module_stop on once they return. Keeping them (and
// their 256 KB stacks) leaked user memory on every level change, until the later, bigger levels ran out of
// heap: their per-frame skinning buffers failed to allocate (flickering, stretched or missing models).
static void module_thread_done(Thread *t) {
	if (t->status != TS_DORMANT) return;   // still running (module_start that doesn't return): keep it
	t->status = TS_DEAD;
	mem_free_uid(t->stack_uid);
	obj_free(t->uid);
}
static void h_StartModule(Ctx *c) {
	LoadedModule *m = obj_get(A(0), OBJ_MODULE);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	LOGI("sceKernelStartModule('%s') entry %08x prio %x stack %x", m->name, m->entry, m->start_prio, m->start_stack);
	u32 status_ptr = A(3);
	Thread *t = thread_create(m->name, m->entry, m->start_prio ? m->start_prio : g_cur->prio, m->start_stack ? m->start_stack : 0x40000, m->start_attr, m->gp);
	thread_start(t, A(1), A(2));
	s32 r = thread_wait(W_THREADEND, t->uid, 0, false);
	LOGI("module_start of '%s' returned %x", m->name, r);
	module_thread_done(t);
	if (status_ptr) SW(status_ptr, r);
	RET(m->uid);
}
static void h_StopModule(Ctx *c) {
	LoadedModule *m = obj_get(A(0), OBJ_MODULE);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	LOGI("sceKernelStopModule('%s')", m->name);
	if (m->stop) {
		Thread *t = thread_create(m->name, m->stop, 0x20, 0x40000, 0, m->gp);
		thread_start(t, A(1), A(2));
		s32 r = thread_wait(W_THREADEND, t->uid, 0, false);
		module_thread_done(t);
		if (A(3)) SW(A(3), r);
	}
	RET(0);
}
static void h_UnloadModule(Ctx *c) {
	LoadedModule *m = obj_get(A(0), OBJ_MODULE);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	LOGI("sceKernelUnloadModule('%s')", m->name);
	for (LoadedModule **pp = &g_modules; *pp; pp = &(*pp)->next) if (*pp == m) { *pp = m->next; break; }
	mem_free_uid(m->mem_uid);
	obj_free(m->uid);
	RET(0);
}
static void h_StopUnloadSelfModuleWithStatus(Ctx *c) {
	LoadedModule *m = module_by_addr(c->r[31]);
	LOGI("sceKernelStopUnloadSelfModuleWithStatus(%x) from '%s'", A(0), m ? m->name : "?");
	if (m) {
		for (LoadedModule **pp = &g_modules; *pp; pp = &(*pp)->next) if (*pp == m) { *pp = m->next; break; }
		mem_free_uid(m->mem_uid);
		obj_free(m->uid);
	}
	exit_cur(c, A(0), true);
}
static void h_GetModuleIdByAddress(Ctx *c) { LoadedModule *m = module_by_addr(A(0)); RET(m ? m->uid : SCE_KERNEL_ERROR_UNKNOWN_MODULE); }
static void h_GetModuleId(Ctx *c) { LoadedModule *m = module_by_addr(c->r[31]); RET(m ? m->uid : SCE_KERNEL_ERROR_UNKNOWN_MODULE); }
static void h_GetModuleIdList(Ctx *c) {
	u32 n = 0;
	for (LoadedModule *m = g_modules; m; m = m->next) { if (n * 4 < A(1)) SW(A(0) + n * 4, m->uid); n++; }
	SW(A(2), n);
	RET(0);
}
static void h_QueryModuleInfo(Ctx *c) {
	LoadedModule *m = obj_get(A(0), OBJ_MODULE);
	if (!m) { RET(SCE_KERNEL_ERROR_UNKNOWN_MODULE); return; }
	u32 p = A(1);
	// SceKernelModuleInfo: size, nsegment, reserved[3], segmentaddr[4], segmentsize[4], entry_addr, gp_value, text_addr, text_size, data_size, bss_size, attribute, version[2], name[28]
	SW(p + 4, 0); SB(p + 4, 1);
	SW(p + 8, m->base); SW(p + 24, m->size);
	SW(p + 40, m->entry); SW(p + 44, m->gp); SW(p + 48, m->base); SW(p + 52, m->size);
	memcpy(PTR(p + 68), m->name, 28);
	RET(0);
}

static void h_ExitGame(Ctx *c) { LOGI("sceKernelExitGame"); fflush(stderr); exit(0); }
static void h_RegisterExitCallback(Ctx *c) { RET(0); }

static const HleEnt kernel_ents[] = {
	{0x446D8DE6, "sceKernelCreateThread", h_CreateThread},
	{0xF475845D, "sceKernelStartThread", h_StartThread},
	{0xAA73C935, "sceKernelExitThread", h_ExitThread},
	{0x809CE29B, "sceKernelExitDeleteThread", h_ExitDeleteThread},
	{0x9FA03CD3, "sceKernelDeleteThread", h_DeleteThread},
	{0x616403BA, "sceKernelTerminateThread", h_TerminateThread},
	{0x383F7BCC, "sceKernelTerminateDeleteThread", h_TerminateDeleteThread},
	{0x293B45B8, "sceKernelGetThreadId", h_GetThreadId},
	{0x278C0DF5, "sceKernelWaitThreadEnd", h_WaitThreadEnd},
	{0x71BC9871, "sceKernelChangeThreadPriority", h_ChangeThreadPriority},
	{0xEA748E31, "sceKernelChangeCurrentThreadAttr", h_ChangeCurrentThreadAttr},
	{0x94AA61EE, "sceKernelGetThreadCurrentPriority", h_GetThreadCurrentPriority},
	{0x912354A7, "sceKernelRotateThreadReadyQueue", h_RotateThreadReadyQueue},
	{0xCEADEB47, "sceKernelDelayThread", h_DelayThread},
	{0x68DA9E36, "sceKernelDelayThreadCB", h_DelayThreadCB},
	{0x9ACE131E, "sceKernelSleepThread", h_SleepThread},
	{0x82826F70, "sceKernelSleepThreadCB", h_SleepThreadCB},
	{0xD59EAD2F, "sceKernelWakeupThread", h_WakeupThread},
	{0x17C1684E, "sceKernelReferThreadStatus", h_ReferThreadStatus},
	{0xDB738F35, "sceKernelGetSystemTime", h_GetSystemTime},
	{0x82BC5777, "sceKernelGetSystemTimeWide", h_GetSystemTimeWide},
	{0xBA6B92E2, "sceKernelSysClock2USec", h_SysClock2USec},
	{0xE1619D7C, "sceKernelSysClock2USecWide", h_SysClock2USecWide},
	{0x110DEC9A, "sceKernelUSec2SysClock", h_USec2SysClock},
	{0x369ED59D, "sceKernelGetSystemTimeLow", h_GetSystemTimeLow},
	{0xD13BDE95, "sceKernelCheckThreadStack", h_CheckThreadStack},
	{0x52089CA1, "sceKernelGetThreadStackFreeSize", h_GetThreadStackFreeSize},
	{0xE81CAF8F, "sceKernelCreateCallback", h_CreateCallback},
	{0xEDBA5844, "sceKernelDeleteCallback", h_DeleteCallback},
	{0xC11BA8C4, "sceKernelNotifyCallback", h_NotifyCallback},
	{0x349D6D6C, "sceKernelCheckCallback", h_CheckCallback},
	{0xD6DA4BA1, "sceKernelCreateSema", h_CreateSema},
	{0x28B6489C, "sceKernelDeleteSema", h_DeleteSema},
	{0x3F53E640, "sceKernelSignalSema", h_SignalSema},
	{0x4E3A1105, "sceKernelWaitSema", h_WaitSema},
	{0x6D212BAC, "sceKernelWaitSemaCB", h_WaitSemaCB},
	{0x58B1F937, "sceKernelPollSema", h_PollSema},
	{0xBC6FEBC5, "sceKernelReferSemaStatus", h_ReferSemaStatus},
	{0x55C20A00, "sceKernelCreateEventFlag", h_CreateEventFlag},
	{0xEF9E4C70, "sceKernelDeleteEventFlag", h_DeleteEventFlag},
	{0x1FB15A32, "sceKernelSetEventFlag", h_SetEventFlag},
	{0x812346E4, "sceKernelClearEventFlag", h_ClearEventFlag},
	{0x402FCF22, "sceKernelWaitEventFlag", h_WaitEventFlag},
	{0x328C546A, "sceKernelWaitEventFlagCB", h_WaitEventFlagCB},
	{0x30FD48F0, "sceKernelPollEventFlag", h_PollEventFlag},
	{0xA66B0120, "sceKernelReferEventFlagStatus", h_ReferEventFlagStatus},
	{0x8125221D, "sceKernelCreateMbx", h_CreateMbx},
	{0x86255ADA, "sceKernelDeleteMbx", h_DeleteMbx},
	{0xE9B3061E, "sceKernelSendMbx", h_SendMbx},
	{0x18260574, "sceKernelReceiveMbx", h_ReceiveMbx},
	{0xF3986382, "sceKernelReceiveMbxCB", h_ReceiveMbxCB},
	{0x0D81716A, "sceKernelPollMbx", h_PollMbx},
	{0xB7D098C6, "sceKernelCreateMutex", h_CreateMutex},
	{0xF8170FBE, "sceKernelDeleteMutex", h_DeleteMutex},
	{0xB011B11F, "sceKernelLockMutex", h_LockMutex},
	{0x5BF4DD27, "sceKernelLockMutexCB", h_LockMutexCB},
	{0x0DDCD2C9, "sceKernelTryLockMutex", h_TryLockMutex},
	{0x6B30100F, "sceKernelUnlockMutex", h_UnlockMutex},
	{0xC07BB470, "sceKernelCreateFpl", h_CreateFpl},
	{0xED1410E0, "sceKernelDeleteFpl", h_DeleteFpl},
	{0xD979E9BF, "sceKernelAllocateFpl", h_AllocateFpl},
	{0xE7282CB6, "sceKernelAllocateFplCB", h_AllocateFplCB},
	{0x623AE665, "sceKernelTryAllocateFpl", h_TryAllocateFpl},
	{0xF6414A71, "sceKernelFreeFpl", h_FreeFpl},
	{0x092968F4, "sceKernelCpuSuspendIntr", h_CpuSuspendIntr},
	{0x5F10D406, "sceKernelCpuResumeIntr", h_CpuResumeIntr},
	{0xCA04A2B9, "sceKernelRegisterSubIntrHandler", h_RegisterSubIntrHandler},
	{0xD61E6961, "sceKernelReleaseSubIntrHandler", h_ReleaseSubIntrHandler},
	{0xFB8E22EC, "sceKernelEnableSubIntr", h_EnableSubIntr},
	{0x8A389411, "sceKernelDisableSubIntr", h_DisableSubIntr},
	{0x237DBD4F, "sceKernelAllocPartitionMemory", h_AllocPartitionMemory},
	{0xB6D61D02, "sceKernelFreePartitionMemory", h_FreePartitionMemory},
	{0x9D9A5BA1, "sceKernelGetBlockHeadAddr", h_GetBlockHeadAddr},
	{0xA291F107, "sceKernelMaxFreeMemSize", h_MaxFreeMemSize},
	{0xF919F628, "sceKernelTotalFreeMemSize", h_TotalFreeMemSize},
	{0x7591C7DB, "sceKernelSetCompiledSdkVersion", h_SetCompiledSdkVersion},
	{0xF77D77CB, "sceKernelSetCompilerVersion", h_Ok},
	{0x3FC9AE6A, "sceKernelDevkitVersion", h_DevkitVersion},
	{0x977DE386, "sceKernelLoadModule", h_LoadModule},
	{0xB7F46618, "sceKernelLoadModuleByID", h_LoadModuleByID},
	{0x50F0C1EC, "sceKernelStartModule", h_StartModule},
	{0xD1FF982A, "sceKernelStopModule", h_StopModule},
	{0x2E0911AA, "sceKernelUnloadModule", h_UnloadModule},
	{0x8F2DF740, "sceKernelStopUnloadSelfModuleWithStatus", h_StopUnloadSelfModuleWithStatus},
	{0xD8B73127, "sceKernelGetModuleIdByAddress", h_GetModuleIdByAddress},
	{0xF0A26395, "sceKernelGetModuleId", h_GetModuleId},
	{0x644395E2, "sceKernelGetModuleIdList", h_GetModuleIdList},
	{0x748CBED9, "sceKernelQueryModuleInfo", h_QueryModuleInfo},
	{0x05572A5F, "sceKernelExitGame", h_ExitGame},
	{0x4AC57943, "sceKernelRegisterExitCallback", h_RegisterExitCallback},
	{0x3EE30821, "sceKernelDcacheWritebackRange", h_Ok},
	{0xBFA98062, "sceKernelDcacheInvalidateRange", h_Ok},
	{0x79D1C3FA, "sceKernelDcacheWritebackAll", h_Ok},
	{0xB435DEC5, "sceKernelDcacheWritebackInvalidateAll", h_Ok},
	{0x34B9FA9E, "sceKernelDcacheWritebackInvalidateRange", h_Ok},
	{0x6AD345D7, "sceKernelSetGPO", h_Ok},
	{0x617F3FE6, "sceDmacMemcpy", h_DmacMemcpy},
	{0xD97F94D8, "sceDmacTryMemcpy", h_DmacMemcpy},
};

void kernel_register_hle(void) { hle_register(kernel_ents, sizeof kernel_ents / sizeof kernel_ents[0]); }
