/*
 * GW2 on Mac: the game thread's sampler under FEX (WINE_HEAT_PROF, jdrly/gw2-on-mac#43), heatprof.c
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

#ifndef __NTDLL_UNIX_HEATPROF_H
#define __NTDLL_UNIX_HEATPROF_H

/***********************************************************************
 *  FEX, as the sampler reads it
 *
 * Every offset and value the sampler takes from FEX's memory, read from FEX at 9d4fa4b46b1 (the
 * commit Mach9 0.3.1 pins). FEX's source isn't in this tree, so nothing checks these at build
 * time. The sampler refuses to start unless the process runs that FEX: libarm64ecfex.dll's
 * CodeView record and image size must be the ones below.
 */

/* Mach9 v0.3.1's lib/wine/aarch64-windows/libarm64ecfex.dll (sha256 99eb322e...): its CodeView
 * record's GUID and age (lld's build id, a hash of the file) and SizeOfImage */
#define HEAT_FEX_PIN_GUID "\x65\x6e\xe1\xa2\x70\x27\x23\x3b\x4c\x4c\x44\x20\x50\x44\x42\x2e"
#define HEAT_FEX_PIN_AGE  1
#define HEAT_FEX_PIN_SIZE 0x426000
#define HEAT_FEX_DLL      "libarm64ecfex.dll"

/* The thread's state is FEXCore::Core::CpuStateFrame, whose first member is CPUState
 * (FEXCore/include/FEXCore/Core/CoreState.h:439-440, :485). The CPU area's EmulatorData[0] points
 * to it (Source/Windows/ARM64EC/Module.cpp:133-135, set at :1554). In CPUState (CoreState.h:90-109): */
#define HEAT_FEX_STATE_BLOCK     0    /* InlineJITBlockHeader: the JITCodeHeader of the block the
                                       * thread entered last, stored by every block's entry
                                       * (FEXCore/Source/Interface/Core/JIT/JIT.cpp:816-819) */
#define HEAT_FEX_STATE_RIP       24   /* rip, not kept current inside translated code */
#define HEAT_FEX_STATE_GREGS     32   /* gregs[16], rax rcx rdx rbx rsp rbp rsi rdi r8-r15 */
#define HEAT_FEX_STATE_CALLRET   176  /* callret_sp, stored on the way into native code
                                       * (FEXCore/Source/Interface/Core/Dispatcher/Dispatcher.cpp:162) */
#define HEAT_FEX_STATE_COPY      256  /* bytes of CPUState a sample copies */

/* The call and return stack: each x86 call pushes { x86 return address, host return address },
 * or two zeros when FEX made no return block (FEXCore/Source/Interface/Core/JIT/BranchOps.cpp:222-230).
 * The first word at callret_sp is the x86 return address of the last call. */
#define HEAT_FEX_CALLRET_COPY    16

/* A block: JITCodeHeader at the block's start, JITCodeTail at OffsetToBlockTail from it
 * (FEXCore/Source/Interface/Core/CPUBackend.h:64-99), the RIP entries at OffsetToRIPEntries from
 * the tail, each a vl64pair of host and guest deltas (FEXCore/Source/Utils/variable_length_integer.h:169-393).
 * RestoreRIPFromHostPC (FEXCore/Source/Interface/Core/Core.cpp:151-187) walks them. */
#define HEAT_FEX_HEADER_TAIL     0    /* uint32_t OffsetToBlockTail */
#define HEAT_FEX_TAIL_SIZE       0    /* size_t Size: the block is [header, header + Size) */
#define HEAT_FEX_TAIL_RIP        8    /* uint64_t RIP: the x86 address the block starts at */
#define HEAT_FEX_TAIL_GUEST_SIZE 16   /* size_t GuestSize */
#define HEAT_FEX_TAIL_ENTRIES    24   /* uint32_t NumberOfRIPEntries */
#define HEAT_FEX_TAIL_ENTRIES_AT 28   /* uint32_t OffsetToRIPEntries, from the tail */
#define HEAT_FEX_TAIL_COPY       40   /* sizeof(JITCodeTail) */

/* FEX_PROFILESTATS=1: the shared memory object fex-<pid>-stats
 * (Source/Windows/UnixLib/FEXUnixLib.cpp:154-157), a ThreadStatsHeader and then ThreadStats slots
 * (FEXCore/include/FEXCore/Utils/SHMStats.h:45-78). A slot with TID 0 is free. Times are in
 * CNTVCT_EL0 ticks, the counter mach_absolute_time() reads. */
#define HEAT_FEX_STATS_VERSION   2
#define HEAT_FEX_STATS_HEADER    64   /* sizeof(ThreadStatsHeader) */
#define HEAT_FEX_STATS_SLOT      112  /* sizeof(ThreadStats), also ThreadStatsHeader::ThreadStatsSize */
#define HEAT_FEX_STATS_H_VERSION 0    /* uint8_t Version */
#define HEAT_FEX_STATS_H_SLOT    2    /* uint16_t ThreadStatsSize */
#define HEAT_FEX_STATS_H_SIZE    56   /* uint32_t Size: the bytes in use */
#define HEAT_FEX_STATS_TID       4    /* uint32_t TID: the Windows thread id */
#define HEAT_FEX_STATS_JIT_TIME  8    /* AccumulatedJITTime: compiling */
#define HEAT_FEX_STATS_SIG_TIME  16   /* AccumulatedSignalTime */
#define HEAT_FEX_STATS_SIGBUS    24   /* AccumulatedSIGBUSCount */
#define HEAT_FEX_STATS_SMC       32   /* AccumulatedSMCCount: writes to pages with translated code */
#define HEAT_FEX_STATS_FLOAT     40   /* AccumulatedFloatFallbackCount */
#define HEAT_FEX_STATS_MISS      48   /* AccumulatedCacheMissCount: lookup cache misses */
#define HEAT_FEX_STATS_RLOCK     56   /* AccumulatedCacheReadLockTime */
#define HEAT_FEX_STATS_WLOCK     64   /* AccumulatedCacheWriteLockTime */
#define HEAT_FEX_STATS_JIT_COUNT 72   /* AccumulatedJITCount: blocks compiled */

/***********************************************************************
 *  the sample file, heat-prof-<pid>.bin
 *
 * One text line, then records back to back:
 *
 *   HEATPROF 1 rec <fixed bytes> stack <max stack bytes> hz <rate> timebase <numer>/<denom>\n
 *
 * Each record is struct heat_prof_rec, little-endian with natural alignment, followed by
 * stack_len bytes of the guest's stack. A reader takes the record's size from the line, not from
 * this header. Times are mach_absolute_time() ticks; nanoseconds = ticks * numer / denom.
 */
#define HEAT_PROF_VERSION 1
#define HEAT_PROF_STACK   2048

/* flags, from the handler */
#define HP_SIM      0x0001  /* the CPU area's InSimulation: in translated code, or FEX working for it */
#define HP_SYSCB    0x0002  /* the CPU area's InSyscallCallback */
#define HP_SYSCALL  0x0004  /* in Wine's unix side: sp on the thread's kernel stack */
#define HP_STATE    0x0008  /* state[] holds the first 256 bytes of FEX's CPUState */
#define HP_CALLRET  0x0010  /* callret[] holds the two words at callret_sp */
/* flags, from the sampler thread */
#define HP_X86      0x0100  /* x86 and block hold the guest address of pc in the block it entered last */
#define HP_FEX      0x0200  /* HP_SIM and pc outside that block: FEX's own work (dispatcher, compile, lookup) */
#define HP_GONE     0x0400  /* HP_SIM, but the block's header or tail couldn't be read, or made no sense */

/* regs[] */
#define HP_REG_FP   29
#define HP_REG_LR   30
#define HP_REG_SP   31
#define HP_REG_PC   32
#define HP_REG_CPSR 33
#define HP_REGS     34

struct heat_prof_rec
{
    unsigned long long t;          /* mach ticks */
    unsigned int       tid;        /* Windows thread id */
    unsigned short     target;     /* 0 game, 1 render, 2 worker */
    unsigned short     flags;      /* HP_* */
    unsigned long long regs[HP_REGS]; /* x0-x28, fp, lr, sp, pc, cpsr, raw from the signal's context */
    unsigned long long sys_sp;     /* HP_SYSCALL: the syscall frame's sp and pc, the PE side's */
    unsigned long long sys_pc;
    unsigned long long x86;        /* HP_X86: pc's x86 address */
    unsigned long long block;      /* HP_X86: the x86 address the block starts at */
    unsigned long long stack_at;   /* the guest stack's address that stack[] starts at */
    unsigned int       stack_len;  /* bytes of stack[] */
    unsigned int       pad;
    unsigned long long callret[2]; /* HP_CALLRET */
    unsigned char      state[HEAT_FEX_STATE_COPY]; /* HP_STATE */
    unsigned char      stack[HEAT_PROF_STACK];     /* only stack_len bytes go to the file */
};

/***********************************************************************
 *  hooks
 */
struct thread_data;
extern BOOL heat_prof_init( const char *dir );
extern int heat_prof_fex_stats( char *buf, int size );
extern void heat_prof_thread_lock(void);
extern void heat_prof_thread_add( struct thread_data *data, pthread_t thread );
extern void heat_prof_thread_gone( struct thread_data *data );

/* spawn_thread, before pthread_create: takes the list's lock */
static inline void heat_prof_spawning(void)
{
    if (heat_mode & HEAT_PROF) heat_prof_thread_lock();
}

/* spawn_thread, after pthread_create: adds the thread, 0 if it failed, and releases the lock */
static inline void heat_prof_spawned( struct thread_data *data, pthread_t thread )
{
    if (heat_mode & HEAT_PROF) heat_prof_thread_add( data, thread );
}

/* pthread_exit_wrapper: the sampler must not signal this thread any more */
static inline void heat_prof_exiting( struct thread_data *data )
{
    if (heat_mode & HEAT_PROF) heat_prof_thread_gone( data );
}

#endif /* __NTDLL_UNIX_HEATPROF_H */
