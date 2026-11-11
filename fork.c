/* Linux module host: fork, exec, ptrace and waitpid, as Easy Anti-Cheat's module launches a game with them.
 *
 * The module forks; the child asks to be traced (PTRACE_TRACEME), stops itself (raise(SIGSTOP)) so that the parent
 * can set it up, then prepares and execs the game. The parent waits for the child's stops with waitpid, sets trace
 * options, may read or write the child, and continues it with PTRACE_CONT.
 *
 * A Windows process cannot fork. Here the parent and the child are two contexts on the same thread, each with its
 * own copy of the thread's stack, and the heap shared. fork saves the parent's registers and stack and runs the
 * child; the child's SIGSTOP saves the child and resumes the parent at fork's return with the child's id; the
 * parent's PTRACE_CONT saves the parent and resumes the child; the child's exec starts the program as a Windows
 * process (suspended, while it is traced) and resumes the parent where it continued the child. A child that execs
 * without stopping, or exits, resumes the parent at fork's return. Memory the parent reads or writes in a child
 * that has not exec'd yet is the child's stack copy for addresses in the stack, and shared memory otherwise. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

/* ---------------------------------------------------------------------------------------------------------
 * Contexts
 */

/* the registers a System V call keeps, where the call returns to, and the stack from just below rsp to the base */
struct context
{
    unsigned long long rbx, rbp, r12, r13, r14, r15, rsp, rip;  /* 0x00 .. 0x38 */
    char *lo;                                                    /* 0x40: the start of the stack copy */
    char *copy;                                                  /* 0x48 */
    unsigned long long len;                                      /* 0x50 */
    char *alt_stack;                                             /* 0x58: the top of a stack to copy from */
};

/* where the last captured call came from (capture entries, below) */
static __thread struct context caller;
static __thread char alt_stack[0x8000] __attribute__((aligned(16)));

struct context *caller_context(void) { return &caller; }

/* Entry points that record their caller's registers and return address before the C function runs, with the
 * arguments as they came: rdi, rsi, rdx, rcx, r8, r9 are kept around the call that finds this thread's record. */
#define CAPTURE(name, target) \
    __asm__( ".text\n" \
             ".globl " #name "\n" \
             #name ":\n" \
             "    pushq %rdi\n" \
             "    pushq %rsi\n" \
             "    pushq %rdx\n" \
             "    pushq %rcx\n" \
             "    pushq %r8\n" \
             "    pushq %r9\n" \
             "    subq $0x28, %rsp\n" \
             "    call caller_context\n" \
             "    addq $0x28, %rsp\n" \
             "    popq %r9\n" \
             "    popq %r8\n" \
             "    popq %rcx\n" \
             "    popq %rdx\n" \
             "    popq %rsi\n" \
             "    popq %rdi\n" \
             "    movq %rbx, 0x00(%rax)\n" \
             "    movq %rbp, 0x08(%rax)\n" \
             "    movq %r12, 0x10(%rax)\n" \
             "    movq %r13, 0x18(%rax)\n" \
             "    movq %r14, 0x20(%rax)\n" \
             "    movq %r15, 0x28(%rax)\n" \
             "    leaq 8(%rsp), %r11\n" \
             "    movq %r11, 0x30(%rax)\n" \
             "    movq (%rsp), %r11\n" \
             "    movq %r11, 0x38(%rax)\n" \
             "    jmp " #target "\n" )

/* switch_to( ctx, rax ): copies the context's stack back in place, from another stack, and returns from the call it
 * was captured at with rax. Windows convention (called from C): rcx is the context, rdx the result. */
__asm__( ".text\n"
         "switch_to:\n"
         "    movq 0x58(%rcx), %rsp\n"
         "    pushq %rdx\n"
         "    pushq %rcx\n"
         "    movq 0x40(%rcx), %rdi\n"
         "    movq 0x48(%rcx), %rsi\n"
         "    movq 0x50(%rcx), %rcx\n"
         "    cld\n"
         "    rep movsb\n"
         "    popq %rcx\n"
         "    popq %rax\n"
         "    movq 0x00(%rcx), %rbx\n"
         "    movq 0x08(%rcx), %rbp\n"
         "    movq 0x10(%rcx), %r12\n"
         "    movq 0x18(%rcx), %r13\n"
         "    movq 0x20(%rcx), %r14\n"
         "    movq 0x28(%rcx), %r15\n"
         "    movq 0x30(%rcx), %rsp\n"
         "    jmp *0x38(%rcx)\n" );
extern void switch_to( struct context *ctx, long long rax ) __attribute__((noreturn));

/* keeps a copy of a captured context and of the stack it runs on, from its red zone up to the thread's stack base */
static void save_context( struct context *to, const struct context *from )
{
    char *base = (char *)((NT_TIB *)NtCurrentTeb())->StackBase;

    free( to->copy );
    *to = *from;
    to->lo = (char *)from->rsp - 256;
    to->len = base - to->lo;
    if (!(to->copy = malloc( to->len ))) abort();
    memcpy( to->copy, to->lo, to->len );
    to->alt_stack = alt_stack + sizeof(alt_stack);
}

/* ---------------------------------------------------------------------------------------------------------
 * Children
 */

enum child_state { CHILD_NONE, CHILD_RUNNING, CHILD_STOPPED, CHILD_EXECED, CHILD_EXITED };
enum { STOP_SIGSTOP = 1, STOP_EXEC = 2 };

struct child
{
    int pid;                 /* what the module sees: fixed at fork, as on Linux */
    enum child_state state;
    HANDLE process, thread;  /* after exec */
    int status;              /* the wait status once it ended */
    int reaped;
    int traced;
    int stops[4], stop_count, stop_index;  /* what waitpid reports in turn */
    int awaiting_cont;
    int suspended;           /* the Windows process has not started running */
    unsigned long long options;
};

static struct child children[16];
static unsigned int child_count;
static int next_pid = 0x7000;

/* the fork in progress on this thread: which side runs, and the saved contexts */
static __thread struct child *forking;   /* the child of the fork in progress */
static __thread int in_child;            /* the child's side is running */
static __thread struct context parent_ctx, child_ctx;
static __thread int parent_in_cont;      /* the parent is in PTRACE_CONT, to be resumed after the child's exec */

static struct child *find_child( int pid )
{
    unsigned int i;

    for (i = 0; i < child_count; i++) if (children[i].pid == pid) return &children[i];
    return NULL;
}

/* fork: the parent's context is saved and the child's branch runs (fork returns 0) */
SYSV int lx_fork_impl(void)
{
    struct child *c;

    if (forking || child_count == ARRAY_SIZE(children))
    {
        *linuxhost_errno() = 11;  /* EAGAIN */
        return -1;
    }
    c = &children[child_count++];
    memset( c, 0, sizeof(*c) );
    c->pid = next_pid++;
    c->state = CHILD_RUNNING;
    save_context( &parent_ctx, &caller );
    forking = c;
    in_child = 1;
    parent_in_cont = 0;
    linuxhost_trace( "fork: child %d\n", c->pid );
    return 0;
}
CAPTURE( lx_fork, lx_fork_impl );

/* back to the parent after the child exec'd or exited: where it continued the child, or at fork's return */
static void __attribute__((noreturn)) resume_parent(void)
{
    struct child *c = forking;

    in_child = 0;
    forking = NULL;
    if (parent_in_cont)
    {
        parent_in_cont = 0;
        switch_to( &parent_ctx, 0 );   /* PTRACE_CONT returns 0 */
    }
    switch_to( &parent_ctx, c->pid );  /* fork returns the child's id */
}

/* ---------------------------------------------------------------------------------------------------------
 * exec
 */

static void append_quoted( WCHAR *cmd, size_t size, const char *arg )
{
    WCHAR warg[2048];
    size_t len = wcslen( cmd );

    MultiByteToWideChar( CP_UTF8, 0, arg, -1, warg, ARRAY_SIZE(warg) );
    _snwprintf( cmd + len, size - len, len ? L" \"%s\"" : L"\"%s\"", warg );
}

/* Starts what exec was asked for, as a Windows process: a Windows program (.exe) directly; Wine's loader with its
 * first argument as the program. Returns 0 on failure. */
static int spawn( struct child *c, const char *path, char *const argv[] )
{
    typedef WCHAR *(CDECL *get_dos_fn)( const char * );
    get_dos_fn get_dos = (get_dos_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "wine_get_dos_file_name" );
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmd[8192] = L"", *app = NULL;
    const char *base = strrchr( path, '/' ) ? strrchr( path, '/' ) + 1 : path;
    int first = 1, i;

    for (i = 0; argv && argv[i]; i++) linuxhost_trace( "exec %s argv[%d] = %s\n", path, i, argv[i] );
    if (!strncmp( base, "wine", 4 ))
    {
        /* Wine's loader or preloader: its leading arguments name Wine's binaries; the program follows them */
        while (argv && argv[first] && argv[first][0] == '/' &&
               !strncmp( strrchr( argv[first], '/' ) + 1, "wine", 4 )) first++;
    }
    else
    {
        size_t len = strlen( path );

        if (len < 4 || _stricmp( path + len - 4, ".exe" ))
        {
            linuxhost_trace( "exec %s: not a Windows program, not started\n", path );
            return 0;
        }
        if (get_dos && path[0] == '/') app = get_dos( path );
        append_quoted( cmd, ARRAY_SIZE(cmd), path );
    }
    for (i = first; argv && argv[i]; i++) append_quoted( cmd, ARRAY_SIZE(cmd), argv[i] );
    linuxhost_trace( "exec: CreateProcess %ls%s\n", cmd, c->traced ? " (traced: suspended)" : "" );
    linuxhost_export_fds();
    if (!CreateProcessW( app, cmd, NULL, NULL, FALSE, c->traced ? CREATE_SUSPENDED : 0, NULL, NULL, &si, &pi ))
    {
        linuxhost_trace( "exec: CreateProcess failed: %lu\n", GetLastError() );
        if (app) HeapFree( GetProcessHeap(), 0, app );
        return 0;
    }
    if (app) HeapFree( GetProcessHeap(), 0, app );
    SetEnvironmentVariableA( "LINUXHOST_INHERIT", NULL );
    c->process = pi.hProcess;
    c->thread = pi.hThread;
    c->state = CHILD_EXECED;
    if (c->traced)
    {
        c->stops[c->stop_count++] = STOP_EXEC;  /* Linux stops a traced program after exec */
        c->suspended = 1;
    }
    linuxhost_trace( "exec: child %d is Windows process %lu\n", c->pid, pi.dwProcessId );
    return 1;
}

static SYSV int lx_execv( const char *path, char *const argv[] )
{
    struct child *c = forking;

    if (!in_child || !c)
    {
        linuxhost_trace( "exec %s outside a forked child: not supported\n", path );
        *linuxhost_errno() = 38;
        return -1;
    }
    if (!spawn( c, path, argv ))
    {
        *linuxhost_errno() = 2;  /* ENOENT: the child carries on, as after a failed exec */
        return -1;
    }
    resume_parent();
}

static SYSV int lx_execvp( const char *file, char *const argv[] ) { return lx_execv( file, argv ); }

SYSV void lx_exit_impl( int code )
{
    linuxhost_trace( "exit %d%s\n", code, in_child ? " (a forked child)" : "" );
    if (in_child && forking)
    {
        forking->state = CHILD_EXITED;
        forking->status = (code & 0xff) << 8;
        resume_parent();
    }
    ExitProcess( code );
}
CAPTURE( lx_exit, lx_exit_impl );

/* ---------------------------------------------------------------------------------------------------------
 * Signals, ptrace and waiting
 */

SYSV int lx_raise_impl( int sig )
{
    linuxhost_trace( "raise %d%s\n", sig, in_child ? " (in a forked child)" : "" );
    if (sig == 19 && in_child && forking && forking->traced)
    {
        /* the child stops: its context is kept, and the parent runs until it continues it */
        forking->state = CHILD_STOPPED;
        forking->stops[forking->stop_count++] = STOP_SIGSTOP;
        save_context( &child_ctx, &caller );
        in_child = 0;
        linuxhost_trace( "child %d stopped; the parent runs\n", forking->pid );
        switch_to( &parent_ctx, forking->pid );  /* fork returns the child's id */
    }
    if (sig == 6) { linuxhost_trace( "abort\n" ); ExitProcess( 134 ); }
    return 0;
}
CAPTURE( lx_raise, lx_raise_impl );

/* memory of a child: its stack copy while it has not exec'd, the Windows process after */
static int child_memory( struct child *c, void *addr, void *buf, int write )
{
    SIZE_T done;

    if (c->state == CHILD_EXECED)
        return write ? WriteProcessMemory( c->process, addr, buf, 8, &done ) && done == 8
                     : ReadProcessMemory( c->process, addr, buf, 8, &done ) && done == 8;
    if (c == forking && c->state == CHILD_STOPPED && (char *)addr >= child_ctx.lo &&
        (char *)addr + 8 <= child_ctx.lo + child_ctx.len)
    {
        char *p = child_ctx.copy + ((char *)addr - child_ctx.lo);

        if (write) memcpy( p, buf, 8 ); else memcpy( buf, p, 8 );
        return 1;
    }
    {
        MEMORY_BASIC_INFORMATION info;

        if (VirtualQuery( addr, &info, sizeof(info) ) != sizeof(info) || info.State != MEM_COMMIT) return 0;
        if (write) memcpy( addr, buf, 8 ); else memcpy( buf, addr, 8 );
        return 1;
    }
}

SYSV long long lx_ptrace_impl( int request, int pid, void *addr, void *data )
{
    struct child *c = find_child( pid );
    unsigned long long value = 0;

    switch (request)
    {
    case 0:  /* PTRACE_TRACEME */
        if (!in_child || !forking) break;
        forking->traced = 1;
        linuxhost_trace( "ptrace TRACEME in child %d\n", forking->pid );
        return 0;
    case 1: case 2:  /* PTRACE_PEEKTEXT, PTRACE_PEEKDATA */
        if (!c || !c->traced) break;
        if (!child_memory( c, addr, &value, 0 )) { *linuxhost_errno() = 5; return -1; }
        linuxhost_trace( "ptrace PEEK %d %p = %#llx\n", pid, addr, value );
        *linuxhost_errno() = 0;
        return value;
    case 4: case 5:  /* PTRACE_POKETEXT, PTRACE_POKEDATA */
        if (!c || !c->traced) break;
        value = (unsigned long long)data;
        linuxhost_trace( "ptrace POKE %d %p = %#llx\n", pid, addr, value );
        if (!child_memory( c, addr, &value, 1 )) { *linuxhost_errno() = 5; return -1; }
        return 0;
    case 7: case 17: case 24:  /* PTRACE_CONT, PTRACE_DETACH, PTRACE_SYSCALL (for now without its syscall stops) */
        if (!c || !c->traced) break;
        linuxhost_trace( "ptrace %s %d\n", request == 7 ? "CONT" : request == 17 ? "DETACH" : "SYSCALL", pid );
        c->awaiting_cont = 0;
        if (request == 17) c->traced = 0;
        if (c == forking && c->state == CHILD_STOPPED)
        {
            /* the child goes on from its SIGSTOP; the parent continues after the child's exec */
            save_context( &parent_ctx, &caller );
            parent_in_cont = 1;
            c->state = CHILD_RUNNING;
            in_child = 1;
            switch_to( &child_ctx, 0 );  /* raise returns 0 */
        }
        if (c->suspended && (c->stop_index >= c->stop_count || request == 17))
        {
            ResumeThread( c->thread );
            c->suspended = 0;
        }
        return 0;
    case 0x4200:  /* PTRACE_SETOPTIONS */
        if (!c || !c->traced) break;
        c->options = (unsigned long long)data;
        linuxhost_trace( "ptrace SETOPTIONS %d %#llx\n", pid, c->options );
        return 0;
    case 0x4201:  /* PTRACE_GETEVENTMSG */
        if (!c || !c->traced) break;
        *(unsigned long long *)data = 0;
        return 0;
    }
    linuxhost_trace( "ptrace %#x pid %d addr %p data %p: refused\n", request, pid, addr, data );
    *linuxhost_errno() = c ? 5 : 3;  /* EIO, ESRCH */
    return -1;
}
CAPTURE( lx_ptrace, lx_ptrace_impl );

static SYSV int lx_waitpid( int pid, int *status, int options )
{
    unsigned int i;

    for (i = 0; i < child_count; i++)
    {
        struct child *c = &children[i];
        DWORD code;

        if (pid != -1 && c->pid != pid) continue;
        if (c->reaped) continue;
        if (c->traced && !c->awaiting_cont && c->stop_index < c->stop_count)
        {
            int stop = c->stops[c->stop_index++], st;

            if (stop == STOP_SIGSTOP) st = (19 << 8) | 0x7f;
            else if (c->options & 0x10) st = ((5 | (4 << 8)) << 8) | 0x7f;  /* PTRACE_EVENT_EXEC */
            else st = (5 << 8) | 0x7f;                                      /* SIGTRAP */
            c->awaiting_cont = 1;
            if (status) *status = st;
            linuxhost_trace( "waitpid %d: stopped, status %#x\n", c->pid, st );
            return c->pid;
        }
        if (c->state == CHILD_EXITED)
        {
            c->reaped = 1;
            if (status) *status = c->status;
            linuxhost_trace( "waitpid %d: exited, status %#x\n", c->pid, c->status );
            return c->pid;
        }
        if (c->state == CHILD_EXECED && c->process)
        {
            if (c->awaiting_cont && (options & 1)) return 0;
            if (WaitForSingleObject( c->process, (options & 1) ? 0 : INFINITE ) == WAIT_TIMEOUT) return 0;
            GetExitCodeProcess( c->process, &code );
            c->status = (code & 0xff) << 8;
            c->reaped = 1;
            if (status) *status = c->status;
            linuxhost_trace( "waitpid %d: exited, status %#x\n", c->pid, c->status );
            return c->pid;
        }
        if (options & 1) return 0;
        linuxhost_trace( "waitpid %d: would wait on a child that cannot change state\n", c->pid );
        return 0;
    }
    *linuxhost_errno() = 10;  /* ECHILD */
    return -1;
}

static SYSV int lx_kill( int pid, int sig )
{
    struct child *c = find_child( pid );

    linuxhost_trace( "kill %d %d\n", pid, sig );
    if (!c)
    {
        if (pid == (int)GetCurrentProcessId() || pid == 0) return 0;
        *linuxhost_errno() = 3;
        return -1;
    }
    if (!sig) return c->state == CHILD_EXITED || (c->process && WaitForSingleObject( c->process, 0 ) != WAIT_TIMEOUT)
                     ? (*linuxhost_errno() = 3, -1) : 0;
    if (c->process && (sig == 9 || sig == 15)) TerminateProcess( c->process, 128 + sig );
    return 0;
}

extern void lx_fork(void), lx_exit(void), lx_raise(void), lx_ptrace(void);

const struct libc_function linuxhost_fork_functions[] =
{
    { "fork", lx_fork }, { "execv", lx_execv }, { "execvp", lx_execvp }, { "_exit", lx_exit }, { "exit", lx_exit },
    { "raise", lx_raise }, { "ptrace", lx_ptrace }, { "waitpid", lx_waitpid }, { "kill", lx_kill },
    { NULL, NULL }
};
