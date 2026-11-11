/* Linux module host, native: what the glibc layer's files share. */
#ifndef LINUXHOST_NATIVE_GLIBC_INTERNAL_H
#define LINUXHOST_NATIVE_GLIBC_INTERNAL_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include "glibc.h"

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

struct glibc_export { const char *name; void *addr; };

#define PASS(name) { #name, (void *)name }
#define WRAP(name) { #name, (void *)lnx_##name }
#define ALIAS(name, func) { name, (void *)(func) }
#define END { NULL, NULL }

/* each area's table, ended by END */
extern const struct glibc_export glibc_core_exports[], glibc_thread_exports[], glibc_signal_exports[],
                                 glibc_stdio_exports[], glibc_net_exports[], glibc_locale_exports[],
                                 glibc_dl_exports[];

/* the result of a macOS call that returns -1 on failure, with errno in Linux's numbering */
static inline long linux_ret( long ret )
{
    if (ret == -1) errno = glibc_errno_to_linux( errno );
    return ret;
}

/* a failure with a Linux errno */
static inline long linux_fail( int linux_errno )
{
    errno = linux_errno;
    return -1;
}

/* Linux's va_list on aarch64: a variadic function's register arguments, saved by its entry point
 * (glibc_variadic.S), and where the rest are on the stack. Functions taking a va_list get a pointer to it. */
struct lnx_va_list
{
    void *stack;
    void *gr_top;
    void *vr_top;
    int gr_offs;
    int vr_offs;
};

uint64_t lnx_va_arg_gp( struct lnx_va_list *ap );
double lnx_va_arg_double( struct lnx_va_list *ap );
long double lnx_va_arg_long_double( struct lnx_va_list *ap );  /* Linux's quad precision, as macOS's double */

/* the Linux files about the process and machine (glibc_proc.c): -2 when path is not one of them */
int glibc_proc_open( const char *path );
long glibc_proc_readlink( const char *path, char *buf, size_t size );
int glibc_proc_exists( const char *path );

/* signal numbers */
int glibc_signal_from_linux( int sig );
int glibc_signal_to_linux( int sig );

/* the address family and socket address of the two systems */
int glibc_af_from_linux( int af );
int glibc_af_to_linux( int af );

/* quad precision (Linux's long double) from and to double */
unsigned __int128 glibc_quad_from_double( double d );
double glibc_quad_to_double( unsigned __int128 q );

#endif
