/* Linux module host, native: glibc's ABI on macOS's libc. See glibc.h.
 *
 * Most of what a library imports means the same on both systems and is passed through: the string, memory,
 * math and allocation functions, and the time functions whose structures have the same layout. The rest is
 * converted here: structures whose layout differs (stat, dirent, utsname), constants whose values differ (open
 * flags, clock ids, sysconf names, errno numbers above 34) and glibc's own internals (__ctype_b_loc and so on).
 *
 * errno: __errno_location is macOS's own errno, so a passed-through function's error is seen by the library.
 * Linux and macOS number errors 1 to 34 the same way; a wrapper that can fail with a higher number converts
 * errno to Linux's before it returns. */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <math.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include <pwd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <utime.h>

#include "elfload.h"
#include "glibc_internal.h"

/* ---------------------------------------------------------------------------------------------------------
 * errno
 */

static const struct { int mac, linux; } errno_map[] =
{
    { EAGAIN, 11 }, { EDEADLK, 35 }, { ENAMETOOLONG, 36 }, { ENOLCK, 37 }, { ENOSYS, 38 }, { ENOTEMPTY, 39 },
    { ELOOP, 40 }, { ENOMSG, 42 }, { EIDRM, 43 }, { ENOSTR, 60 }, { ENODATA, 61 }, { ETIME, 62 }, { ENOSR, 63 },
    { ENOLINK, 67 }, { EPROTO, 71 }, { EMULTIHOP, 72 }, { EBADMSG, 74 }, { EOVERFLOW, 75 }, { EILSEQ, 84 },
    { EUSERS, 87 }, { ENOTSOCK, 88 }, { EDESTADDRREQ, 89 }, { EMSGSIZE, 90 }, { EPROTOTYPE, 91 },
    { ENOPROTOOPT, 92 }, { EPROTONOSUPPORT, 93 }, { ESOCKTNOSUPPORT, 94 }, { EOPNOTSUPP, 95 }, { ENOTSUP, 95 },
    { EPFNOSUPPORT, 96 }, { EAFNOSUPPORT, 97 }, { EADDRINUSE, 98 }, { EADDRNOTAVAIL, 99 }, { ENETDOWN, 100 },
    { ENETUNREACH, 101 }, { ENETRESET, 102 }, { ECONNABORTED, 103 }, { ECONNRESET, 104 }, { ENOBUFS, 105 },
    { EISCONN, 106 }, { ENOTCONN, 107 }, { ESHUTDOWN, 108 }, { ETOOMANYREFS, 109 }, { ETIMEDOUT, 110 },
    { ECONNREFUSED, 111 }, { EHOSTDOWN, 112 }, { EHOSTUNREACH, 113 }, { EALREADY, 114 }, { EINPROGRESS, 115 },
    { ESTALE, 116 }, { EDQUOT, 122 }, { ECANCELED, 125 }, { EOWNERDEAD, 130 }, { ENOTRECOVERABLE, 131 },
};

int glibc_errno_to_linux( int err )
{
    unsigned int i;

    if (err <= 34) return err;
    for (i = 0; i < ARRAY_SIZE(errno_map); i++) if (errno_map[i].mac == err) return errno_map[i].linux;
    return 5;  /* EIO */
}

int glibc_errno_from_linux( int err )
{
    unsigned int i;

    if (err <= 34 && err != 11) return err;
    for (i = 0; i < ARRAY_SIZE(errno_map); i++) if (errno_map[i].linux == err) return errno_map[i].mac;
    return EIO;
}

static int *lnx___errno_location( void ) { return &errno; }

/* ---------------------------------------------------------------------------------------------------------
 * glibc internals and data
 */

uintptr_t lnx___stack_chk_guard = 0x5ba1c0de2a17c9e3ull;
static FILE *lnx_stdin, *lnx_stdout, *lnx_stderr;

static void lnx___stack_chk_fail( void )
{
    fprintf( stderr, "linuxhost: stack smashing detected in a Linux library\n" );
    abort();
}

static void lnx___assert_fail( const char *assertion, const char *file, unsigned int line, const char *function )
{
    fprintf( stderr, "linuxhost: %s:%u: %s: assertion `%s' failed\n", file, line, function ? function : "?",
             assertion );
    abort();
}

/* glibc's character class tables: indexed from -128 to 255, with glibc's bit for each class */
enum { G_UPPER = 0x100, G_LOWER = 0x200, G_ALPHA = 0x400, G_DIGIT = 0x800, G_XDIGIT = 0x1000, G_SPACE = 0x2000,
       G_PRINT = 0x4000, G_GRAPH = 0x8000, G_BLANK = 0x1, G_CNTRL = 0x2, G_PUNCT = 0x4, G_ALNUM = 0x8 };
static unsigned short ctype_b[384];
static int32_t ctype_tolower[384], ctype_toupper[384];
static const unsigned short *ctype_b_ptr = ctype_b + 128;
static const int32_t *ctype_tolower_ptr = ctype_tolower + 128, *ctype_toupper_ptr = ctype_toupper + 128;

static void init_ctype( void )
{
    int c;

    for (c = -128; c < 256; c++)
    {
        unsigned short bits = 0;
        int i = c + 128;

        ctype_tolower[i] = ctype_toupper[i] = c;
        if (c < 0 || c > 127) continue;
        if (isupper( c )) bits |= G_UPPER;
        if (islower( c )) bits |= G_LOWER;
        if (isalpha( c )) bits |= G_ALPHA;
        if (isdigit( c )) bits |= G_DIGIT;
        if (isxdigit( c )) bits |= G_XDIGIT;
        if (isspace( c )) bits |= G_SPACE;
        if (isprint( c )) bits |= G_PRINT;
        if (isgraph( c )) bits |= G_GRAPH;
        if (c == ' ' || c == '\t') bits |= G_BLANK;
        if (iscntrl( c )) bits |= G_CNTRL;
        if (ispunct( c )) bits |= G_PUNCT;
        if (isalnum( c )) bits |= G_ALNUM;
        ctype_b[i] = bits;
        ctype_tolower[i] = tolower( c );
        ctype_toupper[i] = toupper( c );
    }
}

static const unsigned short **lnx___ctype_b_loc( void ) { return &ctype_b_ptr; }
static const int32_t **lnx___ctype_tolower_loc( void ) { return &ctype_tolower_ptr; }
static const int32_t **lnx___ctype_toupper_loc( void ) { return &ctype_toupper_ptr; }
static size_t lnx___ctype_get_mb_cur_max( void ) { return 6; }  /* UTF-8, as glibc counts it */

/* __cxa_atexit: run at the library's unload (never, for now) */
static int lnx___cxa_atexit( void (*func)(void *), void *arg, void *dso ) { return 0; }
static void lnx___cxa_finalize( void *dso ) { }
static int lnx___register_atfork( void (*prepare)(void), void (*parent)(void), void (*child)(void), void *dso )
{
    return 0;
}

static char *lnx___xpg_basename( char *path )
{
    char *p;

    if (!path || !*path) return ".";
    p = path + strlen( path );
    while (p > path + 1 && p[-1] == '/') *--p = 0;
    p = strrchr( path, '/' );
    return p && p[1] ? p + 1 : path;
}

static void *lnx___rawmemchr( const void *s, int c )
{
    const unsigned char *p = s;

    while (*p != (unsigned char)c) p++;
    return (void *)p;
}

static int lnx___xpg_strerror_r( int err, char *buf, size_t len )
{
    return strerror_r( glibc_errno_from_linux( err ), buf, len );
}

/* GNU strerror_r returns the string */
static char *lnx_strerror_r( int err, char *buf, size_t len )
{
    if (strerror_r( glibc_errno_from_linux( err ), buf, len )) snprintf( buf, len, "Unknown error %d", err );
    return buf;
}

static char *lnx_strerror( int err ) { return strerror( glibc_errno_from_linux( err ) ); }

/* ---------------------------------------------------------------------------------------------------------
 * Math the two libcs spell differently
 */

static void lnx_sincos( double x, double *s, double *c ) { *s = sin( x ); *c = cos( x ); }
static void lnx_sincosf( float x, float *s, float *c ) { *s = sinf( x ); *c = cosf( x ); }
static int lnx___signbit( double x ) { return signbit( x ); }
static __int128 lnx___multi3( __int128 a, __int128 b ) { return a * b; }

/* __lttf2: a < b for IEEE quad precision (Linux's long double), which macOS does not have. Returns -1, 0 or 1;
 * 1 when either is NaN. */
static int lnx___lttf2( unsigned __int128 a, unsigned __int128 b )
{
    const unsigned __int128 sign = (unsigned __int128)1 << 127, abs_mask = ~sign;
    const unsigned __int128 inf = (unsigned __int128)0x7fff << 112;
    unsigned __int128 aa = a & abs_mask, ab = b & abs_mask;

    if (aa > inf || ab > inf) return 1;
    if (!(aa | ab)) return 0;
    if ((a & sign) != (b & sign)) return (a & sign) ? -1 : 1;
    if (a == b) return 0;
    if (a & sign) return aa > ab ? -1 : 1;
    return aa < ab ? -1 : 1;
}

/* ---------------------------------------------------------------------------------------------------------
 * Time and system information
 */

static clockid_t clock_from_linux( int id )
{
    switch (id)
    {
    case 0: case 5: return CLOCK_REALTIME;
    case 1: case 6: case 7: return CLOCK_MONOTONIC;
    case 2: return CLOCK_PROCESS_CPUTIME_ID;
    case 3: return CLOCK_THREAD_CPUTIME_ID;
    case 4: return CLOCK_MONOTONIC_RAW;
    default: return (clockid_t)-1;
    }
}

static int lnx_clock_gettime( int id, struct timespec *ts )
{
    clockid_t clock = clock_from_linux( id );

    if (clock == (clockid_t)-1)
    {
        errno = EINVAL;
        return -1;
    }
    return linux_ret( clock_gettime( clock, ts ) );
}

static long lnx_sysconf( int name )
{
    switch (name)
    {
    case 0: return sysconf( _SC_ARG_MAX );
    case 1: return sysconf( _SC_CHILD_MAX );
    case 2: return 100;  /* _SC_CLK_TCK, as on Linux */
    case 3: return sysconf( _SC_NGROUPS_MAX );
    case 4: return sysconf( _SC_OPEN_MAX );
    case 30: return getpagesize();  /* _SC_PAGESIZE */
    case 69: case 70: return 4096;  /* _SC_GETGR_R_SIZE_MAX, _SC_GETPW_R_SIZE_MAX */
    case 75: return 16384;  /* _SC_THREAD_STACK_MIN */
    case 83: return sysconf( _SC_NPROCESSORS_CONF );
    case 84: return sysconf( _SC_NPROCESSORS_ONLN );
    case 85: return sysconf( _SC_PHYS_PAGES );
    case 86: return sysconf( _SC_PHYS_PAGES ) / 2;  /* _SC_AVPHYS_PAGES: macOS has no such count */
    case 149: return 200809;  /* _SC_MONOTONIC_CLOCK */
    case 180: return 64;  /* _SC_HOST_NAME_MAX */
    case 190: return 128;  /* _SC_LEVEL1_DCACHE_LINESIZE */
    default:
        errno = EINVAL;
        return -1;
    }
}

/* Linux's struct utsname. The library is told it runs on Linux, as it would under Proton. */
struct lnx_utsname { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; };

static int lnx_uname( struct lnx_utsname *buf )
{
    memset( buf, 0, sizeof(*buf) );
    strcpy( buf->sysname, "Linux" );
    gethostname( buf->nodename, sizeof(buf->nodename) - 1 );
    strcpy( buf->release, "6.5.0" );
    strcpy( buf->version, "#1 SMP PREEMPT_DYNAMIC" );
    strcpy( buf->machine, "aarch64" );
    strcpy( buf->domainname, "(none)" );
    return 0;
}

/* Linux's struct sysinfo */
struct lnx_sysinfo
{
    long uptime;
    unsigned long loads[3], totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    unsigned short procs, pad;
    unsigned long totalhigh, freehigh;
    unsigned int mem_unit;  /* glibc's padding after it, 20 - 2 * sizeof(long) - sizeof(int), is nothing on 64-bit */
};
_Static_assert( sizeof(struct lnx_sysinfo) == 112, "glibc's struct sysinfo on aarch64 is 112 bytes" );

static int lnx_sysinfo( struct lnx_sysinfo *info )
{
    struct timeval boot;
    size_t size = sizeof(boot);
    uint64_t mem = 0;
    double loads[3];
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };

    memset( info, 0, sizeof(*info) );
    if (!sysctl( mib, 2, &boot, &size, NULL, 0 )) info->uptime = time( NULL ) - boot.tv_sec;
    if (getloadavg( loads, 3 ) == 3)
        for (int i = 0; i < 3; i++) info->loads[i] = (unsigned long)(loads[i] * 65536.0);
    size = sizeof(mem);
    sysctlbyname( "hw.memsize", &mem, &size, NULL, 0 );
    info->totalram = mem;
    info->freeram = mem / 2;
    info->procs = 1;
    info->mem_unit = 1;
    return 0;
}

/* AArch64 hardware capabilities Linux reports, for what every Apple M-series core has */
static unsigned long lnx_getauxval( unsigned long type )
{
    static unsigned char random_bytes[16];
    static int have_random;

    switch (type)
    {
    case 6: return getpagesize();  /* AT_PAGESZ */
    case 15: return (unsigned long)"aarch64";  /* AT_PLATFORM */
    case 16:  /* AT_HWCAP: fp asimd evtstrm aes pmull sha1 sha2 crc32 atomics fphp asimdhp asimdrdm jscvt fcma lrcpc
               * dcpop sha3 asimddp sha512 asimdfhm dit uscat ilrcpc flagm ssbs sb */
        return 0x3fb7f7ff & ~(1ul << 11);
    case 23: return 0;  /* AT_SECURE */
    case 25:  /* AT_RANDOM */
        if (!have_random) { arc4random_buf( random_bytes, sizeof(random_bytes) ); have_random = 1; }
        return (unsigned long)random_bytes;
    case 26: return 0;  /* AT_HWCAP2 */
    case 51: return 5120;  /* AT_MINSIGSTKSZ */
    default:
        errno = 2;  /* ENOENT */
        return 0;
    }
}

/* ---------------------------------------------------------------------------------------------------------
 * Files
 */

static int open_flags_from_linux( int flags )
{
    int ret = flags & 3;

    if (flags & 0x40) ret |= O_CREAT;
    if (flags & 0x80) ret |= O_EXCL;
    if (flags & 0x100) ret |= O_NOCTTY;
    if (flags & 0x200) ret |= O_TRUNC;
    if (flags & 0x400) ret |= O_APPEND;
    if (flags & 0x800) ret |= O_NONBLOCK;
    if (flags & 0x1000) ret |= O_DSYNC;
    if (flags & 0x4000) ret |= O_DIRECTORY;
    if (flags & 0x8000) ret |= O_NOFOLLOW;
    if (flags & 0x80000) ret |= O_CLOEXEC;
    if (flags & 0x101000) ret |= O_SYNC;
    return ret;
}

static int open_flags_to_linux( int flags )
{
    int ret = flags & 3;

    if (flags & O_APPEND) ret |= 0x400;
    if (flags & O_NONBLOCK) ret |= 0x800;
    return ret;
}

/* open's mode is variadic; Linux passes it in a register, which is where a fixed third argument goes too */
static int lnx_open( const char *path, int flags, unsigned int mode )
{
    int fd = glibc_proc_open( path );

    if (fd != -2) return fd;
    return linux_ret( open( path, open_flags_from_linux( flags ), mode ) );
}

static int lnx_creat( const char *path, unsigned int mode ) { return linux_ret( creat( path, mode ) ); }

/* Linux's struct stat on aarch64 (and stat64, the same) */
struct lnx_stat
{
    unsigned long st_dev, st_ino;
    unsigned int st_mode, st_nlink, st_uid, st_gid;
    unsigned long st_rdev, pad1;
    long st_size;
    int st_blksize, pad2;
    long st_blocks;
    long st_atime_sec; unsigned long st_atime_nsec;
    long st_mtime_sec; unsigned long st_mtime_nsec;
    long st_ctime_sec; unsigned long st_ctime_nsec;
    unsigned int unused[2];
};

static void stat_to_linux( const struct stat *st, struct lnx_stat *out )
{
    memset( out, 0, sizeof(*out) );
    out->st_dev = st->st_dev;
    out->st_ino = st->st_ino;
    out->st_mode = st->st_mode;
    out->st_nlink = st->st_nlink;
    out->st_uid = st->st_uid;
    out->st_gid = st->st_gid;
    out->st_rdev = st->st_rdev;
    out->st_size = st->st_size;
    out->st_blksize = st->st_blksize;
    out->st_blocks = st->st_blocks;
    out->st_atime_sec = st->st_atimespec.tv_sec;
    out->st_atime_nsec = st->st_atimespec.tv_nsec;
    out->st_mtime_sec = st->st_mtimespec.tv_sec;
    out->st_mtime_nsec = st->st_mtimespec.tv_nsec;
    out->st_ctime_sec = st->st_ctimespec.tv_sec;
    out->st_ctime_nsec = st->st_ctimespec.tv_nsec;
}

static int proc_stat( const char *path, struct lnx_stat *buf );

static int lnx___xstat( int ver, const char *path, struct lnx_stat *buf )
{
    struct stat st;
    int ret = proc_stat( path, buf );

    if (ret != -2) return ret;
    if (stat( path, &st )) return linux_ret( -1 );
    stat_to_linux( &st, buf );
    return 0;
}

static int lnx___lxstat( int ver, const char *path, struct lnx_stat *buf )
{
    struct stat st;
    int ret = proc_stat( path, buf );

    if (ret != -2) return ret;
    if (lstat( path, &st )) return linux_ret( -1 );
    stat_to_linux( &st, buf );
    return 0;
}

static int lnx___fxstat( int ver, int fd, struct lnx_stat *buf )
{
    struct stat st;

    if (fstat( fd, &st )) return linux_ret( -1 );
    stat_to_linux( &st, buf );
    return 0;
}

/* Linux's struct dirent, kept with the DIR it came from */
struct lnx_dirent { unsigned long d_ino; long d_off; unsigned short d_reclen; unsigned char d_type; char d_name[256]; };
struct lnx_dir { DIR *dir; struct lnx_dirent entry; };

static struct lnx_dir *lnx_opendir( const char *path )
{
    struct lnx_dir *dir = calloc( 1, sizeof(*dir) );

    if (!dir) return NULL;
    if (!(dir->dir = opendir( path )))
    {
        free( dir );
        linux_ret( -1 );
        return NULL;
    }
    return dir;
}

static struct lnx_dirent *lnx_readdir( struct lnx_dir *dir )
{
    struct dirent *entry;

    if (!(entry = readdir( dir->dir ))) return NULL;
    dir->entry.d_ino = entry->d_ino;
    dir->entry.d_off = entry->d_seekoff;
    dir->entry.d_reclen = sizeof(dir->entry);
    dir->entry.d_type = entry->d_type;
    snprintf( dir->entry.d_name, sizeof(dir->entry.d_name), "%s", entry->d_name );
    return &dir->entry;
}

static int lnx_closedir( struct lnx_dir *dir )
{
    int ret = closedir( dir->dir );

    free( dir );
    return linux_ret( ret );
}

static int lnx_access( const char *path, int mode )
{
    int ret = glibc_proc_exists( path );

    if (ret != -2) return ret;
    return linux_ret( access( path, mode ) );
}

/* stat of one of the Linux files: a directory for /proc itself, a read-only file for the rest */
static int proc_stat( const char *path, struct lnx_stat *buf )
{
    int ret = glibc_proc_exists( path );

    if (ret != 0) return ret;
    memset( buf, 0, sizeof(*buf) );
    buf->st_mode = (!strcmp( path, "/proc" ) || !strcmp( path, "/proc/self" )) ? 0040555 : 0100444;
    buf->st_nlink = 1;
    buf->st_blksize = 4096;
    return 0;
}
static int lnx_mkdir( const char *path, unsigned int mode ) { return linux_ret( mkdir( path, mode ) ); }
static int lnx_rmdir( const char *path ) { return linux_ret( rmdir( path ) ); }
static int lnx_unlink( const char *path ) { return linux_ret( unlink( path ) ); }
static int lnx_rename( const char *from, const char *to ) { return linux_ret( rename( from, to ) ); }
static int lnx_chmod( const char *path, unsigned int mode ) { return linux_ret( chmod( path, mode ) ); }
static int lnx_fchmod( int fd, unsigned int mode ) { return linux_ret( fchmod( fd, mode ) ); }
static int lnx_close( int fd ) { return linux_ret( close( fd ) ); }
static long lnx_read( int fd, void *buf, size_t size ) { return linux_ret( read( fd, buf, size ) ); }
static long lnx_write( int fd, const void *buf, size_t size ) { return linux_ret( write( fd, buf, size ) ); }
static long lnx_lseek( int fd, long offset, int whence ) { return linux_ret( lseek( fd, offset, whence ) ); }
static int lnx_fsync( int fd ) { return linux_ret( fsync( fd ) ); }
static int lnx_fdatasync( int fd ) { return linux_ret( fsync( fd ) ); }
static int lnx_ftruncate( int fd, long length ) { return linux_ret( ftruncate( fd, length ) ); }
static int lnx_pipe( int fds[2] ) { return linux_ret( pipe( fds ) ); }

static int lnx_pipe2( int fds[2], int flags )
{
    if (pipe( fds )) return linux_ret( -1 );
    for (int i = 0; i < 2; i++)
    {
        if (flags & 0x80000) fcntl( fds[i], F_SETFD, FD_CLOEXEC );
        if (flags & 0x800) fcntl( fds[i], F_SETFL, fcntl( fds[i], F_GETFL ) | O_NONBLOCK );
    }
    return 0;
}

static long lnx_readlink( const char *path, char *buf, size_t size )
{
    long ret = glibc_proc_readlink( path, buf, size );

    if (ret != -2) return ret;
    return linux_ret( readlink( path, buf, size ) );
}

static int lnx_flock( int fd, int op ) { return linux_ret( flock( fd, op ) ); }
static int lnx_utime( const char *path, const struct utimbuf *times ) { return linux_ret( utime( path, times ) ); }

/* fcntl's third argument is variadic; Linux passes it in a register, as a fixed one */
struct lnx_flock { short l_type, l_whence; long l_start, l_len; int l_pid; };

static int lnx_fcntl( int fd, int cmd, unsigned long arg )
{
    static const short lock_types[] = { F_RDLCK, F_WRLCK, F_UNLCK };
    struct lnx_flock *lfl = (struct lnx_flock *)arg;
    struct flock fl;
    int ret;

    switch (cmd)
    {
    case 0: return linux_ret( fcntl( fd, F_DUPFD, (int)arg ) );
    case 1030: return linux_ret( fcntl( fd, F_DUPFD_CLOEXEC, (int)arg ) );
    case 1: return linux_ret( fcntl( fd, F_GETFD ) );
    case 2: return linux_ret( fcntl( fd, F_SETFD, (int)arg ) );
    case 3:
        if ((ret = fcntl( fd, F_GETFL )) == -1) return linux_ret( -1 );
        return open_flags_to_linux( ret );
    case 4: return linux_ret( fcntl( fd, F_SETFL, open_flags_from_linux( (int)arg ) ) );
    case 5: case 6: case 7:  /* F_GETLK, F_SETLK, F_SETLKW */
        if (lfl->l_type < 0 || lfl->l_type > 2) return linux_fail( 22 );
        fl.l_type = lock_types[lfl->l_type];
        fl.l_whence = lfl->l_whence;
        fl.l_start = lfl->l_start;
        fl.l_len = lfl->l_len;
        fl.l_pid = lfl->l_pid;
        if (fcntl( fd, cmd == 5 ? F_GETLK : cmd == 6 ? F_SETLK : F_SETLKW, &fl ) == -1) return linux_ret( -1 );
        if (cmd == 5)
        {
            lfl->l_type = fl.l_type == F_RDLCK ? 0 : fl.l_type == F_WRLCK ? 1 : 2;
            lfl->l_whence = fl.l_whence;
            lfl->l_start = fl.l_start;
            lfl->l_len = fl.l_len;
            lfl->l_pid = fl.l_pid;
        }
        return 0;
    default:
        return linux_fail( 22 );  /* EINVAL */
    }
}

static int lnx_ioctl( int fd, unsigned long request, unsigned long arg )
{
    switch (request)
    {
    case 0x541b: return linux_ret( ioctl( fd, FIONREAD, (int *)arg ) );  /* FIONREAD */
    case 0x5421: return linux_ret( ioctl( fd, FIONBIO, (int *)arg ) );   /* FIONBIO */
    default: return linux_fail( 25 );  /* ENOTTY */
    }
}

static void *lnx_mmap( void *addr, size_t len, int prot, int flags, int fd, long offset )
{
    int mac_flags = flags & 3;  /* MAP_SHARED, MAP_PRIVATE */
    void *ret;

    if (flags & 0x10) mac_flags |= MAP_FIXED;
    if (flags & 0x20) mac_flags |= MAP_ANON;
    if (flags & 0x4000) mac_flags |= MAP_NORESERVE;
    if ((ret = mmap( addr, len, prot & 7, mac_flags, (flags & 0x20) ? -1 : fd, offset )) == MAP_FAILED)
        errno = glibc_errno_to_linux( errno );
    return ret;
}

static int lnx_madvise( void *addr, size_t len, int advice )
{
    switch (advice)
    {
    case 0: case 1: case 2: case 3: case 4: return linux_ret( madvise( addr, len, advice ) );
    case 8: return linux_ret( madvise( addr, len, MADV_FREE ) );
    default: return 0;  /* advice macOS does not take: Linux would ignore most of it too */
    }
}

static void *lnx_sbrk( long increment ) { errno = 12; return (void *)-1; }
static int lnx_shmget( int key, size_t size, int flags ) { return linux_fail( 38 ); }
static void *lnx_shmat( int id, const void *addr, int flags ) { errno = 38; return (void *)-1; }
static int lnx_shmdt( const void *addr ) { return linux_fail( 22 ); }

/* A Linux library never starts programs or changes credentials in a Wine process */
static int lnx_fork( void ) { return linux_fail( 38 ); }
static int lnx_execl( const char *path, const char *arg, ... ) { return linux_fail( 38 ); }
static FILE *lnx_popen( const char *command, const char *mode ) { errno = 38; return NULL; }
static int lnx_pclose( FILE *f ) { return linux_fail( 10 ); }
static int lnx_setuid( unsigned int uid ) { return linux_fail( 1 ); }
static int lnx_setgid( unsigned int gid ) { return linux_fail( 1 ); }
static int lnx_initgroups( const char *user, unsigned int gid ) { return linux_fail( 1 ); }
static int lnx_tcgetattr( int fd, void *termios ) { return linux_fail( 25 ); }
static int lnx_tcsetattr( int fd, int action, const void *termios ) { return linux_fail( 25 ); }

/* Linux's struct passwd */
struct lnx_passwd { char *pw_name, *pw_passwd; unsigned int pw_uid, pw_gid; char *pw_gecos, *pw_dir, *pw_shell; };

static int lnx_getpwuid_r( unsigned int uid, struct lnx_passwd *out, char *buf, size_t size, struct lnx_passwd **result )
{
    struct passwd pw, *found = NULL;
    char tmp[4096];
    const char *fields[5];
    char **dest[5] = { &out->pw_name, &out->pw_passwd, &out->pw_gecos, &out->pw_dir, &out->pw_shell };
    size_t used = 0;
    int ret, i;

    *result = NULL;
    if ((ret = getpwuid_r( uid, &pw, tmp, sizeof(tmp), &found ))) return glibc_errno_to_linux( ret );
    if (!found) return 0;
    fields[0] = pw.pw_name; fields[1] = pw.pw_passwd; fields[2] = pw.pw_gecos;
    fields[3] = pw.pw_dir; fields[4] = pw.pw_shell;
    for (i = 0; i < 5; i++)
    {
        size_t len = strlen( fields[i] ? fields[i] : "" ) + 1;

        if (used + len > size) return 34;  /* ERANGE */
        memcpy( buf + used, fields[i] ? fields[i] : "", len );
        *dest[i] = buf + used;
        used += len;
    }
    out->pw_uid = pw.pw_uid;
    out->pw_gid = pw.pw_gid;
    *result = out;
    return 0;
}

static struct lnx_passwd *lnx_getpwuid( unsigned int uid )
{
    static struct lnx_passwd pw;
    static char buf[4096];
    struct lnx_passwd *result;

    return lnx_getpwuid_r( uid, &pw, buf, sizeof(buf), &result ) ? NULL : result;
}

static int lnx_kill( int pid, int sig ) { return linux_ret( kill( pid, glibc_signal_from_linux( sig ) ) ); }

/* syscall's arguments are variadic; Linux passes them in registers, as fixed ones */
static long lnx_syscall( long number, long a1, long a2, long a3, long a4, long a5, long a6 )
{
    switch (number)
    {
    case 172: return getpid();   /* getpid */
    case 178:                    /* gettid */
    {
        uint64_t tid;

        pthread_threadid_np( NULL, &tid );
        return (long)tid;
    }
    case 278:                    /* getrandom */
        arc4random_buf( (void *)a1, a2 );
        return a2;
    default:
        return linux_fail( 38 );  /* ENOSYS */
    }
}

/* prctl's arguments are variadic; Linux passes them in registers, as fixed ones */
static int lnx_prctl( int option, unsigned long a2, unsigned long a3, unsigned long a4, unsigned long a5 )
{
    switch (option)
    {
    case 15:  /* PR_SET_NAME */
        pthread_setname_np( (const char *)a2 );
        return 0;
    case 16:  /* PR_GET_NAME */
        pthread_getname_np( pthread_self(), (char *)a2, 16 );
        return 0;
    case 1:   /* PR_SET_PDEATHSIG */
    case 4:   /* PR_SET_DUMPABLE */
    case 38:  /* PR_SET_NO_NEW_PRIVS */
        return 0;
    case 3:   /* PR_GET_DUMPABLE */
        return 1;
    default:
        return linux_fail( 22 );
    }
}

/* ---------------------------------------------------------------------------------------------------------
 * The table
 */

const struct glibc_export glibc_core_exports[] =
{
    /* glibc internals and data */
    ALIAS( "__errno_location", lnx___errno_location ),
    ALIAS( "__stack_chk_guard", &lnx___stack_chk_guard ),
    ALIAS( "__stack_chk_fail", lnx___stack_chk_fail ),
    ALIAS( "__assert_fail", lnx___assert_fail ),
    ALIAS( "__ctype_b_loc", lnx___ctype_b_loc ),
    ALIAS( "__ctype_tolower_loc", lnx___ctype_tolower_loc ),
    ALIAS( "__ctype_toupper_loc", lnx___ctype_toupper_loc ),
    ALIAS( "__ctype_get_mb_cur_max", lnx___ctype_get_mb_cur_max ),
    ALIAS( "__cxa_atexit", lnx___cxa_atexit ),
    ALIAS( "__cxa_finalize", lnx___cxa_finalize ),
    ALIAS( "__register_atfork", lnx___register_atfork ),
    ALIAS( "__xpg_basename", lnx___xpg_basename ),
    ALIAS( "__rawmemchr", lnx___rawmemchr ),
    ALIAS( "__xpg_strerror_r", lnx___xpg_strerror_r ),
    ALIAS( "__strdup", strdup ),
    ALIAS( "__strtok_r", strtok_r ),
    ALIAS( "stdin", &lnx_stdin ),
    ALIAS( "stdout", &lnx_stdout ),
    ALIAS( "stderr", &lnx_stderr ),
    ALIAS( "in6addr_any", &in6addr_any ),
    ALIAS( "in6addr_loopback", &in6addr_loopback ),
    WRAP( strerror ), WRAP( strerror_r ),

    /* strings and memory */
    PASS( memchr ), PASS( memcmp ), PASS( memcpy ), PASS( memmove ), PASS( memset ), PASS( bcmp ),
    PASS( strlen ), PASS( strnlen ), PASS( strcmp ), PASS( strncmp ), PASS( strcpy ), PASS( strncpy ),
    PASS( strcat ), PASS( strchr ), PASS( strrchr ), PASS( strstr ), PASS( strspn ), PASS( strcspn ),
    PASS( strcasecmp ), PASS( strncasecmp ), PASS( strdup ), PASS( strtok ), PASS( strtok_r ),
    PASS( strtol ), PASS( strtoll ), PASS( strtoul ), PASS( strtoull ), PASS( strtod ), PASS( strtof ),
    PASS( qsort ), PASS( rand ), PASS( srand ), PASS( random ),
    PASS( wcslen ), PASS( wcscmp ), PASS( wcsncmp ), PASS( wcsstr ), PASS( wcstol ), PASS( wcstoll ),
    PASS( wcstoul ), PASS( wcstoull ), PASS( wcstod ), PASS( wcstof ),
    PASS( wmemchr ), PASS( wmemcmp ), PASS( wmemcpy ), PASS( wmemmove ), PASS( wmemset ),
    PASS( iswalnum ), PASS( iswalpha ), PASS( iswdigit ), PASS( iswspace ), PASS( iswxdigit ),
    PASS( dirname ),

    /* memory */
    PASS( malloc ), PASS( calloc ), PASS( realloc ), PASS( free ), PASS( posix_memalign ),
    PASS( munmap ), PASS( mprotect ), PASS( mlock ),

    /* environment, process */
    PASS( getenv ), ALIAS( "secure_getenv", getenv ), PASS( setenv ), PASS( unsetenv ),
    PASS( getpid ), PASS( getuid ), PASS( geteuid ), PASS( getgid ), PASS( getegid ), PASS( gethostname ),
    PASS( getentropy ), PASS( abort ), PASS( exit ), PASS( _Exit ), PASS( getpriority ), PASS( setpriority ),
    PASS( getdtablesize ), PASS( getrusage ),
    WRAP( sysconf ), WRAP( uname ), WRAP( sysinfo ), WRAP( getauxval ),

    /* time */
    PASS( time ), PASS( clock ), PASS( usleep ), PASS( nanosleep ), PASS( sched_yield ), PASS( gettimeofday ),
    PASS( gmtime_r ), PASS( localtime_r ), PASS( mktime ), PASS( strftime ),
    WRAP( clock_gettime ),

    /* math */
    PASS( acos ), PASS( acosf ), PASS( asin ), PASS( asinf ), PASS( atan ), PASS( atan2 ), PASS( atan2f ),
    PASS( atanf ), PASS( cos ), PASS( cosf ), PASS( exp ), PASS( exp2 ), PASS( exp2f ), PASS( expf ),
    PASS( expm1 ), PASS( fmod ), PASS( fmodf ), PASS( frexp ), PASS( ldexp ), PASS( ldexpf ), PASS( log ),
    PASS( log10 ), PASS( log10f ), PASS( log2f ), PASS( logf ), PASS( modf ), PASS( nan ), PASS( nanf ),
    PASS( pow ), PASS( powf ), PASS( scalbn ), PASS( scalbnf ), PASS( sin ), PASS( sinf ), PASS( sqrt ),
    PASS( sqrtf ), PASS( tan ), PASS( tanf ), PASS( tanhf ),
    WRAP( sincos ), WRAP( sincosf ), WRAP( __signbit ), WRAP( __multi3 ), WRAP( __lttf2 ),

    /* files */
    WRAP( open ), ALIAS( "open64", lnx_open ), WRAP( creat ), WRAP( close ), WRAP( read ), WRAP( write ),
    WRAP( lseek ), WRAP( fsync ), WRAP( fdatasync ), WRAP( ftruncate ), WRAP( pipe ), WRAP( pipe2 ),
    WRAP( access ), WRAP( mkdir ), WRAP( rmdir ), WRAP( unlink ), WRAP( rename ), WRAP( chmod ), WRAP( fchmod ),
    WRAP( readlink ),
    WRAP( __xstat ), ALIAS( "__xstat64", lnx___xstat ), WRAP( __lxstat ), WRAP( __fxstat ),
    ALIAS( "__fxstat64", lnx___fxstat ),
    WRAP( opendir ), WRAP( readdir ), WRAP( closedir ), WRAP( flock ), WRAP( utime ), WRAP( fcntl ), WRAP( ioctl ),

    /* memory and process */
    WRAP( mmap ), WRAP( madvise ), WRAP( sbrk ), WRAP( shmget ), WRAP( shmat ), WRAP( shmdt ),
    WRAP( fork ), WRAP( execl ), WRAP( popen ), WRAP( pclose ), WRAP( setuid ), WRAP( setgid ), WRAP( initgroups ),
    WRAP( tcgetattr ), WRAP( tcsetattr ), WRAP( getpwuid_r ), WRAP( getpwuid ), WRAP( kill ), WRAP( syscall ),
    WRAP( prctl ),
    END
};

static pthread_once_t init_once = PTHREAD_ONCE_INIT;

static void init( void )
{
    init_ctype();
    lnx_stdin = stdin;
    lnx_stdout = stdout;
    lnx_stderr = stderr;
}

void *glibc_resolve( const char *name, const char *version, void *context )
{
    static const struct glibc_export *const tables[] =
    {
        glibc_core_exports, glibc_thread_exports, glibc_signal_exports, glibc_stdio_exports,
        glibc_net_exports, glibc_locale_exports, glibc_dl_exports,
    };
    unsigned int i;
    const struct glibc_export *e;

    pthread_once( &init_once, init );
    for (i = 0; i < ARRAY_SIZE(tables); i++)
        for (e = tables[i]; e->name; e++) if (!strcmp( e->name, name )) return e->addr;
    return NULL;
}
