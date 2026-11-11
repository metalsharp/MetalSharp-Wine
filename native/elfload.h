/* Linux module host, native: load aarch64 Linux shared libraries into an arm64 macOS process.
 *
 * Apple silicon runs aarch64 Linux code as it is: the instruction set and the procedure call standard are the
 * same, and the library's imports are resolved to functions that give it glibc's ABI on top of macOS's libc
 * (glibc.c). What differs is handled at the edges:
 *  - Variadic functions: Linux passes the variadic arguments in registers, Apple on the stack. glibc.c's
 *    variadic entry points are written for the Linux convention.
 *  - x18: Linux code uses it as a scratch register; Apple reserves it, and Wine keeps the TEB in it. Code that
 *    calls into a library restores it afterwards.
 *  - tpidr_el0: Linux's thread pointer, which macOS uses for the CPU number and changes when a thread moves to
 *    another core. TLS descriptors resolve against its value at the time (see elf_tlsdesc_resolve). */
#ifndef LINUXHOST_NATIVE_ELFLOAD_H
#define LINUXHOST_NATIVE_ELFLOAD_H

#include <stddef.h>
#include <stdint.h>

struct elf_module;

/* the address of an imported symbol (name, and the version glibc gives it, which may be NULL), or NULL */
typedef void *(*elf_resolver)( const char *name, const char *version, void *context );

struct elf_stats
{
    unsigned int relocations;      /* applied */
    unsigned int imports;          /* undefined symbols resolved */
    unsigned int unresolved;       /* undefined symbols nobody provides: a call to one reports it and aborts */
    unsigned int weak_missing;     /* weak symbols left at 0, as on Linux */
    unsigned int unsupported;      /* relocation types the loader does not handle */
};

/* Returns NULL and fills err on failure. */
struct elf_module *elf_load( const char *path, elf_resolver resolve, void *context, char *err, size_t err_size );

/* runs DT_INIT and the DT_INIT_ARRAY constructors, once */
void elf_run_constructors( struct elf_module *module, int argc, char **argv, char **envp );

/* an exported symbol's address, or NULL */
void *elf_symbol( struct elf_module *module, const char *name );

const struct elf_stats *elf_get_stats( const struct elf_module *module );
const char *elf_get_name( const struct elf_module *module );
const char *elf_get_path( const struct elf_module *module );
void elf_get_range( const struct elf_module *module, void **base, size_t *size );

/* the names of the unresolved imports, one per call (index from 0), or NULL past the end */
const char *elf_unresolved_name( const struct elf_module *module, unsigned int index );

/* a loaded module by file name or soname, or the one containing an address, or NULL */
struct elf_module *elf_find( const char *name );
struct elf_module *elf_find_address( const void *addr );

/* for dl_iterate_phdr and dladdr */
const void *elf_get_phdrs( const struct elf_module *module, unsigned int *count );
uintptr_t elf_get_base( const struct elf_module *module );
int elf_symbol_for_address( const void *addr, const char **name, void **start );
struct elf_module *elf_first( void );
struct elf_module *elf_next( struct elf_module *module );

/* called with the name of an unresolved import a library has just called; must not return */
extern void (*elf_unresolved_handler)( const char *name );

#endif
