/* Linux module host: load x86_64 Linux shared libraries into a Windows x86_64 process.
 *
 * The host is ordinary x86_64 Windows code. Under MetalSharp Wine it is run by FEX, and so is the
 * Linux library it maps, which FEX translates like any other x86_64 code in the process. The
 * library's imports are resolved to functions with the System V calling convention (glibc.c). */
#ifndef LINUXHOST_ELFLOAD_H
#define LINUXHOST_ELFLOAD_H

#include <stddef.h>
#include <stdint.h>

#define SYSV __attribute__((sysv_abi))

struct elf_module;

/* returns the address of an imported symbol, or NULL */
typedef void *(*elf_resolver)( const char *name, void *context );

struct elf_stats
{
    unsigned int relocations;      /* applied */
    unsigned int imports;          /* undefined symbols resolved */
    unsigned int unresolved;       /* undefined symbols nobody provides (calls to them stop the program) */
    unsigned int weak_missing;     /* weak symbols left at 0, as on Linux */
    unsigned int unsupported;      /* relocation types the loader does not handle */
    char unresolved_names[2048];   /* the first of them, space separated */
};

/* path: a Windows path, or a Unix path starting with '/'. Returns NULL and fills err on failure. */
struct elf_module *elf_load( const char *path, elf_resolver resolve, void *context, char *err, size_t err_size );

/* the same, from an image already in memory; path is the name it is known by */
struct elf_module *elf_load_buffer( const char *path, const uint8_t *data, size_t size, elf_resolver resolve,
                                    void *context, char *err, size_t err_size );

/* runs DT_INIT and the DT_INIT_ARRAY constructors, once */
void elf_run_constructors( struct elf_module *module );

/* whether an exported symbol is a function */
int elf_symbol_is_function( struct elf_module *module, const char *name );

/* an exported symbol's address, or NULL */
void *elf_symbol( struct elf_module *module, const char *name );

const struct elf_stats *elf_get_stats( const struct elf_module *module );
const char *elf_get_name( const struct elf_module *module );
void elf_get_range( const struct elf_module *module, void **base, size_t *size );

/* thread-local storage of a module for the calling thread (the general-dynamic model, __tls_get_addr) */
void *elf_tls_block( struct elf_module *module );
struct elf_module *elf_module_by_tls_id( unsigned long id );

/* the loaded module containing an address, or NULL */
struct elf_module *elf_find_address( const void *addr );

/* the module's .eh_frame_hdr (PT_GNU_EH_FRAME), or NULL */
const void *elf_eh_frame_hdr( const struct elf_module *module );

/* calls callback for each loaded module (base is the load bias), until it returns nonzero; returns that */
int elf_iterate_phdr( int (*callback)( struct elf_module *module, void *base, const char *name, const void *phdr,
                                        unsigned int phnum, unsigned long tls_id, void *data ), void *data );

/* a loaded module by file name or soname, or NULL */
struct elf_module *elf_find( const char *name );

#endif
