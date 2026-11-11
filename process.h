/* Linux module host: the Linux process a Windows program expects under Proton. See process.c. */
#ifndef LINUXHOST_PROCESS_H
#define LINUXHOST_PROCESS_H

#include <stddef.h>
#include <windows.h>

/* a function the made-up libc.so.6 exports */
struct libc_export { const char *name; void *function; };

/* makes up libc.so.6 with these exports; returns 0 on failure */
int linuxhost_process_init( const struct libc_export *exports, unsigned int count );

/* before exec: the in-memory files go to the new process (it reads them at start) */
void linuxhost_export_fds(void);

/* gives the calling thread a Linux thread control block in %fs */
void linuxhost_thread_init(void);

/* an address a Windows program can call a Linux function at: it sets up %fs first if the thread needs it */
void *linuxhost_entry_stub( void *function, const char *name );

/* the path Linux code was loaded under, for /proc/self/maps; how an in-memory file shows there */
void linuxhost_note_module_path( void *base, size_t size, const char *path );
int linuxhost_fd_maps_name( int fd, char *buf, size_t size );
int linuxhost_fd_link( int fd, char *buf, size_t size );  /* what /proc/self/fd/<fd> links to */

/* descriptors for Windows objects: kinds as process.c's enum (FD_SOCKET 3, FD_EPOLL 4) */
enum { LINUXHOST_FD_SOCKET = 3, LINUXHOST_FD_EPOLL = 4 };
int linuxhost_new_fd( int kind, HANDLE handle, const char *name );
int linuxhost_fd_kind( int fd, HANDLE *handle );
void **linuxhost_fd_private( int fd );

/* socket I/O and closing (libc_net.c), for read, write and close on a socket descriptor */
long long linuxhost_socket_io( UINT_PTR s, void *buf, size_t size, int write );
void linuxhost_socket_close( UINT_PTR s );
void linuxhost_epoll_free( void *set );
int linuxhost_socket_set_nonblock( int fd, int on );  /* 0 if fd is not a socket */

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
WCHAR *linuxhost_unix_path( const char *path );
int linuxhost_is_proc_path( const char *path );

/* the names in a made-up directory (/proc, /proc/self/fd...), NUL-separated in names; -1 if it is not one */
int linuxhost_proc_dir( const char *path, char *names, size_t size );

/* a child of fork (fork.c), as /proc/<pid> shows it: 0 if pid is not one */
int linuxhost_child_info( int pid, char *state, int *traced, unsigned long *windows_pid );
int linuxhost_exe_path( char *buf, size_t size );      /* Wine's loader, as /proc/self/exe */
int linuxhost_program_path( char *buf, size_t size );  /* the Windows program */

/* Linux's struct stat for a file */
void linuxhost_stat_from_info( const BY_HANDLE_FILE_INFORMATION *info, void *st );

/* a line on standard error when LINUXHOST_TRACE is set */
void linuxhost_trace( const char *format, ... );

#endif
