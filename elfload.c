/* Linux module host: ELF loader for x86_64 shared libraries. See elfload.h. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"

typedef struct { uint8_t ident[16]; uint16_t type, machine; uint32_t version; uint64_t entry, phoff, shoff;
                 uint32_t flags; uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx; } Elf64_Ehdr;
typedef struct { uint32_t type, flags; uint64_t offset, vaddr, paddr, filesz, memsz, align; } Elf64_Phdr;
typedef struct { int64_t tag; uint64_t val; } Elf64_Dyn;
typedef struct { uint32_t name; uint8_t info, other; uint16_t shndx; uint64_t value, size; } Elf64_Sym;
typedef struct { uint64_t offset, info; int64_t addend; } Elf64_Rela;

enum { PT_LOAD = 1, PT_DYNAMIC = 2, PT_TLS = 7 };
enum { DT_NEEDED = 1, DT_PLTRELSZ = 2, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_RELA = 7, DT_RELASZ = 8,
       DT_INIT = 12, DT_SONAME = 14, DT_JMPREL = 23, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35,
       DT_RELR = 36, DT_GNU_HASH = 0x6ffffef5 };
enum { R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7, R_X86_64_RELATIVE = 8,
       R_X86_64_DTPMOD64 = 16, R_X86_64_DTPOFF64 = 17, R_X86_64_TPOFF64 = 18, R_X86_64_IRELATIVE = 37 };
enum { STB_WEAK = 2 };

struct elf_module
{
    struct elf_module *next;
    char name[128], soname[128];
    uint8_t *base;                 /* load bias: the address of virtual address 0 */
    uint8_t *mapping;              /* what VirtualAlloc returned */
    size_t size;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned int nsyms;
    uint64_t init, init_array, init_arraysz;
    const uint8_t *tls_image;
    size_t tls_filesz, tls_memsz;
    unsigned long tls_id;          /* 1-based, what DTPMOD64 stores */
    DWORD tls_slot;
    int constructed;
    struct elf_stats stats;
};

static struct elf_module *modules;
static unsigned long next_tls_id = 1;

static void set_error( char *err, size_t size, const char *format, const char *arg )
{
    if (err && size) snprintf( err, size, format, arg );
}

/* a call through an import nobody provides ends here */
static SYSV void unresolved_import(void)
{
    static const char msg[] = "linuxhost: call to an unresolved import\n";
    DWORD written;

    WriteFile( GetStdHandle( STD_ERROR_HANDLE ), msg, sizeof(msg) - 1, &written, NULL );
    ExitProcess( 70 );
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

static HANDLE open_library( const char *path )
{
    WCHAR wide[1024];
    int len = 0;

    /* Wine opens Unix paths given as \\?\unix\... */
    if (path[0] == '/') len = MultiByteToWideChar( CP_UTF8, 0, "\\\\?\\unix", -1, wide, 16 ) - 1;
    if (!MultiByteToWideChar( CP_UTF8, 0, path, -1, wide + len, ARRAY_SIZE(wide) - len )) return INVALID_HANDLE_VALUE;
    return CreateFileW( wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL );
}

struct elf_module *elf_find( const char *name )
{
    const char *base = strrchr( name, '/' );
    struct elf_module *module;

    base = base ? base + 1 : name;
    for (module = modules; module; module = module->next)
        if (!strcmp( module->name, base ) || !strcmp( module->soname, base )) return module;
    return NULL;
}

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

static uint64_t resolve_symbol( struct elf_module *module, uint32_t index, elf_resolver resolve, void *context )
{
    const Elf64_Sym *sym = &module->symtab[index];
    const char *name = module->strtab + sym->name;
    void *addr;

    if (!index) return 0;
    if (sym->shndx) return (uint64_t)(module->base + sym->value);
    if (resolve && (addr = resolve( name, context )))
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
    if (strlen( module->stats.unresolved_names ) + strlen( name ) + 2 < sizeof(module->stats.unresolved_names) &&
        !strstr( module->stats.unresolved_names, name ))
    {
        strcat( module->stats.unresolved_names, name );
        strcat( module->stats.unresolved_names, " " );
    }
    return (uint64_t)unresolved_import;
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
        case R_X86_64_RELATIVE:
            *where = (uint64_t)(module->base + rela[i].addend);
            break;
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            *where = resolve_symbol( module, index, resolve, context );
            break;
        case R_X86_64_64:
            *where = resolve_symbol( module, index, resolve, context ) + rela[i].addend;
            break;
        case R_X86_64_DTPMOD64:
            *where = module->tls_id;
            break;
        case R_X86_64_DTPOFF64:
            *where = (index ? module->symtab[index].value : 0) + rela[i].addend;
            break;
        default:  /* TPOFF64 needs the Linux thread pointer in %fs, IRELATIVE a resolver call */
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

struct elf_module *elf_load( const char *path, elf_resolver resolve, void *context, char *err, size_t err_size )
{
    uint64_t low = ~0ull, high = 0, dynamic = 0, dynamic_size = 0, strtab = 0, symtab = 0, hash = 0, gnu_hash = 0;
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltrelsz = 0, relr = 0, relrsz = 0, soname = 0;
    struct elf_module *module = NULL;
    const Elf64_Ehdr *ehdr;
    const Elf64_Phdr *phdr;
    const Elf64_Dyn *dyn;
    const char *file_name;
    uint8_t *file = NULL;
    LARGE_INTEGER file_size;
    DWORD got;
    HANDLE handle;
    unsigned int i;

    if ((handle = open_library( path )) == INVALID_HANDLE_VALUE)
    {
        set_error( err, err_size, "cannot open %s", path );
        return NULL;
    }
    if (!GetFileSizeEx( handle, &file_size ) || file_size.QuadPart < sizeof(*ehdr) || file_size.QuadPart > 0x40000000 ||
        !(file = malloc( file_size.QuadPart )) || !ReadFile( handle, file, file_size.QuadPart, &got, NULL ) ||
        got != file_size.QuadPart)
    {
        set_error( err, err_size, "cannot read %s", path );
        goto failed;
    }
    CloseHandle( handle );
    handle = INVALID_HANDLE_VALUE;

    ehdr = (const Elf64_Ehdr *)file;
    if (memcmp( ehdr->ident, "\177ELF", 4 ) || ehdr->ident[4] != 2 || ehdr->ident[5] != 1 || ehdr->machine != 62 ||
        ehdr->type != 3 || ehdr->phoff + ehdr->phnum * (uint64_t)sizeof(*phdr) > (uint64_t)file_size.QuadPart)
    {
        set_error( err, err_size, "%s is not an x86_64 Linux shared library", path );
        goto failed;
    }
    phdr = (const Elf64_Phdr *)(file + ehdr->phoff);
    for (i = 0; i < ehdr->phnum; i++)
    {
        if (phdr[i].type != PT_LOAD) continue;
        if (phdr[i].vaddr < low) low = phdr[i].vaddr;
        if (phdr[i].vaddr + phdr[i].memsz > high) high = phdr[i].vaddr + phdr[i].memsz;
        if (phdr[i].offset + phdr[i].filesz > (uint64_t)file_size.QuadPart)
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
    /* One region for the whole library, readable, writable and executable. FEX translates the code; nothing in it
     * runs as host code. */
    if (!(module->mapping = VirtualAlloc( NULL, module->size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE )))
    {
        set_error( err, err_size, "no memory for %s", path );
        goto failed;
    }
    module->base = module->mapping - low;
    file_name = strrchr( path, '/' );
    if (!file_name) file_name = strrchr( path, '\\' );
    snprintf( module->name, sizeof(module->name), "%s", file_name ? file_name + 1 : path );

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
            module->tls_id = next_tls_id++;
            module->tls_slot = TlsAlloc();
            break;
        }
    }
    free( file );
    file = NULL;

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
        }
    }
    if (!strtab || !symtab)
    {
        set_error( err, err_size, "%s has no symbol table", path );
        goto failed;
    }
    module->strtab = (const char *)(module->base + strtab);
    module->symtab = (const Elf64_Sym *)(module->base + symtab);
    module->nsyms = count_symbols( module, hash, gnu_hash );
    if (soname) snprintf( module->soname, sizeof(module->soname), "%s", module->strtab + soname );

    /* on the list before its relocations, so a resolver can find its own exports */
    module->next = modules;
    modules = module;

    if (relr) apply_relr( module, relr, relrsz );
    if (rela) apply_rela( module, rela, relasz, resolve, context );
    if (jmprel) apply_rela( module, jmprel, pltrelsz, resolve, context );
    return module;

failed:
    if (handle != INVALID_HANDLE_VALUE) CloseHandle( handle );
    if (module && module->mapping) VirtualFree( module->mapping, 0, MEM_RELEASE );
    free( module );
    free( file );
    return NULL;
}

void elf_run_constructors( struct elf_module *module )
{
    typedef void (SYSV *constructor)( int argc, char **argv, char **envp );
    static char *no_strings[] = { NULL };
    uint64_t i;

    if (module->constructed) return;
    module->constructed = 1;
    if (module->init) ((constructor)(module->base + module->init))( 0, no_strings, no_strings );
    for (i = 0; i < module->init_arraysz / sizeof(uint64_t); i++)
    {
        uint64_t fn = ((const uint64_t *)(module->base + module->init_array))[i];

        if (fn && fn != ~0ull) ((constructor)fn)( 0, no_strings, no_strings );
    }
}

void *elf_tls_block( struct elf_module *module )
{
    void *block;

    if (!module->tls_memsz) return NULL;
    if ((block = TlsGetValue( module->tls_slot ))) return block;
    if (!(block = calloc( 1, module->tls_memsz ))) return NULL;
    memcpy( block, module->tls_image, module->tls_filesz );
    TlsSetValue( module->tls_slot, block );
    return block;
}

struct elf_module *elf_module_by_tls_id( unsigned long id )
{
    struct elf_module *module;

    for (module = modules; module; module = module->next) if (module->tls_id == id) return module;
    return NULL;
}

const struct elf_stats *elf_get_stats( const struct elf_module *module ) { return &module->stats; }
const char *elf_get_name( const struct elf_module *module ) { return module->name; }

void elf_get_range( const struct elf_module *module, void **base, size_t *size )
{
    *base = module->mapping;
    *size = module->size;
}
