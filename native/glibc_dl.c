/* Linux module host, native: glibc's dynamic loading, and C++ exceptions.
 *
 * dlopen loads more aarch64 libraries with the same loader; glibc's own libraries (libc.so.6 and the rest) are
 * this layer, and get a handle that looks symbols up here. dl_iterate_phdr and dladdr describe the loaded
 * libraries.
 *
 * Exceptions: libgcc's _Unwind_* and macOS's libunwind implement the same Itanium ABI, so a library's calls go
 * to macOS's. For the unwinder to walk a library's frames, each library's .eh_frame entries are registered with
 * it (glibc_module_loaded). */
#include <dlfcn.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unwind.h>

#include "elfload.h"
#include "glibc_internal.h"

extern void __register_frame( const void *fde );

typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Elf64_Phdr;
enum { PT_LOAD = 1, PT_GNU_EH_FRAME = 0x6474e550 };

static __thread char dl_error[512];
static __thread int dl_error_set;
static char glibc_handle;  /* what dlopen returns for glibc's own libraries */

static void set_dl_error( const char *fmt, const char *arg )
{
    snprintf( dl_error, sizeof(dl_error), fmt, arg );
    dl_error_set = 1;
}

/* reads a DWARF pointer as .eh_frame_hdr encodes eh_frame_ptr */
static const uint8_t *read_encoded( const uint8_t *p, uint8_t encoding, uint64_t *value )
{
    const uint8_t *start = p;
    uint64_t v;

    switch (encoding & 0x0f)
    {
    case 0x00: memcpy( &v, p, 8 ); p += 8; break;                               /* absptr */
    case 0x03: { uint32_t u; memcpy( &u, p, 4 ); v = u; p += 4; break; }      /* udata4 */
    case 0x0b: { int32_t s; memcpy( &s, p, 4 ); v = (uint64_t)(int64_t)s; p += 4; break; }  /* sdata4 */
    case 0x04: case 0x0c: memcpy( &v, p, 8 ); p += 8; break;                   /* udata8, sdata8 */
    default: *value = 0; return NULL;
    }
    if ((encoding & 0x70) == 0x10) v += (uint64_t)start;  /* pcrel */
    *value = v;
    return p;
}

/* registers every FDE of a library's .eh_frame with macOS's unwinder */
void glibc_module_loaded( struct elf_module *module )
{
    const Elf64_Phdr *phdrs;
    unsigned int count, i;
    uint64_t base = elf_get_base( module ), frame = 0, end = 0;

    phdrs = elf_get_phdrs( module, &count );
    for (i = 0; i < count; i++)
    {
        const uint8_t *hdr;

        if (phdrs[i].type != PT_GNU_EH_FRAME) continue;
        hdr = (const uint8_t *)(base + phdrs[i].vaddr);
        if (hdr[0] != 1 || !read_encoded( hdr + 4, hdr[1], &frame )) return;
        break;
    }
    if (!frame) return;
    for (i = 0; i < count; i++)
        if (phdrs[i].type == PT_LOAD && frame >= base + phdrs[i].vaddr &&
            frame < base + phdrs[i].vaddr + phdrs[i].filesz)
            end = base + phdrs[i].vaddr + phdrs[i].filesz;
    while (frame + 8 <= end)
    {
        const uint8_t *p = (const uint8_t *)frame;
        uint64_t length;
        uint32_t len32, id;
        size_t header = 4;

        memcpy( &len32, p, 4 );
        if (!len32) break;
        length = len32;
        if (len32 == 0xffffffff)
        {
            memcpy( &length, p + 4, 8 );
            header = 12;
        }
        memcpy( &id, p + header, 4 );
        if (id) __register_frame( p );  /* an FDE; id 0 is a CIE */
        frame += header + length;
    }
}

static struct elf_module *load_library( const char *path )
{
    struct elf_module *module;
    char err[1024];

    if ((module = elf_find( path ))) return module;
    if (!(module = elf_load( path, glibc_resolve, NULL, err, sizeof(err) )))
    {
        set_dl_error( "%s", err );
        return NULL;
    }
    glibc_module_loaded( module );
    elf_run_constructors( module, 0, NULL, NULL );
    return module;
}

static int is_glibc_library( const char *name )
{
    static const char *const names[] = { "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
                                         "libgcc_s.so.1", "ld-linux-aarch64.so.1", "libc.so", "libdl.so" };

    for (unsigned int i = 0; i < ARRAY_SIZE(names); i++) if (!strcmp( name, names[i] )) return 1;
    return 0;
}

static void *lnx_dlopen( const char *name, int flags )
{
    struct elf_module *module;
    char path[1024];

    if (!name) return &glibc_handle;
    if (strchr( name, '/' )) return load_library( name );
    if (is_glibc_library( name )) return &glibc_handle;
    if ((module = elf_find( name ))) return module;
    /* next to the libraries already loaded */
    for (module = elf_first(); module; module = elf_next( module ))
    {
        char dir[1024];

        snprintf( dir, sizeof(dir), "%s", elf_get_path( module ) );
        snprintf( path, sizeof(path), "%s/%s", dirname( dir ), name );
        if (!access( path, 0 )) return load_library( path );
    }
    set_dl_error( "%s: cannot open shared object file: No such file or directory", name );
    return NULL;
}

static int lnx_dlclose( void *handle ) { return 0; }

static void *lnx_dlsym( void *handle, const char *name )
{
    struct elf_module *module;
    void *addr;

    if (handle && handle != (void *)-1 && handle != &glibc_handle)
    {
        if ((addr = elf_symbol( handle, name ))) return addr;
    }
    else
    {
        for (module = elf_first(); module; module = elf_next( module ))
            if ((addr = elf_symbol( module, name ))) return addr;
        if ((addr = glibc_resolve( name, NULL, NULL ))) return addr;
    }
    set_dl_error( "undefined symbol: %s", name );
    return NULL;
}

static char *lnx_dlerror( void )
{
    if (!dl_error_set) return NULL;
    dl_error_set = 0;
    return dl_error;
}

/* glibc's Dl_info has the same layout as macOS's */
static int lnx_dladdr( const void *addr, Dl_info *info )
{
    struct elf_module *module = elf_find_address( addr );
    const char *name;
    void *start, *base;
    size_t size;

    if (!module) return 0;
    elf_get_range( module, &base, &size );
    info->dli_fname = elf_get_path( module );
    info->dli_fbase = (void *)elf_get_base( module );
    if (elf_symbol_for_address( addr, &name, &start ))
    {
        info->dli_sname = name;
        info->dli_saddr = start;
    }
    else
    {
        info->dli_sname = NULL;
        info->dli_saddr = NULL;
    }
    return 1;
}

/* glibc's struct dl_phdr_info */
struct lnx_dl_phdr_info
{
    uint64_t addr;
    const char *name;
    const void *phdr;
    uint16_t phnum;
    unsigned long long adds, subs;
    size_t tls_modid;
    void *tls_data;
};

static int lnx_dl_iterate_phdr( int (*callback)(struct lnx_dl_phdr_info *, size_t, void *), void *data )
{
    struct elf_module *module;
    int ret = 0;

    for (module = elf_first(); module && !ret; module = elf_next( module ))
    {
        struct lnx_dl_phdr_info info = { 0 };
        unsigned int count;

        info.addr = elf_get_base( module );
        info.name = elf_get_path( module );
        info.phdr = elf_get_phdrs( module, &count );
        info.phnum = count;
        ret = callback( &info, sizeof(info), data );
    }
    return ret;
}

const struct glibc_export glibc_dl_exports[] =
{
    WRAP( dlopen ), WRAP( dlclose ), WRAP( dlsym ), WRAP( dlerror ), WRAP( dladdr ), WRAP( dl_iterate_phdr ),
    PASS( _Unwind_Resume ), PASS( _Unwind_RaiseException ), PASS( _Unwind_DeleteException ), PASS( _Unwind_SetGR ),
    PASS( _Unwind_SetIP ), PASS( _Unwind_GetLanguageSpecificData ), PASS( _Unwind_GetIP ),
    PASS( _Unwind_GetRegionStart ),
    END
};
