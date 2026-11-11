/* Linux module host: glibc's threads, condition variables, semaphores and scheduling, on Windows threads.
 *
 * A thread a Linux library creates starts in Windows code that gives it a Linux thread control block (%fs) and
 * calls the library's System V start routine. pthread_t is the Windows thread id. A pthread_cond_t holds a
 * CONDITION_VARIABLE in its first 8 bytes (zero is its initial state, as glibc's), used with the critical section
 * of the library's mutex (libc.c); a sem_t holds a Windows semaphore made by sem_init. */
#include <windows.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"

typedef unsigned long long lx_size_t;
struct lx_timespec { long long sec, nsec; };

typedef void *(SYSV *lx_start_fn)( void * );

/* the threads made here, for pthread_join */
struct thread
{
    DWORD id;
    HANDLE handle;
    lx_start_fn start;
    void *arg, *result;
    int detached;
};
static struct thread *threads[256];
static SRWLOCK threads_lock = SRWLOCK_INIT;

static DWORD WINAPI thread_start( void *param )
{
    struct thread *t = param;

    linuxhost_thread_init();
    t->result = t->start( t->arg );
    return 0;
}

/* glibc's pthread_attr_t: only whether the thread starts detached matters here, which pthread_attr_setdetachstate
 * keeps in the first int of the attribute (libc.c does not provide those, so it is read as glibc lays it out) */
static SYSV int lx_pthread_create( unsigned long long *thread, const void *attr, lx_start_fn start, void *arg )
{
    struct thread *t = calloc( 1, sizeof(*t) );
    unsigned int i;

    if (!t) return 11;  /* EAGAIN */
    t->start = start;
    t->arg = arg;
    if (!(t->handle = CreateThread( NULL, 8 << 20, thread_start, t, CREATE_SUSPENDED | STACK_SIZE_PARAM_IS_A_RESERVATION,
                                    &t->id )))
    {
        free( t );
        return 11;
    }
    AcquireSRWLockExclusive( &threads_lock );
    for (i = 0; i < ARRAY_SIZE(threads); i++) if (!threads[i]) { threads[i] = t; break; }
    ReleaseSRWLockExclusive( &threads_lock );
    *thread = t->id;
    linuxhost_trace( "pthread_create: thread %lu\n", t->id );
    ResumeThread( t->handle );
    return 0;
}

static SYSV int lx_pthread_join( unsigned long long thread, void **result )
{
    struct thread *t = NULL;
    unsigned int i;

    AcquireSRWLockExclusive( &threads_lock );
    for (i = 0; i < ARRAY_SIZE(threads); i++)
        if (threads[i] && threads[i]->id == thread) { t = threads[i]; threads[i] = NULL; break; }
    ReleaseSRWLockExclusive( &threads_lock );
    if (!t) return 3;  /* ESRCH */
    WaitForSingleObject( t->handle, INFINITE );
    CloseHandle( t->handle );
    if (result) *result = t->result;
    free( t );
    return 0;
}

static SYSV unsigned long long lx_pthread_self(void) { return GetCurrentThreadId(); }
static SYSV int lx_pthread_setschedparam( unsigned long long thread, int policy, const void *param ) { return 0; }
static SYSV int lx_sched_yield(void) { SwitchToThread(); return 0; }
static SYSV int lx_sched_get_priority_max( int policy ) { return policy == 1 || policy == 2 ? 99 : 0; }
static SYSV int lx_sched_get_priority_min( int policy ) { return policy == 1 || policy == 2 ? 1 : 0; }

/* glibc's pthread_once_t: an int, 0 until it has run */
static SYSV int lx_pthread_once( volatile LONG *once, void (SYSV *fn)(void) )
{
    if (*once == 2) return 0;
    if (!InterlockedCompareExchange( once, 1, 0 ))
    {
        fn();
        InterlockedExchange( once, 2 );
        return 0;
    }
    while (*once != 2) SwitchToThread();
    return 0;
}

/* ---- condition variables ---- */

static SYSV int lx_pthread_cond_init( void *cond, const void *attr ) { memset( cond, 0, 48 ); return 0; }
static SYSV int lx_pthread_cond_destroy( void *cond ) { return 0; }
static SYSV int lx_pthread_cond_signal( void *cond ) { WakeConditionVariable( cond ); return 0; }
static SYSV int lx_pthread_cond_broadcast( void *cond ) { WakeAllConditionVariable( cond ); return 0; }

static SYSV int lx_pthread_cond_wait( void *cond, void *mutex )
{
    SleepConditionVariableCS( cond, linuxhost_mutex_cs( mutex ), INFINITE );
    return 0;
}

/* the deadline is CLOCK_REALTIME, which is what glibc uses unless the condition was made for another clock */
static SYSV int lx_pthread_cond_timedwait( void *cond, void *mutex, const struct lx_timespec *deadline )
{
    FILETIME ft;
    ULARGE_INTEGER now;
    long long now_ms, deadline_ms;
    DWORD wait;

    GetSystemTimePreciseAsFileTime( &ft );
    now.LowPart = ft.dwLowDateTime;
    now.HighPart = ft.dwHighDateTime;
    now_ms = (long long)(now.QuadPart - 116444736000000000ull) / 10000;
    deadline_ms = deadline->sec * 1000 + deadline->nsec / 1000000;
    wait = deadline_ms > now_ms ? (DWORD)min( deadline_ms - now_ms, 0x7ffffffe ) : 0;
    if (!SleepConditionVariableCS( cond, linuxhost_mutex_cs( mutex ), wait ))
        return GetLastError() == ERROR_TIMEOUT ? 110 : 22;  /* ETIMEDOUT, EINVAL */
    return 0;
}

/* ---- semaphores ---- */

struct lx_sem { HANDLE handle; char pad[24]; };

static SYSV int lx_sem_init( struct lx_sem *sem, int shared, unsigned int value )
{
    if (!(sem->handle = CreateSemaphoreW( NULL, value, 0x7fffffff, NULL )))
    {
        *linuxhost_errno() = 28;  /* ENOSPC */
        return -1;
    }
    return 0;
}

static SYSV int lx_sem_destroy( struct lx_sem *sem ) { CloseHandle( sem->handle ); sem->handle = NULL; return 0; }
static SYSV int lx_sem_post( struct lx_sem *sem ) { ReleaseSemaphore( sem->handle, 1, NULL ); return 0; }
static SYSV int lx_sem_wait( struct lx_sem *sem ) { WaitForSingleObject( sem->handle, INFINITE ); return 0; }

/* ---- sleeping ---- */

static SYSV int lx_nanosleep( const struct lx_timespec *req, struct lx_timespec *rem )
{
    long long ms = req->sec * 1000 + (req->nsec + 999999) / 1000000;

    Sleep( (DWORD)min( ms, 0x7ffffffe ) );
    if (rem) rem->sec = rem->nsec = 0;
    return 0;
}

const struct libc_function linuxhost_thread_functions[] =
{
    { "pthread_create", lx_pthread_create }, { "pthread_join", lx_pthread_join }, { "pthread_self", lx_pthread_self },
    { "pthread_setschedparam", lx_pthread_setschedparam }, { "sched_yield", lx_sched_yield },
    { "sched_get_priority_max", lx_sched_get_priority_max }, { "sched_get_priority_min", lx_sched_get_priority_min },
    { "pthread_once", lx_pthread_once },
    { "pthread_cond_init", lx_pthread_cond_init }, { "pthread_cond_destroy", lx_pthread_cond_destroy },
    { "pthread_cond_signal", lx_pthread_cond_signal }, { "pthread_cond_broadcast", lx_pthread_cond_broadcast },
    { "pthread_cond_wait", lx_pthread_cond_wait }, { "pthread_cond_timedwait", lx_pthread_cond_timedwait },
    { "sem_init", lx_sem_init }, { "sem_destroy", lx_sem_destroy }, { "sem_post", lx_sem_post },
    { "sem_wait", lx_sem_wait }, { "nanosleep", lx_nanosleep },
    { NULL, NULL }
};
