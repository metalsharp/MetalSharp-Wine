/* Linux module host: direct Linux system calls. See syscall.c. */
#ifndef LINUXHOST_SYSCALL_H
#define LINUXHOST_SYSCALL_H

struct elf_module;

/* has Wine send the library's syscall instructions to the host. Returns 0 when Wine has no support for it. */
int linuxhost_register_syscalls( struct elf_module *module );

/* the same for an address range a program fills with Linux code itself */
int linuxhost_register_syscall_range( void *start, size_t size );

/* how many system calls the host was asked for and does not implement */
unsigned int linuxhost_unimplemented_syscalls(void);

#endif
