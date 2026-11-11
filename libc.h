/* Linux module host: more of glibc (libc.c). */
#ifndef LINUXHOST_LIBC_H
#define LINUXHOST_LIBC_H

struct libc_function { const char *name; void *function; };

/* ended by a NULL name: libc.c's and libc_io.c's */
extern const struct libc_function linuxhost_libc_functions[], linuxhost_libc_io_functions[];

/* glibc.c's own functions, by name */
void *linuxhost_glibc_function( const char *name );

/* the calling thread's Linux errno */
int *linuxhost_errno(void);

/* a Linux system call made as a function call (syscall()): a result, or a negative errno */
long long linuxhost_syscall( long long number, long long a1, long long a2, long long a3, long long a4, long long a5,
                             long long a6 );

#endif
