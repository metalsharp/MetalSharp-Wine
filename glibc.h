/* Linux module host: the glibc layer. See glibc.c. */
#ifndef LINUXHOST_GLIBC_H
#define LINUXHOST_GLIBC_H

/* an elf_resolver: the host's implementation of a glibc function, or NULL */
void *linuxhost_resolve( const char *name, void *context );

unsigned int linuxhost_function_count(void);

struct libc_export;
const struct libc_export *linuxhost_libc_exports( unsigned int *count );

#endif
