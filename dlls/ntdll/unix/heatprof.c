/*
 * GW2 on Mac: the game thread by x86 address under FEX (WINE_HEAT_PROF=1, jdrly/gw2-on-mac#43)
 *
 * Off unless WINE_HEAT_PROF=1 is set for WINE_HEAT_EXE (heat.c decides, in heat_init()). Then a
 * thread of its own that runs no Windows code, with every signal blocked, waits until FEX is
 * loaded, checks that it is the FEX heatprof.h was read from, and only then installs a SIGPROF
 * handler. It sends SIGPROF about WINE_HEAT_PROF_HZ (default 1000) times a second to the game
 * thread, the busiest unnamed thread that started in the exe, and with WINE_HEAT_PROF_THREADS
 * (a list of "render", "worker") to bgfx's render thread and the busiest other such thread.
 * These are wall-clock samples: a signal interrupts a blocked system call too.
 *
 * The handler only copies, into a ring of the target's: the arm64 registers, the CPU area's
 * InSimulation and InSyscallCallback, the first 256 bytes of FEX's CPUState and the two words at
 * its callret_sp, the syscall frame's sp and pc when the thread is in Wine's unix side, and up to
 * 2 KB of the guest's stack within the TEB's stack limits. It takes no lock, allocates nothing and
 * calls nothing of FEX's. Every read of memory that isn't Wine's own goes through heat_prof_peek(),
 * a write() of it into a pipe of the target's: the kernel copies it, and memory that is gone fails
 * the write with EFAULT where a load would fault.
 *
 * The sampler thread resolves each sample while the process runs, since the code buffers are gone
 * after it: from the block FEX entered last to its tail to the x86 address, as FEX's
 * RestoreRIPFromHostPC does (heatprof.h has the layout). It writes, in $WINE_HEAT_LOG (default
 * /tmp), heat-prof-<pid>.bin (heatprof.h has the format) and heat-prof-<pid>.txt, whose lines are:
 *
 *   F <s> fex <guid><age> size <hex> <pinned|refused>   FEX as found, once
 *   L <s> <n>                                           the PE modules, again when their number changes:
 *   M <base> <size> <path>                              n lines follow
 *   J <start> <end>                                     a memory region a sampled block lives in
 *   U <addr> <image base> <image> <symbol> <symbol addr> a unix address, named once by dladdr
 *   C <s> <target> <tid> sent <n> recorded <n> skipped <n> dropped <n> late <n> cpu_ms <n>
 *                                                       each target's counts, every second and at exit
 *
 * <s> is seconds of mach_absolute_time(). sent: signals sent. skipped: ticks without a signal, as
 * the last one wasn't handled yet (the thread didn't run). recorded: samples in the ring. dropped:
 * samples the full ring had no room for. late: a handler that found its thread was no longer a
 * target (process-wide). cpu_ms: the thread's CPU time over the last second.
 *
 * A thread joins the list in spawn_thread() and leaves it in pthread_exit_wrapper(), under the
 * lock the sampler holds around pthread_kill(), so no signal goes to a thread that has exited.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#include "unix_private.h"
#include "heat.h"
#include "heatprof.h"

#define HEAT_PROF_TARGETS 3     /* 0 game, 1 render, 2 worker */
#define HEAT_PROF_RING    512   /* samples a target's ring holds */
#define HEAT_PROF_THREADS 4096  /* threads the list holds */
#define HEAT_PROF_NAMED   (1 << 16)  /* unix addresses named, open addressing */
#define HEAT_PROF_REGIONS 256   /* code regions written */
#define HEAT_PROF_CHUNK   512   /* bytes of RIP entries read at once */

/* struct syscall_frame (signal_arm64.c:191-207), by 64-bit word: x[0-28], fp, lr, sp, pc */
#define HEAT_SYSCALL_X23 23
#define HEAT_SYSCALL_SP  31
#define HEAT_SYSCALL_PC  32

static const char * const heat_prof_target_names[HEAT_PROF_TARGETS] = { "game", "render", "worker" };

struct heat_prof_thread
{
    struct thread_data *data;    /* NULL: free */
    pthread_t           thread;  /* 0 until pthread_create returned */
    unsigned long long  utid;    /* the macOS thread id, 0 until the sampler asked */
    unsigned long long  cpu;     /* its CPU time at the last retarget, ns */
};

struct heat_prof_target
{
    pthread_t           thread;  /* the handler compares pthread_self() with it; 0: none */
    int                 entry;   /* in heat_prof_threads, -1: none */
    DWORD               tid;
    int                 busy;    /* a handler fills the ring, or the sampler retargets */
    pthread_t           pending; /* the thread a signal went to that wasn't handled yet, or 0 */
    unsigned int        write;   /* the handler's */
    unsigned int        read;    /* the sampler's */
    unsigned long long  sent, skipped, recorded, dropped, cpu_ms;
    int                 pipe[2];
    struct heat_prof_rec *ring;
};

static struct heat_prof_thread heat_prof_threads[HEAT_PROF_THREADS];
static pthread_mutex_t heat_prof_lock = PTHREAD_MUTEX_INITIALIZER;  /* the list and the targets */
static pthread_mutex_t heat_prof_file_lock = PTHREAD_MUTEX_INITIALIZER;
static struct heat_prof_target heat_prof_targets[HEAT_PROF_TARGETS];
static unsigned int heat_prof_late;
static unsigned int heat_prof_want = 1;  /* a bit per target */
static unsigned int heat_prof_hz = 1000;
static int heat_prof_own[2];             /* the sampler thread's pipe, after atexit() with heat_prof_file_lock */
static unsigned long long *heat_prof_named;
static mach_timebase_info_data_t heat_prof_timebase;
static char heat_prof_bin_path[1024], heat_prof_txt_path[1024];
static FILE *heat_prof_bin, *heat_prof_txt;
static ULONG_PTR heat_prof_exe_lo, heat_prof_exe_hi;

static double heat_prof_seconds(void)
{
    return mach_absolute_time() * (double)heat_prof_timebase.numer / heat_prof_timebase.denom / 1e9;
}

/* Copies <size> bytes at <src> to <dst> through <fd>, a pipe only its caller uses, and drains it
 * whatever happens. FALSE if the kernel couldn't read all of it. Async-signal-safe: write() and
 * read() only. <size> stays well under a pipe's buffer. */
static BOOL heat_prof_peek( const int fd[2], void *dst, ULONG_PTR src, size_t size )
{
    unsigned char junk[64];
    ssize_t n;

    if (!src) return FALSE;
    if ((n = write( fd[1], (const void *)src, size )) == size && read( fd[0], dst, size ) == size) return TRUE;
    /* a short or failed write may still have left bytes in the pipe */
    while ((n = read( fd[0], junk, sizeof(junk) )) > 0) ;
    return FALSE;
}

/***********************************************************************
 *  the handler
 */
static void heat_prof_sample( struct heat_prof_rec *rec, unsigned int target, ucontext_t *uc, const int fd[2] )
{
    struct thread_data *data = get_thread_data();
    TEB *teb = data ? data->teb : NULL;
    CHPE_V2_CPU_AREA_INFO *cpu = teb ? teb->ChpeV2CpuAreaInfo : NULL;
    ULONG_PTR gsp, gx23, frame, callret, block, lo, hi;
    unsigned short flags = 0;
    unsigned int i;

    rec->t = mach_absolute_time();
    rec->tid = data ? data->tid : 0;
    rec->target = target;
    for (i = 0; i < 29; i++) rec->regs[i] = uc->uc_mcontext->__ss.__x[i];
    rec->regs[HP_REG_FP] = uc->uc_mcontext->__ss.__fp;
    rec->regs[HP_REG_LR] = uc->uc_mcontext->__ss.__lr;
    rec->regs[HP_REG_SP] = uc->uc_mcontext->__ss.__sp;
    rec->regs[HP_REG_PC] = uc->uc_mcontext->__ss.__pc;
    rec->regs[HP_REG_CPSR] = uc->uc_mcontext->__ss.__cpsr;
    rec->sys_sp = rec->sys_pc = rec->x86 = rec->block = rec->stack_at = 0;
    rec->callret[0] = rec->callret[1] = 0;
    rec->stack_len = rec->to_tail = 0;
    if (!teb) goto done;

    /* the guest's stack pointer: x23 in translated code (FEX's ARM64EC register map), sp in
     * native PE code, and the syscall frame's copies of the two in Wine's unix side */
    gsp = rec->regs[HP_REG_SP];
    gx23 = rec->regs[23];
    if (is_inside_syscall( data, rec->regs[HP_REG_SP] ))
    {
        const unsigned long long *sf = (const unsigned long long *)get_syscall_frame( data );
        flags |= HP_SYSCALL;
        if (sf)
        {
            gsp = rec->sys_sp = sf[HEAT_SYSCALL_SP];
            rec->sys_pc = sf[HEAT_SYSCALL_PC];
            gx23 = sf[HEAT_SYSCALL_X23];
        }
    }
    if (cpu)
    {
        if (cpu->InSimulation)
        {
            flags |= HP_SIM;
            gsp = gx23;
        }
        if (cpu->InSyscallCallback) flags |= HP_SYSCB;
        /* FEX's memory: the thread may be in its teardown, or another thread in it for this one */
        if ((frame = (ULONG_PTR)cpu->EmulatorData[0]) && heat_prof_peek( fd, rec->state, frame, HEAT_FEX_STATE_COPY ))
        {
            flags |= HP_STATE;
            memcpy( &callret, rec->state + HEAT_FEX_STATE_CALLRET, sizeof(callret) );
            if (heat_prof_peek( fd, rec->callret, callret, HEAT_FEX_CALLRET_COPY )) flags |= HP_CALLRET;
            /* the block's tail as it is now, which the sampler thread checks its own read against */
            memcpy( &block, rec->state + HEAT_FEX_STATE_BLOCK, sizeof(block) );
            if ((flags & HP_SIM) && heat_prof_peek( fd, &rec->to_tail, block + HEAT_FEX_HEADER_TAIL, sizeof(rec->to_tail) ) &&
                rec->to_tail <= (128 << 20) && heat_prof_peek( fd, rec->tail, block + rec->to_tail, HEAT_FEX_TAIL_COPY ))
                flags |= HP_TAIL;
        }
    }
    lo = (ULONG_PTR)teb->Tib.StackLimit;
    hi = (ULONG_PTR)teb->Tib.StackBase;
    if (gsp >= lo && gsp < hi)
    {
        size_t len = hi - gsp < HEAT_PROF_STACK ? hi - gsp : HEAT_PROF_STACK;
        if (heat_prof_peek( fd, rec->stack, gsp, len ))
        {
            rec->stack_at = gsp;
            rec->stack_len = len;
        }
    }
done:
    rec->flags = flags;
}

static void heat_prof_handler( int sig, siginfo_t *info, void *ucv )
{
    struct heat_prof_target *t = NULL;
    pthread_t self = pthread_self();
    int saved_errno = errno;
    unsigned int k, w;

    for (k = 0; k < HEAT_PROF_TARGETS; k++)
        if (__atomic_load_n( &heat_prof_targets[k].thread, __ATOMIC_ACQUIRE ) == self)
        {
            t = &heat_prof_targets[k];
            break;
        }
    if (!t)
    {
        __atomic_fetch_add( &heat_prof_late, 1, __ATOMIC_RELAXED );
        goto done;
    }
    /* busy gives the ring and the counters one writer at a time: a handler of the thread the
     * target had before a retarget may still run, and the retarget itself takes busy */
    if (__atomic_exchange_n( &t->busy, 1, __ATOMIC_ACQUIRE ))
    {
        __atomic_fetch_add( &heat_prof_late, 1, __ATOMIC_RELAXED );
        goto handled;
    }
    if (__atomic_load_n( &t->thread, __ATOMIC_ACQUIRE ) != self)
        __atomic_fetch_add( &heat_prof_late, 1, __ATOMIC_RELAXED );
    else
    {
        w = t->write;
        if (w - __atomic_load_n( &t->read, __ATOMIC_ACQUIRE ) >= HEAT_PROF_RING)
            __atomic_fetch_add( &t->dropped, 1, __ATOMIC_RELAXED );
        else
        {
            heat_prof_sample( &t->ring[w % HEAT_PROF_RING], k, ucv, t->pipe );
            __atomic_store_n( &t->write, w + 1, __ATOMIC_RELEASE );
            __atomic_fetch_add( &t->recorded, 1, __ATOMIC_RELAXED );
        }
    }
    __atomic_store_n( &t->busy, 0, __ATOMIC_RELEASE );
handled:
    /* only this thread's own signal: a handler of the target's old thread leaves the new one's */
    {
        pthread_t mine = self;
        __atomic_compare_exchange_n( &t->pending, &mine, (pthread_t)0, FALSE, __ATOMIC_RELEASE, __ATOMIC_RELAXED );
    }
done:
    errno = saved_errno;
}

/***********************************************************************
 *  the list of threads
 */
/* spawn_thread() holds the lock from before pthread_create() to the thread's entry, so the thread
 * can't leave the list before it is in it */
void heat_prof_thread_lock(void)
{
    pthread_mutex_lock( &heat_prof_lock );
}

/* with the lock, which it releases; <thread> 0: pthread_create failed */
void heat_prof_thread_add( struct thread_data *data, pthread_t thread )
{
    unsigned int i;

    for (i = 0; thread && i < HEAT_PROF_THREADS; i++)
    {
        if (heat_prof_threads[i].data) continue;
        heat_prof_threads[i].data = data;
        heat_prof_threads[i].thread = thread;
        heat_prof_threads[i].utid = 0;
        heat_prof_threads[i].cpu = 0;
        break;
    }
    pthread_mutex_unlock( &heat_prof_lock );
}

static int heat_prof_find( struct thread_data *data )
{
    int i;
    for (i = 0; i < HEAT_PROF_THREADS; i++) if (heat_prof_threads[i].data == data) return i;
    return -1;
}

void heat_prof_thread_gone( struct thread_data *data )
{
    unsigned int k;
    int i;

    pthread_mutex_lock( &heat_prof_lock );
    if ((i = heat_prof_find( data )) != -1)
    {
        for (k = 0; k < HEAT_PROF_TARGETS; k++)
        {
            if (heat_prof_targets[k].entry != i) continue;
            __atomic_store_n( &heat_prof_targets[k].thread, (pthread_t)0, __ATOMIC_RELEASE );
            heat_prof_targets[k].entry = -1;
            __atomic_store_n( &heat_prof_targets[k].pending, (pthread_t)0, __ATOMIC_RELEASE );
        }
        heat_prof_threads[i].data = NULL;
        heat_prof_threads[i].thread = (pthread_t)0;
    }
    pthread_mutex_unlock( &heat_prof_lock );
}

/* Once a second: the busiest unnamed thread that started in the exe over the last second is the
 * game thread, the next of those the worker, the busiest "bgfx - renderer" the render thread. A
 * target with no candidate keeps its thread. */
static void heat_prof_retarget(void)
{
    enum { KIND_OTHER, KIND_RENDER, KIND_EXE };
    static struct { int entry, kind; struct thread_data *data; pthread_t thread; unsigned long long utid, cpu, d; DWORD tid; }
        seen[HEAT_PROF_THREADS];
    unsigned long long best_d[HEAT_PROF_TARGETS] = { 0 };
    int best[HEAT_PROF_TARGETS] = { -1, -1, -1 };
    unsigned int n = 0, i, j, k;

    pthread_mutex_lock( &heat_prof_lock );
    for (i = 0; i < HEAT_PROF_THREADS; i++)
    {
        struct heat_prof_thread *e = &heat_prof_threads[i];
        ULONG_PTR start;

        if (!e->data || !e->thread) continue;
        if (!e->utid) pthread_threadid_np( e->thread, &e->utid );
        start = (ULONG_PTR)e->data->start;
        seen[n].entry = i;
        seen[n].kind = start >= heat_prof_exe_lo && start < heat_prof_exe_hi ? KIND_EXE : KIND_OTHER;
        seen[n].data = e->data;
        seen[n].thread = e->thread;
        seen[n].utid = e->utid;
        seen[n].tid = e->data->tid;
        n++;
    }
    pthread_mutex_unlock( &heat_prof_lock );

    /* the CPU times and names, without the lock */
    for (i = 0; i < n; i++)
    {
        struct proc_threadinfo pti;

        if (proc_pidinfo( getpid(), PROC_PIDTHREADID64INFO, seen[i].utid, &pti, sizeof(pti) ) != sizeof(pti))
        {
            seen[i].entry = -1;
            continue;
        }
        seen[i].cpu = pti.pth_user_time + pti.pth_system_time;
        if (!strncmp( pti.pth_name, "bgfx - renderer", 15 )) seen[i].kind = KIND_RENDER;
        else if (pti.pth_name[0]) seen[i].kind = KIND_OTHER;
    }

    pthread_mutex_lock( &heat_prof_lock );
    for (i = 0; i < n; i++)
    {
        struct heat_prof_thread *e;
        unsigned long long d;

        if (seen[i].entry == -1) continue;
        e = &heat_prof_threads[seen[i].entry];
        seen[i].d = 0;
        /* it may have exited, and its entry gone to another thread, since the scan */
        if (e->data != seen[i].data || e->thread != seen[i].thread) continue;
        d = seen[i].d = seen[i].cpu - e->cpu;
        e->cpu = seen[i].cpu;
        if (seen[i].kind == KIND_RENDER)
        {
            if (best[1] == -1 || d > best_d[1]) { best[1] = i; best_d[1] = d; }
        }
        else if (seen[i].kind != KIND_EXE) continue;
        else if (best[0] == -1 || d > best_d[0])
        {
            best[2] = best[0];
            best_d[2] = best_d[0];
            best[0] = i;
            best_d[0] = d;
        }
        else if (best[2] == -1 || d > best_d[2])
        {
            best[2] = i;
            best_d[2] = d;
        }
    }
    for (k = 0; k < HEAT_PROF_TARGETS; k++)
    {
        struct heat_prof_target *t = &heat_prof_targets[k];

        if (!(heat_prof_want & (1 << k)) || best[k] == -1) continue;
        if (t->entry == seen[best[k]].entry) continue;
        /* a thread is one target's at a time, so two targets never swap threads: each keeps its
         * own until it ends or a third thread leads */
        for (j = 0; j < HEAT_PROF_TARGETS; j++) if (heat_prof_targets[j].entry == seen[best[k]].entry) break;
        if (j < HEAT_PROF_TARGETS) continue;
        /* with busy, no handler writes the ring or the counts: a handler running now keeps the
         * old target until the next second */
        if (__atomic_exchange_n( &t->busy, 1, __ATOMIC_ACQUIRE )) continue;
        t->entry = seen[best[k]].entry;
        t->tid = seen[best[k]].tid;
        t->sent = t->skipped = t->recorded = t->dropped = 0;
        __atomic_store_n( &t->pending, (pthread_t)0, __ATOMIC_RELEASE );
        __atomic_store_n( &t->thread, seen[best[k]].thread, __ATOMIC_RELEASE );
        __atomic_store_n( &t->busy, 0, __ATOMIC_RELEASE );
    }
    /* each target's own thread's CPU time, whether or not it changed thread */
    for (k = 0; k < HEAT_PROF_TARGETS; k++)
    {
        heat_prof_targets[k].cpu_ms = 0;
        for (i = 0; i < n && heat_prof_targets[k].entry != -1; i++)
            if (seen[i].entry == heat_prof_targets[k].entry) heat_prof_targets[k].cpu_ms = seen[i].d / 1000000;
    }
    pthread_mutex_unlock( &heat_prof_lock );
}

/* a signal to each target whose last one was handled */
static void heat_prof_send(void)
{
    unsigned int k;

    pthread_mutex_lock( &heat_prof_lock );
    for (k = 0; k < HEAT_PROF_TARGETS; k++)
    {
        struct heat_prof_target *t = &heat_prof_targets[k];

        if (!t->thread) continue;
        if (__atomic_load_n( &t->pending, __ATOMIC_ACQUIRE ))
        {
            t->skipped++;
            continue;
        }
        __atomic_store_n( &t->pending, t->thread, __ATOMIC_RELEASE );
        if (pthread_kill( t->thread, SIGPROF )) __atomic_store_n( &t->pending, (pthread_t)0, __ATOMIC_RELEASE );
        else t->sent++;
    }
    pthread_mutex_unlock( &heat_prof_lock );
}

/***********************************************************************
 *  what the sampler thread reads and writes
 */

/* lld's CodeView record, as the debug directory points to it */
struct heat_prof_cv
{
    DWORD         sig;  /* RSDS */
    unsigned char guid[16];
    DWORD         age;
};

static BOOL heat_prof_fex_pinned( ULONG_PTR base, char *found, size_t size )
{
    IMAGE_DOS_HEADER dos;
    IMAGE_NT_HEADERS64 nt;
    IMAGE_DEBUG_DIRECTORY dbg;
    const IMAGE_DATA_DIRECTORY *dir;
    struct heat_prof_cv cv;
    unsigned int i, j;
    int len;

    snprintf( found, size, "no CodeView record" );
    if (!heat_prof_peek( heat_prof_own, &dos, base, sizeof(dos) ) || dos.e_magic != IMAGE_DOS_SIGNATURE) return FALSE;
    if (!heat_prof_peek( heat_prof_own, &nt, base + dos.e_lfanew, sizeof(nt) ) ||
        nt.Signature != IMAGE_NT_SIGNATURE || nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return FALSE;
    dir = &nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    for (i = 0; i < dir->Size / sizeof(dbg) && i < 16; i++)
    {
        if (!heat_prof_peek( heat_prof_own, &dbg, base + dir->VirtualAddress + i * sizeof(dbg), sizeof(dbg) )) break;
        if (dbg.Type != IMAGE_DEBUG_TYPE_CODEVIEW || !dbg.AddressOfRawData) continue;
        if (!heat_prof_peek( heat_prof_own, &cv, base + dbg.AddressOfRawData, sizeof(cv) ) ||
            cv.sig != 0x53445352 /* RSDS */)
            continue;
        for (j = 0, len = 0; j < 16; j++) len += snprintf( found + len, size - len, "%02x", cv.guid[j] );
        snprintf( found + len, size - len, "%x size %#x", (unsigned int)cv.age, (unsigned int)nt.OptionalHeader.SizeOfImage );
        return !memcmp( cv.guid, HEAT_FEX_PIN_GUID, 16 ) && cv.age == HEAT_FEX_PIN_AGE &&
               nt.OptionalHeader.SizeOfImage == HEAT_FEX_PIN_SIZE;
    }
    return FALSE;
}

static BOOL heat_prof_name_is( const WCHAR *name, unsigned int len, const char *want )
{
    unsigned int i;
    for (i = 0; i < len && want[i]; i++)
    {
        WCHAR c = name[i];
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        if (c != (unsigned char)want[i]) return FALSE;
    }
    return i == len && !want[i];
}

/* The PE modules, through heat_prof_peek(), since PE code changes the list under its own lock.
 * Writes them when their number changed. Returns FEX's base, Mach9 installs it as xtajit64.dll,
 * the name Wine loads it by, and as libarm64ecfex.dll. */
static ULONG_PTR heat_prof_modules(void)
{
    static char buf[1 << 18];
    static unsigned int last;
    LDR_DATA_TABLE_ENTRY m;
    LIST_ENTRY link;
    PEB_LDR_DATA *ldr;
    ULONG_PTR head, e, fex = 0;
    unsigned int n, len = 0;

    if (!heat_prof_peek( heat_prof_own, &ldr, (ULONG_PTR)&peb->LdrData, sizeof(ldr) ) || !ldr) return 0;
    head = (ULONG_PTR)&ldr->InLoadOrderModuleList;
    if (!heat_prof_peek( heat_prof_own, &link, head, sizeof(link) )) return 0;
    for (n = 0, e = (ULONG_PTR)link.Flink; e && e != head && n < 4096; n++, e = (ULONG_PTR)m.InLoadOrderLinks.Flink)
    {
        WCHAR name[260];
        unsigned int k, chars;

        if (!heat_prof_peek( heat_prof_own, &m, e, offsetof(LDR_DATA_TABLE_ENTRY, Flags) )) return 0;
        chars = min( m.BaseDllName.Length / sizeof(WCHAR), ARRAY_SIZE(name) );
        if (chars && heat_prof_peek( heat_prof_own, name, (ULONG_PTR)m.BaseDllName.Buffer, chars * sizeof(WCHAR) ) &&
            (heat_prof_name_is( name, chars, "xtajit64.dll" ) || heat_prof_name_is( name, chars, HEAT_FEX_DLL )))
            fex = (ULONG_PTR)m.DllBase;
        chars = min( m.FullDllName.Length / sizeof(WCHAR), ARRAY_SIZE(name) );
        if (!chars || !heat_prof_peek( heat_prof_own, name, (ULONG_PTR)m.FullDllName.Buffer, chars * sizeof(WCHAR) ))
            chars = 0;
        if (len < sizeof(buf) - 400)
        {
            len += snprintf( buf + len, sizeof(buf) - len, "M %p %#x ", m.DllBase, (unsigned int)m.SizeOfImage );
            for (k = 0; k < chars; k++) buf[len++] = name[k] >= 0x20 && name[k] < 0x7f && name[k] != ' ' ? name[k] : '?';
            buf[len++] = '\n';
        }
    }
    if (n != last && heat_prof_txt)
    {
        fprintf( heat_prof_txt, "L %.3f %u\n", heat_prof_seconds(), n );
        fwrite( buf, len, 1, heat_prof_txt );
        last = n;
    }
    return fex;
}

/* the memory region a sampled block lives in, once each */
static void heat_prof_region( ULONG_PTR addr )
{
    static ULONG_PTR lo[HEAT_PROF_REGIONS], hi[HEAT_PROF_REGIONS];
    static unsigned int count;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t n = VM_REGION_BASIC_INFO_COUNT_64;
    mach_vm_address_t start = addr;
    mach_vm_size_t size = 0;
    mach_port_t object = MACH_PORT_NULL;
    unsigned int i;

    for (i = 0; i < count; i++) if (addr >= lo[i] && addr < hi[i]) return;
    if (count == HEAT_PROF_REGIONS) return;
    if (mach_vm_region( mach_task_self(), &start, &size, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info,
                        &n, &object ) != KERN_SUCCESS || start > addr)
        return;
    if (object != MACH_PORT_NULL) mach_port_deallocate( mach_task_self(), object );
    lo[count] = start;
    hi[count++] = start + size;
    fprintf( heat_prof_txt, "J %#lx %#lx\n", (unsigned long)start, (unsigned long)(start + size) );
}

/* one of FEX's RIP entries (vl64pair::Decode): the host and guest deltas to the next entry, and
 * its size, 0 if <avail> bytes don't hold it */
static size_t heat_prof_entry( const unsigned char *p, size_t avail, unsigned long long *arm, unsigned long long *x86 )
{
    int a32, x32;

    if (!avail) return 0;
    if (!(p[0] & 0x80))  /* 1 byte: host (bits 0-3 + 1) * 4, guest bits 4-6 + 1 */
    {
        *arm = ((p[0] & 0xf) + 1) * 4;
        *x86 = ((p[0] >> 4) & 7) + 1;
        return 1;
    }
    if ((p[0] >> 6) == 2)  /* 2 bytes: guest a signed 6 bits, host a signed byte * 4 */
    {
        if (avail < 2) return 0;
        *x86 = (unsigned long long)(long long)((signed char)(p[0] << 2) >> 2);
        *arm = (unsigned long long)((long long)(signed char)p[1] * 4);
        return 2;
    }
    if (p[0] == 0xc0)  /* 9 bytes: two signed 32-bit words */
    {
        if (avail < 9) return 0;
        memcpy( &a32, p + 1, 4 );
        memcpy( &x32, p + 5, 4 );
        *arm = (unsigned long long)(long long)a32;
        *x86 = (unsigned long long)(long long)x32;
        return 9;
    }
    if (avail < 17) return 0;  /* 17 bytes: two 64-bit words */
    memcpy( arm, p + 1, 8 );
    memcpy( x86, p + 9, 8 );
    return 17;
}

/* whether the block at <header> still has the header and tail the handler copied: FEX may have
 * freed it since the sample, and given its memory to another block */
static BOOL heat_prof_same_block( const struct heat_prof_rec *rec, ULONG_PTR header )
{
    unsigned char tail[HEAT_FEX_TAIL_ID];
    unsigned int to_tail;

    return heat_prof_peek( heat_prof_own, &to_tail, header + HEAT_FEX_HEADER_TAIL, sizeof(to_tail) ) &&
           to_tail == rec->to_tail && heat_prof_peek( heat_prof_own, tail, header + to_tail, sizeof(tail) ) &&
           !memcmp( tail, rec->tail, sizeof(tail) );
}

/* RestoreRIPFromHostPC on a sample: the block FEX entered last. The handler copied its header and
 * tail; the RIP entries are read here, between two checks that the block is still the same. */
static void heat_prof_resolve( struct heat_prof_rec *rec )
{
    unsigned char chunk[HEAT_PROF_CHUNK];
    unsigned long long header, pc = rec->regs[HP_REG_PC], size, rip, x86_start, guest_size, host, arm, x86;
    unsigned int entries, entries_at, i, have = 0, used = 0;
    ULONG_PTR at;

    if (!(rec->flags & HP_SIM) || !(rec->flags & HP_STATE)) return;
    memcpy( &header, rec->state + HEAT_FEX_STATE_BLOCK, sizeof(header) );
    if (!header)
    {
        rec->flags |= HP_FEX;
        return;
    }
    if (!(rec->flags & HP_TAIL))
    {
        rec->flags |= HP_GONE;
        return;
    }
    memcpy( &size, rec->tail + HEAT_FEX_TAIL_SIZE, 8 );
    memcpy( &rip, rec->tail + HEAT_FEX_TAIL_RIP, 8 );
    x86_start = rip;
    memcpy( &guest_size, rec->tail + HEAT_FEX_TAIL_GUEST_SIZE, 8 );
    memcpy( &entries, rec->tail + HEAT_FEX_TAIL_ENTRIES, 4 );
    memcpy( &entries_at, rec->tail + HEAT_FEX_TAIL_ENTRIES_AT, 4 );
    if (size > (128 << 20) || guest_size > (1 << 20) || entries > (1 << 20))
    {
        rec->flags |= HP_GONE;
        return;
    }
    if (pc < header || pc >= header + size)
    {
        rec->flags |= HP_FEX;
        return;
    }
    if (!heat_prof_same_block( rec, header ))
    {
        rec->flags |= HP_GONE;
        return;
    }
    heat_prof_region( header );
    host = header;
    at = header + rec->to_tail + entries_at;
    for (i = 0; i < entries; i++)
    {
        size_t n = heat_prof_entry( chunk + used, have - used, &arm, &x86 );
        if (!n)
        {
            /* refill from the first byte not used: a chunk, or near the end of readable memory
             * the one entry, whose first byte gives its size */
            at += used;
            have = HEAT_PROF_CHUNK;
            used = 0;
            if (!heat_prof_peek( heat_prof_own, chunk, at, have ))
            {
                if (heat_prof_peek( heat_prof_own, chunk, at, 1 ))
                    have = !(chunk[0] & 0x80) ? 1 : (chunk[0] >> 6) == 2 ? 2 : chunk[0] == 0xc0 ? 9 : 17;
                if (have == HEAT_PROF_CHUNK || !heat_prof_peek( heat_prof_own, chunk, at, have ))
                {
                    rec->flags |= HP_GONE;
                    return;
                }
            }
            if (!(n = heat_prof_entry( chunk, have, &arm, &x86 )))
            {
                rec->flags |= HP_GONE;
                return;
            }
        }
        used += n;
        if (pc < host + arm) break;
        host += arm;
        rip += x86;
    }
    if (rip - x86_start > guest_size || !heat_prof_same_block( rec, header ))
    {
        rec->flags |= HP_GONE;
        return;
    }
    rec->block = x86_start;
    rec->x86 = rip;
    rec->flags |= HP_X86;
}

/* names a unix address once, with dladdr; PE and translated code have no dyld image */
static void heat_prof_name( unsigned long long addr )
{
    unsigned int h, k;
    Dl_info info;

    if (!addr || !heat_prof_named) return;
    h = (unsigned int)((addr * 0x9e3779b97f4a7c15ull) >> 48) & (HEAT_PROF_NAMED - 1);
    for (k = 0; k < 64; k++, h = (h + 1) & (HEAT_PROF_NAMED - 1))
    {
        if (heat_prof_named[h] == addr) return;
        if (!heat_prof_named[h]) break;
    }
    if (k == 64) return;
    heat_prof_named[h] = addr;
    memset( &info, 0, sizeof(info) );
    if (!dladdr( (void *)(ULONG_PTR)addr, &info ) || !info.dli_fname) return;
    fprintf( heat_prof_txt, "U %#llx %p %s %s %p\n", addr, info.dli_fbase, info.dli_fname,
             info.dli_sname ? info.dli_sname : "?", info.dli_saddr );
}

/* every sample handled since the last call, resolved and written */
static void heat_prof_drain(void)
{
    unsigned int k;

    pthread_mutex_lock( &heat_prof_file_lock );
    for (k = 0; k < HEAT_PROF_TARGETS; k++)
    {
        struct heat_prof_target *t = &heat_prof_targets[k];
        unsigned int w = __atomic_load_n( &t->write, __ATOMIC_ACQUIRE );

        while (t->read != w)
        {
            struct heat_prof_rec *rec = &t->ring[t->read % HEAT_PROF_RING];
            heat_prof_resolve( rec );
            if (!(rec->flags & (HP_X86 | HP_FEX | HP_GONE)))
            {
                heat_prof_name( rec->regs[HP_REG_PC] );
                heat_prof_name( rec->regs[HP_REG_LR] );
            }
            fwrite( rec, offsetof(struct heat_prof_rec, stack) + rec->stack_len, 1, heat_prof_bin );
            __atomic_store_n( &t->read, t->read + 1, __ATOMIC_RELEASE );
        }
    }
    pthread_mutex_unlock( &heat_prof_file_lock );
}

static void heat_prof_counts(void)
{
    unsigned int k;

    pthread_mutex_lock( &heat_prof_file_lock );
    for (k = 0; k < HEAT_PROF_TARGETS; k++)
    {
        struct heat_prof_target *t = &heat_prof_targets[k];
        if (!t->tid) continue;
        fprintf( heat_prof_txt, "C %.3f %s %04x sent %llu recorded %llu skipped %llu dropped %llu late %u cpu_ms %llu\n",
                 heat_prof_seconds(), heat_prof_target_names[k], (unsigned int)t->tid, t->sent,
                 __atomic_load_n( &t->recorded, __ATOMIC_RELAXED ), t->skipped,
                 __atomic_load_n( &t->dropped, __ATOMIC_RELAXED ), __atomic_load_n( &heat_prof_late, __ATOMIC_RELAXED ),
                 t->cpu_ms );
    }
    fflush( heat_prof_bin );
    fflush( heat_prof_txt );
    pthread_mutex_unlock( &heat_prof_file_lock );
}

static void heat_prof_atexit(void)
{
    heat_prof_drain();
    heat_prof_counts();
}

static void heat_prof_sleep_until( unsigned long long when )
{
    unsigned long long now = heat_now();
    struct timespec ts;

    if (when <= now) return;
    ts.tv_sec = (when - now) / 1000000000;
    ts.tv_nsec = (when - now) % 1000000000;
    nanosleep( &ts, NULL );
}

static void *heat_prof_main( void *arg )
{
    const unsigned long long tick = 1000000000ull / heat_prof_hz;
    unsigned long long next, second = 0;
    struct sigaction sa;
    ULONG_PTR fex;
    char found[128];
    BOOL pinned;

    if (!(heat_prof_bin = fopen( heat_prof_bin_path, "wb" )) || !(heat_prof_txt = fopen( heat_prof_txt_path, "w" )))
        return NULL;
    fprintf( heat_prof_bin, "HEATPROF %u rec %u stack %u hz %u timebase %u/%u\n", HEAT_PROF_VERSION,
             (unsigned int)offsetof(struct heat_prof_rec, stack), HEAT_PROF_STACK, heat_prof_hz,
             heat_prof_timebase.numer, heat_prof_timebase.denom );

    /* FEX is loaded by the PE side's process init, after heat_init() */
    while (!(fex = heat_prof_modules())) heat_prof_sleep_until( heat_now() + 100000000 );
    pinned = heat_prof_fex_pinned( fex, found, sizeof(found) );
    fprintf( heat_prof_txt, "F %.3f fex %s %s\n", heat_prof_seconds(), found, pinned ? "pinned" : "refused" );
    fflush( heat_prof_txt );
    if (!pinned) return NULL;

    if (peb->ImageBaseAddress)
    {
        IMAGE_DOS_HEADER dos;
        IMAGE_NT_HEADERS64 nt;
        ULONG_PTR base = (ULONG_PTR)peb->ImageBaseAddress;

        if (heat_prof_peek( heat_prof_own, &dos, base, sizeof(dos) ) &&
            heat_prof_peek( heat_prof_own, &nt, base + dos.e_lfanew, sizeof(nt) ))
        {
            heat_prof_exe_lo = base;
            heat_prof_exe_hi = base + nt.OptionalHeader.SizeOfImage;
        }
    }

    memset( &sa, 0, sizeof(sa) );
    sa.sa_sigaction = heat_prof_handler;
    sa.sa_mask = server_block_set;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    if (sigaction( SIGPROF, &sa, NULL ) == -1) return NULL;
    atexit( heat_prof_atexit );

    for (next = heat_now();; next += tick)
    {
        unsigned long long now = heat_now();

        if (now - second >= 1000000000ull)
        {
            heat_prof_retarget();
            /* heat_prof_own and the text file are heat_prof_atexit()'s too from here on */
            pthread_mutex_lock( &heat_prof_file_lock );
            heat_prof_modules();
            pthread_mutex_unlock( &heat_prof_file_lock );
            heat_prof_counts();
            second = now;
        }
        heat_prof_drain();
        heat_prof_send();
        if (next + tick < now) next = now;  /* behind: no burst to catch up */
        heat_prof_sleep_until( next + tick );
    }
    return NULL;
}

/***********************************************************************
 *  FEX's counters (FEX_PROFILESTATS=1)
 *
 * heat_dump() calls this every 5 seconds and at exit, under its lock: the game thread's FEX
 * counters, as the difference from the last call for the same thread.
 */
int heat_prof_fex_stats( char *buf, int size )
{
    static const unsigned int at[] = { HEAT_FEX_STATS_JIT_TIME, HEAT_FEX_STATS_JIT_COUNT, HEAT_FEX_STATS_MISS,
                                       HEAT_FEX_STATS_RLOCK, HEAT_FEX_STATS_WLOCK, HEAT_FEX_STATS_SMC,
                                       HEAT_FEX_STATS_SIGBUS, HEAT_FEX_STATS_FLOAT, HEAT_FEX_STATS_SIG_TIME };
    static unsigned long long prev[ARRAY_SIZE(at)], prev_t;
    static DWORD prev_tid;
    unsigned long long v[ARRAY_SIZE(at)], d[ARRAY_SIZE(at)], now = mach_absolute_time();
    DWORD tid = __atomic_load_n( &heat_prof_targets[0].tid, __ATOMIC_RELAXED );
    const unsigned char *base, *slot = NULL;
    unsigned int used = 0, off, i, slot_tid = 0, slot_size = 0;
    char name[32];
    struct stat st;
    int fd, len = 0;
    double ms = (double)heat_prof_timebase.numer / heat_prof_timebase.denom / 1e6;

    if (!tid || size <= 0) return 0;
    snprintf( name, sizeof(name), "fex-%d-stats", getpid() );
    if ((fd = shm_open( name, O_RDONLY, 0 )) == -1) return 0;
    if (fstat( fd, &st ) || st.st_size < HEAT_FEX_STATS_HEADER ||
        (base = mmap( NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0 )) == MAP_FAILED)
    {
        close( fd );
        return 0;
    }
    close( fd );
    memcpy( &slot_size, base + HEAT_FEX_STATS_H_SLOT, 2 );
    slot_size &= 0xffff;
    memcpy( &used, base + HEAT_FEX_STATS_H_SIZE, 4 );
    if (used > st.st_size) used = st.st_size;
    if (base[HEAT_FEX_STATS_H_VERSION] == HEAT_FEX_STATS_VERSION && slot_size == HEAT_FEX_STATS_SLOT)
    {
        for (off = HEAT_FEX_STATS_HEADER; off + HEAT_FEX_STATS_SLOT <= used; off += HEAT_FEX_STATS_SLOT)
        {
            memcpy( &slot_tid, base + off + HEAT_FEX_STATS_TID, 4 );
            if (slot_tid == tid) { slot = base + off; break; }
        }
    }
    else len = snprintf( buf, size, "fex stats version %u slot %u, not %u and %u\n", base[HEAT_FEX_STATS_H_VERSION],
                         slot_size, HEAT_FEX_STATS_VERSION, HEAT_FEX_STATS_SLOT );
    if (slot)
    {
        BOOL same = tid == prev_tid;
        for (i = 0; i < ARRAY_SIZE(at); i++)
        {
            memcpy( &v[i], slot + at[i], 8 );
            d[i] = same ? v[i] - prev[i] : v[i];
            prev[i] = v[i];
        }
        /* the first line for a thread has its totals, the others what changed since the last */
        if (same) len = snprintf( buf, size, "fex thread %04x seconds %.1f", (unsigned int)tid, (now - prev_t) * ms / 1000 );
        else len = snprintf( buf, size, "fex thread %04x total", (unsigned int)tid );
        if (len < size)
            len += snprintf( buf + len, size - len, " jit_ms %.3f jit %llu miss %llu rlock_ms %.3f wlock_ms %.3f "
                             "smc %llu sigbus %llu float %llu signal_ms %.3f\n", d[0] * ms, d[1], d[2], d[3] * ms,
                             d[4] * ms, d[5], d[6], d[7], d[8] * ms );
        prev_tid = tid;
        prev_t = now;
    }
    /* FEX gives a slot to each thread at its start while it has room; on macOS it can't grow the
     * object past its first size */
    else if (!len) len = snprintf( buf, size, "fex thread %04x has no slot of %u\n", (unsigned int)tid,
                                   used > HEAT_FEX_STATS_HEADER ? (used - HEAT_FEX_STATS_HEADER) / HEAT_FEX_STATS_SLOT : 0 );
    munmap( (void *)base, st.st_size );
    return len < size ? len : size - 1;
}

/***********************************************************************
 *  heat_prof_init
 *
 * heat_init() calls it once, while this is the only thread. Nothing here installs a handler: the
 * sampler thread does that once FEX checks out.
 */
static BOOL heat_prof_pipe( int fd[2] )
{
    if (pipe( fd ) == -1) return FALSE;
    fcntl( fd[0], F_SETFD, FD_CLOEXEC );
    fcntl( fd[1], F_SETFD, FD_CLOEXEC );
    fcntl( fd[0], F_SETFL, O_NONBLOCK );
    fcntl( fd[1], F_SETFL, O_NONBLOCK );
    return TRUE;
}

BOOL heat_prof_init( const char *dir )
{
    const char *p;
    struct heat_prof_rec *rings;
    sigset_t all, old;
    pthread_t thread;
    unsigned int k;
    int ret;

    if ((p = getenv( "WINE_HEAT_PROF_HZ" )) && *p)
    {
        char *end;
        unsigned long hz = strtoul( p, &end, 10 );
        if (!*end && hz >= 10 && hz <= 10000) heat_prof_hz = hz;
    }
    for (p = getenv( "WINE_HEAT_PROF_THREADS" ); p && *p; p += strcspn( p, "," ), p += (*p == ','))
    {
        size_t n = strcspn( p, "," );
        if (n == 6 && !strncmp( p, "render", 6 )) heat_prof_want |= 1 << 1;
        else if (n == 6 && !strncmp( p, "worker", 6 )) heat_prof_want |= 1 << 2;
    }
    rings = mmap( NULL, HEAT_PROF_TARGETS * HEAT_PROF_RING * sizeof(*rings), PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (rings == MAP_FAILED) return FALSE;
    heat_prof_named = mmap( NULL, HEAT_PROF_NAMED * sizeof(*heat_prof_named), PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANON, -1, 0 );
    if (heat_prof_named == MAP_FAILED) heat_prof_named = NULL;
    if (!heat_prof_pipe( heat_prof_own )) return FALSE;
    for (k = 0; k < HEAT_PROF_TARGETS; k++)
    {
        heat_prof_targets[k].entry = -1;
        heat_prof_targets[k].ring = rings + k * HEAT_PROF_RING;
        if (!heat_prof_pipe( heat_prof_targets[k].pipe )) return FALSE;
    }
    mach_timebase_info( &heat_prof_timebase );
    snprintf( heat_prof_bin_path, sizeof(heat_prof_bin_path), "%s/heat-prof-%d.bin", dir, getpid() );
    snprintf( heat_prof_txt_path, sizeof(heat_prof_txt_path), "%s/heat-prof-%d.txt", dir, getpid() );

    /* not a Wine thread: no signal of Wine's may land on it */
    sigfillset( &all );
    pthread_sigmask( SIG_SETMASK, &all, &old );
    if (!(ret = pthread_create( &thread, NULL, heat_prof_main, NULL ))) pthread_detach( thread );
    pthread_sigmask( SIG_SETMASK, &old, NULL );
    return !ret;
}
