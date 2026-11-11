/* Linux module host, native: ELF loader for aarch64 shared libraries. See elfload.h. */
#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "elfload.h"

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Elf64_Phdr;
typedef struct { int64_t tag; uint64_t val; } Elf64_Dyn;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64_Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Elf64_Rela;
typedef struct { uint16_t version, cnt; uint32_t file, aux, next; } Elf64_Verneed;
typedef struct { uint32_t hash; uint16_t flags, other; uint32_t name, next; } Elf64_Vernaux;

enum { EM_AARCH64 = 183 };
enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_PHDR = 6, PT_TLS = 7, PT_GNU_RELRO = 0x6474e552 };
enum { PF_X = 1, PF_W = 2, PF_R = 4 };
enum { DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_INIT = 12, DT_SONAME = 14, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35,
       DT_RELR = 36, DT_GNU_HASH = 0x6ffffef5, DT_VERSYM = 0x6ffffff0, DT_VERNEED = 0x6ffffffe,
       DT_VERNEEDNUM = 0x6fffffff };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_COPY = 1024, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026,
       R_AARCH64_RELATIVE = 1027, R_AARCH64_TLS_DTPMOD64 = 1028, R_AARCH64_TLS_DTPREL64 = 1029,
       R_AARCH64_TLS_TPREL64 = 1030, R_AARCH64_TLSDESC = 1031, R_AARCH64_IRELATIVE = 1032 };
enum { STB_WEAK = 2 };

/* what a TLS descriptor's second word points to */
struct tlsdesc_arg
{
    struct elf_module *module;
    uint64_t offset;
};

struct elf_module
{
    struct elf_module *next;
    char name[128], soname[128], path[1024];
    uint8_t *base;                 /* load bias: the address of virtual address 0 */
    uint8_t *mapping;              /* what mmap returned, 64K aligned */
    size_t size;
    const Elf64_Phdr *phdrs;       /* a copy, for dl_iterate_phdr */
    unsigned int phnum;
    const Elf64_Sym *symtab;
    const char *strtab;
    const uint16_t *versym;
    uint64_t verneed;
    unsigned int verneednum, nsyms;
    uint64_t init, init_array, init_arraysz;
    const uint8_t *tls_image;
    size_t tls_filesz, tls_memsz, tls_align;
    pthread_key_t tls_key;
    struct tlsdesc_arg *tlsdesc_args;
    unsigned int tlsdesc_count;
    char **unresolved;             /* names */
    unsigned int unresolved_count;
    int constructed;
    struct elf_stats stats;
};

static struct elf_module *modules;
static pthread_mutex_t modules_lock = PTHREAD_MUTEX_INITIALIZER;

/* the names behind the unresolved-import stubs, indexed by the number each stub passes in x16 */
static const char **stub_names;
static unsigned int stub_count, stub_capacity;

void (*elf_unresolved_handler)( const char *name );

/* tlsdesc.S */
extern void elf_tlsdesc_resolver(void);
extern void elf_unresolved_entry(void);

static void set_error( char *err, size_t size, const char *format, const char *arg )
{
    if (err && size) snprintf( err, size, format, arg );
}

/* called by elf_unresolved_entry with the stub's number */
void elf_unresolved_called( uint64_t index )
{
    const char *name = index < stub_count ? stub_names[index] : "?";

    if (elf_unresolved_handler) elf_unresolved_handler( name );
    fprintf( stderr, "linuxhost: call to the unresolved import %s\n", name );
    abort();
}

/* called by elf_tlsdesc_resolver: the address of a TLS variable for the calling thread */
void *elf_tlsdesc_address( struct tlsdesc_arg *arg )
{
    struct elf_module *module = arg->module;
    uint8_t *block = pthread_getspecific( module->tls_key );

    if (!block)
    {
        if (posix_memalign( (void **)&block, module->tls_align > 16 ? module->tls_align : 16, module->tls_memsz + 16 ))
            abort();
        memcpy( block, module->tls_image, module->tls_filesz );
        memset( block + module->tls_filesz, 0, module->tls_memsz - module->tls_filesz + 16 );
        pthread_setspecific( module->tls_key, block );
    }
    return block + arg->offset;
}

static void free_tls_block( void *block ) { free( block ); }

/* An executable stub for an import nobody provides: movz x16, #index; ldr x17, 1f; br x17; nop; 1: .quad entry */
static void *make_stub( const char *name )
{
    static uint8_t *page, *next;
    static size_t left;
    uint32_t *code;
    uint64_t entry = (uint64_t)elf_unresolved_entry;
    unsigned int index;
    size_t page_size = getpagesize();

    if (stub_count == stub_capacity)
    {
        unsigned int capacity = stub_capacity ? stub_capacity * 2 : 256;
        const char **names = realloc( stub_names, capacity * sizeof(*names) );

        if (!names) return NULL;
        stub_names = names;
        stub_capacity = capacity;
    }
    if (stub_count > 0xffff) return NULL;
    if (left < 24)
    {
        page = mmap( NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 );
        if (page == MAP_FAILED) return NULL;
        next = page;
        left = page_size;
    }
    else mprotect( page, page_size, PROT_READ | PROT_WRITE );

    index = stub_count;
    stub_names[stub_count++] = strdup( name );
    code = (uint32_t *)next;
    code[0] = 0xd2800010 | (index << 5);   /* movz x16, #index */
    code[1] = 0x58000071;                   /* ldr x17, #12 (the .quad at code + 4) */
    code[2] = 0xd61f0220;                   /* br x17 */
    code[3] = 0xd503201f;                   /* nop, so the address below is 8-byte aligned */
    memcpy( code + 4, &entry, 8 );
    mprotect( page, page_size, PROT_READ | PROT_EXEC );
    sys_icache_invalidate( next, 24 );
    next += 24;
    left -= 24;
    return code;
}

static unsigned int count_symbols( const struct elf_module *module, uint64_t hash, uint64_t gnu_hash )
{
    if (hash) return ((const uint32_t *)(module->base + hash))[1];
    if (gnu_hash)
    {
        const uint32_t *table = (const uint32_t *)(module->base + gnu_hash);
        uint32_t nbuckets = table[0], symoffset = table[1], bloom_size = table[2], i, last = 0;
        const uint32_t *buckets = table + 4 + bloom_size * 2, *chains = buckets + nbuckets;

        for (i = 0; i < nbuckets; i++) if (buckets[i] > last) last = buckets[i];
        if (last < symoffset) return symoffset;
        while (!(chains[last - symoffset] & 1)) last++;
        return last + 1;
    }
    return 0;
}

/* the version an undefined symbol asks for (GLIBC_2.17 and so on), or NULL */
static const char *symbol_version( const struct elf_module *module, uint32_t index )
{
    const uint8_t *p;
    uint16_t want;
    unsigned int i, j;

    if (!module->versym || !module->verneed) return NULL;
    want = module->versym[index] & 0x7fff;
    if (want < 2) return NULL;
    p = module->base + module->verneed;
    for (i = 0; i < module->verneednum; i++)
    {
        const Elf64_Verneed *need = (const Elf64_Verneed *)p;
        const uint8_t *a = p + need->aux;

        for (j = 0; j < need->cnt; j++)
        {
            const Elf64_Vernaux *aux = (const Elf64_Vernaux *)a;

            if (aux->other == want) return module->strtab + aux->name;
            a += aux->next;
        }
        p += need->next;
    }
    return NULL;
}

struct elf_module *elf_find( const char *name )
{
    const char *base = strrchr( name, '/' );
    struct elf_module *module;

    base = base ? base + 1 : name;
    pthread_mutex_lock( &modules_lock );
    for (module = modules; module; module = module->next)
        if (!strcmp( module->name, base ) || !strcmp( module->soname, base )) break;
    pthread_mutex_unlock( &modules_lock );
    return module;
}

struct elf_module *elf_find_address( const void *addr )
{
    struct elf_module *module;

    pthread_mutex_lock( &modules_lock );
    for (module = modules; module; module = module->next)
        if ((const uint8_t *)addr >= module->mapping && (const uint8_t *)addr < module->mapping + module->size) break;
    pthread_mutex_unlock( &modules_lock );
    return module;
}

struct elf_module *elf_first( void ) { return modules; }
struct elf_module *elf_next( struct elf_module *module ) { return module->next; }

void *elf_symbol( struct elf_module *module, const char *name )
{
    unsigned int i;

    for (i = 1; i < module->nsyms; i++)
    {
        const Elf64_Sym *sym = &module->symtab[i];

        if (sym->shndx && (sym->info >> 4) && !strcmp( module->strtab + sym->name, name ))
            return module->base + sym->value;
    }
    return NULL;
}

int elf_symbol_for_address( const void *addr, const char **name, void **start )
{
    struct elf_module *module = elf_find_address( addr );
    const Elf64_Sym *best = NULL;
    unsigned int i;

    if (!module) return 0;
    for (i = 1; i < module->nsyms; i++)
    {
        const Elf64_Sym *sym = &module->symtab[i];
        const uint8_t *p = module->base + sym->value;

        if (!sym->shndx || !(sym->info >> 4) || p > (const uint8_t *)addr) continue;
        if (sym->size && (const uint8_t *)addr >= p + sym->size) continue;
        if (!best || sym->value > best->value) best = sym;
    }
    if (!best) return 0;
    *name = module->strtab + best->name;
    *start = module->base + best->value;
    return 1;
}

static uint64_t resolve_symbol( struct elf_module *module, uint32_t index, elf_resolver resolve, void *context )
{
    const Elf64_Sym *sym = &module->symtab[index];
    const char *name = module->strtab + sym->name;
    unsigned int i;
    void *addr;

    if (!index) return 0;
    if (sym->shndx) return (uint64_t)(module->base + sym->value);
    if (resolve && (addr = resolve( name, symbol_version( module, index ), context )))
    {
        module->stats.imports++;
        return (uint64_t)addr;
    }
    if ((sym->info >> 4) == STB_WEAK)
    {
        module->stats.weak_missing++;
        return 0;
    }
    module->stats.unresolved++;
    for (i = 0; i < module->unresolved_count; i++) if (!strcmp( module->unresolved[i], name )) break;
    if (i == module->unresolved_count)
    {
        char **names = realloc( module->unresolved, (i + 1) * sizeof(*names) );

        if (names)
        {
            module->unresolved = names;
            module->unresolved[module->unresolved_count++] = strdup( name );
        }
    }
    return (uint64_t)make_stub( name );
}

static void apply_rela( struct elf_module *module, uint64_t table, uint64_t size, elf_resolver resolve, void *context )
{
    const Elf64_Rela *rela = (const Elf64_Rela *)(module->base + table);
    uint64_t i, count = size / sizeof(*rela);

    for (i = 0; i < count; i++)
    {
        uint64_t *where = (uint64_t *)(module->base + rela[i].offset);
        uint32_t type = (uint32_t)rela[i].info, index = (uint32_t)(rela[i].info >> 32);

        switch (type)
        {
        case R_AARCH64_RELATIVE:
            *where = (uint64_t)(module->base + rela[i].addend);
            break;
        case R_AARCH64_GLOB_DAT:
        case R_AARCH64_JUMP_SLOT:
            *where = resolve_symbol( module, index, resolve, context );
            break;
        case R_AARCH64_ABS64:
            *where = resolve_symbol( module, index, resolve, context ) + rela[i].addend;
            break;
        case R_AARCH64_TLSDESC:
        {
            /* only the module's own TLS: [0] the resolver, [1] its argument */
            struct tlsdesc_arg *arg;

            if (index && !module->symtab[index].shndx)
            {
                module->stats.unsupported++;
                continue;
            }
            if (!(arg = calloc( 1, sizeof(*arg) ))) abort();
            arg->module = module;
            arg->offset = (index ? module->symtab[index].value : 0) + rela[i].addend;
            where[0] = (uint64_t)elf_tlsdesc_resolver;
            where[1] = (uint64_t)arg;
            module->tlsdesc_count++;
            break;
        }
        default:  /* TPREL64 needs a fixed thread pointer, which macOS does not give us */
            module->stats.unsupported++;
            continue;
        }
        module->stats.relocations++;
    }
}

/* DT_RELR: relative relocations packed as addresses and bitmaps */
static void apply_relr( struct elf_module *module, uint64_t table, uint64_t size )
{
    const uint64_t *entry = (const uint64_t *)(module->base + table), *end = entry + size / sizeof(*entry);
    uint64_t *where = NULL;

    for (; entry < end; entry++)
    {
        if (!(*entry & 1))
        {
            where = (uint64_t *)(module->base + *entry);
            *where++ += (uint64_t)module->base;
            module->stats.relocations++;
        }
        else
        {
            uint64_t bits = *entry >> 1;
            unsigned int i;

            for (i = 0; bits; i++, bits >>= 1)
                if (bits & 1)
                {
                    where[i] += (uint64_t)module->base;
                    module->stats.relocations++;
                }
            where += 63;
        }
    }
}

static int protection( uint32_t flags )
{
    return ((flags & PF_R) ? PROT_READ : 0) | ((flags & PF_W) ? PROT_WRITE : 0) | ((flags & PF_X) ? PROT_EXEC : 0);
}

struct elf_module *elf_load( const char *path, elf_resolver resolve, void *context, char *err, size_t err_size )
{
    uint64_t low = ~0ull, high = 0, dynamic = 0, dynamic_size = 0, strtab = 0, symtab = 0, hash = 0, gnu_hash = 0;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, relr = 0, relrsz = 0, soname = 0, versym = 0;
    size_t page_size = getpagesize();
    struct elf_module *module = NULL;
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    const Elf64_Dyn *dyn;
    const char *file_name;
    uint8_t *file = MAP_FAILED, *reserved = MAP_FAILED;
    size_t reserved_size = 0;
    struct stat st;
    unsigned int i;
    int fd;

    if ((fd = open( path, O_RDONLY | O_CLOEXEC )) == -1)
    {
        set_error( err, err_size, "cannot open %s", path );
        return NULL;
    }
    if (fstat( fd, &st ) || st.st_size < (off_t)sizeof(*ehdr) ||
        (file = mmap( NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0 )) == MAP_FAILED)
    {
        close( fd );
        set_error( err, err_size, "cannot read %s", path );
        return NULL;
    }
    close( fd );

    ehdr = (const Elf64_Ehdr *)file;
    if (memcmp( ehdr->ident, "\177ELF", 4 ) || ehdr->ident[4] != 2 || ehdr->ident[5] != 1 ||
        ehdr->machine != EM_AARCH64 || ehdr->type != 3 ||
        ehdr->phoff + ehdr->phnum * (uint64_t)sizeof(*phdr) > (uint64_t)st.st_size)
    {
        set_error( err, err_size, "%s is not an aarch64 Linux shared library", path );
        goto failed;
    }
    phdr = (const Elf64_Phdr *)(file + ehdr->phoff);
    for (i = 0; i < ehdr->phnum; i++)
    {
        if (phdr[i].type != PT_LOAD) continue;
        if (phdr[i].vaddr < low) low = phdr[i].vaddr;
        if (phdr[i].vaddr + phdr[i].memsz > high) high = phdr[i].vaddr + phdr[i].memsz;
        if (phdr[i].offset + phdr[i].filesz > (uint64_t)st.st_size)
        {
            set_error( err, err_size, "%s is truncated", path );
            goto failed;
        }
    }
    if (low >= high || !(module = calloc( 1, sizeof(*module) )))
    {
        set_error( err, err_size, "%s has nothing to load", path );
        goto failed;
    }
    low &= ~0xffffull;
    module->size = (high - low + 0xffff) & ~0xffffull;
    /* 64K aligned, as the segments are; writable while it is relocated, then each segment gets its protection */
    reserved_size = module->size + 0x10000;
    if ((reserved = mmap( NULL, reserved_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0 )) == MAP_FAILED)
    {
        set_error( err, err_size, "no memory for %s", path );
        goto failed;
    }
    module->mapping = (uint8_t *)(((uintptr_t)reserved + 0xffff) & ~(uintptr_t)0xffff);
    if (module->mapping > reserved) munmap( reserved, module->mapping - reserved );
    if (module->mapping + module->size < reserved + reserved_size)
        munmap( module->mapping + module->size, reserved + reserved_size - (module->mapping + module->size) );
    reserved = MAP_FAILED;
    module->base = module->mapping - low;
    file_name = strrchr( path, '/' );
    snprintf( module->name, sizeof(module->name), "%s", file_name ? file_name + 1 : path );
    if (!realpath( path, module->path )) snprintf( module->path, sizeof(module->path), "%s", path );

    if (!(module->phdrs = malloc( ehdr->phnum * sizeof(*phdr) )))
    {
        set_error( err, err_size, "no memory for %s", path );
        goto failed;
    }
    memcpy( (void *)module->phdrs, phdr, ehdr->phnum * sizeof(*phdr) );
    module->phnum = ehdr->phnum;

    for (i = 0; i < ehdr->phnum; i++)
    {
        switch (phdr[i].type)
        {
        case PT_LOAD:
            memcpy( module->base + phdr[i].vaddr, file + phdr[i].offset, phdr[i].filesz );
            break;
        case PT_DYNAMIC:
            dynamic = phdr[i].vaddr;
            dynamic_size = phdr[i].memsz;
            break;
        case PT_TLS:
            module->tls_image = module->base + phdr[i].vaddr;
            module->tls_filesz = phdr[i].filesz;
            module->tls_memsz = phdr[i].memsz;
            module->tls_align = phdr[i].align;
            pthread_key_create( &module->tls_key, free_tls_block );
            break;
        }
    }

    if (!dynamic)
    {
        set_error( err, err_size, "%s has no dynamic section", path );
        goto failed;
    }
    for (dyn = (const Elf64_Dyn *)(module->base + dynamic);
         (const uint8_t *)dyn < module->base + dynamic + dynamic_size && dyn->tag; dyn++)
    {
        switch (dyn->tag)
        {
        case DT_STRTAB:       strtab = dyn->val; break;
        case DT_SYMTAB:       symtab = dyn->val; break;
        case DT_HASH:         hash = dyn->val; break;
        case DT_GNU_HASH:     gnu_hash = dyn->val; break;
        case DT_RELA:         rela = dyn->val; break;
        case DT_RELASZ:       relasz = dyn->val; break;
        case DT_JMPREL:       jmprel = dyn->val; break;
        case DT_PLTRELSZ:     pltrelsz = dyn->val; break;
        case DT_RELR:         relr = dyn->val; break;
        case DT_RELRSZ:       relrsz = dyn->val; break;
        case DT_INIT:         module->init = dyn->val; break;
        case DT_INIT_ARRAY:   module->init_array = dyn->val; break;
        case DT_INIT_ARRAYSZ: module->init_arraysz = dyn->val; break;
        case DT_SONAME:       soname = dyn->val; break;
        case DT_VERSYM:       versym = dyn->val; break;
        case DT_VERNEED:      module->verneed = dyn->val; break;
        case DT_VERNEEDNUM:   module->verneednum = dyn->val; break;
        }
    }
    if (!strtab || !symtab)
    {
        set_error( err, err_size, "%s has no symbol table", path );
        goto failed;
    }
    module->strtab = (const char *)(module->base + strtab);
    module->symtab = (const Elf64_Sym *)(module->base + symtab);
    if (versym) module->versym = (const uint16_t *)(module->base + versym);
    module->nsyms = count_symbols( module, hash, gnu_hash );
    if (soname) snprintf( module->soname, sizeof(module->soname), "%s", module->strtab + soname );

    /* on the list before its relocations, so a resolver can find its own exports */
    pthread_mutex_lock( &modules_lock );
    module->next = modules;
    modules = module;
    pthread_mutex_unlock( &modules_lock );

    if (relr) apply_relr( module, relr, relrsz );
    if (rela) apply_rela( module, rela, relasz, resolve, context );
    if (jmprel) apply_rela( module, jmprel, pltrelsz, resolve, context );

    /* each segment's own protection, page by page; a page two segments share gets both */
    for (i = 0; i < ehdr->phnum; i++)
    {
        uintptr_t start, end;

        if (phdr[i].type != PT_LOAD) continue;
        start = (uintptr_t)(module->base + phdr[i].vaddr) & ~(page_size - 1);
        end = ((uintptr_t)(module->base + phdr[i].vaddr + phdr[i].memsz) + page_size - 1) & ~(page_size - 1);
        if (phdr[i].flags & PF_X) sys_icache_invalidate( (void *)start, end - start );
        if (mprotect( (void *)start, end - start, protection( phdr[i].flags ) ))
        {
            set_error( err, err_size, "cannot set the protection of %s", path );
            goto failed_listed;
        }
    }
    for (i = 0; i < ehdr->phnum; i++)
    {
        uintptr_t start, end;

        if (phdr[i].type != PT_GNU_RELRO) continue;
        start = ((uintptr_t)(module->base + phdr[i].vaddr) + page_size - 1) & ~(page_size - 1);
        end = (uintptr_t)(module->base + phdr[i].vaddr + phdr[i].memsz) & ~(page_size - 1);
        if (end > start) mprotect( (void *)start, end - start, PROT_READ );
    }
    munmap( file, st.st_size );
    return module;

failed_listed:
    pthread_mutex_lock( &modules_lock );
    modules = module->next;
    pthread_mutex_unlock( &modules_lock );
failed:
    if (file != MAP_FAILED) munmap( file, st.st_size );
    if (reserved != MAP_FAILED) munmap( reserved, reserved_size );
    if (module && module->mapping) munmap( module->mapping, module->size );
    if (module) free( (void *)module->phdrs );
    free( module );
    return NULL;
}

void elf_run_constructors( struct elf_module *module, int argc, char **argv, char **envp )
{
    typedef void (*constructor)( int argc, char **argv, char **envp );
    uint64_t i;

    if (module->constructed) return;
    module->constructed = 1;
    if (module->init) ((constructor)(module->base + module->init))( argc, argv, envp );
    for (i = 0; i < module->init_arraysz / sizeof(uint64_t); i++)
    {
        uint64_t fn = ((const uint64_t *)(module->base + module->init_array))[i];

        if (getenv( "LINUXHOST_TRACE" )) fprintf( stderr, "linuxhost: constructor %llu of %s at +%#llx\n", i, module->name, fn - (uint64_t)module->base );
        if (fn && fn != ~0ull) ((constructor)fn)( argc, argv, envp );
    }
}

const struct elf_stats *elf_get_stats( const struct elf_module *module ) { return &module->stats; }
const char *elf_get_name( const struct elf_module *module ) { return module->name; }
const char *elf_get_path( const struct elf_module *module ) { return module->path; }
uintptr_t elf_get_base( const struct elf_module *module ) { return (uintptr_t)module->base; }

const void *elf_get_phdrs( const struct elf_module *module, unsigned int *count )
{
    *count = module->phnum;
    return module->phdrs;
}

void elf_get_range( const struct elf_module *module, void **base, size_t *size )
{
    *base = module->mapping;
    *size = module->size;
}

const char *elf_unresolved_name( const struct elf_module *module, unsigned int index )
{
    return index < module->unresolved_count ? module->unresolved[index] : NULL;
}
