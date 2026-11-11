/* Linux module host, native: glibc's ABI on macOS's libc, for aarch64 Linux libraries. */
#ifndef LINUXHOST_NATIVE_GLIBC_H
#define LINUXHOST_NATIVE_GLIBC_H

/* an elf_resolver: the function or data a library imports under name (version may be NULL), or NULL */
void *glibc_resolve( const char *name, const char *version, void *context );

/* registers a loaded library with the parts of this layer that need to know it (exception unwinding) */
struct elf_module;
void glibc_module_loaded( struct elf_module *module );

/* macOS errno value to Linux, and back */
int glibc_errno_to_linux( int err );
int glibc_errno_from_linux( int err );

#endif
