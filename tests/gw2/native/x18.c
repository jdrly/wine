/* gw2 (jdrly/gw2-on-mac#382): what one switch of macOS's custom x18 ABI costs, built natively and
 * run without Wine. WineHQ's f858e69 turns the ABI off on every entry into unix code and on again
 * on the way back, one pair of os_set_custom_x18_abi_enabled() calls per system call, and tests
 * os_custom_x18_abi_enabled() in every signal handler. athei's branch turns it on once per thread
 * (enable_custom_x18_abi() in dlls/ntdll/unix/signal_arm64.c) and leaves it on.
 *
 *   clang -std=gnu23 -O2 -Wall -Werror -mmacosx-version-min=26.4 -o x18 tests/gw2/native/x18.c
 *   codesign -s - -f x18 && ./x18
 *   /usr/libexec/PlistBuddy -c 'Add :com.apple.security.custom-x18-abi-toggle bool true' x18.plist
 *   codesign -s - -f --entitlements x18.plist x18 && ./x18
 *
 * The SDK's os/arch/arm64.h allows no macOS library call while the ABI is on, except the two
 * functions themselves, and a call with the state the thread already has aborts the process. So
 * the clock is read with the ABI off, before the first pair and after the last, and nothing is
 * printed until it is off again. It runs at QOS_CLASS_USER_INTERACTIVE, so the scheduler prefers
 * a performance core. These are the calls alone in a tight loop, so the cost in a dispatcher is
 * at least this.
 *
 *   mode     os_custom_x18_abi_enabled() after the first true, and again after the sleep below
 *   x18      a value in x18 before a 10 ms select() made with a raw svc, and after it, with the
 *            ABI on and, as the control, off. The thread is off its core while it sleeps, and
 *            with the ABI off macOS doesn't keep x18
 *   pair     ns per os_set_custom_x18_abi_enabled(false) and (true), PAIRS of them in a loop,
 *            best and median of RUNS runs
 *   get      ns per os_custom_x18_abi_enabled() alone, with the ABI on and off
 *
 * On an M1 with macOS 26.7.3, ad hoc signed without the entitlement, the mode reads on after true
 * and off after the sleep, and x18 is lost: the thread loses the mode while it sleeps, and a
 * call with false then aborts the process ("attempted to switch out of already disabled custom
 * x18 ABI mode"). So the program makes that call only while the mode reads on.
 *
 * Exits 0 when the ABI was on and kept x18 and prints the costs, 1 when it wasn't on or didn't
 * keep x18. */
#include <mach/mach_time.h>
#include <os/arch/arm64.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <unistd.h>

#define PAIRS 10000000
#define GETS  10000000
#define RUNS  5

#define X18_MAGIC 0x5f3759df12345678ull

static mach_timebase_info_data_t timebase;

static double ns_since( uint64_t start, uint64_t end, uint64_t count )
{
    return (double)(end - start) * timebase.numer / timebase.denom / count;
}

/* Puts value in x18, sleeps 10 ms in select() through svc, not libc, and returns x18. The kernel
 * switches to another thread during the sleep. clang reserves x18 on Darwin, so no compiled code
 * around it reads or writes it. */
static uint64_t x18_across_sleep( uint64_t value )
{
    struct timeval tv = { 0, 10000 };
    uint64_t out;

    __asm__ volatile( "mov x18, %[v]\n\t"
                      "mov x0, #0\n\t"
                      "mov x1, #0\n\t"
                      "mov x2, #0\n\t"
                      "mov x3, #0\n\t"
                      "mov x4, %[tv]\n\t"
                      "mov x16, %[nr]\n\t"
                      "svc #0x80\n\t"
                      "mov %[out], x18\n\t"
                      "mov x18, xzr\n\t"
                      : [out] "=&r" (out)
                      : [v] "r" (value), [tv] "r" (&tv), [nr] "i" (SYS_select)
                      : "x0", "x1", "x2", "x3", "x4", "x16", "cc", "memory" );
    return out;
}

static int cmp_double( const void *a, const void *b )
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main( void )
{
    double pair[RUNS], get_on[RUNS], get_off[RUNS];
    struct { bool after_true, after_sleep; uint64_t x18; } check = { 0 };
    uint64_t x18_off, t0, t1;
    struct utsname un;
    char cpu[128] = "?", os[64] = "?";
    size_t len;
    int run, i;
    volatile bool sink;

    mach_timebase_info( &timebase );
    pthread_set_qos_class_self_np( QOS_CLASS_USER_INTERACTIVE, 0 );
    len = sizeof(cpu);
    sysctlbyname( "machdep.cpu.brand_string", cpu, &len, NULL, 0 );
    len = sizeof(os);
    sysctlbyname( "kern.osproductversion", os, &len, NULL, 0 );
    uname( &un );
    printf( "gw2 test x18: %s, macOS %s (%s)\n", cpu, os, un.release );
    fflush( stdout );

    os_set_custom_x18_abi_enabled( true );
    check.after_true = os_custom_x18_abi_enabled();
    if (check.after_true)
    {
        check.x18 = x18_across_sleep( X18_MAGIC );
        check.after_sleep = os_custom_x18_abi_enabled();
        if (check.after_sleep) os_set_custom_x18_abi_enabled( false );
    }
    x18_off = x18_across_sleep( X18_MAGIC );
    printf( "gw2 test x18: mode after true: %s, after the sleep: %s\n", check.after_true ? "on" : "off",
            !check.after_true ? "-" : check.after_sleep ? "on" : "off" );
    printf( "gw2 test x18: x18 across 10 ms: on %#llx (%s), off %#llx (%s)\n",
            (unsigned long long)check.x18, check.x18 == X18_MAGIC ? "kept" : "lost",
            (unsigned long long)x18_off, x18_off == X18_MAGIC ? "kept" : "lost" );
    if (!check.after_true || !check.after_sleep || check.x18 != X18_MAGIC)
    {
        printf( "gw2 test x18: the ABI wasn't on\n" );
        return 1;
    }
    fflush( stdout );

    for (run = 0; run < RUNS; run++)
    {
        t0 = mach_absolute_time();
        os_set_custom_x18_abi_enabled( true );
        for (i = 0; i < PAIRS; i++)
        {
            os_set_custom_x18_abi_enabled( false );
            os_set_custom_x18_abi_enabled( true );
        }
        os_set_custom_x18_abi_enabled( false );
        t1 = mach_absolute_time();
        pair[run] = ns_since( t0, t1, PAIRS + 1 );

        t0 = mach_absolute_time();
        os_set_custom_x18_abi_enabled( true );
        for (i = 0; i < GETS; i++) sink = os_custom_x18_abi_enabled();
        os_set_custom_x18_abi_enabled( false );
        t1 = mach_absolute_time();
        get_on[run] = ns_since( t0, t1, GETS );

        t0 = mach_absolute_time();
        for (i = 0; i < GETS; i++) sink = os_custom_x18_abi_enabled();
        t1 = mach_absolute_time();
        get_off[run] = ns_since( t0, t1, GETS );
    }
    (void)sink;

    qsort( pair, RUNS, sizeof(*pair), cmp_double );
    qsort( get_on, RUNS, sizeof(*get_on), cmp_double );
    qsort( get_off, RUNS, sizeof(*get_off), cmp_double );
    printf( "gw2 test x18: pair: %.2f ns best, %.2f ns median (%d x %d pairs)\n",
            pair[0], pair[RUNS / 2], RUNS, PAIRS );
    printf( "gw2 test x18: get on: %.2f ns best, %.2f ns median\n", get_on[0], get_on[RUNS / 2] );
    printf( "gw2 test x18: get off: %.2f ns best, %.2f ns median\n", get_off[0], get_off[RUNS / 2] );

    printf( "gw2 test x18: the ABI was on\n" );
    return 0;
}
