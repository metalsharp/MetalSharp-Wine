/* Linux module host: direct Linux system calls.
 *
 * A Linux library can reach the kernel with the syscall instruction instead of calling glibc.
 * Wine sends such a call, when it comes from a library registered with
 * __wine_register_linux_syscalls, to linux_syscall_entry below: the call number is in rax, the
 * arguments in rdi, rsi, rdx, r10, r8 and r9, and the address to return to in rcx. A Linux kernel
 * changes only rax, rcx and r11, so the entry point saves everything else that Windows code may
 * change, hands the registers to linux_syscall(), and returns its result in rax.
 *
 * Only a first few calls are implemented. A call that is not gets -ENOSYS and one line on standard
 * error naming its number, which is how the list of calls a library needs is collected. */
#include <windows.h>
#include <stdio.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"
#include "syscall.h"

#define LX_ENOSYS 38
#define LX_EBADF   9

struct linux_registers { unsigned long long nr, rdi, rsi, rdx, r10, r8, r9; };

static LONG unimplemented_calls;

long long linux_syscall( struct linux_registers *regs )
{
    DWORD written;

    switch (regs->nr)
    {
    case 0:   /* read */
        return linuxhost_read( regs->rdi, (void *)regs->rsi, regs->rdx );
    case 2:   /* open */
        return linuxhost_open( (const char *)regs->rdi, regs->rsi );
    case 257: /* openat: absolute paths only */
        return linuxhost_open( (const char *)regs->rsi, regs->rdx );
    case 3:   /* close */
        return linuxhost_close( regs->rdi );
    case 5:   /* fstat */
        return linuxhost_fstat( regs->rdi, (void *)regs->rsi );
    case 8:   /* lseek */
        return linuxhost_lseek( regs->rdi, regs->rsi, regs->rdx );
    case 87:  /* unlink */
        return linuxhost_unlink( (const char *)regs->rdi );
    case 319: /* memfd_create */
        return linuxhost_memfd_create( (const char *)regs->rdi, regs->rsi );
    case 1:   /* write */
        if (regs->rdi > 2) return linuxhost_write( regs->rdi, (void *)regs->rsi, regs->rdx );
        if (regs->rdi != 1 && regs->rdi != 2) return -LX_EBADF;
        if (!WriteFile( GetStdHandle( regs->rdi == 1 ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE ), (void *)regs->rsi,
                        regs->rdx, &written, NULL )) return -LX_EBADF;
        return written;
    case 39:  /* getpid */
        return GetCurrentProcessId();
    case 186: /* gettid */
        return GetCurrentThreadId();
    case 24:  /* sched_yield */
        SwitchToThread();
        return 0;
    default:
    {
        char line[96];
        int len = snprintf( line, sizeof(line), "linuxhost: Linux system call %llu is not implemented\n", regs->nr );

        InterlockedIncrement( &unimplemented_calls );
        WriteFile( GetStdHandle( STD_ERROR_HANDLE ), line, len, &written, NULL );
        return -LX_ENOSYS;
    }
    }
}

long long linuxhost_syscall( long long number, long long a1, long long a2, long long a3, long long a4, long long a5,
                             long long a6 )
{
    struct linux_registers regs = { number, a1, a2, a3, a4, a5, a6 };

    return linux_syscall( &regs );
}

/* Entered by a jump, with the library's stack pointer untouched. The 128 bytes below it are the library's own (the
 * System V red zone) and its alignment is whatever the library had at the syscall instruction, so the frame is made
 * below the red zone and aligned here. The registers Windows code may change and Linux expects kept are saved: rdi
 * and rsi are in the register block already, and rdx, r8, r9, r10 and xmm0 to xmm5 go beside it. rbx, rbp, r12 to
 * r15 and xmm6 to xmm15 are kept by the Windows convention. r11 is free: a system call changes it. */
__asm__( ".text\n"
         ".globl linux_syscall_entry\n"
         "linux_syscall_entry:\n"
         "    movq %rsp, %r11\n"
         "    leaq -0x80(%rsp), %rsp\n"
         "    andq $-16, %rsp\n"
         "    subq $0xd0, %rsp\n"             /* 0x20 shadow space, 0x38 registers, 0x60 xmm, the stack pointer and return address */
         "    movq %r11, 0xc0(%rsp)\n"
         "    movq %rcx, 0xc8(%rsp)\n"
         "    movq %rax, 0x20(%rsp)\n"
         "    movq %rdi, 0x28(%rsp)\n"
         "    movq %rsi, 0x30(%rsp)\n"
         "    movq %rdx, 0x38(%rsp)\n"
         "    movq %r10, 0x40(%rsp)\n"
         "    movq %r8,  0x48(%rsp)\n"
         "    movq %r9,  0x50(%rsp)\n"
         "    movdqu %xmm0, 0x58(%rsp)\n"
         "    movdqu %xmm1, 0x68(%rsp)\n"
         "    movdqu %xmm2, 0x78(%rsp)\n"
         "    movdqu %xmm3, 0x88(%rsp)\n"
         "    movdqu %xmm4, 0x98(%rsp)\n"
         "    movdqu %xmm5, 0xa8(%rsp)\n"
         "    leaq 0x20(%rsp), %rcx\n"
         "    call linux_syscall\n"
         "    movq 0x28(%rsp), %rdi\n"
         "    movq 0x30(%rsp), %rsi\n"
         "    movq 0x38(%rsp), %rdx\n"
         "    movq 0x40(%rsp), %r10\n"
         "    movq 0x48(%rsp), %r8\n"
         "    movq 0x50(%rsp), %r9\n"
         "    movdqu 0x58(%rsp), %xmm0\n"
         "    movdqu 0x68(%rsp), %xmm1\n"
         "    movdqu 0x78(%rsp), %xmm2\n"
         "    movdqu 0x88(%rsp), %xmm3\n"
         "    movdqu 0x98(%rsp), %xmm4\n"
         "    movdqu 0xa8(%rsp), %xmm5\n"
         "    movq 0xc8(%rsp), %rcx\n"
         "    movq 0xc0(%rsp), %rsp\n"
         "    jmp *%rcx\n" );

extern void linux_syscall_entry(void);

int linuxhost_register_syscalls( struct elf_module *module )
{
    typedef LONG (__cdecl *register_fn)( void *start, SIZE_T size, void *handler );
    register_fn reg = (register_fn)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "__wine_register_linux_syscalls" );
    void *base;
    size_t size;

    if (!reg) return 0;  /* not this Wine: direct system calls of the library go to Windows */
    elf_get_range( module, &base, &size );
    return !reg( base, size, linux_syscall_entry );
}

/* system calls from code anywhere in [start, start + size): Linux code a Windows program put there itself */
int linuxhost_register_syscall_range( void *start, size_t size )
{
    typedef LONG (__cdecl *register_fn)( void *start, SIZE_T size, void *handler );
    register_fn reg = (register_fn)GetProcAddress( GetModuleHandleA( "ntdll.dll" ), "__wine_register_linux_syscalls" );

    return reg && !reg( start, size, linux_syscall_entry );
}

unsigned int linuxhost_unimplemented_syscalls(void)
{
    return unimplemented_calls;
}
