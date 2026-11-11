/* Linux module host: the rest of the glibc a Linux C++ library imports — locales (libstdc++'s GNU locale model),
 * wide characters, message catalogs, memory mappings, the password database, POSIX shared memory, signals and rand.
 *
 * There is one locale, "C", with UTF-8 as its multibyte encoding (as glibc's C.UTF-8); libstdc++ reads its character
 * class and case tables straight from the locale object, so they are laid out as glibc's struct __locale_struct. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wctype.h>

#include "elfload.h"
#include "glibc.h"
#include "libc.h"
#include "process.h"

typedef unsigned long long lx_size_t;
typedef long long lx_long;
typedef uint32_t lx_wchar;
#define LX_WEOF 0xffffffffu

static lx_long fail( int err )
{
    *linuxhost_errno() = err;
    return -1;
}

/* ---------------------------------------------------------------------------------------------------------
 * Locales
 */

/* glibc's character classes, as its little-endian _ISbit() places them */
enum { IS_UPPER = 0x100, IS_LOWER = 0x200, IS_ALPHA = 0x400, IS_DIGIT = 0x800, IS_XDIGIT = 0x1000, IS_SPACE = 0x2000,
       IS_PRINT = 0x4000, IS_GRAPH = 0x8000, IS_BLANK = 0x1, IS_CNTRL = 0x2, IS_PUNCT = 0x4, IS_ALNUM = 0x8 };

struct lx_locale
{
    void *locales[13];
    const unsigned short *ctype_b;
    const int *ctype_tolower, *ctype_toupper;
    const char *names[13];
};

/* indexed -128..255, as glibc's */
static unsigned short ctype_b[384];
static int ctype_tolower[384], ctype_toupper[384];
static struct lx_locale c_locale;
static __thread struct lx_locale *current_locale;  /* NULL: the global one (LC_GLOBAL_LOCALE) */

static unsigned short class_of( int c )
{
    unsigned short m = 0;

    if (c < 0 || c > 127) return 0;
    if (c >= 'A' && c <= 'Z') m |= IS_UPPER | IS_ALPHA | IS_ALNUM | IS_PRINT | IS_GRAPH;
    if (c >= 'a' && c <= 'z') m |= IS_LOWER | IS_ALPHA | IS_ALNUM | IS_PRINT | IS_GRAPH;
    if (c >= '0' && c <= '9') m |= IS_DIGIT | IS_ALNUM | IS_PRINT | IS_GRAPH;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= IS_XDIGIT;
    if (c == ' ' || (c >= '\t' && c <= '\r')) m |= IS_SPACE;
    if (c == ' ' || c == '\t') m |= IS_BLANK;
    if (c < 32 || c == 127) m |= IS_CNTRL;
    if (c == ' ') m |= IS_PRINT;
    if (c > 32 && c < 127 && !(m & IS_ALNUM)) m |= IS_PUNCT | IS_PRINT | IS_GRAPH;
    return m;
}

static struct lx_locale *get_c_locale(void)
{
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    BOOL pending;
    int i;

    if (!InitOnceBeginInitialize( &once, 0, &pending, NULL ) || !pending) return &c_locale;
    for (i = -128; i < 256; i++)
    {
        ctype_b[i + 128] = class_of( i );
        ctype_tolower[i + 128] = (i >= 'A' && i <= 'Z') ? i + 32 : i;
        ctype_toupper[i + 128] = (i >= 'a' && i <= 'z') ? i - 32 : i;
    }
    c_locale.ctype_b = ctype_b + 128;
    c_locale.ctype_tolower = ctype_tolower + 128;
    c_locale.ctype_toupper = ctype_toupper + 128;
    for (i = 0; i < 13; i++) c_locale.names[i] = "C";
    InitOnceComplete( &once, 0, NULL );
    return &c_locale;
}

/* every locale is "C": a name the host has no data for gets C's rules, as a C.UTF-8 system would give */
static SYSV struct lx_locale *lx___newlocale( int mask, const char *name, struct lx_locale *base )
{
    linuxhost_trace( "newlocale %#x \"%s\"\n", mask, name ? name : "(null)" );
    return get_c_locale();
}
static SYSV struct lx_locale *lx___duplocale( struct lx_locale *l ) { return get_c_locale(); }
static SYSV void lx___freelocale( struct lx_locale *l ) {}

static SYSV struct lx_locale *lx___uselocale( struct lx_locale *l )
{
    struct lx_locale *old = current_locale ? current_locale : (struct lx_locale *)-1;

    if (l) current_locale = l == (struct lx_locale *)-1 ? NULL : l;
    return old;
}

static SYSV char *lx_setlocale( int category, const char *name ) { return (char *)"C"; }

/* nl_langinfo: C's answers; an LC_NUMERIC wide character is returned in place of the pointer, as glibc does */
static SYSV const char *lx___nl_langinfo_l( int item, struct lx_locale *l )
{
    static const char *const days[] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
    static const char *const months[] = { "January", "February", "March", "April", "May", "June", "July", "August",
                                          "September", "October", "November", "December" };
    static char abbrev[19][4];
    int i;

    switch (item)
    {
    case 14: return "UTF-8";                              /* CODESET */
    case 0x10000: return ".";                             /* RADIXCHAR */
    case 0x10001: return "";                              /* THOUSEP */
    case 0x10002: return "";                              /* GROUPING */
    case 0x10003: return (const char *)(uintptr_t)'.';    /* _NL_NUMERIC_DECIMAL_POINT_WC */
    case 0x10004: return (const char *)(uintptr_t)0;      /* _NL_NUMERIC_THOUSANDS_SEP_WC */
    case 0x20026: return "AM";
    case 0x20027: return "PM";
    case 0x20028: return "%a %b %e %H:%M:%S %Y";          /* D_T_FMT */
    case 0x20029: return "%m/%d/%y";                      /* D_FMT */
    case 0x2002a: return "%H:%M:%S";                      /* T_FMT */
    case 0x2002b: return "%I:%M:%S %p";                   /* T_FMT_AMPM */
    }
    if (item >= 0x20000 && item < 0x20026)
    {
        int n = item - 0x20000;

        if (n >= 7 && n < 14) return days[n - 7];
        if (n >= 26) return months[n - 26];
        if (!abbrev[0][0])
        {
            for (i = 0; i < 7; i++) memcpy( abbrev[i], days[i], 3 );
            for (i = 0; i < 12; i++) memcpy( abbrev[7 + i], months[i], 3 );
        }
        return n < 7 ? abbrev[n] : abbrev[7 + n - 14];
    }
    return "";
}

static SYSV int lx___strcoll_l( const char *a, const char *b, struct lx_locale *l ) { return strcmp( a, b ); }

static SYSV lx_size_t lx___strxfrm_l( char *dst, const char *src, lx_size_t n, struct lx_locale *l )
{
    lx_size_t len = strlen( src );

    if (len < n) memcpy( dst, src, len + 1 );
    return len;
}

static lx_size_t wlen( const lx_wchar *s ) { lx_size_t n = 0; while (s[n]) n++; return n; }

static SYSV int lx_wcscmp( const lx_wchar *a, const lx_wchar *b )
{
    while (*a && *a == *b) { a++; b++; }
    return *a < *b ? -1 : *a > *b;
}
static SYSV int lx___wcscoll_l( const lx_wchar *a, const lx_wchar *b, struct lx_locale *l ) { return lx_wcscmp( a, b ); }

static SYSV lx_size_t lx___wcsxfrm_l( lx_wchar *dst, const lx_wchar *src, lx_size_t n, struct lx_locale *l )
{
    lx_size_t len = wlen( src );

    if (len < n) memcpy( dst, src, (len + 1) * sizeof(*src) );
    return len;
}

typedef SYSV lx_size_t (*strftime_fn)( char *, lx_size_t, const char *, const void * );
typedef SYSV lx_size_t (*wcsftime_fn)( lx_wchar *, lx_size_t, const lx_wchar *, const void * );

static SYSV lx_size_t lx___strftime_l( char *s, lx_size_t max, const char *fmt, const void *tm, struct lx_locale *l )
{
    return ((strftime_fn)linuxhost_resolve( "strftime", NULL ))( s, max, fmt, tm );
}

static SYSV lx_size_t lx___wcsftime_l( lx_wchar *s, lx_size_t max, const lx_wchar *fmt, const void *tm,
                                       struct lx_locale *l )
{
    return ((wcsftime_fn)linuxhost_resolve( "wcsftime", NULL ))( s, max, fmt, tm );
}

static SYSV double lx___strtod_l( const char *s, char **end, struct lx_locale *l ) { return strtod( s, end ); }
static SYSV float lx___strtof_l( const char *s, char **end, struct lx_locale *l ) { return strtof( s, end ); }
static SYSV long double lx_strtold_l( const char *s, char **end, struct lx_locale *l ) { return strtold( s, end ); }

/* ---- wide character classes ---- */

static SYSV lx_wchar lx_towlower( lx_wchar c )
{
    if (c < 128) return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    return c <= 0xffff ? towlower( (wint_t)c ) : c;
}

static SYSV lx_wchar lx_towupper( lx_wchar c )
{
    if (c < 128) return (c >= 'a' && c <= 'z') ? c - 32 : c;
    return c <= 0xffff ? towupper( (wint_t)c ) : c;
}

static SYSV lx_wchar lx___towlower_l( lx_wchar c, struct lx_locale *l ) { return lx_towlower( c ); }
static SYSV lx_wchar lx___towupper_l( lx_wchar c, struct lx_locale *l ) { return lx_towupper( c ); }

static const char *const wctype_names[] = { NULL, "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower",
                                            "print", "punct", "space", "upper", "xdigit" };

static SYSV unsigned long long lx___wctype_l( const char *name, struct lx_locale *l )
{
    unsigned int i;

    for (i = 1; i < ARRAY_SIZE(wctype_names); i++) if (!strcmp( name, wctype_names[i] )) return i;
    return 0;
}

static SYSV int lx___iswctype_l( lx_wchar c, unsigned long long desc, struct lx_locale *l )
{
    static const unsigned short ascii[] = { 0, IS_ALNUM, IS_ALPHA, IS_BLANK, IS_CNTRL, IS_DIGIT, IS_GRAPH, IS_LOWER,
                                            IS_PRINT, IS_PUNCT, IS_SPACE, IS_UPPER, IS_XDIGIT };
    static const wctype_t windows[] = { 0, _ALPHA | _DIGIT, _ALPHA, _BLANK, _CONTROL, _DIGIT,
                                        _ALPHA | _DIGIT | _PUNCT, _LOWER, _ALPHA | _DIGIT | _PUNCT | _BLANK, _PUNCT,
                                        _SPACE, _UPPER, _HEX };

    if (!desc || desc >= ARRAY_SIZE(ascii)) return 0;
    if (c < 128) return !!(class_of( c ) & ascii[desc]);
    if (c > 0xffff) return desc == 2 || desc == 1 || desc == 6 || desc == 8;  /* astral: letters, as glibc's UTF-8 */
    return !!iswctype( (wint_t)c, windows[desc] );
}

static SYSV int lx_isspace( int c ) { return c == ' ' || (c >= '\t' && c <= '\r'); }
static SYSV lx_size_t lx___ctype_get_mb_cur_max(void) { return 6; }
static SYSV lx_wchar lx_btowc( int c ) { return c >= 0 && c < 128 ? (lx_wchar)c : LX_WEOF; }
static SYSV int lx_wctob( lx_wchar c ) { return c < 128 ? (int)c : -1; }

/* ---- UTF-8 conversions; there is no shift state ---- */

/* one character from at most n bytes: its length, 0 for NUL, -2 if incomplete, -1 if invalid */
static long long decode( const unsigned char *s, lx_size_t n, lx_wchar *out )
{
    lx_wchar c;
    int len, i;

    if (!n) return -2;
    if (s[0] < 0x80) { *out = s[0]; return s[0] ? 1 : 0; }
    if ((s[0] & 0xe0) == 0xc0) { len = 2; c = s[0] & 0x1f; }
    else if ((s[0] & 0xf0) == 0xe0) { len = 3; c = s[0] & 0x0f; }
    else if ((s[0] & 0xf8) == 0xf0) { len = 4; c = s[0] & 0x07; }
    else return -1;
    for (i = 1; i < len; i++)
    {
        if ((lx_size_t)i >= n) return -2;
        if ((s[i] & 0xc0) != 0x80) return -1;
        c = (c << 6) | (s[i] & 0x3f);
    }
    if ((len == 2 && c < 0x80) || (len == 3 && c < 0x800) || (len == 4 && c < 0x10000) || c > 0x10ffff ||
        (c >= 0xd800 && c <= 0xdfff))
        return -1;
    *out = c;
    return len;
}

static int encode( lx_wchar c, char *out )
{
    if (c < 0x80) { out[0] = c; return 1; }
    if (c < 0x800) { out[0] = 0xc0 | (c >> 6); out[1] = 0x80 | (c & 0x3f); return 2; }
    if (c >= 0xd800 && c <= 0xdfff) return -1;
    if (c < 0x10000) { out[0] = 0xe0 | (c >> 12); out[1] = 0x80 | ((c >> 6) & 0x3f); out[2] = 0x80 | (c & 0x3f); return 3; }
    if (c > 0x10ffff) return -1;
    out[0] = 0xf0 | (c >> 18); out[1] = 0x80 | ((c >> 12) & 0x3f); out[2] = 0x80 | ((c >> 6) & 0x3f);
    out[3] = 0x80 | (c & 0x3f);
    return 4;
}

static SYSV lx_size_t lx_mbrtowc( lx_wchar *pwc, const char *s, lx_size_t n, void *state )
{
    lx_wchar c = 0;
    long long ret;

    if (!s) return 0;
    if ((ret = decode( (const unsigned char *)s, n, &c )) == -1) return fail( 84 );  /* EILSEQ */
    if (ret == -2) return (lx_size_t)-2;
    if (pwc) *pwc = c;
    return ret;
}

static SYSV lx_size_t lx_mbsnrtowcs( lx_wchar *dst, const char **src, lx_size_t nms, lx_size_t len, void *state )
{
    const unsigned char *s = (const unsigned char *)*src, *end = s + nms;
    lx_size_t count = 0;

    while (!dst || count < len)
    {
        lx_wchar c;
        long long ret = decode( s, end - s, &c );

        if (ret == -1) { if (dst) *src = (const char *)s; return fail( 84 ); }
        if (ret == -2) break;  /* the rest of the input is an incomplete character */
        if (dst) dst[count] = c;
        if (!ret)
        {
            if (dst) *src = NULL;
            return count;
        }
        count++;
        s += ret;
    }
    if (dst) *src = (const char *)s;
    return count;
}

static SYSV lx_size_t lx_mbsrtowcs( lx_wchar *dst, const char **src, lx_size_t len, void *state )
{
    return lx_mbsnrtowcs( dst, src, (lx_size_t)-1 / 2, len, state );
}

static SYSV lx_size_t lx_wcrtomb( char *s, lx_wchar wc, void *state )
{
    char buf[4];
    int len;

    if (!s) return 1;
    if ((len = encode( wc, buf )) < 0) return fail( 84 );
    memcpy( s, buf, len );
    return len;
}

static SYSV lx_size_t lx_wcsnrtombs( char *dst, const lx_wchar **src, lx_size_t nwc, lx_size_t len, void *state )
{
    const lx_wchar *s = *src;
    lx_size_t count = 0, i;
    char buf[4];

    for (i = 0; i < nwc; i++, s++)
    {
        int n = encode( *s, buf );

        if (n < 0) { if (dst) *src = s; return fail( 84 ); }
        if (dst && count + n > len) break;
        if (dst) memcpy( dst + count, buf, n );
        if (!*s)
        {
            if (dst) *src = NULL;
            return count;
        }
        count += n;
    }
    if (dst) *src = s;
    return count;
}

/* wide stdio, through libc_io.c's byte functions */
typedef SYSV int (*getc_fn)( void * );
typedef SYSV int (*putc_fn)( int, void * );

static SYSV lx_wchar lx_getwc( void *f )
{
    getc_fn get = linuxhost_resolve( "getc", NULL );
    unsigned char buf[4];
    lx_wchar c;
    int n = 0, b;

    while (n < 4 && (b = get( f )) != -1)
    {
        long long ret;

        buf[n++] = b;
        if ((ret = decode( buf, n, &c )) >= 0) return c;
        if (ret == -1) break;
    }
    if (n) *linuxhost_errno() = 84;
    return LX_WEOF;
}

static SYSV lx_wchar lx_putwc( lx_wchar wc, void *f )
{
    putc_fn put = linuxhost_resolve( "fputc", NULL );
    char buf[4];
    int len = encode( wc, buf ), i;

    if (len < 0) { fail( 84 ); return LX_WEOF; }
    for (i = 0; i < len; i++) if (put( (unsigned char)buf[i], f ) == -1) return LX_WEOF;
    return wc;
}

static SYSV lx_wchar lx_ungetwc( lx_wchar wc, void *f )
{
    putc_fn unget = linuxhost_resolve( "ungetc", NULL );

    if (wc >= 128) return LX_WEOF;  /* one byte of push-back */
    return unget( (int)wc, f ) == -1 ? LX_WEOF : wc;
}

/* ---- message catalogs: there are none, so messages stay as written ---- */

static SYSV char *lx_dgettext( const char *domain, const char *msg ) { return (char *)msg; }
static SYSV char *lx_bindtextdomain( const char *domain, const char *dir )
{
    return (char *)(dir ? dir : "/usr/share/locale");
}
static SYSV char *lx_bind_textdomain_codeset( const char *domain, const char *codeset ) { return (char *)codeset; }

/* ---------------------------------------------------------------------------------------------------------
 * Memory mappings
 */

static DWORD page_protection( int prot )
{
    switch (prot & 7)
    {
    case 0: return PAGE_NOACCESS;
    case 1: return PAGE_READONLY;
    case 2: case 3: return PAGE_READWRITE;
    case 4: case 5: return PAGE_EXECUTE_READ;
    default: return PAGE_EXECUTE_READWRITE;
    }
}

/* Anonymous memory is Windows memory. A file is read into a private copy: a MAP_SHARED mapping does not write
 * back, which nothing the host runs relies on. */
static SYSV void *lx_mmap( void *addr, lx_size_t len, int prot, int flags, int fd, lx_long offset )
{
    DWORD protection = page_protection( prot );
    void *ret = NULL;

    if (!len) { fail( 22 ); return (void *)-1; }
    if (addr) ret = VirtualAlloc( addr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if (!ret && addr && (flags & 0x10))  /* MAP_FIXED over memory already reserved */
    {
        if ((ret = VirtualAlloc( addr, len, MEM_COMMIT, PAGE_READWRITE ))) memset( ret, 0, len );
    }
    if (!ret && !(flags & 0x10)) ret = VirtualAlloc( NULL, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE );
    if (!ret)
    {
        linuxhost_trace( "mmap %p %#llx: no memory\n", addr, len );
        fail( 12 );
        return (void *)-1;
    }
    if (!(flags & 0x20) && fd >= 0)  /* not MAP_ANONYMOUS: the file's contents */
    {
        const void *data;
        size_t size;

        if (linuxhost_fd_contents( fd, &data, &size ))
        {
            if ((lx_size_t)offset < size) memcpy( ret, (const char *)data + offset, min( len, size - offset ) );
        }
        else
        {
            long long pos = linuxhost_lseek( fd, 0, 1 ), done = 0, n;

            if (linuxhost_lseek( fd, offset, 0 ) >= 0)
                while ((lx_size_t)done < len && (n = linuxhost_read( fd, (char *)ret + done, len - done )) > 0)
                    done += n;
            if (pos >= 0) linuxhost_lseek( fd, pos, 0 );
        }
    }
    if (protection != PAGE_READWRITE)
    {
        DWORD old;

        VirtualProtect( ret, len, protection, &old );
    }
    linuxhost_trace( "mmap %p %#llx prot %d flags %#x fd %d: %p\n", addr, len, prot, flags, fd, ret );
    return ret;
}

static SYSV int lx_munmap( void *addr, lx_size_t len )
{
    MEMORY_BASIC_INFORMATION info;

    if (!VirtualQuery( addr, &info, sizeof(info) ) || info.State == MEM_FREE) return 0;
    if (info.AllocationBase == addr)
    {
        /* the whole allocation goes if the range covers it */
        MEMORY_BASIC_INFORMATION next;
        char *p = addr;

        while (VirtualQuery( p, &next, sizeof(next) ) && next.AllocationBase == addr && next.State != MEM_FREE)
            p = (char *)next.BaseAddress + next.RegionSize;
        if ((lx_size_t)(p - (char *)addr) <= ((len + 0xfff) & ~0xfffull))
            return VirtualFree( addr, 0, MEM_RELEASE ) ? 0 : fail( 22 );
    }
    return VirtualFree( addr, len, MEM_DECOMMIT ) ? 0 : fail( 22 );
}

/* ---------------------------------------------------------------------------------------------------------
 * Users, shared memory, signals, random numbers
 */

/* glibc's struct passwd */
struct lx_passwd
{
    char *name, *passwd;
    unsigned int uid, gid;
    char *gecos, *dir, *shell;
};

static SYSV struct lx_passwd *lx_getpwuid( unsigned int uid )
{
    static struct lx_passwd pw;
    static char name[64], dir[512];
    DWORD size = sizeof(name);

    if (!GetEnvironmentVariableA( "USER", name, sizeof(name) ) && !GetUserNameA( name, &size ))
        strcpy( name, "user" );
    if (!GetEnvironmentVariableA( "HOME", dir, sizeof(dir) ) || dir[0] != '/')
        snprintf( dir, sizeof(dir), "/home/%s", name );
    pw.name = name;
    pw.passwd = (char *)"x";
    pw.uid = uid;
    pw.gid = uid;
    pw.gecos = name;
    pw.dir = dir;
    pw.shell = (char *)"/bin/bash";
    linuxhost_trace( "getpwuid %u: %s %s\n", uid, name, dir );
    return &pw;
}

/* POSIX shared memory: Linux keeps it in /dev/shm; here, files in the Mac's temporary directory */
static void shm_path( const char *name, char *path, size_t size )
{
    while (*name == '/') name++;
    snprintf( path, size, "/tmp/.linuxhost-shm-%s", name );
}

static SYSV int lx_shm_open( const char *name, int flags, unsigned int mode )
{
    char path[512];
    long long fd;

    shm_path( name, path, sizeof(path) );
    fd = linuxhost_open( path, flags );
    linuxhost_trace( "shm_open %s %#x: %lld\n", name, flags, fd );
    return fd < 0 ? fail( (int)-fd ) : (int)fd;
}

static SYSV int lx_shm_unlink( const char *name )
{
    char path[512];
    long long ret;

    shm_path( name, path, sizeof(path) );
    ret = linuxhost_unlink( path );
    return ret < 0 ? fail( (int)-ret ) : 0;
}

/* Signal handlers: no Linux signal is delivered to a Windows process, so a handler is never called. */
static SYSV void *lx_signal( int sig, void *handler )
{
    linuxhost_trace( "signal %d %p\n", sig, handler );
    return NULL;  /* SIG_DFL */
}

/* rand: glibc's generator (TYPE_3, additive feedback: r[i] = r[i-31] + r[i-3]), so a seed gives the sequence it
 * gives on Linux. The last 34 values are all it needs. */
static int32_t rand_ring[34];
static unsigned int rand_pos;
static int rand_seeded;
static SRWLOCK rand_lock = SRWLOCK_INIT;

static int32_t next_random(void)
{
    int32_t value = (int32_t)((uint32_t)rand_ring[(rand_pos + 34 - 31) % 34] + (uint32_t)rand_ring[(rand_pos + 34 - 3) % 34]);

    rand_ring[rand_pos] = value;
    rand_pos = (rand_pos + 1) % 34;
    return value;
}

static void seed_random( unsigned int seed )
{
    int i;

    rand_ring[0] = seed ? seed : 1;
    for (i = 1; i < 31; i++)
    {
        /* 16807 * r mod (2^31 - 1), as glibc computes it (Schrage's method) */
        int32_t hi = rand_ring[i - 1] / 127773, lo = rand_ring[i - 1] % 127773, word = 16807 * lo - 2836 * hi;

        rand_ring[i] = word < 0 ? word + 2147483647 : word;
    }
    for (i = 31; i < 34; i++) rand_ring[i] = rand_ring[i - 31];
    rand_pos = 0;  /* 34 mod 34 */
    for (i = 0; i < 310; i++) next_random();
}

static SYSV int lx_rand(void)
{
    int ret;

    AcquireSRWLockExclusive( &rand_lock );
    if (!rand_seeded) { seed_random( 1 ); rand_seeded = 1; }
    ret = (uint32_t)next_random() >> 1;
    ReleaseSRWLockExclusive( &rand_lock );
    return ret;
}

static SYSV void lx_srand( unsigned int seed )
{
    AcquireSRWLockExclusive( &rand_lock );
    seed_random( seed );
    rand_seeded = 1;
    ReleaseSRWLockExclusive( &rand_lock );
}

const struct libc_function linuxhost_misc_functions[] =
{
    { "__newlocale", lx___newlocale }, { "newlocale", lx___newlocale }, { "__duplocale", lx___duplocale },
    { "duplocale", lx___duplocale }, { "__freelocale", lx___freelocale }, { "freelocale", lx___freelocale },
    { "__uselocale", lx___uselocale }, { "uselocale", lx___uselocale }, { "setlocale", lx_setlocale },
    { "__nl_langinfo_l", lx___nl_langinfo_l }, { "nl_langinfo_l", lx___nl_langinfo_l },
    { "__strcoll_l", lx___strcoll_l }, { "__strxfrm_l", lx___strxfrm_l }, { "__wcscoll_l", lx___wcscoll_l },
    { "__wcsxfrm_l", lx___wcsxfrm_l }, { "wcscmp", lx_wcscmp }, { "__strftime_l", lx___strftime_l },
    { "__wcsftime_l", lx___wcsftime_l }, { "__strtod_l", lx___strtod_l }, { "__strtof_l", lx___strtof_l },
    { "strtold_l", lx_strtold_l }, { "__strtold_l", lx_strtold_l },
    { "towlower", lx_towlower }, { "towupper", lx_towupper }, { "__towlower_l", lx___towlower_l },
    { "__towupper_l", lx___towupper_l }, { "__wctype_l", lx___wctype_l }, { "__iswctype_l", lx___iswctype_l },
    { "isspace", lx_isspace }, { "__ctype_get_mb_cur_max", lx___ctype_get_mb_cur_max }, { "btowc", lx_btowc },
    { "wctob", lx_wctob }, { "mbrtowc", lx_mbrtowc }, { "mbsnrtowcs", lx_mbsnrtowcs }, { "mbsrtowcs", lx_mbsrtowcs },
    { "wcrtomb", lx_wcrtomb }, { "wcsnrtombs", lx_wcsnrtombs }, { "getwc", lx_getwc }, { "putwc", lx_putwc },
    { "ungetwc", lx_ungetwc },
    { "dgettext", lx_dgettext }, { "bindtextdomain", lx_bindtextdomain },
    { "bind_textdomain_codeset", lx_bind_textdomain_codeset },
    { "mmap", lx_mmap }, { "mmap64", lx_mmap }, { "munmap", lx_munmap },
    { "getpwuid", lx_getpwuid }, { "shm_open", lx_shm_open }, { "shm_unlink", lx_shm_unlink },
    { "signal", lx_signal }, { "rand", lx_rand }, { "srand", lx_srand },
    { NULL, NULL }
};
