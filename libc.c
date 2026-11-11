/* Linux module host: more of glibc, for Easy Anti-Cheat's Linux module (see process.c).
 *
 * Called by Linux code with the System V convention and glibc's types: long is 64 bits, wchar_t 32, errno is
 * Linux's numbering (kept per thread, apart from the C runtime's), struct tm has tm_gmtoff and tm_zone. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"

typedef unsigned long long lx_size_t;
typedef long long lx_long;
typedef uint32_t lx_wchar;
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#define LX_EPERM 1
#define LX_ENOENT 2
#define LX_EINTR 4
#define LX_EBADF 9
#define LX_ENOMEM 12
#define LX_EACCES 13
#define LX_EINVAL 22
#define LX_ERANGE 34
#define LX_ENOSYS 38

/* ---------------------------------------------------------------------------------------------------------
 * errno
 */

static __thread int lx_errno;

int *linuxhost_errno(void) { return &lx_errno; }
static SYSV int *lx___errno_location(void) { return &lx_errno; }

static lx_long fail( int err )
{
    lx_errno = err;
    return -1;
}

static const char *const error_texts[] =
{
    "Success", "Operation not permitted", "No such file or directory", "No such process", "Interrupted system call",
    "Input/output error", "No such device or address", "Argument list too long", "Exec format error",
    "Bad file descriptor", "No child processes", "Resource temporarily unavailable", "Cannot allocate memory",
    "Permission denied", "Bad address", "Block device required", "Device or resource busy", "File exists",
    "Invalid cross-device link", "No such device", "Not a directory", "Is a directory", "Invalid argument",
    "Too many open files in system", "Too many open files", "Inappropriate ioctl for device", "Text file busy",
    "File too large", "No space left on device", "Illegal seek", "Read-only file system", "Too many links",
    "Broken pipe", "Numerical argument out of domain", "Numerical result out of range",
};

static SYSV char *lx_strerror( int err )
{
    static char unknown[32];

    if (err >= 0 && err < (int)(sizeof(error_texts) / sizeof(error_texts[0]))) return (char *)error_texts[err];
    snprintf( unknown, sizeof(unknown), "Unknown error %d", err );
    return unknown;
}

/* ---------------------------------------------------------------------------------------------------------
 * Strings
 */

static SYSV void *lx_memchr( const void *s, int c, lx_size_t n ) { return memchr( s, c, n ); }
static SYSV int lx_atoi( const char *s ) { return atoi( s ); }
static SYSV unsigned long long lx_strtoul( const char *s, char **end, int base ) { return strtoull( s, end, base ); }

static SYSV char *lx___xpg_basename( char *path )
{
    char *p;

    if (!path || !*path) return ".";
    p = path + strlen( path );
    while (p > path + 1 && p[-1] == '/') *--p = 0;
    p = strrchr( path, '/' );
    return p && p[1] ? p + 1 : path;
}

static SYSV char *lx_dirname( char *path )
{
    char *p;

    if (!path || !*path || !strchr( path, '/' )) return ".";
    p = path + strlen( path );
    while (p > path + 1 && p[-1] == '/') *--p = 0;
    p = strrchr( path, '/' );
    if (p == path) { path[1] = 0; return path; }
    *p = 0;
    return path;
}

/* wide characters are 32 bits on Linux */
static SYSV lx_size_t lx_wcslen( const lx_wchar *s ) { const lx_wchar *p = s; while (*p) p++; return p - s; }

static SYSV lx_wchar *lx_wmemchr( const lx_wchar *s, lx_wchar c, lx_size_t n )
{
    for (; n; n--, s++) if (*s == c) return (lx_wchar *)s;
    return NULL;
}

static SYSV int lx_wmemcmp( const lx_wchar *a, const lx_wchar *b, lx_size_t n )
{
    for (; n; n--, a++, b++) if (*a != *b) return *a < *b ? -1 : 1;
    return 0;
}

static SYSV lx_wchar *lx_wmemcpy( lx_wchar *d, const lx_wchar *s, lx_size_t n ) { return memcpy( d, s, n * 4 ); }
static SYSV lx_wchar *lx_wmemmove( lx_wchar *d, const lx_wchar *s, lx_size_t n ) { return memmove( d, s, n * 4 ); }
static SYSV lx_wchar *lx_wmemset( lx_wchar *d, lx_wchar c, lx_size_t n )
{
    for (lx_size_t i = 0; i < n; i++) d[i] = c;
    return d;
}

/* UTF-8 one character: returns its length, or 0 for an invalid sequence */
static int utf8_decode( const unsigned char *s, uint32_t *out )
{
    if (s[0] < 0x80) { *out = s[0]; return 1; }
    if ((s[0] & 0xe0) == 0xc0 && (s[1] & 0xc0) == 0x80) { *out = ((s[0] & 0x1f) << 6) | (s[1] & 0x3f); return 2; }
    if ((s[0] & 0xf0) == 0xe0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80)
    {
        *out = ((s[0] & 0x0f) << 12) | ((s[1] & 0x3f) << 6) | (s[2] & 0x3f);
        return 3;
    }
    if ((s[0] & 0xf8) == 0xf0 && (s[1] & 0xc0) == 0x80 && (s[2] & 0xc0) == 0x80 && (s[3] & 0xc0) == 0x80)
    {
        *out = ((s[0] & 0x07) << 18) | ((s[1] & 0x3f) << 12) | ((s[2] & 0x3f) << 6) | (s[3] & 0x3f);
        return 4;
    }
    return 0;
}

static int utf8_encode( uint32_t c, char *out )
{
    if (c < 0x80) { out[0] = c; return 1; }
    if (c < 0x800) { out[0] = 0xc0 | (c >> 6); out[1] = 0x80 | (c & 0x3f); return 2; }
    if (c < 0x10000) { out[0] = 0xe0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 0x3f); out[2] = 0x80 | (c & 0x3f); return 3; }
    out[0] = 0xf0 | (c >> 18); out[1] = 0x80 | ((c >> 12) & 0x3f); out[2] = 0x80 | ((c >> 6) & 0x3f);
    out[3] = 0x80 | (c & 0x3f);
    return 4;
}

static SYSV lx_size_t lx_mbstowcs( lx_wchar *dst, const char *src, lx_size_t n )
{
    lx_size_t count = 0;
    uint32_t c;
    int len;

    while (*src && (!dst || count < n))
    {
        if (!(len = utf8_decode( (const unsigned char *)src, &c ))) { lx_errno = 84; return (lx_size_t)-1; }
        if (dst) dst[count] = c;
        count++;
        src += len;
    }
    if (dst && count < n) dst[count] = 0;
    return count;
}

static SYSV lx_size_t lx_wcstombs( char *dst, const lx_wchar *src, lx_size_t n )
{
    lx_size_t count = 0;
    char buf[4];

    for (; *src; src++)
    {
        int len = utf8_encode( *src, buf );

        if (dst && count + len > n) break;
        if (dst) memcpy( dst + count, buf, len );
        count += len;
    }
    if (dst && count < n) dst[count] = 0;
    return count;
}

static SYSV char *lx_gettext( const char *msg ) { return (char *)msg; }

/* iconv between UTF-8, UTF-16LE, UTF-32LE (WCHAR_T) and ASCII */
enum charset { CS_UTF8, CS_UTF16, CS_UTF32, CS_INVALID };

static enum charset charset( const char *name )
{
    char n[32];
    size_t i;

    for (i = 0; name[i] && i < sizeof(n) - 1; i++) n[i] = (name[i] >= 'a' && name[i] <= 'z') ? name[i] - 32 : name[i];
    n[i] = 0;
    if (strstr( n, "UTF-8" ) || strstr( n, "UTF8" ) || !strcmp( n, "ASCII" ) || strstr( n, "ANSI_X3.4" )) return CS_UTF8;
    if (strstr( n, "UTF-16" ) || strstr( n, "UCS-2" )) return CS_UTF16;
    if (strstr( n, "UTF-32" ) || strstr( n, "UCS-4" ) || strstr( n, "WCHAR_T" )) return CS_UTF32;
    return CS_INVALID;
}

static SYSV void *lx_iconv_open( const char *to, const char *from )
{
    enum charset t = charset( to ), f = charset( from );

    if (t == CS_INVALID || f == CS_INVALID) { lx_errno = LX_EINVAL; return (void *)-1; }
    return (void *)(uintptr_t)(0x100 | (f << 4) | t);
}

static SYSV int lx_iconv_close( void *cd ) { return 0; }

static SYSV lx_size_t lx_iconv( void *cd, char **in, lx_size_t *in_left, char **out, lx_size_t *out_left )
{
    enum charset f = ((uintptr_t)cd >> 4) & 0xf, t = (uintptr_t)cd & 0xf;

    if (!in || !*in) return 0;
    while (*in_left)
    {
        uint32_t c;
        int len, out_len;
        char buf[4];

        switch (f)
        {
        case CS_UTF8: if (!(len = utf8_decode( (unsigned char *)*in, &c )) || (lx_size_t)len > *in_left) goto bad; break;
        case CS_UTF16:
            if (*in_left < 2) goto bad;
            c = *(uint16_t *)*in; len = 2;
            if (c >= 0xd800 && c < 0xdc00 && *in_left >= 4)
            {
                c = 0x10000 + ((c - 0xd800) << 10) + (((uint16_t *)*in)[1] - 0xdc00);
                len = 4;
            }
            break;
        default: if (*in_left < 4) goto bad; c = *(uint32_t *)*in; len = 4; break;
        }
        switch (t)
        {
        case CS_UTF8: out_len = utf8_encode( c, buf ); break;
        case CS_UTF16:
            if (c >= 0x10000)
            {
                uint16_t pair[2] = { 0xd800 + ((c - 0x10000) >> 10), 0xdc00 + ((c - 0x10000) & 0x3ff) };
                memcpy( buf, pair, 4 ); out_len = 4;
            }
            else { uint16_t u = c; memcpy( buf, &u, 2 ); out_len = 2; }
            break;
        default: memcpy( buf, &c, 4 ); out_len = 4; break;
        }
        if ((lx_size_t)out_len > *out_left) { lx_errno = 7; return (lx_size_t)-1; }  /* E2BIG */
        memcpy( *out, buf, out_len );
        *out += out_len; *out_left -= out_len;
        *in += len; *in_left -= len;
    }
    return 0;
bad:
    lx_errno = 84;  /* EILSEQ */
    return (lx_size_t)-1;
}

/* ---------------------------------------------------------------------------------------------------------
 * Memory
 */

static SYSV int lx_mprotect( void *addr, lx_size_t len, int prot )
{
    static const DWORD protections[8] =
    {
        PAGE_NOACCESS, PAGE_READONLY, PAGE_READWRITE, PAGE_READWRITE,
        PAGE_EXECUTE, PAGE_EXECUTE_READ, PAGE_EXECUTE_READWRITE, PAGE_EXECUTE_READWRITE,
    };
    DWORD old;

    if (!VirtualProtect( addr, len, protections[prot & 7], &old )) return fail( LX_ENOMEM );
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------
 * Time: Linux's struct timespec and timeval have 64-bit fields, struct tm two more than Windows'
 */

struct lx_timespec { long long sec, nsec; };
struct lx_timeval { long long sec, usec; };
struct lx_tm { int sec, min, hour, mday, mon, year, wday, yday, isdst; int pad; long long gmtoff; const char *zone; };

static long long unix_time_100ns(void)
{
    FILETIME ft;
    ULARGE_INTEGER t;

    GetSystemTimePreciseAsFileTime( &ft );
    t.LowPart = ft.dwLowDateTime;
    t.HighPart = ft.dwHighDateTime;
    return t.QuadPart - 116444736000000000ll;
}

static SYSV lx_long lx_time( lx_long *t )
{
    lx_long now = unix_time_100ns() / 10000000;

    if (t) *t = now;
    return now;
}

static SYSV int lx_clock_gettime( int clock, struct lx_timespec *ts )
{
    if (clock == 0 || clock == 5)  /* CLOCK_REALTIME, _COARSE */
    {
        long long t = unix_time_100ns();

        ts->sec = t / 10000000;
        ts->nsec = (t % 10000000) * 100;
        return 0;
    }
    if (clock == 2 || clock == 3)  /* CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID */
    {
        FILETIME create, exit_time, kernel, user;
        ULARGE_INTEGER k, u;

        if (clock == 2) GetProcessTimes( GetCurrentProcess(), &create, &exit_time, &kernel, &user );
        else GetThreadTimes( GetCurrentThread(), &create, &exit_time, &kernel, &user );
        k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
        u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
        ts->sec = (k.QuadPart + u.QuadPart) / 10000000;
        ts->nsec = ((k.QuadPart + u.QuadPart) % 10000000) * 100;
        return 0;
    }
    {
        LARGE_INTEGER count, freq;

        QueryPerformanceCounter( &count );
        QueryPerformanceFrequency( &freq );
        ts->sec = count.QuadPart / freq.QuadPart;
        ts->nsec = (count.QuadPart % freq.QuadPart) * 1000000000ll / freq.QuadPart;
        return 0;
    }
}

static SYSV int lx_gettimeofday( struct lx_timeval *tv, void *tz )
{
    long long t = unix_time_100ns();

    if (tv)
    {
        tv->sec = t / 10000000;
        tv->usec = (t % 10000000) / 10;
    }
    return 0;
}

static SYSV struct lx_tm *lx_gmtime_r( const lx_long *t, struct lx_tm *out )
{
    __time64_t tt = *t;
    struct tm tm;

    if (_gmtime64_s( &tm, &tt )) return NULL;
    memset( out, 0, sizeof(*out) );
    out->sec = tm.tm_sec; out->min = tm.tm_min; out->hour = tm.tm_hour; out->mday = tm.tm_mday;
    out->mon = tm.tm_mon; out->year = tm.tm_year; out->wday = tm.tm_wday; out->yday = tm.tm_yday;
    out->zone = "GMT";
    return out;
}

static SYSV struct lx_tm *lx_gmtime( const lx_long *t )
{
    static __thread struct lx_tm tm;

    return lx_gmtime_r( t, &tm );
}

static SYSV double lx_difftime( lx_long a, lx_long b ) { return (double)(a - b); }

static SYSV lx_size_t lx_strftime( char *s, lx_size_t max, const char *fmt, const struct lx_tm *ltm )
{
    struct tm tm = { ltm->sec, ltm->min, ltm->hour, ltm->mday, ltm->mon, ltm->year, ltm->wday, ltm->yday, ltm->isdst };

    return strftime( s, max, fmt, &tm );
}

static SYSV lx_size_t lx_wcsftime( lx_wchar *s, lx_size_t max, const lx_wchar *fmt, const struct lx_tm *ltm )
{
    char narrow_fmt[256], narrow_out[512];
    lx_size_t i, len;

    for (i = 0; fmt[i] && i < sizeof(narrow_fmt) - 1; i++) narrow_fmt[i] = fmt[i] < 128 ? (char)fmt[i] : '?';
    narrow_fmt[i] = 0;
    len = lx_strftime( narrow_out, sizeof(narrow_out), narrow_fmt, ltm );
    if (len >= max) return 0;
    for (i = 0; i <= len; i++) s[i] = (unsigned char)narrow_out[i];
    return len;
}

static SYSV unsigned int lx_alarm( unsigned int seconds ) { return 0; }

/* ---------------------------------------------------------------------------------------------------------
 * Environment: the process's Windows environment, kept in Linux code's own copies so getenv's results last
 */

struct env_entry { char *name, *value; };
static struct env_entry *env_cache;
static unsigned int env_count;
static SRWLOCK env_lock = SRWLOCK_INIT;

static SYSV char *lx_getenv( const char *name )
{
    char buffer[32768];
    DWORD len;
    unsigned int i;
    char *ret = NULL;

    if (!name) return NULL;
    AcquireSRWLockExclusive( &env_lock );
    len = GetEnvironmentVariableA( name, buffer, sizeof(buffer) );
    for (i = 0; i < env_count; i++) if (!strcmp( env_cache[i].name, name )) break;
    if (len && len < sizeof(buffer))
    {
        if (i < env_count && !strcmp( env_cache[i].value, buffer )) ret = env_cache[i].value;
        else
        {
            if (i == env_count)
            {
                struct env_entry *cache = realloc( env_cache, (env_count + 1) * sizeof(*cache) );

                if (cache)
                {
                    env_cache = cache;
                    env_cache[env_count].name = _strdup( name );
                    env_cache[env_count].value = NULL;
                    env_count++;
                }
            }
            if (i < env_count)
            {
                env_cache[i].value = _strdup( buffer );  /* the old copy stays valid: a caller may still hold it */
                ret = env_cache[i].value;
            }
        }
    }
    ReleaseSRWLockExclusive( &env_lock );
    return ret;
}

static SYSV int lx_setenv( const char *name, const char *value, int overwrite )
{
    if (!overwrite && GetEnvironmentVariableA( name, NULL, 0 )) return 0;
    return SetEnvironmentVariableA( name, value ) ? 0 : fail( LX_ENOMEM );
}

static SYSV int lx_unsetenv( const char *name )
{
    SetEnvironmentVariableA( name, NULL );
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------
 * The process
 */

/* ---------------------------------------------------------------------------------------------------------
 * fork and exec, as launchers use them: fork, a few calls in the child, then exec or _exit. fork saves the
 * caller's registers and returns 0, so the child's branch runs here; exec starts the program as a Windows process
 * and then the parent's branch resumes at fork's return with the new process's id, as after vfork.
 */

struct fork_context
{
    unsigned long long rbx, rbp, r12, r13, r14, r15, rsp, rip;
    int active;
};
static __thread struct fork_context fork_ctx;

/* lx_fork saves the registers a call keeps and the return address; fork_resume returns from it again with rax */
__asm__( ".text\n"
         ".globl lx_fork_entry\n"
         "lx_fork_entry:\n"
         "    pushq %rdi\n"
         "    subq $0x28, %rsp\n"
         "    call fork_context_ptr\n"          /* rax: this thread's context */
         "    addq $0x28, %rsp\n"
         "    popq %rdi\n"
         "    movq %rbx, 0x00(%rax)\n"
         "    movq %rbp, 0x08(%rax)\n"
         "    movq %r12, 0x10(%rax)\n"
         "    movq %r13, 0x18(%rax)\n"
         "    movq %r14, 0x20(%rax)\n"
         "    movq %r15, 0x28(%rax)\n"
         "    leaq 8(%rsp), %rcx\n"
         "    movq %rcx, 0x30(%rax)\n"           /* the stack pointer after fork returns */
         "    movq (%rsp), %rcx\n"
         "    movq %rcx, 0x38(%rax)\n"           /* where fork returns to */
         "    movl $1, 0x40(%rax)\n"
         "    xorl %eax, %eax\n"                  /* the child's branch */
         "    ret\n"
         ".globl fork_resume\n"
         "fork_resume:\n"                         /* rcx: context, rdx: pid (Windows convention) */
         "    movl $0, 0x40(%rcx)\n"
         "    movq 0x00(%rcx), %rbx\n"
         "    movq 0x08(%rcx), %rbp\n"
         "    movq 0x10(%rcx), %r12\n"
         "    movq 0x18(%rcx), %r13\n"
         "    movq 0x20(%rcx), %r14\n"
         "    movq 0x28(%rcx), %r15\n"
         "    movq 0x30(%rcx), %rsp\n"
         "    movq %rdx, %rax\n"
         "    jmp *0x38(%rcx)\n" );
extern void lx_fork_entry(void);
extern void fork_resume( struct fork_context *ctx, long long pid ) __attribute__((noreturn));

struct fork_context *fork_context_ptr(void) { return &fork_ctx; }

/* the processes started this way, for waitpid and kill */
static struct
{
    int pid;
    HANDLE process, thread;
    int status, reaped;
    int traced;          /* started under PTRACE_TRACEME */
    int stops[4];        /* the stops waitpid reports in turn: the child's own SIGSTOP, then exec */
    int stop_count, stop_index;
    int awaiting_cont;   /* a stop was reported; the tracer has not continued it yet */
    int suspended;       /* the Windows process has not started running */
    unsigned long long options;
} children[32];
static unsigned int child_count;
static __thread int trace_next_exec;  /* the forked child asked for PTRACE_TRACEME */
static __thread int sigstop_before_exec;  /* the forked child stopped itself (raise(SIGSTOP)) */

enum { STOP_SIGSTOP = 1, STOP_EXEC = 2 };

static int add_child( HANDLE process, DWORD pid )
{
    if (child_count < 32)
    {
        memset( &children[child_count], 0, sizeof(children[0]) );
        children[child_count].pid = pid;
        children[child_count].process = process;
        child_count++;
    }
    return pid;
}

static int find_child( int pid )
{
    unsigned int i;

    for (i = 0; i < child_count; i++) if (children[i].pid == pid) return i;
    return -1;
}

static void append_quoted( WCHAR *cmd, size_t size, const char *arg )
{
    WCHAR warg[2048];
    size_t len = wcslen( cmd );

    MultiByteToWideChar( CP_UTF8, 0, arg, -1, warg, ARRAY_SIZE(warg) );
    _snwprintf( cmd + len, size - len, len ? L" \"%s\"" : L"\"%s\"", warg );
}

/* Starts what exec was asked for, as a Windows process: a Windows program (.exe) directly; Wine's own launcher
 * with its first argument as the program. Returns the process id, or 0. */
static int spawn( const char *path, char *const argv[] )
{
    typedef WCHAR *(CDECL *get_dos_fn)( const char * );
    get_dos_fn get_dos = (get_dos_fn)GetProcAddress( GetModuleHandleA( "kernel32.dll" ), "wine_get_dos_file_name" );
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    WCHAR cmd[8192] = L"", *app = NULL;
    const char *base = strrchr( path, '/' ) ? strrchr( path, '/' ) + 1 : path;
    int first = 1, i;

    for (i = 0; argv && argv[i]; i++) linuxhost_trace( "exec %s argv[%d] = %s\n", path, i, argv[i] );
    /* wine, wine64 or a preloader: the program is the first argument */
    if (!strncmp( base, "wine", 4 )) first = 1;
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
    linuxhost_trace( "exec: CreateProcess %ls%s\n", cmd, trace_next_exec ? " (traced: suspended)" : "" );
    if (!CreateProcessW( app, cmd, NULL, NULL, FALSE, trace_next_exec ? CREATE_SUSPENDED : 0, NULL, NULL, &si, &pi ))
    {
        linuxhost_trace( "exec: CreateProcess failed: %lu\n", GetLastError() );
        if (app) HeapFree( GetProcessHeap(), 0, app );
        return 0;
    }
    if (app) HeapFree( GetProcessHeap(), 0, app );
    add_child( pi.hProcess, pi.dwProcessId );
    children[child_count - 1].thread = pi.hThread;
    if (trace_next_exec)
    {
        /* a traced child stops where Linux would stop it: at its own SIGSTOP and after exec. The Windows
         * process waits suspended until the tracer continues it past the last of those. */
        children[child_count - 1].traced = 1;
        if (sigstop_before_exec) children[child_count - 1].stops[children[child_count - 1].stop_count++] = STOP_SIGSTOP;
        children[child_count - 1].stops[children[child_count - 1].stop_count++] = STOP_EXEC;
        children[child_count - 1].suspended = 1;
        trace_next_exec = 0;
        sigstop_before_exec = 0;
    }
    return pi.dwProcessId;
}

static SYSV int lx_execv( const char *path, char *const argv[] )
{
    int pid = spawn( path, argv );

    if (!fork_ctx.active)
    {
        linuxhost_trace( "exec %s without fork: the process ends\n", path );
        if (pid) ExitProcess( 0 );
        *linuxhost_errno() = 2;
        return -1;
    }
    if (!pid) pid = add_child( NULL, 0x7fff0000 + child_count );  /* a child that failed to exec: exits 127 */
    if (!children[child_count - 1].process) children[child_count - 1].status = 127 << 8, children[child_count - 1].reaped = 0;
    fork_resume( &fork_ctx, pid );
}

static SYSV int lx_execvp( const char *file, char *const argv[] ) { return lx_execv( file, argv ); }

static SYSV int lx_waitpid( int pid, int *status, int options )
{
    unsigned int i;

    for (i = 0; i < child_count; i++)
    {
        DWORD code;

        if (pid != -1 && children[i].pid != pid) continue;
        if (children[i].reaped) continue;
        if (children[i].traced && !children[i].awaiting_cont && children[i].stop_index < children[i].stop_count)
        {
            int stop = children[i].stops[children[i].stop_index++], st;

            if (stop == STOP_SIGSTOP) st = (19 << 8) | 0x7f;                       /* stopped by SIGSTOP */
            else if (children[i].options & 0x10) st = ((5 | (4 << 8)) << 8) | 0x7f;  /* PTRACE_EVENT_EXEC */
            else st = (5 << 8) | 0x7f;                                              /* SIGTRAP after exec */
            children[i].awaiting_cont = 1;
            if (status) *status = st;
            linuxhost_trace( "waitpid %d: stopped, status %#x\n", children[i].pid, st );
            return children[i].pid;
        }
        if (children[i].traced && children[i].awaiting_cont)
        {
            if (options & 1) return 0;  /* WNOHANG: still stopped */
            linuxhost_trace( "waitpid %d: waits on a stopped child that was not continued\n", children[i].pid );
        }
        if (children[i].process)
        {
            if (WaitForSingleObject( children[i].process, (options & 1) ? 0 : INFINITE ) == WAIT_TIMEOUT) return 0;
            GetExitCodeProcess( children[i].process, &code );
            children[i].status = (code & 0xff) << 8;
        }
        children[i].reaped = 1;
        if (status) *status = children[i].status;
        linuxhost_trace( "waitpid %d: status %#x\n", children[i].pid, children[i].status );
        return children[i].pid;
    }
    *linuxhost_errno() = 10;  /* ECHILD */
    return -1;
}

static SYSV int lx_kill( int pid, int sig )
{
    unsigned int i;

    linuxhost_trace( "kill %d %d\n", pid, sig );
    for (i = 0; i < child_count; i++)
    {
        if (children[i].pid != pid) continue;
        if (!sig) return children[i].process && WaitForSingleObject( children[i].process, 0 ) == WAIT_TIMEOUT ? 0 : (*linuxhost_errno() = 3, -1);
        if (children[i].process && (sig == 9 || sig == 15)) TerminateProcess( children[i].process, 128 + sig );
        return 0;
    }
    if (pid == (int)GetCurrentProcessId()) return 0;
    *linuxhost_errno() = 3;  /* ESRCH */
    return -1;
}

/* ptrace on the Windows processes started by exec: what has a Windows counterpart works (being traced from the
 * start, the stop after exec, continuing, reading and writing memory); the rest is logged and refused */
static SYSV long long lx_ptrace( int request, int pid, void *addr, void *data )
{
    int i = find_child( pid );
    unsigned long long value = 0;
    SIZE_T done;

    switch (request)
    {
    case 0:  /* PTRACE_TRACEME */
        if (!fork_ctx.active) break;
        trace_next_exec = 1;
        linuxhost_trace( "ptrace TRACEME in a forked child\n" );
        return 0;
    case 1: case 2:  /* PTRACE_PEEKTEXT, PTRACE_PEEKDATA */
        if (i < 0 || !children[i].traced) break;
        if (!ReadProcessMemory( children[i].process, addr, &value, 8, &done ) || done != 8)
        {
            *linuxhost_errno() = 5;  /* EIO */
            return -1;
        }
        *linuxhost_errno() = 0;
        return value;
    case 4: case 5:  /* PTRACE_POKETEXT, PTRACE_POKEDATA */
        if (i < 0 || !children[i].traced) break;
        value = (unsigned long long)data;
        if (!WriteProcessMemory( children[i].process, addr, &value, 8, &done ) || done != 8)
        {
            *linuxhost_errno() = 5;
            return -1;
        }
        return 0;
    case 7: case 17:  /* PTRACE_CONT, PTRACE_DETACH */
        if (i < 0 || !children[i].traced) break;
        children[i].awaiting_cont = 0;
        if (children[i].suspended && (children[i].stop_index >= children[i].stop_count || request == 17))
        {
            ResumeThread( children[i].thread );
            children[i].suspended = 0;
        }
        if (request == 17) children[i].traced = 0;
        linuxhost_trace( "ptrace %s %d\n", request == 7 ? "CONT" : "DETACH", pid );
        return 0;
    case 0x4200:  /* PTRACE_SETOPTIONS */
        if (i < 0 || !children[i].traced) break;
        children[i].options = (unsigned long long)data;
        linuxhost_trace( "ptrace SETOPTIONS %d %#llx\n", pid, children[i].options );
        return 0;
    case 0x4201:  /* PTRACE_GETEVENTMSG */
        if (i < 0 || !children[i].traced) break;
        *(unsigned long long *)data = 0;
        return 0;
    }
    linuxhost_trace( "ptrace %#x pid %d addr %p data %p%s: refused\n", request, pid, addr, data,
                     fork_ctx.active ? " (in a forked child)" : "" );
    *linuxhost_errno() = i < 0 ? 3 : 5;  /* ESRCH, EIO */
    return -1;
}

static SYSV int lx_raise( int sig )
{
    linuxhost_trace( "raise %d%s\n", sig, fork_ctx.active ? " (in a forked child)" : "" );
    if (sig == 19 && fork_ctx.active && trace_next_exec)
    {
        sigstop_before_exec = 1;  /* reported to the tracer by waitpid */
        return 0;
    }
    if (sig == 6) { linuxhost_trace( "abort\n" ); ExitProcess( 134 ); }
    return 0;
}

static SYSV int lx_getpid(void) { return GetCurrentProcessId(); }
static SYSV int lx_getppid(void) { return 1; }
static SYSV unsigned int lx_getuid(void) { return 1000; }
static SYSV void lx_abort(void) { linuxhost_trace( "abort\n" ); ExitProcess( 134 ); }
static SYSV void lx_exit( int code )
{
    linuxhost_trace( "exit %d%s\n", code, fork_ctx.active ? " (a forked child)" : "" );
    if (fork_ctx.active)
    {
        int pid = add_child( NULL, 0x7fff0000 + child_count );

        children[child_count - 1].status = (code & 0xff) << 8;
        fork_resume( &fork_ctx, pid );
    }
    ExitProcess( code );
}
static SYSV void lx___stack_chk_fail(void) { linuxhost_trace( "stack smashing detected\n" ); ExitProcess( 134 ); }

/* Linux's struct utsname. The module is on Linux, as it would be under Proton. */
struct lx_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

static SYSV int lx_uname( struct lx_utsname *u )
{
    DWORD size = sizeof(u->nodename) - 1;

    memset( u, 0, sizeof(*u) );
    strcpy( u->sysname, "Linux" );
    if (!GetComputerNameA( u->nodename, &size )) strcpy( u->nodename, "localhost" );
    strcpy( u->release, "6.5.0" );
    strcpy( u->version, "#1 SMP PREEMPT_DYNAMIC" );
    strcpy( u->machine, "x86_64" );
    strcpy( u->domainname, "(none)" );
    return 0;
}

static SYSV lx_long lx_sysconf( int name )
{
    SYSTEM_INFO si;
    MEMORYSTATUSEX mem = { sizeof(mem) };

    GetSystemInfo( &si );
    switch (name)
    {
    case 2: return 100;                                 /* _SC_CLK_TCK */
    case 4: return 1024;                                /* _SC_OPEN_MAX */
    case 30: return 4096;                               /* _SC_PAGESIZE */
    case 83: case 84: return si.dwNumberOfProcessors;   /* _SC_NPROCESSORS_CONF, _ONLN */
    case 85: GlobalMemoryStatusEx( &mem ); return mem.ullTotalPhys / 4096;  /* _SC_PHYS_PAGES */
    case 86: GlobalMemoryStatusEx( &mem ); return mem.ullAvailPhys / 4096;  /* _SC_AVPHYS_PAGES */
    case 69: case 70: return 4096;                      /* _SC_GETGR_R_SIZE_MAX, _SC_GETPW_R_SIZE_MAX */
    default: return fail( LX_EINVAL );
    }
}

/* syscall(number, ...): Linux passes the arguments in registers, as fixed ones */
static SYSV lx_long lx_syscall( lx_long number, lx_long a1, lx_long a2, lx_long a3, lx_long a4, lx_long a5, lx_long a6 )
{
    lx_long ret = linuxhost_syscall( number, a1, a2, a3, a4, a5, a6 );

    if (ret < 0 && ret > -4096) return fail( -ret );
    return ret;
}

static SYSV int lx_prctl( int option, unsigned long long a2, unsigned long long a3, unsigned long long a4,
                          unsigned long long a5 )
{
    linuxhost_trace( "prctl %d %#llx\n", option, a2 );
    switch (option)
    {
    case 15: case 1: case 4: case 38: return 0;  /* PR_SET_NAME, PR_SET_PDEATHSIG, PR_SET_DUMPABLE, PR_SET_NO_NEW_PRIVS */
    case 3: return 1;                            /* PR_GET_DUMPABLE */
    case 16: strcpy( (char *)a2, "eac" ); return 0;  /* PR_GET_NAME */
    default: return fail( LX_EINVAL );
    }
}

/* ---------------------------------------------------------------------------------------------------------
 * Threads: glibc's pthread_mutex_t (40 bytes) holds a pointer to a critical section, made on first use; the kind
 * of a statically initialised one is its fifth int
 */

struct lx_mutex { CRITICAL_SECTION *cs; int pad[2]; int kind; int rest[5]; };
struct lx_mutexattr { int kind; };

static CRITICAL_SECTION *get_cs( struct lx_mutex *m )
{
    CRITICAL_SECTION *cs = m->cs, *prev;

    if (cs) return cs;
    if (!(cs = malloc( sizeof(*cs) ))) abort();
    InitializeCriticalSection( cs );
    if ((prev = InterlockedCompareExchangePointer( (void **)&m->cs, cs, NULL )))
    {
        DeleteCriticalSection( cs );
        free( cs );
        return prev;
    }
    return cs;
}

static SYSV int lx_pthread_mutexattr_init( struct lx_mutexattr *a ) { a->kind = 0; return 0; }
static SYSV int lx_pthread_mutexattr_settype( struct lx_mutexattr *a, int kind ) { a->kind = kind; return 0; }

static SYSV int lx_pthread_mutex_init( struct lx_mutex *m, const struct lx_mutexattr *a )
{
    memset( m, 0, sizeof(*m) );
    m->kind = a ? a->kind : 0;
    return 0;
}

static SYSV int lx_pthread_mutex_destroy( struct lx_mutex *m )
{
    if (m->cs) { DeleteCriticalSection( m->cs ); free( m->cs ); m->cs = NULL; }
    return 0;
}

/* critical sections are recursive, which serves every glibc kind: a normal mutex is never relocked by its owner */
static SYSV int lx_pthread_mutex_lock( struct lx_mutex *m ) { EnterCriticalSection( get_cs( m ) ); return 0; }
static SYSV int lx_pthread_mutex_trylock( struct lx_mutex *m ) { return TryEnterCriticalSection( get_cs( m ) ) ? 0 : 16; }
static SYSV int lx_pthread_mutex_unlock( struct lx_mutex *m ) { LeaveCriticalSection( get_cs( m ) ); return 0; }

static SYSV int lx___pthread_key_create( unsigned int *key, void (*destructor)(void *) )
{
    DWORD index = FlsAlloc( NULL );  /* the destructor is Linux code: not called, threads here rarely end */

    if (index == FLS_OUT_OF_INDEXES) return 11;
    *key = index;
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------
 * Compiler support and what the module asks for but does not need here
 */

static SYSV unsigned __int128 lx___udivti3( unsigned __int128 a, unsigned __int128 b ) { return a / b; }

static SYSV void lx_unwind_unsupported(void)
{
    linuxhost_trace( "a C++ exception was thrown: not supported yet\n" );
    ExitProcess( 71 );
}

static SYSV int lx_dladdr( const void *addr, void *info )
{
    struct { const char *fname; void *fbase; const char *sname; void *saddr; } *out = info;
    struct elf_module *module = elf_find_address( addr );
    void *base;
    size_t size;

    if (!module) return 0;
    elf_get_range( module, &base, &size );
    out->fname = elf_get_name( module );
    out->fbase = base;
    out->sname = NULL;
    out->saddr = NULL;
    return 1;
}

static SYSV int lx_dl_iterate_phdr( void *callback, void *data )
{
    linuxhost_trace( "dl_iterate_phdr: no modules listed\n" );
    return 0;
}

const struct libc_function linuxhost_libc_functions[] =
{
    { "__errno_location", lx___errno_location }, { "strerror", lx_strerror },
    { "memchr", lx_memchr }, { "atoi", lx_atoi }, { "strtoul", lx_strtoul }, { "__xpg_basename", lx___xpg_basename },
    { "dirname", lx_dirname }, { "wcslen", lx_wcslen }, { "wmemchr", lx_wmemchr }, { "wmemcmp", lx_wmemcmp },
    { "wmemcpy", lx_wmemcpy }, { "wmemmove", lx_wmemmove }, { "wmemset", lx_wmemset }, { "mbstowcs", lx_mbstowcs },
    { "wcstombs", lx_wcstombs }, { "gettext", lx_gettext }, { "iconv_open", lx_iconv_open }, { "iconv", lx_iconv },
    { "iconv_close", lx_iconv_close },
    { "mprotect", lx_mprotect },
    { "time", lx_time }, { "clock_gettime", lx_clock_gettime }, { "gettimeofday", lx_gettimeofday },
    { "gmtime_r", lx_gmtime_r }, { "gmtime", lx_gmtime }, { "difftime", lx_difftime }, { "strftime", lx_strftime },
    { "wcsftime", lx_wcsftime }, { "alarm", lx_alarm },
    { "getenv", lx_getenv }, { "secure_getenv", lx_getenv }, { "setenv", lx_setenv }, { "unsetenv", lx_unsetenv },
    { "fork", lx_fork_entry }, { "execv", lx_execv }, { "execvp", lx_execvp }, { "waitpid", lx_waitpid },
    { "kill", lx_kill }, { "_exit", lx_exit }, { "ptrace", lx_ptrace }, { "raise", lx_raise },
    { "getpid", lx_getpid }, { "getppid", lx_getppid }, { "getuid", lx_getuid }, { "abort", lx_abort },
    { "exit", lx_exit }, { "__stack_chk_fail", lx___stack_chk_fail }, { "uname", lx_uname },
    { "sysconf", lx_sysconf }, { "syscall", lx_syscall }, { "prctl", lx_prctl },
    { "pthread_mutexattr_init", lx_pthread_mutexattr_init }, { "pthread_mutexattr_settype", lx_pthread_mutexattr_settype },
    { "pthread_mutex_init", lx_pthread_mutex_init }, { "pthread_mutex_destroy", lx_pthread_mutex_destroy },
    { "pthread_mutex_lock", lx_pthread_mutex_lock }, { "pthread_mutex_trylock", lx_pthread_mutex_trylock },
    { "pthread_mutex_unlock", lx_pthread_mutex_unlock }, { "__pthread_key_create", lx___pthread_key_create },
    { "__udivti3", lx___udivti3 },
    { "_Unwind_RaiseException", lx_unwind_unsupported }, { "_Unwind_Resume", lx_unwind_unsupported },
    { "_Unwind_Resume_or_Rethrow", lx_unwind_unsupported },
    { "dladdr", lx_dladdr }, { "dl_iterate_phdr", lx_dl_iterate_phdr },
    { NULL, NULL }
};
