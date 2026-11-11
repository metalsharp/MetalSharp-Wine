/* Linux module host, native: glibc's threads, on macOS's pthreads.
 *
 * glibc's synchronisation objects are smaller than macOS's (a mutex is 48 bytes, not 64) and a library allocates
 * them, often statically with an initializer of zeros. Each one here holds a pointer to a macOS object in its
 * first eight bytes, made on first use, so the zero initializers work as they do on Linux. glibc's attribute
 * objects are only read by these functions, so they hold what the macOS calls need. */
#include <mach/mach.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include "glibc_internal.h"

/* ---------------------------------------------------------------------------------------------------------
 * Mutexes
 */

/* glibc's pthread_mutex_t: the kind is the fifth int (PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP sets it) */
struct lnx_mutex { _Atomic(pthread_mutex_t *) mutex; int pad[2]; int kind; int rest[7]; };
struct lnx_mutexattr { int type; int pad; };

static pthread_mutex_t *create_mutex( int kind )
{
    pthread_mutexattr_t attr;
    pthread_mutex_t *mutex = malloc( sizeof(*mutex) );

    if (!mutex) abort();
    pthread_mutexattr_init( &attr );
    if (kind == 1) pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    else if (kind == 2) pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_ERRORCHECK );
    pthread_mutex_init( mutex, &attr );
    pthread_mutexattr_destroy( &attr );
    return mutex;
}

static pthread_mutex_t *get_mutex( struct lnx_mutex *m )
{
    pthread_mutex_t *mutex = atomic_load( &m->mutex ), *expected = NULL;

    if (mutex) return mutex;
    mutex = create_mutex( m->kind & 3 );
    if (atomic_compare_exchange_strong( &m->mutex, &expected, mutex )) return mutex;
    pthread_mutex_destroy( mutex );
    free( mutex );
    return expected;
}

static int lnx_pthread_mutexattr_init( struct lnx_mutexattr *attr ) { attr->type = 0; return 0; }
static int lnx_pthread_mutexattr_destroy( struct lnx_mutexattr *attr ) { return 0; }
static int lnx_pthread_mutexattr_settype( struct lnx_mutexattr *attr, int type )
{
    if (type < 0 || type > 3) return 22;
    attr->type = type;
    return 0;
}

static int lnx_pthread_mutex_init( struct lnx_mutex *m, const struct lnx_mutexattr *attr )
{
    memset( m, 0, sizeof(*m) );
    m->kind = attr ? attr->type : 0;
    atomic_store( &m->mutex, create_mutex( m->kind & 3 ) );
    return 0;
}

static int lnx_pthread_mutex_destroy( struct lnx_mutex *m )
{
    pthread_mutex_t *mutex = atomic_exchange( &m->mutex, NULL );

    if (mutex)
    {
        pthread_mutex_destroy( mutex );
        free( mutex );
    }
    return 0;
}

static int lnx_pthread_mutex_lock( struct lnx_mutex *m ) { return glibc_errno_to_linux( pthread_mutex_lock( get_mutex( m ) ) ); }
static int lnx_pthread_mutex_trylock( struct lnx_mutex *m ) { return glibc_errno_to_linux( pthread_mutex_trylock( get_mutex( m ) ) ); }
static int lnx_pthread_mutex_unlock( struct lnx_mutex *m ) { return glibc_errno_to_linux( pthread_mutex_unlock( get_mutex( m ) ) ); }

/* ---------------------------------------------------------------------------------------------------------
 * Condition variables: glibc's can wait on CLOCK_MONOTONIC (pthread_condattr_setclock), macOS's cannot, so a
 * deadline becomes a relative wait.
 */

struct mac_cond { pthread_cond_t cond; int clock; };
struct lnx_cond { _Atomic(struct mac_cond *) cond; int rest[10]; };
struct lnx_condattr { int clock; int pad; };

static struct mac_cond *get_cond( struct lnx_cond *c, int clock )
{
    struct mac_cond *cond = atomic_load( &c->cond ), *expected = NULL;

    if (cond) return cond;
    if (!(cond = malloc( sizeof(*cond) ))) abort();
    pthread_cond_init( &cond->cond, NULL );
    cond->clock = clock;
    if (atomic_compare_exchange_strong( &c->cond, &expected, cond )) return cond;
    pthread_cond_destroy( &cond->cond );
    free( cond );
    return expected;
}

static int lnx_pthread_condattr_init( struct lnx_condattr *attr ) { attr->clock = 0; return 0; }
static int lnx_pthread_condattr_destroy( struct lnx_condattr *attr ) { return 0; }
static int lnx_pthread_condattr_setclock( struct lnx_condattr *attr, int clock )
{
    if (clock != 0 && clock != 1) return 22;
    attr->clock = clock;
    return 0;
}

static int lnx_pthread_cond_init( struct lnx_cond *c, const struct lnx_condattr *attr )
{
    memset( c, 0, sizeof(*c) );
    get_cond( c, attr ? attr->clock : 0 );
    return 0;
}

static int lnx_pthread_cond_destroy( struct lnx_cond *c )
{
    struct mac_cond *cond = atomic_exchange( &c->cond, NULL );

    if (cond)
    {
        pthread_cond_destroy( &cond->cond );
        free( cond );
    }
    return 0;
}

static int lnx_pthread_cond_signal( struct lnx_cond *c ) { return pthread_cond_signal( &get_cond( c, 0 )->cond ); }
static int lnx_pthread_cond_broadcast( struct lnx_cond *c ) { return pthread_cond_broadcast( &get_cond( c, 0 )->cond ); }
static int lnx_pthread_cond_wait( struct lnx_cond *c, struct lnx_mutex *m )
{
    return glibc_errno_to_linux( pthread_cond_wait( &get_cond( c, 0 )->cond, get_mutex( m ) ) );
}

static int lnx_pthread_cond_timedwait( struct lnx_cond *c, struct lnx_mutex *m, const struct timespec *deadline )
{
    struct mac_cond *cond = get_cond( c, 0 );
    struct timespec now, rel;
    int ret;

    clock_gettime( cond->clock == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &now );
    rel.tv_sec = deadline->tv_sec - now.tv_sec;
    rel.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) { rel.tv_nsec += 1000000000; rel.tv_sec--; }
    if (rel.tv_sec < 0) { rel.tv_sec = 0; rel.tv_nsec = 0; }
    ret = pthread_cond_timedwait_relative_np( &cond->cond, get_mutex( m ), &rel );
    return glibc_errno_to_linux( ret );
}

/* ---------------------------------------------------------------------------------------------------------
 * Read-write locks
 */

struct lnx_rwlock { _Atomic(pthread_rwlock_t *) lock; int rest[12]; };

static pthread_rwlock_t *get_rwlock( struct lnx_rwlock *l )
{
    pthread_rwlock_t *lock = atomic_load( &l->lock ), *expected = NULL;

    if (lock) return lock;
    if (!(lock = malloc( sizeof(*lock) ))) abort();
    pthread_rwlock_init( lock, NULL );
    if (atomic_compare_exchange_strong( &l->lock, &expected, lock )) return lock;
    pthread_rwlock_destroy( lock );
    free( lock );
    return expected;
}

static int lnx_pthread_rwlock_init( struct lnx_rwlock *l, const void *attr )
{
    memset( l, 0, sizeof(*l) );
    get_rwlock( l );
    return 0;
}

static int lnx_pthread_rwlock_destroy( struct lnx_rwlock *l )
{
    pthread_rwlock_t *lock = atomic_exchange( &l->lock, NULL );

    if (lock)
    {
        pthread_rwlock_destroy( lock );
        free( lock );
    }
    return 0;
}

static int lnx_pthread_rwlock_rdlock( struct lnx_rwlock *l ) { return glibc_errno_to_linux( pthread_rwlock_rdlock( get_rwlock( l ) ) ); }
static int lnx_pthread_rwlock_wrlock( struct lnx_rwlock *l ) { return glibc_errno_to_linux( pthread_rwlock_wrlock( get_rwlock( l ) ) ); }
static int lnx_pthread_rwlock_unlock( struct lnx_rwlock *l ) { return glibc_errno_to_linux( pthread_rwlock_unlock( get_rwlock( l ) ) ); }

/* ---------------------------------------------------------------------------------------------------------
 * Once, keys
 */

/* glibc's pthread_once_t is an int, zero until it has run */
static int lnx_pthread_once( _Atomic int *once, void (*func)(void) )
{
    int expected = 0;

    if (atomic_load( once ) == 2) return 0;
    if (atomic_compare_exchange_strong( once, &expected, 1 ))
    {
        func();
        atomic_store( once, 2 );
        return 0;
    }
    while (atomic_load( once ) != 2) sched_yield();
    return 0;
}

static int lnx_pthread_key_create( unsigned int *key, void (*destructor)(void *) )
{
    pthread_key_t k;
    int ret = pthread_key_create( &k, destructor );

    if (!ret) *key = (unsigned int)k;
    return glibc_errno_to_linux( ret );
}

static int lnx_pthread_key_delete( unsigned int key ) { return pthread_key_delete( key ); }
static void *lnx_pthread_getspecific( unsigned int key ) { return pthread_getspecific( key ); }
static int lnx_pthread_setspecific( unsigned int key, const void *value ) { return pthread_setspecific( key, value ); }

/* ---------------------------------------------------------------------------------------------------------
 * Threads
 */

struct lnx_attr { int magic, detached; size_t stack_size; void *stack_addr; char pad[40]; };
#define ATTR_MAGIC 0x6174746c

static int lnx_pthread_attr_init( struct lnx_attr *attr )
{
    memset( attr, 0, sizeof(*attr) );
    attr->magic = ATTR_MAGIC;
    attr->stack_size = 8 << 20;  /* glibc's default */
    return 0;
}

static int lnx_pthread_attr_destroy( struct lnx_attr *attr ) { return 0; }
static int lnx_pthread_attr_setdetachstate( struct lnx_attr *attr, int state ) { attr->detached = state; return 0; }
static int lnx_pthread_attr_setstacksize( struct lnx_attr *attr, size_t size ) { attr->stack_size = size; return 0; }
static int lnx_pthread_attr_getstacksize( const struct lnx_attr *attr, size_t *size )
{
    *size = attr->stack_size;
    return 0;
}

static int lnx_pthread_attr_setstack( struct lnx_attr *attr, void *addr, size_t size )
{
    attr->stack_addr = addr;
    attr->stack_size = size;
    return 0;
}

static int lnx_pthread_create( unsigned long *thread, const struct lnx_attr *lattr, void *(*start)(void *), void *arg )
{
    pthread_attr_t attr;
    pthread_t t;
    size_t page = getpagesize();
    int ret;

    pthread_attr_init( &attr );
    if (lattr && lattr->magic == ATTR_MAGIC)
    {
        if (lattr->detached) pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
        if (lattr->stack_addr) pthread_attr_setstack( &attr, lattr->stack_addr, lattr->stack_size );
        else if (lattr->stack_size)
            pthread_attr_setstacksize( &attr, (lattr->stack_size + page - 1) & ~(page - 1) );
    }
    else pthread_attr_setstacksize( &attr, 8 << 20 );
    ret = pthread_create( &t, &attr, start, arg );
    pthread_attr_destroy( &attr );
    if (!ret) *thread = (unsigned long)t;
    return glibc_errno_to_linux( ret );
}

static int lnx_pthread_join( unsigned long thread, void **ret ) { return glibc_errno_to_linux( pthread_join( (pthread_t)thread, ret ) ); }
static int lnx_pthread_detach( unsigned long thread ) { return glibc_errno_to_linux( pthread_detach( (pthread_t)thread ) ); }
static unsigned long lnx_pthread_self( void ) { return (unsigned long)pthread_self(); }
static void lnx_pthread_exit( void *ret ) { pthread_exit( ret ); }
static int lnx_pthread_setcanceltype( int type, int *old ) { if (old) *old = 0; return 0; }

static int lnx_pthread_setname_np( unsigned long thread, const char *name )
{
    if ((pthread_t)thread == pthread_self()) pthread_setname_np( name );
    return 0;
}

static int policy_from_linux( int policy )
{
    switch (policy) { case 1: return SCHED_FIFO; case 2: return SCHED_RR; default: return SCHED_OTHER; }
}

static int lnx_pthread_setschedparam( unsigned long thread, int policy, const struct sched_param *param )
{
    pthread_setschedparam( (pthread_t)thread, policy_from_linux( policy ), param );
    return 0;  /* priorities are a hint; Linux refuses most changes to unprivileged threads too */
}

static int lnx_sched_get_priority_min( int policy ) { return sched_get_priority_min( policy_from_linux( policy ) ); }
static int lnx_sched_get_priority_max( int policy ) { return sched_get_priority_max( policy_from_linux( policy ) ); }

/* Linux's cpu_set_t: a bit per CPU */
static int lnx_sched_getaffinity( int pid, size_t size, unsigned char *set )
{
    long count = sysconf( _SC_NPROCESSORS_ONLN );

    memset( set, 0, size );
    for (long i = 0; i < count && i / 8 < (long)size; i++) set[i / 8] |= 1 << (i % 8);
    return 0;
}

static int lnx___sched_cpucount( size_t size, const unsigned char *set )
{
    int count = 0;

    for (size_t i = 0; i < size; i++) count += __builtin_popcount( set[i] );
    return count;
}

/* ---------------------------------------------------------------------------------------------------------
 * Unnamed semaphores, which macOS does not implement
 */

struct semaphore { pthread_mutex_t mutex; pthread_cond_t cond; unsigned int value; };
struct lnx_sem { struct semaphore *sem; char pad[24]; };

static int lnx_sem_init( struct lnx_sem *s, int shared, unsigned int value )
{
    struct semaphore *sem = malloc( sizeof(*sem) );

    if (!sem) return linux_fail( 12 );
    pthread_mutex_init( &sem->mutex, NULL );
    pthread_cond_init( &sem->cond, NULL );
    sem->value = value;
    s->sem = sem;
    return 0;
}

static int lnx_sem_destroy( struct lnx_sem *s )
{
    pthread_mutex_destroy( &s->sem->mutex );
    pthread_cond_destroy( &s->sem->cond );
    free( s->sem );
    s->sem = NULL;
    return 0;
}

static int lnx_sem_post( struct lnx_sem *s )
{
    pthread_mutex_lock( &s->sem->mutex );
    s->sem->value++;
    pthread_cond_signal( &s->sem->cond );
    pthread_mutex_unlock( &s->sem->mutex );
    return 0;
}

static int lnx_sem_wait( struct lnx_sem *s )
{
    pthread_mutex_lock( &s->sem->mutex );
    while (!s->sem->value) pthread_cond_wait( &s->sem->cond, &s->sem->mutex );
    s->sem->value--;
    pthread_mutex_unlock( &s->sem->mutex );
    return 0;
}

static int lnx_sem_trywait( struct lnx_sem *s )
{
    int ret = 0;

    pthread_mutex_lock( &s->sem->mutex );
    if (s->sem->value) s->sem->value--;
    else ret = 11;  /* EAGAIN */
    pthread_mutex_unlock( &s->sem->mutex );
    return ret ? linux_fail( ret ) : 0;
}

/* the deadline is CLOCK_REALTIME */
static int lnx_sem_timedwait( struct lnx_sem *s, const struct timespec *deadline )
{
    int ret = 0;

    pthread_mutex_lock( &s->sem->mutex );
    while (!s->sem->value && !ret) ret = pthread_cond_timedwait( &s->sem->cond, &s->sem->mutex, deadline );
    if (s->sem->value)
    {
        s->sem->value--;
        ret = 0;
    }
    pthread_mutex_unlock( &s->sem->mutex );
    return ret ? linux_fail( ret == ETIMEDOUT ? 110 : glibc_errno_to_linux( ret ) ) : 0;
}

static int lnx_sem_getvalue( struct lnx_sem *s, int *value )
{
    *value = s->sem->value;
    return 0;
}

const struct glibc_export glibc_thread_exports[] =
{
    WRAP( pthread_mutexattr_init ), WRAP( pthread_mutexattr_destroy ), WRAP( pthread_mutexattr_settype ),
    WRAP( pthread_mutex_init ), WRAP( pthread_mutex_destroy ), WRAP( pthread_mutex_lock ),
    WRAP( pthread_mutex_trylock ), WRAP( pthread_mutex_unlock ),
    WRAP( pthread_condattr_init ), WRAP( pthread_condattr_destroy ), WRAP( pthread_condattr_setclock ),
    WRAP( pthread_cond_init ), WRAP( pthread_cond_destroy ), WRAP( pthread_cond_signal ),
    WRAP( pthread_cond_broadcast ), WRAP( pthread_cond_wait ), WRAP( pthread_cond_timedwait ),
    WRAP( pthread_rwlock_init ), WRAP( pthread_rwlock_destroy ), WRAP( pthread_rwlock_rdlock ),
    WRAP( pthread_rwlock_wrlock ), WRAP( pthread_rwlock_unlock ),
    WRAP( pthread_once ), WRAP( pthread_key_create ), WRAP( pthread_key_delete ), WRAP( pthread_getspecific ),
    WRAP( pthread_setspecific ),
    WRAP( pthread_attr_init ), WRAP( pthread_attr_destroy ), WRAP( pthread_attr_setdetachstate ),
    WRAP( pthread_attr_setstacksize ), WRAP( pthread_attr_getstacksize ), WRAP( pthread_attr_setstack ),
    WRAP( pthread_create ), WRAP( pthread_join ), WRAP( pthread_detach ), WRAP( pthread_self ),
    WRAP( pthread_exit ), WRAP( pthread_setcanceltype ), WRAP( pthread_setname_np ),
    WRAP( pthread_setschedparam ), WRAP( sched_get_priority_min ), WRAP( sched_get_priority_max ),
    WRAP( sched_getaffinity ), WRAP( __sched_cpucount ),
    WRAP( sem_init ), WRAP( sem_destroy ), WRAP( sem_post ), WRAP( sem_wait ), WRAP( sem_trywait ),
    WRAP( sem_timedwait ), WRAP( sem_getvalue ),
    END
};
