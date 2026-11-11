/* Linux module host: the Linux process a Windows program expects under Proton. See process.c. */
#ifndef LINUXHOST_PROCESS_H
#define LINUXHOST_PROCESS_H

#include <stddef.h>

/* a function the made-up libc.so.6 exports */
struct libc_export { const char *name; void *function; };

/* makes up libc.so.6 with these exports; returns 0 on failure */
int linuxhost_process_init( const struct libc_export *exports, unsigned int count );

/* gives the calling thread a Linux thread control block in %fs */
void linuxhost_thread_init(void);

/* an address a Windows program can call a Linux function at: it sets up %fs first if the thread needs it */
void *linuxhost_entry_stub( void *function, const char *name );

/* the path Linux code was loaded under, for /proc/self/maps; how an in-memory file shows there */
void linuxhost_note_module_path( void *base, size_t size, const char *path );
int linuxhost_fd_maps_name( int fd, char *buf, size_t size );

/* the contents of an in-memory file, for dlopen("/proc/self/fd/<n>") */
int linuxhost_fd_contents( int fd, const void **data, size_t *size );

/* system calls: a result, or a negative Linux errno */
long long linuxhost_open( const char *path, int flags );
long long linuxhost_memfd_create( const char *name, unsigned int flags );
long long linuxhost_read( int fd, void *buf, size_t size );
long long linuxhost_write( int fd, const void *buf, size_t size );
long long linuxhost_lseek( int fd, long long offset, int whence );
long long linuxhost_close( int fd );
long long linuxhost_seals( int fd, int add, unsigned int seals );  /* F_ADD_SEALS (add), F_GET_SEALS */
long long linuxhost_fstat( int fd, void *buf );
long long linuxhost_unlink( const char *path );

/* paths: Wine's form of a Unix path (to free), whether a path is under /proc or /sys, this program's Unix path */
#include <windows.h>
WCHAR *linuxhost_unix_path( const char *path );
int linuxhost_is_proc_path( const char *path );
int linuxhost_exe_path( char *buf, size_t size );      /* Wine's loader, as /proc/self/exe */
int linuxhost_program_path( char *buf, size_t size );  /* the Windows program */

/* Linux's struct stat for a file */
void linuxhost_stat_from_info( const BY_HANDLE_FILE_INFORMATION *info, void *st );

/* a line on standard error when LINUXHOST_TRACE is set */
void linuxhost_trace( const char *format, ... );

#endif
