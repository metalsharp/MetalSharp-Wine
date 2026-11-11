/* Linux module host, native: glibc's locales, multibyte text and long double.
 *
 * The library gets the C locale with UTF-8 multibyte text, whatever it asks for: newlocale hands out a token and
 * the *_l functions behave as in the C locale. The multibyte functions are implemented here, because glibc's
 * mbstate_t is 8 bytes and macOS's 128. Linux's long double is IEEE quad precision, which macOS lacks:
 * strtold and friends parse a double and widen it. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <wctype.h>
#include <ctype.h>
#include <time.h>

#include "glibc_internal.h"

/* ---------------------------------------------------------------------------------------------------------
 * Quad precision
 */

unsigned __int128 glibc_quad_from_double( double d )
{
    uint64_t bits, mant;
    unsigned __int128 sign, q;
    int exp;

    memcpy( &bits, &d, 8 );
    sign = (unsigned __int128)(bits >> 63) << 127;
    exp = (bits >> 52) & 0x7ff;
    mant = bits & ((1ull << 52) - 1);
    if (exp == 0x7ff) return sign | ((unsigned __int128)0x7fff << 112) | ((unsigned __int128)mant << 60);
    if (!exp)
    {
        if (!mant) return sign;
        /* subnormal: normalise */
        exp = 1;
        while (!(mant & (1ull << 52))) { mant <<= 1; exp--; }
        mant &= (1ull << 52) - 1;
    }
    q = sign | ((unsigned __int128)(exp - 1023 + 16383) << 112) | ((unsigned __int128)mant << 60);
    return q;
}

double glibc_quad_to_double( unsigned __int128 q )
{
    uint64_t sign = (uint64_t)(q >> 127), mant = (uint64_t)(q >> 60) & ((1ull << 52) - 1), bits;
    int exp = (int)(q >> 112) & 0x7fff;
    double d;

    if (exp == 0x7fff) bits = (sign << 63) | (0x7ffull << 52) | mant | (mant ? 0 : 0);
    else if (!exp) bits = sign << 63;
    else
    {
        exp = exp - 16383 + 1023;
        if (exp >= 0x7ff) bits = (sign << 63) | (0x7ffull << 52);
        else if (exp <= 0) bits = sign << 63;  /* underflows to zero: double has no room for it */
        else bits = (sign << 63) | ((uint64_t)exp << 52) | mant;
    }
    memcpy( &d, &bits, 8 );
    return d;
}

/* returning a long double in Linux's convention: the 128 bits in q0, which is how a 128-bit vector returns */
typedef unsigned __int128 lnx_long_double __attribute__((vector_size(16)));

static lnx_long_double quad_return( double d )
{
    unsigned __int128 q = glibc_quad_from_double( d );
    lnx_long_double v;

    memcpy( &v, &q, 16 );
    return v;
}

static lnx_long_double lnx_strtold( const char *s, char **end ) { return quad_return( strtod( s, end ) ); }
static lnx_long_double lnx_strtold_l( const char *s, char **end, void *loc ) { return quad_return( strtod( s, end ) ); }
static lnx_long_double lnx_wcstold( const wchar_t *s, wchar_t **end ) { return quad_return( wcstod( s, end ) ); }

/* ---------------------------------------------------------------------------------------------------------
 * Locales: one token for every locale
 */

static char locale_token[64];

static void *lnx_newlocale( int mask, const char *name, void *base ) { return locale_token; }
static void *lnx_uselocale( void *loc ) { return locale_token; }
static void lnx_freelocale( void *loc ) { }

static char *lnx_setlocale( int category, const char *name )
{
    static char c_name[] = "C";

    if (!name || !*name || !strcmp( name, "C" ) || !strcmp( name, "POSIX" ) || strstr( name, "UTF-8" ) ||
        strstr( name, "utf8" ))
        return c_name;
    return NULL;
}

/* glibc's struct lconv, for the C locale */
struct lnx_lconv
{
    char *decimal_point, *thousands_sep, *grouping, *int_curr_symbol, *currency_symbol, *mon_decimal_point,
         *mon_thousands_sep, *mon_grouping, *positive_sign, *negative_sign;
    char int_frac_digits, frac_digits, p_cs_precedes, p_sep_by_space, n_cs_precedes, n_sep_by_space,
         p_sign_posn, n_sign_posn, int_p_cs_precedes, int_p_sep_by_space, int_n_cs_precedes,
         int_n_sep_by_space, int_p_sign_posn, int_n_sign_posn;
};

static struct lnx_lconv *lnx_localeconv( void )
{
    static char dot[] = ".", empty[] = "";
    static struct lnx_lconv lc =
    {
        dot, empty, empty, empty, empty, empty, empty, empty, empty, empty,
        127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127, 127,
    };

    return &lc;
}

static int lnx_strcoll_l( const char *a, const char *b, void *loc ) { return strcmp( a, b ); }
static size_t lnx_strxfrm_l( char *dst, const char *src, size_t n, void *loc )
{
    size_t len = strlen( src );

    if (n) { strncpy( dst, src, n ); if (len >= n) dst[n - 1] = 0; }
    return len;
}
static int lnx_wcscoll_l( const wchar_t *a, const wchar_t *b, void *loc ) { return wcscmp( a, b ); }
static size_t lnx_wcsxfrm_l( wchar_t *dst, const wchar_t *src, size_t n, void *loc )
{
    size_t len = wcslen( src );

    if (n) { wcsncpy( dst, src, n ); if (len >= n) dst[n - 1] = 0; }
    return len;
}
static size_t lnx_strftime_l( char *s, size_t n, const char *fmt, const struct tm *tm, void *loc )
{
    return strftime( s, n, fmt, tm );
}

static int lnx_toupper_l( int c, void *loc ) { return c >= 0 && c < 128 ? toupper( c ) : c; }
static int lnx_tolower_l( int c, void *loc ) { return c >= 0 && c < 128 ? tolower( c ) : c; }
static wint_t lnx_towupper_l( wint_t c, void *loc ) { return towupper( c ); }
static wint_t lnx_towlower_l( wint_t c, void *loc ) { return towlower( c ); }
static int lnx_iswspace_l( wint_t c, void *loc ) { return iswspace( c ); }
static int lnx_iswprint_l( wint_t c, void *loc ) { return iswprint( c ); }
static int lnx_iswblank_l( wint_t c, void *loc ) { return iswblank( c ); }
static int lnx_iswcntrl_l( wint_t c, void *loc ) { return iswcntrl( c ); }
static int lnx_iswupper_l( wint_t c, void *loc ) { return iswupper( c ); }
static int lnx_iswlower_l( wint_t c, void *loc ) { return iswlower( c ); }
static int lnx_iswalpha_l( wint_t c, void *loc ) { return iswalpha( c ); }
static int lnx_iswdigit_l( wint_t c, void *loc ) { return iswdigit( c ); }
static int lnx_iswpunct_l( wint_t c, void *loc ) { return iswpunct( c ); }
static int lnx_iswxdigit_l( wint_t c, void *loc ) { return iswxdigit( c ); }
static long long lnx_strtoll_l( const char *s, char **e, int b, void *loc ) { return strtoll( s, e, b ); }
static unsigned long long lnx_strtoull_l( const char *s, char **e, int b, void *loc ) { return strtoull( s, e, b ); }
static float lnx_strtof_l( const char *s, char **e, void *loc ) { return strtof( s, e ); }
static double lnx_strtod_l( const char *s, char **e, void *loc ) { return strtod( s, e ); }

/* ---------------------------------------------------------------------------------------------------------
 * UTF-8. glibc's mbstate_t (8 bytes) holds a partial character: the bytes still wanted and the value so far.
 */

struct lnx_mbstate { uint32_t want, value; };

static size_t lnx_mbrtowc( wchar_t *wc, const char *s, size_t n, struct lnx_mbstate *state )
{
    static struct lnx_mbstate internal;
    size_t i = 0;

    if (!state) state = &internal;
    if (!s) { state->want = 0; return 0; }
    if (!n) return (size_t)-2;
    if (!state->want)
    {
        unsigned char c = s[i++];

        if (c < 0x80) { if (wc) *wc = c; return c ? 1 : 0; }
        if (c >= 0xc2 && c <= 0xdf) { state->want = 1; state->value = c & 0x1f; }
        else if (c >= 0xe0 && c <= 0xef) { state->want = 2; state->value = c & 0x0f; }
        else if (c >= 0xf0 && c <= 0xf4) { state->want = 3; state->value = c & 0x07; }
        else { errno = 84; return (size_t)-1; }  /* EILSEQ */
    }
    for (; i < n && state->want; i++)
    {
        unsigned char c = s[i];

        if ((c & 0xc0) != 0x80) { state->want = 0; errno = 84; return (size_t)-1; }
        state->value = (state->value << 6) | (c & 0x3f);
        state->want--;
    }
    if (state->want) return (size_t)-2;
    if (wc) *wc = state->value;
    return state->value ? i : 0;
}

static int lnx_mbtowc( wchar_t *wc, const char *s, size_t n )
{
    struct lnx_mbstate state = { 0 };
    size_t ret;

    if (!s) return 0;
    ret = lnx_mbrtowc( wc, s, n, &state );
    return ret == (size_t)-1 || ret == (size_t)-2 ? -1 : (int)ret;
}

static size_t lnx_wcrtomb( char *s, wchar_t wc, struct lnx_mbstate *state )
{
    char buf[4];
    uint32_t c = (uint32_t)wc;

    if (!s) s = buf;
    if (c < 0x80) { s[0] = (char)c; return 1; }
    if (c < 0x800) { s[0] = 0xc0 | (c >> 6); s[1] = 0x80 | (c & 0x3f); return 2; }
    if (c >= 0xd800 && c < 0xe000) { errno = 84; return (size_t)-1; }
    if (c < 0x10000)
    {
        s[0] = 0xe0 | (c >> 12); s[1] = 0x80 | ((c >> 6) & 0x3f); s[2] = 0x80 | (c & 0x3f);
        return 3;
    }
    if (c < 0x110000)
    {
        s[0] = 0xf0 | (c >> 18); s[1] = 0x80 | ((c >> 12) & 0x3f); s[2] = 0x80 | ((c >> 6) & 0x3f);
        s[3] = 0x80 | (c & 0x3f);
        return 4;
    }
    errno = 84;
    return (size_t)-1;
}

static size_t lnx_mbsnrtowcs( wchar_t *dst, const char **src, size_t nms, size_t len, struct lnx_mbstate *state )
{
    struct lnx_mbstate internal = { 0 };
    const char *s = *src;
    size_t count = 0;

    if (!state) state = &internal;
    while ((!dst || count < len) && nms)
    {
        wchar_t wc;
        size_t ret = lnx_mbrtowc( &wc, s, nms, state );

        if (ret == (size_t)-1) { *src = s; return (size_t)-1; }
        if (ret == (size_t)-2) { s += nms; nms = 0; break; }
        if (dst) dst[count] = wc;
        if (!ret) { if (dst) *src = NULL; return count; }
        s += ret;
        nms -= ret;
        count++;
    }
    if (dst) *src = s;
    return count;
}

static size_t lnx_mbsrtowcs( wchar_t *dst, const char **src, size_t len, struct lnx_mbstate *state )
{
    return lnx_mbsnrtowcs( dst, src, (size_t)-1 / 2, len, state );
}

static size_t lnx_wcsnrtombs( char *dst, const wchar_t **src, size_t nwc, size_t len, struct lnx_mbstate *state )
{
    const wchar_t *s = *src;
    size_t count = 0;

    for (; nwc; nwc--, s++)
    {
        char buf[4];
        size_t ret = lnx_wcrtomb( buf, *s, NULL );

        if (ret == (size_t)-1) { if (dst) *src = s; return (size_t)-1; }
        if (dst && count + ret > len) break;
        if (dst) memcpy( dst + count, buf, ret );
        if (!*s) { if (dst) *src = NULL; return count; }
        count += ret;
    }
    if (dst) *src = s;
    return count;
}

/* message catalogs: none, so the default text */
static void *lnx_catopen( const char *name, int flag ) { return (void *)-1; }
static char *lnx_catgets( void *cat, int set, int num, const char *def ) { return (char *)def; }
static int lnx_catclose( void *cat ) { return 0; }

const struct glibc_export glibc_locale_exports[] =
{
    WRAP( strtold ), WRAP( strtold_l ), WRAP( wcstold ),
    WRAP( newlocale ), WRAP( uselocale ), WRAP( freelocale ), WRAP( setlocale ), WRAP( localeconv ),
    WRAP( strcoll_l ), WRAP( strxfrm_l ), WRAP( wcscoll_l ), WRAP( wcsxfrm_l ), WRAP( strftime_l ),
    WRAP( toupper_l ), WRAP( tolower_l ), WRAP( towupper_l ), WRAP( towlower_l ), WRAP( iswspace_l ),
    WRAP( iswprint_l ), WRAP( iswblank_l ), WRAP( iswcntrl_l ), WRAP( iswupper_l ), WRAP( iswlower_l ),
    WRAP( iswalpha_l ), WRAP( iswdigit_l ), WRAP( iswpunct_l ), WRAP( iswxdigit_l ), WRAP( strtoll_l ),
    WRAP( strtoull_l ), WRAP( strtof_l ), WRAP( strtod_l ),
    WRAP( mbrtowc ), WRAP( mbtowc ), WRAP( wcrtomb ), WRAP( mbsnrtowcs ), WRAP( mbsrtowcs ), WRAP( wcsnrtombs ),
    WRAP( catopen ), WRAP( catgets ), WRAP( catclose ),
    END
};
