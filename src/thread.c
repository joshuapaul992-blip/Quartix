/*
 * Quartix - pthreads and scheduling for the Android engine, on Win32.
 * Copyright (C) 2026 Quartix contributors. GPLv3.
 *
 * libroblox.so imports 44 pthread_* symbols plus a handful of sched_* ones and
 * __cxa_thread_atexit_impl. This file implements them on top of Win32 primitives
 * that all exist on the NT 6.0 (Vista) floor: CRITICAL_SECTION for mutexes,
 * CONDITION_VARIABLE for condition variables, a reader/writer lock built from the
 * two for rwlocks, Win32 threads for pthread_create, TLS for pthread_self, and
 * fibre-local storage (FLS) - whose per-thread exit callback is the one Win32
 * mechanism that can run pthread key destructors and C++ thread_local teardown.
 *
 * ABI: the Android code is System V AMD64; every function it can call is declared
 * SYSV. Callbacks it hands us - a thread start routine, a key destructor, a
 * pthread_once initialiser, a __cxa_thread_atexit function - are engine code and
 * so are SysV too, and are typed that way. The Win32 trampoline and the FLS
 * callback, by contrast, are called by Windows and keep the native ABI.
 *
 * The bionic objects (pthread_mutex_t, pthread_cond_t, ...) have fixed sizes set
 * by the engine's copy of the bionic headers, and we only ever receive pointers
 * to them. Rather than guess those sizes and store state in place, we key a side
 * table on the object's address and keep our Win32 object off to the side. That
 * makes the static initialisers (PTHREAD_MUTEX_INITIALIZER and friends, which are
 * just zeroed storage) work: the first lock of an un-init'd mutex lazily creates
 * its CRITICAL_SECTION.
 */
#include "quartix.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SYSV __attribute__((sysv_abi))

/* bionic error numbers (Linux values), returned by value from these calls. */
#define QX_EBUSY 16
#define QX_ETIMEDOUT 110

/* Linux clock ids, as pthread_condattr_setclock takes them. */
#define QX_CLOCK_REALTIME 0
#define QX_CLOCK_MONOTONIC 1

/* pthread_t is a scalar the engine stores in an 8-byte slot (bionic LP64), so it
 * must be 64-bit here (Win64 is LLP64: long is 32-bit). We use it to carry a
 * pointer to the thread control block. */
typedef uintptr_t qxp_t;

/* bionic timespec on LP64: two 64-bit fields. long is 32-bit on Win64, so the
 * layout is spelled with int64 to match the engine's struct byte-for-byte. */
struct qx_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

/* ---- one-time init ---------------------------------------------------------
 * The first pthread call happens single-threaded during engine start-up, before
 * any engine thread exists, so this initialises cleanly; the interlocked guard
 * only covers the pathological concurrent-first-call case. */
static volatile LONG g_init_started;
static volatile LONG g_init_done;
static CRITICAL_SECTION g_lock;   /* guards the side table, keys and TCB lifecycle */
static CRITICAL_SECTION g_once;   /* serialises pthread_once initialisers */
static DWORD g_self_tls;          /* TLS slot: current thread's TCB */
static DWORD g_fls;               /* FLS slot: current thread's key/atexit block */

static void fls_cleanup(void *p); /* fwd (defined below, native ABI) */

static void thread_init(void)
{
    if (InterlockedCompareExchange(&g_init_started, 1, 0) == 0) {
        InitializeCriticalSection(&g_lock);
        InitializeCriticalSection(&g_once);
        g_self_tls = TlsAlloc();
        g_fls = FlsAlloc((PFLS_CALLBACK_FUNCTION)fls_cleanup);
        InterlockedExchange(&g_init_done, 1);
    } else {
        while (!g_init_done)
            Sleep(0);
    }
}

/* ---- address-keyed side table ---------------------------------------------- */
struct objmap_entry {
    void *key;
    void *val;
};
static struct objmap_entry *g_map;
static size_t g_map_cap;   /* power of two */
static size_t g_map_count;

static size_t map_hash(void *key, size_t cap)
{
    uint64_t h = (uint64_t)(uintptr_t)key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t)h & (cap - 1);
}

/* caller holds g_lock */
static void map_grow(void)
{
    size_t newcap = g_map_cap ? g_map_cap * 2 : 256;
    struct objmap_entry *n = (struct objmap_entry *)calloc(newcap, sizeof(*n));
    size_t i;
    if (!n)
        return;
    for (i = 0; i < g_map_cap; i++) {
        if (g_map[i].key) {
            size_t j = map_hash(g_map[i].key, newcap);
            while (n[j].key)
                j = (j + 1) & (newcap - 1);
            n[j] = g_map[i];
        }
    }
    free(g_map);
    g_map = n;
    g_map_cap = newcap;
}

/* caller holds g_lock */
static void *map_get(void *key)
{
    size_t j;
    if (!g_map_cap)
        return NULL;
    j = map_hash(key, g_map_cap);
    while (g_map[j].key) {
        if (g_map[j].key == key)
            return g_map[j].val;
        j = (j + 1) & (g_map_cap - 1);
    }
    return NULL;
}

/* caller holds g_lock */
static void map_put(void *key, void *val)
{
    size_t j;
    if ((g_map_count + 1) * 10 >= g_map_cap * 7)
        map_grow();
    if (!g_map_cap)
        return;
    j = map_hash(key, g_map_cap);
    while (g_map[j].key) {
        if (g_map[j].key == key) {
            g_map[j].val = val;
            return;
        }
        j = (j + 1) & (g_map_cap - 1);
    }
    g_map[j].key = key;
    g_map[j].val = val;
    g_map_count++;
}

/* caller holds g_lock */
static void map_del(void *key)
{
    size_t j, k;
    if (!g_map_cap)
        return;
    j = map_hash(key, g_map_cap);
    while (g_map[j].key) {
        if (g_map[j].key == key) {
            /* remove, then re-insert the following cluster to keep probing valid */
            g_map[j].key = NULL;
            g_map[j].val = NULL;
            g_map_count--;
            k = (j + 1) & (g_map_cap - 1);
            while (g_map[k].key) {
                void *ek = g_map[k].key, *ev = g_map[k].val;
                g_map[k].key = NULL;
                g_map[k].val = NULL;
                g_map_count--;
                map_put(ek, ev);
                k = (k + 1) & (g_map_cap - 1);
            }
            return;
        }
        j = (j + 1) & (g_map_cap - 1);
    }
}

/* ---- mutexes (CRITICAL_SECTION) -------------------------------------------- */
struct qx_mutex {
    CRITICAL_SECTION cs;
};

/* A CRITICAL_SECTION is always recursive, which is a harmless superset of a
 * normal pthread mutex for correct code; pthread mutex type is therefore ignored.
 * Lazily creates the object so a statically-initialised mutex works on first use. */
static struct qx_mutex *mutex_of(void *addr)
{
    struct qx_mutex *m;
    EnterCriticalSection(&g_lock);
    m = (struct qx_mutex *)map_get(addr);
    if (!m) {
        m = (struct qx_mutex *)calloc(1, sizeof(*m));
        if (m) {
            InitializeCriticalSection(&m->cs);
            map_put(addr, m);
        }
    }
    LeaveCriticalSection(&g_lock);
    return m;
}

static SYSV int qx_mutex_init(void *mtx, const void *attr)
{
    (void)attr;
    thread_init();
    mutex_of(mtx);
    return 0;
}
static SYSV int qx_mutex_lock(void *mtx)
{
    struct qx_mutex *m = (thread_init(), mutex_of(mtx));
    if (m)
        EnterCriticalSection(&m->cs);
    return 0;
}
static SYSV int qx_mutex_trylock(void *mtx)
{
    struct qx_mutex *m = (thread_init(), mutex_of(mtx));
    if (m && TryEnterCriticalSection(&m->cs))
        return 0;
    return QX_EBUSY;
}
static SYSV int qx_mutex_unlock(void *mtx)
{
    struct qx_mutex *m = (thread_init(), mutex_of(mtx));
    if (m)
        LeaveCriticalSection(&m->cs);
    return 0;
}
static SYSV int qx_mutex_destroy(void *mtx)
{
    struct qx_mutex *m;
    thread_init();
    EnterCriticalSection(&g_lock);
    m = (struct qx_mutex *)map_get(mtx);
    if (m)
        map_del(mtx);
    LeaveCriticalSection(&g_lock);
    if (m) {
        DeleteCriticalSection(&m->cs);
        free(m);
    }
    return 0;
}

/* mutex attributes: type is ignored (recursive CS covers every type), so these
 * only have to accept the calls. */
static SYSV int qx_mutexattr_init(void *a) { (void)a; return 0; }
static SYSV int qx_mutexattr_destroy(void *a) { (void)a; return 0; }
static SYSV int qx_mutexattr_settype(void *a, int t) { (void)a; (void)t; return 0; }

/* ---- condition variables (CONDITION_VARIABLE) ------------------------------ */
struct qx_cond {
    CONDITION_VARIABLE cv;
    int clock;
};

static struct qx_cond *cond_of(void *addr, int clock)
{
    struct qx_cond *c;
    EnterCriticalSection(&g_lock);
    c = (struct qx_cond *)map_get(addr);
    if (!c) {
        c = (struct qx_cond *)calloc(1, sizeof(*c));
        if (c) {
            InitializeConditionVariable(&c->cv);
            c->clock = clock;
            map_put(addr, c);
        }
    }
    LeaveCriticalSection(&g_lock);
    return c;
}

static int64_t now_ns(int clock)
{
    if (clock == QX_CLOCK_MONOTONIC) {
        /* Must share its zero with a future clock_gettime(MONOTONIC); both use
         * GetTickCount64. A mismatched base only lengthens or shortens a wait,
         * which the engine's predicate loop around cond_wait absorbs. */
        return (int64_t)GetTickCount64() * 1000000LL;
    } else {
        FILETIME ft;
        ULARGE_INTEGER u;
        GetSystemTimeAsFileTime(&ft);
        u.LowPart = ft.dwLowDateTime;
        u.HighPart = ft.dwHighDateTime;
        /* 100ns ticks since 1601 -> ns since 1970 */
        return (int64_t)(u.QuadPart - 116444736000000000ULL) * 100LL;
    }
}

static SYSV int qx_cond_init(void *cnd, const void *attr)
{
    int clock = QX_CLOCK_REALTIME;
    thread_init();
    if (attr) {
        EnterCriticalSection(&g_lock);
        {
            int *stored = (int *)map_get((void *)attr);
            if (stored)
                clock = *stored;
        }
        LeaveCriticalSection(&g_lock);
    }
    cond_of(cnd, clock);
    return 0;
}
static SYSV int qx_cond_destroy(void *cnd)
{
    struct qx_cond *c;
    thread_init();
    EnterCriticalSection(&g_lock);
    c = (struct qx_cond *)map_get(cnd);
    if (c)
        map_del(cnd);
    LeaveCriticalSection(&g_lock);
    free(c);
    return 0;
}
static SYSV int qx_cond_signal(void *cnd)
{
    struct qx_cond *c = (thread_init(), cond_of(cnd, QX_CLOCK_REALTIME));
    if (c)
        WakeConditionVariable(&c->cv);
    return 0;
}
static SYSV int qx_cond_broadcast(void *cnd)
{
    struct qx_cond *c = (thread_init(), cond_of(cnd, QX_CLOCK_REALTIME));
    if (c)
        WakeAllConditionVariable(&c->cv);
    return 0;
}
static SYSV int qx_cond_wait(void *cnd, void *mtx)
{
    struct qx_cond *c = (thread_init(), cond_of(cnd, QX_CLOCK_REALTIME));
    struct qx_mutex *m = mutex_of(mtx);
    if (c && m)
        SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
    return 0;
}
static SYSV int qx_cond_timedwait(void *cnd, void *mtx, const struct qx_timespec *ts)
{
    struct qx_cond *c = (thread_init(), cond_of(cnd, QX_CLOCK_REALTIME));
    struct qx_mutex *m = mutex_of(mtx);
    int64_t deadline, rel;
    DWORD ms;
    if (!c || !m)
        return 0;
    if (!ts)
        return qx_cond_wait(cnd, mtx);
    deadline = ts->tv_sec * 1000000000LL + ts->tv_nsec;
    rel = deadline - now_ns(c->clock);
    if (rel <= 0)
        ms = 0;
    else {
        int64_t m2 = (rel + 999999LL) / 1000000LL;
        ms = m2 > 0x7fffffffLL ? 0x7fffffffU : (DWORD)m2;
    }
    if (!SleepConditionVariableCS(&c->cv, &m->cs, ms) && GetLastError() == ERROR_TIMEOUT)
        return QX_ETIMEDOUT;
    return 0;
}

/* condition-variable attributes: only the clock matters, stored in the side
 * table as a bare int keyed on the attr address. */
static SYSV int qx_condattr_init(void *a)
{
    int *v;
    thread_init();
    v = (int *)calloc(1, sizeof(int));
    if (v) {
        *v = QX_CLOCK_REALTIME;
        EnterCriticalSection(&g_lock);
        map_put(a, v);
        LeaveCriticalSection(&g_lock);
    }
    return 0;
}
static SYSV int qx_condattr_setclock(void *a, int clock)
{
    int *v;
    thread_init();
    EnterCriticalSection(&g_lock);
    v = (int *)map_get(a);
    if (v)
        *v = clock;
    LeaveCriticalSection(&g_lock);
    return 0;
}
static SYSV int qx_condattr_destroy(void *a)
{
    int *v;
    thread_init();
    EnterCriticalSection(&g_lock);
    v = (int *)map_get(a);
    if (v)
        map_del(a);
    LeaveCriticalSection(&g_lock);
    free(v);
    return 0;
}

/* ---- read/write locks ------------------------------------------------------
 * SRWLOCK release is mode-specific but pthread_rwlock_unlock is not, so a plain
 * CRITICAL_SECTION + two condition variables is used instead: it lets unlock tell
 * a writer release from a reader release by remembering the writer's thread id.
 * Writers are preferred, so a steady stream of readers cannot starve a writer. */
struct qx_rwlock {
    CRITICAL_SECTION lk;
    CONDITION_VARIABLE readers_ok;
    CONDITION_VARIABLE writers_ok;
    int readers;         /* active readers */
    int writer;          /* a writer holds the lock */
    int waiting_writers; /* writers blocked, so new readers wait */
    DWORD writer_tid;
};

static struct qx_rwlock *rwlock_of(void *addr)
{
    struct qx_rwlock *r;
    EnterCriticalSection(&g_lock);
    r = (struct qx_rwlock *)map_get(addr);
    if (!r) {
        r = (struct qx_rwlock *)calloc(1, sizeof(*r));
        if (r) {
            InitializeCriticalSection(&r->lk);
            InitializeConditionVariable(&r->readers_ok);
            InitializeConditionVariable(&r->writers_ok);
            map_put(addr, r);
        }
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

static SYSV int qx_rwlock_init(void *rw, const void *attr)
{
    (void)attr;
    thread_init();
    rwlock_of(rw);
    return 0;
}
static SYSV int qx_rwlock_rdlock(void *rw)
{
    struct qx_rwlock *r = (thread_init(), rwlock_of(rw));
    if (!r)
        return 0;
    EnterCriticalSection(&r->lk);
    while (r->writer || r->waiting_writers)
        SleepConditionVariableCS(&r->readers_ok, &r->lk, INFINITE);
    r->readers++;
    LeaveCriticalSection(&r->lk);
    return 0;
}
static SYSV int qx_rwlock_wrlock(void *rw)
{
    struct qx_rwlock *r = (thread_init(), rwlock_of(rw));
    if (!r)
        return 0;
    EnterCriticalSection(&r->lk);
    r->waiting_writers++;
    while (r->writer || r->readers > 0)
        SleepConditionVariableCS(&r->writers_ok, &r->lk, INFINITE);
    r->waiting_writers--;
    r->writer = 1;
    r->writer_tid = GetCurrentThreadId();
    LeaveCriticalSection(&r->lk);
    return 0;
}
static SYSV int qx_rwlock_unlock(void *rw)
{
    struct qx_rwlock *r = (thread_init(), rwlock_of(rw));
    if (!r)
        return 0;
    EnterCriticalSection(&r->lk);
    if (r->writer && r->writer_tid == GetCurrentThreadId()) {
        r->writer = 0;
        r->writer_tid = 0;
    } else if (r->readers > 0) {
        r->readers--;
    }
    /* hand off to a waiting writer first, else release readers */
    if (r->writer == 0 && r->readers == 0) {
        if (r->waiting_writers)
            WakeConditionVariable(&r->writers_ok);
        else
            WakeAllConditionVariable(&r->readers_ok);
    }
    LeaveCriticalSection(&r->lk);
    return 0;
}
static SYSV int qx_rwlock_destroy(void *rw)
{
    struct qx_rwlock *r;
    thread_init();
    EnterCriticalSection(&g_lock);
    r = (struct qx_rwlock *)map_get(rw);
    if (r)
        map_del(rw);
    LeaveCriticalSection(&g_lock);
    if (r) {
        DeleteCriticalSection(&r->lk);
        free(r);
    }
    return 0;
}

/* ---- thread-local storage and thread-exit destructors ----------------------
 * One FLS slot holds a per-thread block carrying every pthread key value and the
 * list of __cxa_thread_atexit functions. The single FLS exit callback walks that
 * block on thread termination, which is how pthread key destructors and C++
 * thread_local teardown get to run on Win32. */
typedef SYSV void (*sysv_dtor)(void *);

struct atexit_ent {
    sysv_dtor fn;
    void *arg;
};
struct tls_block {
    void **vals;   /* key id -> value */
    size_t valcap;
    struct atexit_ent *ax;
    size_t ax_n, ax_cap;
};

/* key registry: id -> destructor. Ids are small and reused. */
static sysv_dtor *g_key_dtor;
static int *g_key_used;
static int g_key_cap;

static struct tls_block *tls_get(int create)
{
    struct tls_block *b = (struct tls_block *)FlsGetValue(g_fls);
    if (!b && create) {
        b = (struct tls_block *)calloc(1, sizeof(*b));
        if (b)
            FlsSetValue(g_fls, b);
    }
    return b;
}

/* FLS exit callback (native ABI, called by Windows on thread exit). */
static void fls_cleanup(void *p)
{
    struct tls_block *b = (struct tls_block *)p;
    size_t i;
    if (!b)
        return;
    /* C++ thread_local destructors, most-recent first */
    for (i = b->ax_n; i-- > 0;)
        if (b->ax[i].fn)
            b->ax[i].fn(b->ax[i].arg);
    /* pthread key destructors: one pass (pthreads allows up to four; one is a
     * documented simplification that suffices for teardown that does not re-set
     * its own key). */
    EnterCriticalSection(&g_lock);
    for (i = 0; i < b->valcap; i++) {
        void *v = b->vals[i];
        if (v && (int)i < g_key_cap && g_key_used[i] && g_key_dtor[i]) {
            sysv_dtor d = g_key_dtor[i];
            b->vals[i] = NULL;
            LeaveCriticalSection(&g_lock);
            d(v);
            EnterCriticalSection(&g_lock);
        }
    }
    LeaveCriticalSection(&g_lock);
    free(b->vals);
    free(b->ax);
    free(b);
}

static SYSV int qx_key_create(unsigned *key, sysv_dtor dtor)
{
    int i, id = -1;
    thread_init();
    EnterCriticalSection(&g_lock);
    for (i = 0; i < g_key_cap; i++)
        if (!g_key_used[i]) {
            id = i;
            break;
        }
    if (id < 0) {
        int newcap = g_key_cap ? g_key_cap * 2 : 32;
        sysv_dtor *nd = (sysv_dtor *)realloc(g_key_dtor, (size_t)newcap * sizeof(*nd));
        int *nu = (int *)realloc(g_key_used, (size_t)newcap * sizeof(*nu));
        if (nd)
            g_key_dtor = nd;
        if (nu)
            g_key_used = nu;
        if (nd && nu) {
            for (i = g_key_cap; i < newcap; i++) {
                g_key_dtor[i] = NULL;
                g_key_used[i] = 0;
            }
            id = g_key_cap;
            g_key_cap = newcap;
        }
    }
    if (id >= 0) {
        g_key_used[id] = 1;
        g_key_dtor[id] = dtor;
    }
    LeaveCriticalSection(&g_lock);
    if (id < 0)
        return QX_EBUSY;
    *key = (unsigned)id;
    return 0;
}
static SYSV int qx_key_delete(unsigned key)
{
    thread_init();
    EnterCriticalSection(&g_lock);
    if ((int)key < g_key_cap) {
        g_key_used[key] = 0;
        g_key_dtor[key] = NULL;
    }
    LeaveCriticalSection(&g_lock);
    return 0;
}
static SYSV void *qx_getspecific(unsigned key)
{
    struct tls_block *b = (thread_init(), tls_get(0));
    if (b && key < b->valcap)
        return b->vals[key];
    return NULL;
}
static SYSV int qx_setspecific(unsigned key, const void *val)
{
    struct tls_block *b = (thread_init(), tls_get(1));
    if (!b)
        return QX_EBUSY;
    if (key >= b->valcap) {
        size_t nc = b->valcap ? b->valcap * 2 : 32;
        void **nv;
        while (nc <= key)
            nc *= 2;
        nv = (void **)realloc(b->vals, nc * sizeof(*nv));
        if (!nv)
            return QX_EBUSY;
        memset(nv + b->valcap, 0, (nc - b->valcap) * sizeof(*nv));
        b->vals = nv;
        b->valcap = nc;
    }
    b->vals[key] = (void *)val;
    return 0;
}

static SYSV int qx_cxa_thread_atexit(sysv_dtor fn, void *arg, void *dso)
{
    struct tls_block *b = (thread_init(), tls_get(1));
    (void)dso;
    if (!b)
        return -1;
    if (b->ax_n == b->ax_cap) {
        size_t nc = b->ax_cap ? b->ax_cap * 2 : 8;
        struct atexit_ent *na = (struct atexit_ent *)realloc(b->ax, nc * sizeof(*na));
        if (!na)
            return -1;
        b->ax = na;
        b->ax_cap = nc;
    }
    b->ax[b->ax_n].fn = fn;
    b->ax[b->ax_n].arg = arg;
    b->ax_n++;
    return 0;
}

/* ---- pthread_once ---------------------------------------------------------- */
static SYSV int qx_once(int *control, SYSV void (*init)(void))
{
    thread_init();
    if (*(volatile int *)control == 2)
        return 0;
    EnterCriticalSection(&g_once);
    if (*control != 2) {
        LeaveCriticalSection(&g_once);
        /* run the initialiser outside g_once so a nested pthread_once on a
         * different control does not self-deadlock */
        init();
        EnterCriticalSection(&g_once);
        *control = 2;
    }
    LeaveCriticalSection(&g_once);
    return 0;
}

/* ---- threads --------------------------------------------------------------- */
typedef SYSV void *(*sysv_start)(void *);

struct tcb {
    HANDLE h;
    DWORD tid;
    sysv_start start;
    void *arg;
    void *ret;
    int detached;
    int done;
};

/* attributes: our own small layout, kept off to the side so no bionic struct
 * size is assumed. */
struct qx_attr {
    void *stackaddr;
    size_t stacksize;
    int detach;
};

static struct qx_attr *attr_of(void *addr, int create)
{
    struct qx_attr *a;
    EnterCriticalSection(&g_lock);
    a = (struct qx_attr *)map_get(addr);
    if (!a && create) {
        a = (struct qx_attr *)calloc(1, sizeof(*a));
        if (a)
            map_put(addr, a);
    }
    LeaveCriticalSection(&g_lock);
    return a;
}

/* retire a finished thread: free it if detached, else leave it for join */
static void finish_thread(struct tcb *t, void *ret)
{
    int det;
    EnterCriticalSection(&g_lock);
    t->ret = ret;
    t->done = 1;
    det = t->detached;
    LeaveCriticalSection(&g_lock);
    if (det) {
        CloseHandle(t->h);
        free(t);
    }
}

static DWORD WINAPI thread_trampoline(LPVOID p)
{
    struct tcb *t = (struct tcb *)p;
    void *ret;
    TlsSetValue(g_self_tls, t);
    qx_diag_stack_cushion(); /* leave room for the crash handler on engine threads */
    ret = t->start(t->arg);
    finish_thread(t, ret);
    return 0;
}

static SYSV int qx_create(qxp_t *out, const void *attr, sysv_start start, void *arg)
{
    struct tcb *t;
    SIZE_T stack = 0;
    thread_init();
    if (attr) {
        struct qx_attr *a = attr_of((void *)attr, 0);
        if (a)
            stack = (SIZE_T)a->stacksize;
    }
    t = (struct tcb *)calloc(1, sizeof(*t));
    if (!t)
        return QX_EBUSY;
    t->start = start;
    t->arg = arg;
    t->h = CreateThread(NULL, stack, thread_trampoline, t, 0, &t->tid);
    if (!t->h) {
        free(t);
        return QX_EBUSY;
    }
    *out = (qxp_t)t;
    return 0;
}

static struct tcb *self_tcb(void)
{
    struct tcb *t = (struct tcb *)TlsGetValue(g_self_tls);
    if (!t) {
        /* a thread Quartix did not create (e.g. the main thread) asking its own
         * identity: give it a control block so pthread_self/equal are stable */
        t = (struct tcb *)calloc(1, sizeof(*t));
        if (t) {
            t->tid = GetCurrentThreadId();
            t->done = 0;
            TlsSetValue(g_self_tls, t);
        }
    }
    return t;
}

static SYSV qxp_t qx_self(void)
{
    thread_init();
    return (qxp_t)self_tcb();
}
static SYSV int qx_equal(qxp_t a, qxp_t b) { return a == b; }

static SYSV int qx_join(qxp_t thread, void **ret)
{
    struct tcb *t = (struct tcb *)thread;
    thread_init();
    if (!t)
        return 0;
    if (t->h)
        WaitForSingleObject(t->h, INFINITE);
    if (ret)
        *ret = t->ret;
    if (t->h)
        CloseHandle(t->h);
    free(t);
    return 0;
}
static SYSV int qx_detach(qxp_t thread)
{
    struct tcb *t = (struct tcb *)thread;
    int done;
    thread_init();
    if (!t)
        return 0;
    EnterCriticalSection(&g_lock);
    done = t->done;
    if (!done)
        t->detached = 1;
    LeaveCriticalSection(&g_lock);
    if (done) {
        if (t->h)
            CloseHandle(t->h);
        free(t);
    }
    return 0;
}
static SYSV void qx_exit(void *ret)
{
    struct tcb *t = (thread_init(), (struct tcb *)TlsGetValue(g_self_tls));
    /* run thread-exit destructors now; FlsSetValue(NULL) stops the FLS callback
     * running them a second time when the thread unwinds. */
    {
        struct tls_block *b = (struct tls_block *)FlsGetValue(g_fls);
        if (b) {
            FlsSetValue(g_fls, NULL);
            fls_cleanup(b);
        }
    }
    if (t)
        finish_thread(t, ret);
    ExitThread(0);
}

/* ---- scheduling (best effort) ---------------------------------------------- */
static SYSV int qx_sched_yield(void) { SwitchToThread(); return 0; }
static SYSV int qx_sched_get_priority_max(int policy) { (void)policy; return 0; }
static SYSV int qx_sched_get_priority_min(int policy) { (void)policy; return 0; }
static SYSV int qx_sched_getcpu(void) { return (int)GetCurrentProcessorNumber(); }
static SYSV int qx_sched_getscheduler(int pid) { (void)pid; return 0; /* SCHED_OTHER */ }
static SYSV int qx_sched_setscheduler(int pid, int policy, const void *p)
{
    (void)pid; (void)policy; (void)p;
    return 0;
}
static SYSV int qx_sched_getparam(int pid, int *param)
{
    (void)pid;
    if (param)
        *param = 0;
    return 0;
}

/* pthread scheduling: accepted but not mapped onto Win32 priorities, which do
 * not share the POSIX policy/priority model. */
static SYSV int qx_getschedparam(qxp_t t, int *policy, int *param)
{
    (void)t;
    if (policy)
        *policy = 0;
    if (param)
        *param = 0;
    return 0;
}
static SYSV int qx_setschedparam(qxp_t t, int policy, const void *param)
{
    (void)t; (void)policy; (void)param;
    return 0;
}
/* SetThreadDescription is Windows 10+, below our floor, so naming is a no-op. */
static SYSV int qx_setname(qxp_t t, const char *name) { (void)t; (void)name; return 0; }
/* Windows has no POSIX signals. */
static SYSV int qx_sigmask(int how, const void *set, void *old)
{
    (void)how; (void)set; (void)old;
    return 0;
}

/* ---- attributes ------------------------------------------------------------ */
static SYSV int qx_attr_init(void *a)
{
    thread_init();
    attr_of(a, 1);
    return 0;
}
static SYSV int qx_attr_destroy(void *a)
{
    struct qx_attr *p;
    thread_init();
    EnterCriticalSection(&g_lock);
    p = (struct qx_attr *)map_get(a);
    if (p)
        map_del(a);
    LeaveCriticalSection(&g_lock);
    free(p);
    return 0;
}
static SYSV int qx_attr_setstacksize(void *a, size_t sz)
{
    struct qx_attr *p = (thread_init(), attr_of(a, 1));
    if (p)
        p->stacksize = sz;
    return 0;
}
static SYSV int qx_attr_setdetachstate(void *a, int st)
{
    struct qx_attr *p = (thread_init(), attr_of(a, 1));
    if (p)
        p->detach = st;
    return 0;
}
static SYSV int qx_attr_setschedparam(void *a, const void *param)
{
    (void)a; (void)param;
    return 0;
}
static SYSV int qx_attr_getstack(const void *a, void **base, size_t *size)
{
    struct qx_attr *p = (thread_init(), attr_of((void *)a, 0));
    if (p) {
        if (base)
            *base = p->stackaddr;
        if (size)
            *size = p->stacksize;
    }
    return 0;
}
/* Fill an attr with a thread's stack bounds. For the calling thread these come
 * from VirtualQuery on a stack address, which yields the committed (safely
 * scannable) stack region - the span a GC self-stack scan asks for. Other
 * threads get a conservative default. (VirtualQuery is used rather than the TEB
 * intrinsic, whose inlining trips a false -Warray-bounds under -O2.) */
static SYSV int qx_getattr_np(qxp_t thread, void *a)
{
    struct qx_attr *p = (thread_init(), attr_of(a, 1));
    struct tcb *t = (struct tcb *)thread;
    if (!p)
        return QX_EBUSY;
    if (!t || t->tid == GetCurrentThreadId()) {
        MEMORY_BASIC_INFORMATION mbi;
        int probe;
        if (VirtualQuery(&probe, &mbi, sizeof(mbi)) == sizeof(mbi)) {
            p->stackaddr = mbi.BaseAddress;
            p->stacksize = (size_t)mbi.RegionSize;
        } else {
            p->stackaddr = NULL;
            p->stacksize = 8u * 1024u * 1024u;
        }
    } else {
        p->stackaddr = NULL;
        p->stacksize = 8u * 1024u * 1024u;
    }
    return 0;
}

/* ---- resolver -------------------------------------------------------------- */
struct thread_sym {
    const char *name;
    void *fn;
};
static const struct thread_sym g_thread_table[] = {
    {"pthread_mutex_init", (void *)qx_mutex_init},
    {"pthread_mutex_destroy", (void *)qx_mutex_destroy},
    {"pthread_mutex_lock", (void *)qx_mutex_lock},
    {"pthread_mutex_trylock", (void *)qx_mutex_trylock},
    {"pthread_mutex_unlock", (void *)qx_mutex_unlock},
    {"pthread_mutexattr_init", (void *)qx_mutexattr_init},
    {"pthread_mutexattr_destroy", (void *)qx_mutexattr_destroy},
    {"pthread_mutexattr_settype", (void *)qx_mutexattr_settype},
    {"pthread_cond_init", (void *)qx_cond_init},
    {"pthread_cond_destroy", (void *)qx_cond_destroy},
    {"pthread_cond_signal", (void *)qx_cond_signal},
    {"pthread_cond_broadcast", (void *)qx_cond_broadcast},
    {"pthread_cond_wait", (void *)qx_cond_wait},
    {"pthread_cond_timedwait", (void *)qx_cond_timedwait},
    {"pthread_condattr_init", (void *)qx_condattr_init},
    {"pthread_condattr_destroy", (void *)qx_condattr_destroy},
    {"pthread_condattr_setclock", (void *)qx_condattr_setclock},
    {"pthread_rwlock_init", (void *)qx_rwlock_init},
    {"pthread_rwlock_destroy", (void *)qx_rwlock_destroy},
    {"pthread_rwlock_rdlock", (void *)qx_rwlock_rdlock},
    {"pthread_rwlock_wrlock", (void *)qx_rwlock_wrlock},
    {"pthread_rwlock_unlock", (void *)qx_rwlock_unlock},
    {"pthread_key_create", (void *)qx_key_create},
    {"pthread_key_delete", (void *)qx_key_delete},
    {"pthread_getspecific", (void *)qx_getspecific},
    {"pthread_setspecific", (void *)qx_setspecific},
    {"pthread_once", (void *)qx_once},
    {"pthread_create", (void *)qx_create},
    {"pthread_join", (void *)qx_join},
    {"pthread_detach", (void *)qx_detach},
    {"pthread_exit", (void *)qx_exit},
    {"pthread_self", (void *)qx_self},
    {"pthread_equal", (void *)qx_equal},
    {"pthread_getschedparam", (void *)qx_getschedparam},
    {"pthread_setschedparam", (void *)qx_setschedparam},
    {"pthread_setname_np", (void *)qx_setname},
    {"pthread_sigmask", (void *)qx_sigmask},
    {"pthread_attr_init", (void *)qx_attr_init},
    {"pthread_attr_destroy", (void *)qx_attr_destroy},
    {"pthread_attr_setstacksize", (void *)qx_attr_setstacksize},
    {"pthread_attr_setdetachstate", (void *)qx_attr_setdetachstate},
    {"pthread_attr_setschedparam", (void *)qx_attr_setschedparam},
    {"pthread_attr_getstack", (void *)qx_attr_getstack},
    {"pthread_getattr_np", (void *)qx_getattr_np},
    {"__cxa_thread_atexit_impl", (void *)qx_cxa_thread_atexit},
    {"sched_yield", (void *)qx_sched_yield},
    {"sched_get_priority_max", (void *)qx_sched_get_priority_max},
    {"sched_get_priority_min", (void *)qx_sched_get_priority_min},
    {"sched_getcpu", (void *)qx_sched_getcpu},
    {"sched_getscheduler", (void *)qx_sched_getscheduler},
    {"sched_setscheduler", (void *)qx_sched_setscheduler},
    {"sched_getparam", (void *)qx_sched_getparam},
};

void *qx_thread_resolve(const char *name)
{
    size_t i;
    if (!name)
        return NULL;
    for (i = 0; i < sizeof(g_thread_table) / sizeof(g_thread_table[0]); i++)
        if (strcmp(name, g_thread_table[i].name) == 0)
            return g_thread_table[i].fn;
    return NULL;
}
