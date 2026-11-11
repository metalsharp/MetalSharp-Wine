/* Linux module host, native: glibc's stdio. A library's FILE * is macOS's.
 *
 * Formatted output: the arguments come in Linux's va_list (registers, then stack; see glibc_asm.S). They are read
 * as the format says and laid out in 8-byte slots, which is what a va_list is on Apple's arm64, and macOS's own
 * vsnprintf and friends format them. Linux's long double (quad precision) is passed as macOS's (a double).
 * glibc's %m becomes the error text. Scanning: every argument is a pointer, read from the va_list in order. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "glibc_internal.h"

#define MAX_SLOTS 64

uint64_t lnx_va_arg_gp( struct lnx_va_list *ap )
{
    uint64_t value;

    if (ap->gr_offs < 0)
    {
        value = *(uint64_t *)((char *)ap->gr_top + ap->gr_offs);
        ap->gr_offs += 8;
        return value;
    }
    value = *(uint64_t *)ap->stack;
    ap->stack = (char *)ap->stack + 8;
    return value;
}

double lnx_va_arg_double( struct lnx_va_list *ap )
{
    double value;

    if (ap->vr_offs < 0)
    {
        value = *(double *)((char *)ap->vr_top + ap->vr_offs);
        ap->vr_offs += 16;
        return value;
    }
    value = *(double *)ap->stack;
    ap->stack = (char *)ap->stack + 8;
    return value;
}

long double lnx_va_arg_long_double( struct lnx_va_list *ap )
{
    unsigned __int128 q;

    if (ap->vr_offs < 0)
    {
        memcpy( &q, (char *)ap->vr_top + ap->vr_offs, 16 );
        ap->vr_offs += 16;
    }
    else
    {
        ap->stack = (void *)(((uintptr_t)ap->stack + 15) & ~(uintptr_t)15);
        memcpy( &q, ap->stack, 16 );
        ap->stack = (char *)ap->stack + 16;
    }
    return glibc_quad_to_double( q );
}

/* Reads the arguments fmt asks for into slots. out (if not NULL) gets the format for macOS: %m replaced by the
 * error text. Returns the number of slots, or -1 for a format this does not handle (positional arguments). */
static int gather_printf( const char *fmt, struct lnx_va_list *ap, uint64_t *slots, char *out, size_t out_size )
{
    size_t len = 0;
    int count = 0;

#define EMIT(c) do { if (out && len + 1 < out_size) out[len] = (c); len++; } while (0)
    while (*fmt)
    {
        const char *start = fmt;
        int longs = 0, half = 0, quad = 0;

        if (*fmt != '%')
        {
            EMIT( *fmt++ );
            continue;
        }
        fmt++;
        if (*fmt == '%') { EMIT( '%' ); EMIT( '%' ); fmt++; continue; }
        if (*fmt == 'm')
        {
            const char *text = strerror( glibc_errno_from_linux( errno ) );

            for (; *text; text++) { if (*text == '%') EMIT( '%' ); EMIT( *text ); }
            fmt++;
            continue;
        }
        while (strchr( "-+ #0'I", *fmt )) fmt++;
        if (*fmt == '*')
        {
            if (count < MAX_SLOTS) slots[count++] = (int64_t)(int)lnx_va_arg_gp( ap );
            fmt++;
        }
        else while (*fmt >= '0' && *fmt <= '9') fmt++;
        if (*fmt == '$') return -1;
        if (*fmt == '.')
        {
            fmt++;
            if (*fmt == '*')
            {
                if (count < MAX_SLOTS) slots[count++] = (int64_t)(int)lnx_va_arg_gp( ap );
                fmt++;
            }
            else while (*fmt >= '0' && *fmt <= '9') fmt++;
        }
        for (;; fmt++)
        {
            if (*fmt == 'l') longs++;
            else if (*fmt == 'h') half++;
            else if (*fmt == 'L' || *fmt == 'q') quad = 1;
            else if (*fmt == 'j' || *fmt == 'z' || *fmt == 'Z' || *fmt == 't') longs = 2;
            else break;
        }
        if (!*fmt) return -1;
        switch (*fmt)
        {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'c': case 'C':
        {
            uint64_t v = lnx_va_arg_gp( ap );

            if (!longs && !quad) v = (*fmt == 'd' || *fmt == 'i') ? (uint64_t)(int64_t)(int)v : (uint32_t)v;
            if (count < MAX_SLOTS) slots[count++] = v;
            break;
        }
        case 's': case 'S': case 'p': case 'n':
            if (count < MAX_SLOTS) slots[count++] = lnx_va_arg_gp( ap );
            break;
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A':
        {
            double d = quad ? (double)lnx_va_arg_long_double( ap ) : lnx_va_arg_double( ap );

            if (count < MAX_SLOTS) memcpy( &slots[count++], &d, 8 );
            break;
        }
        default:
            return -1;
        }
        fmt++;
        /* copy the conversion, without the flags macOS does not know and with long double spelled as macOS's */
        for (; start < fmt; start++)
        {
            if (*start == '\'' || *start == 'I' || *start == 'Z') continue;
            if (*start == 'q') { EMIT( 'l' ); EMIT( 'l' ); continue; }
            EMIT( *start );
        }
        (void)half;
    }
    if (out) out[len < out_size ? len : out_size - 1] = 0;
#undef EMIT
    return count;
}

/* the slots and macOS format for a printf call, or -1 */
struct formatted { uint64_t slots[MAX_SLOTS]; char fmt[4096]; };

static int prepare( struct formatted *f, const char *fmt, struct lnx_va_list *ap )
{
    if (strlen( fmt ) >= sizeof(f->fmt) - 512 || gather_printf( fmt, ap, f->slots, f->fmt, sizeof(f->fmt) ) < 0)
    {
        errno = 22;
        return -1;
    }
    return 0;
}

#define SLOTS(f) ((va_list)(void *)(f).slots)

int lnx_vsnprintf( char *buf, size_t size, const char *fmt, struct lnx_va_list *ap )
{
    struct formatted f;

    if (prepare( &f, fmt, ap )) return -1;
    return vsnprintf( buf, size, f.fmt, SLOTS(f) );
}

int lnx_vsprintf_core( char *buf, const char *fmt, struct lnx_va_list *ap )
{
    struct formatted f;

    if (prepare( &f, fmt, ap )) return -1;
    return vsprintf( buf, f.fmt, SLOTS(f) );
}

int lnx_vfprintf( FILE *file, const char *fmt, struct lnx_va_list *ap )
{
    struct formatted f;

    if (prepare( &f, fmt, ap )) return -1;
    return vfprintf( file, f.fmt, SLOTS(f) );
}

int lnx_vprintf_core( const char *fmt, struct lnx_va_list *ap )
{
    struct formatted f;

    if (prepare( &f, fmt, ap )) return -1;
    return vprintf( f.fmt, SLOTS(f) );
}

int lnx_vasprintf( char **ret, const char *fmt, struct lnx_va_list *ap )
{
    struct formatted f;

    if (prepare( &f, fmt, ap )) return -1;
    return vasprintf( ret, f.fmt, SLOTS(f) );
}

/* the wide format is classified through a narrow copy (conversions are ASCII) and passed to macOS as it is */
int lnx_vswprintf( wchar_t *buf, size_t size, const wchar_t *fmt, struct lnx_va_list *ap )
{
    uint64_t slots[MAX_SLOTS];
    char narrow[4096];
    size_t i;

    for (i = 0; fmt[i] && i < sizeof(narrow) - 1; i++) narrow[i] = fmt[i] < 128 ? (char)fmt[i] : 'x';
    narrow[i] = 0;
    if (fmt[i] || gather_printf( narrow, ap, slots, NULL, 0 ) < 0)
    {
        errno = 22;
        return -1;
    }
    return vswprintf( buf, size, fmt, (va_list)(void *)slots );
}

/* syslog: formatted and dropped, as the library has no log daemon to talk to here */
void lnx_vsyslog( int priority, const char *fmt, struct lnx_va_list *ap )
{
    char buf[1024];

    lnx_vsnprintf( buf, sizeof(buf), fmt, ap );
}

static void lnx_openlog( const char *ident, int option, int facility ) { }
static void lnx_closelog( void ) { }

/* every scanf argument is a pointer: read as many as the format assigns */
int lnx_vsscanf( const char *str, const char *fmt, struct lnx_va_list *ap )
{
    void *p[32] = { 0 };
    const char *s = fmt;
    int count = 0;

    while ((s = strchr( s, '%' )))
    {
        s++;
        if (*s == '%') { s++; continue; }
        if (*s == '*') continue;
        if (count == 32)
        {
            errno = 22;
            return -1;
        }
        p[count++] = (void *)lnx_va_arg_gp( ap );
        while (*s && !strchr( "diouxXaAeEfFgGsScCpn[", *s )) s++;
        if (*s == '[')
        {
            s++;
            if (*s == '^') s++;
            if (*s == ']') s++;
            while (*s && *s != ']') s++;
        }
    }
    return sscanf( str, fmt, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12],
                   p[13], p[14], p[15], p[16], p[17], p[18], p[19], p[20], p[21], p[22], p[23], p[24], p[25],
                   p[26], p[27], p[28], p[29], p[30], p[31] );
}

/* glibc_asm.S */
extern void lnx_printf(void), lnx_fprintf(void), lnx_sprintf(void), lnx_snprintf(void), lnx_sscanf(void),
            lnx_swprintf(void), lnx_syslog(void);

/* fopen: glibc's 'e' (close on exec) and 'm' (mmap) mode flags are left out */
static FILE *lnx_fopen( const char *path, const char *mode )
{
    char m[16];
    size_t len = 0;
    FILE *f;

    int fd = glibc_proc_open( path );

    for (; *mode && len < sizeof(m) - 1; mode++) if (*mode != 'e' && *mode != 'm') m[len++] = *mode;
    m[len] = 0;
    if (fd == -1) return NULL;
    if (fd >= 0) return fdopen( fd, "r" );
    if (!(f = fopen( path, m ))) errno = glibc_errno_to_linux( errno );
    return f;
}

static FILE *lnx_fdopen( int fd, const char *mode )
{
    char m[16];
    size_t len = 0;
    FILE *f;

    for (; *mode && len < sizeof(m) - 1; mode++) if (*mode != 'e' && *mode != 'm') m[len++] = *mode;
    m[len] = 0;
    if (!(f = fdopen( fd, m ))) errno = glibc_errno_to_linux( errno );
    return f;
}

static void lnx_perror( const char *s )
{
    int err = errno;

    errno = glibc_errno_from_linux( err );
    perror( s );
    errno = err;
}

const struct glibc_export glibc_stdio_exports[] =
{
    WRAP( fopen ), ALIAS( "fopen64", lnx_fopen ), WRAP( fdopen ), PASS( fclose ), PASS( fread ), PASS( fwrite ),
    PASS( fputc ), PASS( fputs ), PASS( fgets ), PASS( fflush ), PASS( fseek ), PASS( fseeko ),
    ALIAS( "fseeko64", fseeko ), PASS( ftell ), PASS( ftello ), PASS( feof ), PASS( ferror ), PASS( clearerr ),
    PASS( fileno ), PASS( setbuf ), PASS( setvbuf ), PASS( getdelim ), WRAP( perror ),
    WRAP( printf ), WRAP( fprintf ), WRAP( sprintf ), WRAP( snprintf ), WRAP( vsnprintf ), WRAP( vfprintf ),
    WRAP( vasprintf ), WRAP( sscanf ), ALIAS( "__isoc99_sscanf", lnx_sscanf ), WRAP( vsscanf ),
    WRAP( swprintf ), WRAP( syslog ), WRAP( openlog ), WRAP( closelog ),
    END
};
