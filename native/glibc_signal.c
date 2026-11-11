/* Linux module host, native: glibc's signals.
 *
 * A library in a Wine process must not take over signals: Wine handles SIGSEGV, SIGBUS, SIGILL, SIGTRAP, SIGUSR1
 * and others itself. The library's handlers and masks are recorded and reported back to it, as it set them, and
 * not installed. What libraries do with signals is mostly to ignore SIGPIPE, which sockets here never raise
 * (SO_NOSIGPIPE), and to block signals in their own threads. */
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "glibc_internal.h"

/* Linux's numbers, 1 to 31, and macOS's for each (0 where macOS has none) */
static const int signals_from_linux[32] =
{
    0, SIGHUP, SIGINT, SIGQUIT, SIGILL, SIGTRAP, SIGABRT, SIGBUS, SIGFPE, SIGKILL, SIGUSR1, SIGSEGV, SIGUSR2,
    SIGPIPE, SIGALRM, SIGTERM, 0 /* SIGSTKFLT */, SIGCHLD, SIGCONT, SIGSTOP, SIGTSTP, SIGTTIN, SIGTTOU, SIGURG,
    SIGXCPU, SIGXFSZ, SIGVTALRM, SIGPROF, SIGWINCH, SIGIO, 0 /* SIGPWR */, SIGSYS,
};

int glibc_signal_from_linux( int sig )
{
    return sig > 0 && sig < 32 ? signals_from_linux[sig] : 0;
}

int glibc_signal_to_linux( int sig )
{
    for (int i = 1; i < 32; i++) if (signals_from_linux[i] == sig) return i;
    return 0;
}

/* glibc's sigset_t: 1024 bits, signal n at bit n - 1 */
struct lnx_sigset { unsigned long bits[16]; };

static int lnx_sigemptyset( struct lnx_sigset *set ) { memset( set, 0, sizeof(*set) ); return 0; }
static int lnx_sigfillset( struct lnx_sigset *set ) { memset( set, 0xff, sizeof(*set) ); return 0; }

static int lnx_sigaddset( struct lnx_sigset *set, int sig )
{
    if (sig < 1 || sig > 64) return linux_fail( 22 );
    set->bits[(sig - 1) / 64] |= 1ul << ((sig - 1) % 64);
    return 0;
}

static int lnx_sigdelset( struct lnx_sigset *set, int sig )
{
    if (sig < 1 || sig > 64) return linux_fail( 22 );
    set->bits[(sig - 1) / 64] &= ~(1ul << ((sig - 1) % 64));
    return 0;
}

/* each thread's mask, as the library set it */
static __thread struct lnx_sigset thread_mask;

static int set_mask( int how, const struct lnx_sigset *set, struct lnx_sigset *old )
{
    struct lnx_sigset prev = thread_mask;

    if (set)
    {
        for (int i = 0; i < 16; i++)
        {
            switch (how)
            {
            case 0: thread_mask.bits[i] |= set->bits[i]; break;   /* SIG_BLOCK */
            case 1: thread_mask.bits[i] &= ~set->bits[i]; break;  /* SIG_UNBLOCK */
            case 2: thread_mask.bits[i] = set->bits[i]; break;    /* SIG_SETMASK */
            default: return 22;
            }
        }
    }
    if (old) *old = prev;
    return 0;
}

static int lnx_sigprocmask( int how, const struct lnx_sigset *set, struct lnx_sigset *old )
{
    int ret = set_mask( how, set, old );

    return ret ? linux_fail( ret ) : 0;
}

static int lnx_pthread_sigmask( int how, const struct lnx_sigset *set, struct lnx_sigset *old )
{
    return set_mask( how, set, old );
}

/* glibc's struct sigaction on aarch64 */
struct lnx_sigaction
{
    void *handler;
    struct lnx_sigset mask;
    int flags;
    void (*restorer)(void);
};

static struct lnx_sigaction actions[65];
static pthread_mutex_t actions_lock = PTHREAD_MUTEX_INITIALIZER;

static int lnx_sigaction( int sig, const struct lnx_sigaction *act, struct lnx_sigaction *old )
{
    if (sig < 1 || sig > 64 || sig == 9 || sig == 19) return linux_fail( 22 );
    pthread_mutex_lock( &actions_lock );
    if (old) *old = actions[sig];
    if (act)
    {
        actions[sig] = *act;
        /* the one disposition taken over: ignoring SIGPIPE, which Wine ignores as well */
        if (sig == 13 && act->handler == (void *)1) signal( SIGPIPE, SIG_IGN );
    }
    pthread_mutex_unlock( &actions_lock );
    return 0;
}

static void *lnx_signal( int sig, void *handler )
{
    struct lnx_sigaction act = { handler }, old;

    if (lnx_sigaction( sig, &act, &old )) return (void *)-1;  /* SIG_ERR */
    return old.handler;
}

/* glibc's stack_t on aarch64 */
struct lnx_stack { void *sp; int flags; size_t size; };
static __thread struct lnx_stack alt_stack = { NULL, 2 /* SS_DISABLE */, 0 };

static int lnx_sigaltstack( const struct lnx_stack *ss, struct lnx_stack *old )
{
    if (old) *old = alt_stack;
    if (ss) alt_stack = *ss;
    return 0;
}

static int lnx_pause( void ) { pause(); return linux_fail( 4 ); }

/* glibc_asm.S */
extern void lnx__setjmp(void), lnx___sigsetjmp(void), lnx__longjmp(void), lnx_siglongjmp(void);
extern void lnx_getcontext(void), lnx_setcontext(void), lnx_makecontext(void);

const struct glibc_export glibc_signal_exports[] =
{
    WRAP( sigemptyset ), WRAP( sigfillset ), WRAP( sigaddset ), WRAP( sigdelset ), WRAP( sigprocmask ),
    WRAP( pthread_sigmask ), WRAP( sigaction ), WRAP( signal ), WRAP( sigaltstack ), WRAP( pause ),
    WRAP( _setjmp ), WRAP( __sigsetjmp ), WRAP( _longjmp ), WRAP( siglongjmp ),
    WRAP( getcontext ), WRAP( setcontext ), WRAP( makecontext ),
    END
};
