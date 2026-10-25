#pragma once
#include "rt.h"
#ifdef __cplusplus
extern "C" {
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "platform.h"

// ---------------------------------------------------------------- logging
extern int g_loglevel;
void logf_(int lvl, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) logf_(0, __VA_ARGS__)
#define LOGI(...) logf_(1, __VA_ARGS__)
#define LOGD(...) logf_(2, __VA_ARGS__)
#define LOGV(...) logf_(3, __VA_ARGS__)
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));

// ---------------------------------------------------------------- HLE helpers
#define A(n) (c->r[4 + (n)])
#define RET(v) (c->r[2] = (u32)(v))
#define RET64(v) do { u64 v_ = (u64)(v); c->r[2] = (u32)v_; c->r[3] = (u32)(v_ >> 32); } while (0)
#define RETF(v) (c->f[0] = (float)(v))
typedef void (*HleFn)(Ctx *c);
typedef struct HleEnt { u32 nid; const char *name; HleFn fn; } HleEnt;
void hle_register(const HleEnt *ents, int n);
HleFn hle_lookup(u32 nid);   // the registered handler for a NID, or NULL
const char *nid_name(u32 nid);
static inline char *Pstr(u32 a) { return (char *)PTR(a); }

// ---------------------------------------------------------------- errors
#define SCE_KERNEL_ERROR_ILLEGAL_ADDR        0x800200D3
#define SCE_KERNEL_ERROR_NO_MEMORY           0x80020190
#define SCE_KERNEL_ERROR_ILLEGAL_PRIORITY    0x80020193
#define SCE_KERNEL_ERROR_UNKNOWN_UID         0x800200CB
#define SCE_KERNEL_ERROR_UNKNOWN_THID        0x80020198
#define SCE_KERNEL_ERROR_UNKNOWN_SEMID       0x80020199
#define SCE_KERNEL_ERROR_UNKNOWN_EVFID       0x8002019A
#define SCE_KERNEL_ERROR_UNKNOWN_MBXID       0x8002019B
#define SCE_KERNEL_ERROR_UNKNOWN_FPLID       0x8002019E
#define SCE_KERNEL_ERROR_UNKNOWN_CBID        0x800201A1
#define SCE_KERNEL_ERROR_NOT_DORMANT         0x800201A4
#define SCE_KERNEL_ERROR_DORMANT             0x800201A2
#define SCE_KERNEL_ERROR_WAIT_TIMEOUT        0x800201A8
#define SCE_KERNEL_ERROR_WAIT_CANCEL         0x800201A9
#define SCE_KERNEL_ERROR_WAIT_DELETE         0x800201B5
#define SCE_KERNEL_ERROR_SEMA_ZERO           0x800201AD
#define SCE_KERNEL_ERROR_SEMA_OVF            0x800201AE
#define SCE_KERNEL_ERROR_EVF_COND            0x800201AF
#define SCE_KERNEL_ERROR_EVF_MULTI           0x800201B0
#define SCE_KERNEL_ERROR_EVF_ILPAT           0x800201B1
#define SCE_KERNEL_ERROR_MBOX_NOMSG          0x800201B2
#define SCE_KERNEL_ERROR_NO_FREE_FPL_MEM     0x800201B7  /* placeholder; real: TRY_ALLOC fails */
#define SCE_KERNEL_ERROR_ILLEGAL_COUNT       0x800201BD
#define SCE_KERNEL_ERROR_MUTEX_LOCKED        0x800201C4
#define SCE_KERNEL_ERROR_MUTEX_UNLOCKED      0x800201C5
#define SCE_KERNEL_ERROR_MUTEX_LOCK_OVERFLOW 0x800201C6
#define SCE_KERNEL_ERROR_MUTEX_UNLOCK_UNDERFLOW 0x800201C7
#define SCE_KERNEL_ERROR_MUTEX_RECURSIVE_NOT_ALLOWED 0x800201C8
#define SCE_KERNEL_ERROR_UNKNOWN_MUTEXID     0x800201C3
#define SCE_KERNEL_ERROR_UNKNOWN_MODULE      0x8002012E
#define SCE_ERROR_FILE_NOT_FOUND             0x80010002

// ---------------------------------------------------------------- objects
enum ObjType { OBJ_NONE, OBJ_THREAD, OBJ_SEMA, OBJ_EVF, OBJ_MBX, OBJ_MUTEX, OBJ_FPL, OBJ_CB, OBJ_MEMBLOCK, OBJ_MODULE, OBJ_VPL, OBJ_ALARM };
void *obj_get(u32 uid, int type);
u32 obj_new(int type, void *p);
void obj_free(u32 uid);

// ---------------------------------------------------------------- memory
u32 mem_alloc(int part, const char *name, int type, u32 size, u32 addr, u32 align, u32 *uid_out);
void mem_free_uid(u32 uid);
u32 mem_block_addr(u32 uid);
u32 mem_block_by_name(const char *name, u32 *size);   // a used user-memory block by its allocation name
u32 mem_max_free(void);
u32 mem_total_free(void);
u32 kalloc(u32 size);  // permanent kernel-side scratch allocations in PSP memory

// ---------------------------------------------------------------- threads
enum { TS_RUNNING = 1, TS_READY = 2, TS_WAITING = 4, TS_SUSPEND = 8, TS_DORMANT = 16, TS_DEAD = 32 };
enum { W_NONE, W_DELAY, W_SLEEP, W_SEMA, W_EVF, W_MBX, W_MUTEX, W_FPL, W_THREADEND, W_VBLANK, W_IO, W_GE, W_AUDIO, W_MODULE };

typedef struct Thread {
	u32 uid;
	char name[32];
	u32 entry, prio, init_prio, stack_size, stack_addr, stack_uid, attr, gp;
	int status;
	Ctx ctx;
	host_ctx uc;
	void *hoststack;
	int wait_type;
	u32 wait_id;
	u64 wake_time;      // 0 = no timeout
	u32 timeout_ptr;
	u32 wait_val, wait_val2, wait_ptr;
	s32 wait_result;
	s32 exit_status;
	bool cb_wait;       // in a CB-enabled wait
	bool cb_wake;       // resumed only to run callbacks, then keeps waiting
	u64 seq;
	u32 wakeup_count;
	void (*host_entry)(struct Thread *);   // runtime-owned thread: runs this host function instead of guest code
	struct Thread *next_all;
} Thread;

extern Thread *g_cur;
u64 now_us(void);
void kernel_vblank_rephase(u64 at);
extern u32 g_vblank_us;   // emulated vblank period
void kernel_add_pause_time(u64 us);
bool kernel_is_virtual_time(void);
void kernel_init(void);
Thread *thread_create(const char *name, u32 entry, u32 prio, u32 stacksize, u32 attr, u32 gp);
void thread_start(Thread *t, u32 argsize, u32 argp);
s32 thread_wait(int wait_type, u32 id, u32 timeout_ptr, bool cb);  // blocks current thread
void thread_wake(Thread *t, s32 result);
void thread_resched(void);
void thread_delay_until(u64 when);                      // yield if a higher priority thread is ready
Thread *thread_find_waiting(int wait_type, u32 id, Thread *after);
void sched_run(void) __attribute__((noreturn));
u32 cur_module_gp(void);
void kernel_wait_vblank(Ctx *c, bool cb);
extern u64 g_vblank_count;
u32 run_guest(Ctx *tmpl, u32 addr, u32 a0, u32 a1, u32 a2);  // call guest code on a scratch copy of tmpl (sp lowered)
void intr_queue(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp);
// handler may be 0 (only `done` runs). `done(ud, v0)` runs in the scheduler after the handler returns.
void intr_queue_ex(u32 handler, u32 a0, u32 a1, u32 a2, u32 gp, void (*done)(void *, u32), void *ud);  // run handler from the scheduler (interrupt context)
extern bool g_in_intr;

void callbacks_check(void);
void callback_notify(u32 uid, u32 arg);

// ---------------------------------------------------------------- modules
typedef struct LoadedModule {
	u32 uid;
	const RecompModule *rm;
	char name[32];
	char path[256];
	u32 base, size, mem_uid;
	u32 entry, stop, gp;
	u32 start_prio, start_stack, start_attr;
	RecompFn *fntab;   // indexed by (off - text_start) / 4
	u32 text_start, text_end;
	u32 stub_lo, stub_hi; // import stubs (relocated)
	u32 *stub_nids;
	u32 *exp_nids, *exp_addrs; // library function exports (other modules' imports resolve to these)
	u32 nexp;
	u32 *expv_nids, *expv_addrs; // library variable exports
	u32 nexpv;
	int status;
	struct LoadedModule *next;
} LoadedModule;
extern LoadedModule *g_modules;
LoadedModule *module_load(const char *psp_path, s32 *err);
LoadedModule *module_by_addr(u32 addr);
void modules_register_all(void);
u32 module_start_thread(LoadedModule *m, u32 argsize, u32 argp);

// ---------------------------------------------------------------- io
int io_host_path(const char *psp, char *out, size_t outsz);
int io_game_path(const char *psp, char *out, size_t outsz);   // the game's own file, never a mod's
const char *io_fd_path(u32 fd);
extern char g_game_root[512];
extern char g_save_root[512];
extern bool g_start_2p;
void io_init(void);
void ge_init(void);
void display_init(void);
void hle_misc_init(void);
void ge_vblank(void);

#ifdef __cplusplus
}
#endif
