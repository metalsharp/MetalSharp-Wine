/* Linux module host: glibc's files, directories and stdio, on the host's Linux file descriptors (process.c). */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"

typedef unsigned long long lx_size_t;
typedef long long lx_long;

static lx_long fail( int err )
{
    *linuxhost_errno() = err;
    return -1;
}

/* a host result: the value, or -1 with errno for a negative Linux errno */
static lx_long result( long long ret )
{
    if (ret < 0 && ret > -4096) return fail( (int)-ret );
    return ret;
}

static int error_from_win32( DWORD err )
{
    switch (err)
    {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: case ERROR_INVALID_NAME: return 2;
    case ERROR_ACCESS_DENIED: case ERROR_SHARING_VIOLATION: return 13;
    case ERROR_FILE_EXISTS: case ERROR_ALREADY_EXISTS: return 17;
    case ERROR_DIR_NOT_EMPTY: return 39;
    case ERROR_DIRECTORY: return 20;
    default: return 5;
    }
}

/* ---------------------------------------------------------------------------------------------------------
 * File descriptors
 */

/* open's mode is variadic; Linux passes it in a register, where a fixed third argument goes */
static SYSV int lx_open( const char *path, int flags, unsigned int mode ) { return result( linuxhost_open( path, flags ) ); }
static SYSV lx_long lx_read( int fd, void *buf, lx_size_t size ) { return result( linuxhost_read( fd, buf, size ) ); }
static SYSV int lx_close( int fd ) { return result( linuxhost_close( fd ) ); }

static SYSV lx_long lx_write( int fd, const void *buf, lx_size_t size )
{
    DWORD written;

    if (fd == 1 || fd == 2)
    {
        WriteFile( GetStdHandle( fd == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE ), buf, size, &written, NULL );
        return size;
    }
    return result( linuxhost_write( fd, buf, size ) );
}

static SYSV int lx_fcntl( int fd, int cmd, unsigned long long arg )
{
    switch (cmd)
    {
    case 1: case 2: case 3: case 4: return 0;  /* F_GETFD, F_SETFD, F_GETFL, F_SETFL */
    default: linuxhost_trace( "fcntl %d %d\n", fd, cmd ); return fail( 22 );
    }
}

static SYSV int lx___xstat( int ver, const char *path, void *st )
{
    BY_HANDLE_FILE_INFORMATION info;
    HANDLE handle;
    WCHAR *wpath;
    long long fd;

    if (linuxhost_is_proc_path( path ))
    {
        if ((fd = linuxhost_open( path, 0 )) < 0) return fail( (int)-fd );
        linuxhost_fstat( fd, st );
        linuxhost_close( fd );
        return 0;
    }
    if (!(wpath = linuxhost_unix_path( path ))) return fail( 12 );
    handle = CreateFileW( wpath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                          FILE_FLAG_BACKUP_SEMANTICS, NULL );
    free( wpath );
    if (handle == INVALID_HANDLE_VALUE)
    {
        linuxhost_trace( "stat %s: not found\n", path );
        return fail( error_from_win32( GetLastError() ) );
    }
    GetFileInformationByHandle( handle, &info );
    CloseHandle( handle );
    linuxhost_stat_from_info( &info, st );
    return 0;
}

static SYSV int lx_mkdir( const char *path, unsigned int mode )
{
    WCHAR *wpath = linuxhost_unix_path( path );
    BOOL ok = wpath && CreateDirectoryW( wpath, NULL );

    free( wpath );
    return ok ? 0 : fail( error_from_win32( GetLastError() ) );
}

static SYSV int lx_unlink( const char *path )
{
    WCHAR *wpath = linuxhost_unix_path( path );
    BOOL ok = wpath && DeleteFileW( wpath );

    linuxhost_trace( "unlink %s: %d\n", path, ok );
    free( wpath );
    return ok ? 0 : fail( error_from_win32( GetLastError() ) );
}

static SYSV int lx_remove( const char *path )
{
    WCHAR *wpath = linuxhost_unix_path( path );
    BOOL ok = wpath && (DeleteFileW( wpath ) || RemoveDirectoryW( wpath ));

    free( wpath );
    return ok ? 0 : fail( error_from_win32( GetLastError() ) );
}

static SYSV int lx_rename( const char *from, const char *to )
{
    WCHAR *wfrom = linuxhost_unix_path( from ), *wto = linuxhost_unix_path( to );
    BOOL ok = wfrom && wto && MoveFileExW( wfrom, wto, MOVEFILE_REPLACE_EXISTING );

    free( wfrom );
    free( wto );
    return ok ? 0 : fail( error_from_win32( GetLastError() ) );
}

static SYSV int lx_fchmod( int fd, unsigned int mode ) { return 0; }

static SYSV lx_long lx_readlink( const char *path, char *buf, lx_size_t size )
{
    char exe[1024];
    size_t len;

    linuxhost_trace( "readlink %s\n", path );
    if ((!strcmp( path, "/proc/self/exe" ) || !strncmp( path, "/proc/", 6 )) && strstr( path, "/exe" ) &&
        linuxhost_exe_path( exe, sizeof(exe) ))
    {
        len = min( strlen( exe ), size );
        memcpy( buf, exe, len );
        return len;
    }
    return fail( 22 );
}

static SYSV char *lx_realpath( const char *path, char *resolved )
{
    if (!resolved && !(resolved = malloc( 4096 ))) return NULL;
    snprintf( resolved, 4096, "%s", path );
    return resolved;
}

/* ---------------------------------------------------------------------------------------------------------
 * Directories: glibc's struct dirent on x86_64
 */

struct lx_dirent { unsigned long long ino; long long off; unsigned short reclen; unsigned char type; char name[256]; };
struct lx_dir { HANDLE find; WIN32_FIND_DATAW data; int first, done; struct lx_dirent entry; };

static SYSV struct lx_dir *lx_opendir( const char *path )
{
    struct lx_dir *dir;
    WCHAR *wpath, *pattern;
    size_t len;

    linuxhost_trace( "opendir %s\n", path );
    if (linuxhost_is_proc_path( path )) { fail( 2 ); return NULL; }
    if (!(wpath = linuxhost_unix_path( path ))) { fail( 12 ); return NULL; }
    len = wcslen( wpath );
    pattern = malloc( (len + 3) * sizeof(WCHAR) );
    wcscpy( pattern, wpath );
    wcscat( pattern, L"\\*" );
    free( wpath );
    if (!(dir = calloc( 1, sizeof(*dir) )))
    {
        free( pattern );
        fail( 12 );
        return NULL;
    }
    dir->find = FindFirstFileW( pattern, &dir->data );
    free( pattern );
    if (dir->find == INVALID_HANDLE_VALUE)
    {
        fail( error_from_win32( GetLastError() ) );
        free( dir );
        return NULL;
    }
    dir->first = 1;
    return dir;
}

static SYSV struct lx_dirent *lx_readdir( struct lx_dir *dir )
{
    if (dir->done) return NULL;
    if (!dir->first && !FindNextFileW( dir->find, &dir->data ))
    {
        dir->done = 1;
        return NULL;
    }
    dir->first = 0;
    memset( &dir->entry, 0, sizeof(dir->entry) );
    dir->entry.ino = 1;
    dir->entry.reclen = sizeof(dir->entry);
    dir->entry.type = (dir->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
    WideCharToMultiByte( CP_UTF8, 0, dir->data.cFileName, -1, dir->entry.name, sizeof(dir->entry.name), NULL, NULL );
    return &dir->entry;
}

static SYSV int lx_closedir( struct lx_dir *dir )
{
    FindClose( dir->find );
    free( dir );
    return 0;
}

/* ---------------------------------------------------------------------------------------------------------
 * stdio: a FILE is the host's own, on a Linux file descriptor
 */

struct lx_file { int fd, eof, error, ungot; };
static struct lx_file lx_stdin_file = { 0, 0, 0, -1 }, lx_stdout_file = { 1, 0, 0, -1 }, lx_stderr_file = { 2, 0, 0, -1 };
static struct lx_file *lx_stdin = &lx_stdin_file, *lx_stdout = &lx_stdout_file, *lx_stderr = &lx_stderr_file;

static SYSV struct lx_file *lx_fopen( const char *path, const char *mode )
{
    int flags = 0, fd;
    struct lx_file *f;

    if (mode[0] == 'r') flags = strchr( mode, '+' ) ? 2 : 0;
    else if (mode[0] == 'w') flags = (strchr( mode, '+' ) ? 2 : 1) | 0x40 | 0x200;
    else if (mode[0] == 'a') flags = (strchr( mode, '+' ) ? 2 : 1) | 0x40 | 0x400;
    if ((fd = lx_open( path, flags, 0644 )) < 0) return NULL;
    if (!(f = calloc( 1, sizeof(*f) ))) { linuxhost_close( fd ); fail( 12 ); return NULL; }
    f->fd = fd;
    f->ungot = -1;
    return f;
}

static SYSV int lx_fclose( struct lx_file *f )
{
    if (f->fd > 2) linuxhost_close( f->fd );
    if (f != lx_stdin && f != lx_stdout && f != lx_stderr) free( f );
    return 0;
}

static SYSV lx_size_t lx_fread( void *buf, lx_size_t size, lx_size_t count, struct lx_file *f )
{
    lx_size_t total = size * count, got = 0;
    long long n;

    if (!total) return 0;
    while (got < total)
    {
        if ((n = linuxhost_read( f->fd, (char *)buf + got, total - got )) <= 0)
        {
            if (n < 0) f->error = 1; else f->eof = 1;
            break;
        }
        got += n;
    }
    return got / size;
}

static SYSV lx_size_t lx_fwrite( const void *buf, lx_size_t size, lx_size_t count, struct lx_file *f )
{
    lx_long n = lx_write( f->fd, buf, size * count );

    if (n < 0) { f->error = 1; return 0; }
    return size ? n / size : 0;
}

static SYSV int lx_fputc( int c, struct lx_file *f ) { char ch = c; return lx_fwrite( &ch, 1, 1, f ) ? (unsigned char)c : -1; }
static SYSV int lx_fputs( const char *s, struct lx_file *f ) { return lx_fwrite( s, 1, strlen( s ), f ) ? 1 : -1; }
static SYSV int lx_putchar( int c ) { return lx_fputc( c, lx_stdout ); }
static SYSV int lx_puts( const char *s ) { lx_fputs( s, lx_stdout ); return lx_fputc( '\n', lx_stdout ); }

static SYSV char *lx_fgets( char *buf, int size, struct lx_file *f )
{
    int len = 0;
    char c;

    while (len < size - 1)
    {
        long long n = linuxhost_read( f->fd, &c, 1 );

        if (n <= 0) { if (n < 0) f->error = 1; else f->eof = 1; break; }
        buf[len++] = c;
        if (c == '\n') break;
    }
    if (!len) return NULL;
    buf[len] = 0;
    return buf;
}

static SYSV int lx_feof( struct lx_file *f ) { return f->eof; }
static SYSV int lx_ferror( struct lx_file *f ) { return f->error; }

static SYSV int lx_fseek( struct lx_file *f, lx_long offset, int whence )
{
    f->eof = 0;
    return linuxhost_lseek( f->fd, offset, whence ) < 0 ? -1 : 0;
}

static SYSV lx_long lx_ftell( struct lx_file *f ) { return linuxhost_lseek( f->fd, 0, 1 ); }
static SYSV void lx_rewind( struct lx_file *f ) { lx_fseek( f, 0, 0 ); f->error = 0; }

/* ---------------------------------------------------------------------------------------------------------
 * Formatted output, with glibc.c's formatter. Variadic entry points (below) build Linux's va_list.
 */

struct lx_va_list { unsigned int gp_offset, fp_offset; void *overflow_arg_area, *reg_save_area; };
typedef SYSV int (*vsnprintf_fn)( char *, lx_size_t, const char *, struct lx_va_list * );

static vsnprintf_fn formatter(void)
{
    static vsnprintf_fn fn;

    if (!fn) fn = (vsnprintf_fn)linuxhost_glibc_function( "vsnprintf" );
    return fn;
}

SYSV int lx_vprintf_core( const char *fmt, struct lx_va_list *ap )
{
    char buf[4096];
    int len = formatter()( buf, sizeof(buf), fmt, ap );

    lx_write( 1, buf, min( len, (int)sizeof(buf) - 1 ) );
    return len;
}

SYSV int lx_vsnprintf_core( char *buf, lx_size_t size, const char *fmt, struct lx_va_list *ap )
{
    int ret = formatter()( buf, size, fmt, ap );

    linuxhost_trace( "vsnprintf \"%s\" -> \"%.200s\"\n", fmt, size ? buf : "" );
    return ret;
}

SYSV int lx_sprintf_chk_core( char *buf, int flag, lx_size_t size, const char *fmt, struct lx_va_list *ap )
{
    return formatter()( buf, size, fmt, ap );
}

/* vswprintf: a wide format, formatted through a narrow copy; its %ls arguments are not converted */
static SYSV int lx_vswprintf( uint32_t *buf, lx_size_t size, const uint32_t *fmt, struct lx_va_list *ap )
{
    char narrow_fmt[1024], out[4096];
    lx_size_t i;
    int len;

    for (i = 0; fmt[i] && i < sizeof(narrow_fmt) - 1; i++) narrow_fmt[i] = fmt[i] < 128 ? (char)fmt[i] : '?';
    narrow_fmt[i] = 0;
    len = formatter()( out, sizeof(out), narrow_fmt, ap );
    if (len < 0 || (lx_size_t)len >= size) return -1;
    for (i = 0; i <= (lx_size_t)len; i++) buf[i] = (unsigned char)out[i];
    return len;
}

/* A System V variadic function: the register save area gets rdi..r9 and xmm0..xmm7, the va_list points at it and
 * at the caller's stacked arguments, and the core function gets the named arguments and the va_list. */
#define VARIADIC(name, core, named, va_reg) \
    __asm__( ".text\n" \
             ".globl " #name "\n" \
             #name ":\n" \
             "    pushq %rbp\n" \
             "    movq %rsp, %rbp\n" \
             "    subq $0xd0, %rsp\n" \
             "    movq %rdi, 0x00(%rsp)\n" \
             "    movq %rsi, 0x08(%rsp)\n" \
             "    movq %rdx, 0x10(%rsp)\n" \
             "    movq %rcx, 0x18(%rsp)\n" \
             "    movq %r8,  0x20(%rsp)\n" \
             "    movq %r9,  0x28(%rsp)\n" \
             "    movaps %xmm0, 0x30(%rsp)\n" \
             "    movaps %xmm1, 0x40(%rsp)\n" \
             "    movaps %xmm2, 0x50(%rsp)\n" \
             "    movaps %xmm3, 0x60(%rsp)\n" \
             "    movaps %xmm4, 0x70(%rsp)\n" \
             "    movaps %xmm5, 0x80(%rsp)\n" \
             "    movaps %xmm6, 0x90(%rsp)\n" \
             "    movaps %xmm7, 0xa0(%rsp)\n" \
             "    movl $" #named "*8, 0xb0(%rsp)\n"  /* gp_offset */ \
             "    movl $48, 0xb4(%rsp)\n"            /* fp_offset */ \
             "    leaq 16(%rbp), %rax\n" \
             "    movq %rax, 0xb8(%rsp)\n"           /* overflow_arg_area */ \
             "    movq %rsp, 0xc0(%rsp)\n"           /* reg_save_area */ \
             "    leaq 0xb0(%rsp), " va_reg "\n" \
             "    call " #core "\n" \
             "    movq %rbp, %rsp\n" \
             "    popq %rbp\n" \
             "    ret\n" )

VARIADIC( lx_printf, lx_vprintf_core, 1, "%rsi" );
VARIADIC( lx_snprintf, lx_vsnprintf_core, 3, "%rcx" );
VARIADIC( lx___sprintf_chk, lx_sprintf_chk_core, 4, "%r8" );
extern void lx_printf(void), lx_snprintf(void), lx___sprintf_chk(void);

const struct libc_function linuxhost_libc_io_functions[] =
{
    { "open", lx_open }, { "read", lx_read }, { "write", lx_write }, { "close", lx_close }, { "fcntl", lx_fcntl },
    { "__xstat", lx___xstat }, { "mkdir", lx_mkdir }, { "unlink", lx_unlink }, { "remove", lx_remove },
    { "rename", lx_rename }, { "fchmod", lx_fchmod }, { "readlink", lx_readlink }, { "realpath", lx_realpath },
    { "opendir", lx_opendir }, { "readdir", lx_readdir }, { "closedir", lx_closedir },
    { "fopen", lx_fopen }, { "fclose", lx_fclose }, { "fread", lx_fread }, { "fwrite", lx_fwrite },
    { "fputc", lx_fputc }, { "fputs", lx_fputs }, { "putchar", lx_putchar }, { "puts", lx_puts },
    { "fgets", lx_fgets }, { "feof", lx_feof }, { "ferror", lx_ferror }, { "fseek", lx_fseek },
    { "ftell", lx_ftell }, { "rewind", lx_rewind }, { "stderr", &lx_stderr }, { "stdout", &lx_stdout },
    { "stdin", &lx_stdin },
    { "printf", lx_printf }, { "snprintf", lx_snprintf }, { "__sprintf_chk", lx___sprintf_chk },
    { "vsnprintf", lx_vsnprintf_core }, { "vswprintf", lx_vswprintf },
    { NULL, NULL }
};
