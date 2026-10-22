/* x87 acceleration for 32-bit programs: Rosetta translates x87 code into
 * slow soft-float, and x87sidecar (github.com/athei/x87sidecar, MIT) hooks
 * Rosetta's translator to emit fast code instead. Wine builds that run every
 * i386 process through the sidecar (MetalSharp) are hooked before the program
 * starts; elsewhere (CrossOver, other Wine builds) this attaches it here, when
 * the program loads OpenGL: x87sidecar --serve-pid=<this pid> registers
 * "x87sidecar.<pid>", and the handshake hands it this task's port. Code
 * Rosetta translated before then stays slow; what the program runs from here
 * on (most of a game: its renderer, its game DLL) is fast.
 *
 * Opt-in for now: WINEMETALGL_X87=1, or, when it is not set, on for the
 * games known to need it (x87_games); WINEMETALGL_X87=0 turns it off for
 * them too. The sidecar is x87sidecar next to winemetalgl.so, or
 * WINEMETALGL_X87_SIDECAR. */

#ifdef __aarch64__

/* An arm64 Wine runs x86 code through FEX, not Rosetta: there is nothing to hook. FEX's
 * equivalent of x87sidecar is its reduced-precision x87 mode, chosen when the process starts
 * (FEX_X87REDUCEDPRECISION=1, or ROSETTA_X87_PATH, which MetalSharp's Wine maps to it). */
#include "wmgl_unix.h"

void wmgl_x87_attach(void)
{
}

#else /* __aarch64__ */

#include <dlfcn.h>
#include <libgen.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include "wmgl_unix.h"

extern char **environ;

/* x87sidecar's handshake (its rosetta_loader/src/coop_proto.h). */
#define X87_COOP_MSGH_ID 0x78383721

typedef struct
{
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t task_port;
    mach_msg_port_descriptor_t thread_port;
} x87_coop_request_t;

typedef struct
{
    mach_msg_header_t header;
    uint64_t icache_addr[2];
    uint64_t icache_len[2];
} x87_coop_reply_t;

/* 32-bit games whose x87 math holds them far below their frame rate
 * without the sidecar: id Tech 4's (Quake 4 runs at 60 fps only with it,
 * as does Doom 3: BFG Edition, issue #25). */
static const char *const x87_games[] = { "Quake4.exe", "Doom3.exe", "Doom3BFG.exe" };

static BOOL x87_wanted(void)
{
    const char *enabled = getenv( "WINEMETALGL_X87" ), *program = wmgl_program(), *name = program;
    const char *p;
    unsigned int i;

    if (enabled) return !strcmp( enabled, "1" );
    for (p = program; *p; p++) if (*p == '\\' || *p == '/') name = p + 1;
    for (i = 0; i < ARRAY_SIZE(x87_games); i++)
        if (!strcasecmp( name, x87_games[i] ))
        {
            WMGL_INFO( "x87: on for %s (WINEMETALGL_X87=0 turns it off)\n", name );
            return TRUE;
        }
    return FALSE;
}

static BOOL find_sidecar( char *path, size_t size )
{
    const char *env = getenv( "WINEMETALGL_X87_SIDECAR" );
    Dl_info info;
    char so[4096];

    if (env && *env) snprintf( path, size, "%s", env );
    else
    {
        if (!dladdr( (void *)find_sidecar, &info ) || !info.dli_fname) return FALSE;
        snprintf( so, sizeof(so), "%s", info.dli_fname );
        snprintf( path, size, "%s/x87sidecar", dirname( so ) );
    }
    return access( path, X_OK ) == 0;
}

/* Every thread but this one, suspended while the sidecar patches Rosetta's
 * translator, so none enters it half-written. Mach calls only: a suspended
 * thread may hold the malloc lock. */
static void suspend_others( thread_act_array_t *threads, mach_msg_type_number_t *count, BOOL resume )
{
    thread_t self = mach_thread_self();
    mach_msg_type_number_t i;

    if (!resume && task_threads( mach_task_self(), threads, count ) != KERN_SUCCESS)
    {
        *threads = NULL;
        *count = 0;
    }
    for (i = 0; i < *count; i++)
    {
        if ((*threads)[i] == self) continue;
        if (resume)
        {
            thread_resume( (*threads)[i] );
            mach_port_deallocate( mach_task_self(), (*threads)[i] );
        }
        else thread_suspend( (*threads)[i] );
    }
    if (resume && *threads)
        vm_deallocate( mach_task_self(), (vm_address_t)*threads, *count * sizeof(thread_t) );
    mach_port_deallocate( mach_task_self(), self );
}

static BOOL handshake( mach_port_t service )
{
    struct { x87_coop_reply_t reply; mach_msg_trailer_t trailer; char slack[64]; } rep;
    x87_coop_request_t msg;
    thread_act_array_t threads;
    mach_msg_type_number_t count;
    mach_port_t reply;
    kern_return_t kr;
    BOOL ok;
    int i;

    if (mach_port_allocate( mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply ) != KERN_SUCCESS) return FALSE;
    memset( &msg, 0, sizeof(msg) );
    msg.header.msgh_bits = MACH_MSGH_BITS( MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND_ONCE ) | MACH_MSGH_BITS_COMPLEX;
    msg.header.msgh_size = sizeof(msg);
    msg.header.msgh_remote_port = service;
    msg.header.msgh_local_port = reply;
    msg.header.msgh_id = X87_COOP_MSGH_ID;
    msg.body.msgh_descriptor_count = 2;
    msg.task_port.name = mach_task_self();
    msg.task_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.task_port.type = MACH_MSG_PORT_DESCRIPTOR;
    msg.thread_port.name = mach_thread_self();
    msg.thread_port.disposition = MACH_MSG_TYPE_COPY_SEND;
    msg.thread_port.type = MACH_MSG_PORT_DESCRIPTOR;
    memset( &rep, 0, sizeof(rep) );

    suspend_others( &threads, &count, FALSE );
    kr = mach_msg( &msg.header, MACH_SEND_MSG, sizeof(msg), 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL );
    if (kr == KERN_SUCCESS)
        kr = mach_msg( &rep.reply.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(rep), reply, 30000, MACH_PORT_NULL );
    /* Anything but the reply (the send-once notification of a sidecar that
     * died, a timeout) leaves the program unhooked, and running. */
    ok = kr == KERN_SUCCESS && rep.reply.header.msgh_id == X87_COOP_MSGH_ID + 1 &&
         rep.reply.header.msgh_size == sizeof(x87_coop_reply_t);
    if (ok)
        for (i = 0; i < 2; i++)
            if (rep.reply.icache_len[i])
                sys_icache_invalidate( (void *)(uintptr_t)rep.reply.icache_addr[i], rep.reply.icache_len[i] );
    suspend_others( &threads, &count, TRUE );
    mach_port_deallocate( mach_task_self(), msg.thread_port.name );
    mach_port_mod_refs( mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1 );
    return ok;
}

static void attach(void)
{
    char sidecar[4096], pid_arg[32], name[64];
    char *argv[] = { sidecar, pid_arg, NULL };
    mach_port_t bootstrap = MACH_PORT_NULL, service = MACH_PORT_NULL;
    posix_spawnattr_t attr;
    pid_t child;
    int i;

    if (!x87_wanted()) return;
    snprintf( name, sizeof(name), "x87sidecar.%d", (int)getpid() );
    if (task_get_bootstrap_port( mach_task_self(), &bootstrap ) != KERN_SUCCESS) return;
    /* Hooked at launch (the sidecar keeps its service for its translations). */
    if (bootstrap_look_up( bootstrap, name, &service ) == KERN_SUCCESS)
    {
        WMGL_INFO( "x87: already accelerated (%s)\n", name );
        mach_port_deallocate( mach_task_self(), service );
        return;
    }
    if (!find_sidecar( sidecar, sizeof(sidecar) ))
    {
        WMGL_WARN( "x87: on, but no x87sidecar at %s\n", sidecar );
        return;
    }
    snprintf( pid_arg, sizeof(pid_arg), "--serve-pid=%d", (int)getpid() );
    posix_spawnattr_init( &attr );
    posix_spawnattr_setflags( &attr, POSIX_SPAWN_SETSID );
    i = posix_spawn( &child, sidecar, NULL, &attr, argv, environ );
    posix_spawnattr_destroy( &attr );
    if (i)
    {
        WMGL_WARN( "x87: cannot start %s: %s\n", sidecar, strerror( i ) );
        return;
    }
    /* The sidecar checks Rosetta, then registers its service. */
    for (i = 0; i < 300; i++)
    {
        if (bootstrap_look_up( bootstrap, name, &service ) == KERN_SUCCESS) break;
        if (waitpid( child, NULL, WNOHANG ) == child) { child = 0; break; }
        usleep( 10000 );
    }
    if (!service)
    {
        WMGL_WARN( "x87: %s did not start serving (this Rosetta may be unsupported); x87 code stays slow\n", sidecar );
        if (child) { kill( child, SIGTERM ); waitpid( child, NULL, 0 ); }
        return;
    }
    if (handshake( service )) WMGL_INFO( "x87: accelerated by %s (pid %d)\n", sidecar, (int)child );
    else
    {
        WMGL_WARN( "x87: %s did not hook Rosetta; x87 code stays slow\n", sidecar );
        kill( child, SIGTERM );
        waitpid( child, NULL, 0 );
    }
    mach_port_deallocate( mach_task_self(), service );
}

static pthread_once_t x87_once = PTHREAD_ONCE_INIT;

/* 32-bit processes only (unix_wow64.c): x86_64 code has no x87 to speak of. */
void wmgl_x87_attach(void)
{
    pthread_once( &x87_once, attach );
}

#endif /* __aarch64__ */
