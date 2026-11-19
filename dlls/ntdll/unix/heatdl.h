/*
 * GW2 on Mac: dlopen, dlsym and dlclose with the server's signals blocked (jdrly/gw2-on-mac#304)
 *
 * dyld holds its loader lock through each of them. A thread that another thread suspends there
 * (SIGUSR1, usr1_handler, wait_suspend) keeps that lock until it is resumed, and every other
 * thread's dlopen, dlsym and dlclose waits for it. Steam's web helper deadlocked on that: its
 * stack sampler suspended a thread inside dlopen, a second thread waited for dyld's lock with
 * virtual_mutex held, and the sampler waited for virtual_mutex before it could resume the first.
 * Here the signals are blocked as server_enter_uninterrupted_section blocks them, without a mutex.
 * A suspend that arrives meanwhile is taken when the mask is put back, outside dyld's lock.
 * docs/gw2/adr/0020 has the sample's stacks.
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

#ifndef __NTDLL_UNIX_HEATDL_H
#define __NTDLL_UNIX_HEATDL_H

#include <dlfcn.h>
#include <signal.h>

static inline void *heat_dlopen( const char *name, int flags )
{
    sigset_t sigset;
    void *ret;

    pthread_sigmask( SIG_BLOCK, &server_block_set, &sigset );
    ret = dlopen( name, flags );
    pthread_sigmask( SIG_SETMASK, &sigset, NULL );
    return ret;
}

static inline void *heat_dlsym( void *handle, const char *name )
{
    sigset_t sigset;
    void *ret;

    pthread_sigmask( SIG_BLOCK, &server_block_set, &sigset );
    ret = dlsym( handle, name );
    pthread_sigmask( SIG_SETMASK, &sigset, NULL );
    return ret;
}

static inline int heat_dlclose( void *handle )
{
    sigset_t sigset;
    int ret;

    pthread_sigmask( SIG_BLOCK, &server_block_set, &sigset );
    ret = dlclose( handle );
    pthread_sigmask( SIG_SETMASK, &sigset, NULL );
    return ret;
}

#endif /* __NTDLL_UNIX_HEATDL_H */
