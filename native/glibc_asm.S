/* Linux module host, native: the parts of the glibc layer that need assembly.
 *
 * setjmp and longjmp, getcontext, makecontext and setcontext, on glibc's structures for aarch64, which only
 * these functions read. jmp_buf: x19-x28, x29, x30, sp, d8-d15. ucontext_t: regs[] at 184, sp at 432, pc at 440,
 * and d8-d15 at the start of __reserved (464). Signal masks are not saved or restored: the library's signal
 * state is virtual (glibc_signal.c).
 *
 * Variadic entry points: Linux passes a variadic function's arguments in x0-x7 and q0-q7 and then on the stack;
 * Apple's convention puts the variadic ones on the stack. Each entry point here saves the registers, builds
 * Linux's va_list over them and calls a C function with the named arguments and a pointer to that va_list. */

    .text

/* ------------------------------------------------------------------------------------------------------ */

    .p2align 2
    .globl _lnx__setjmp
    .globl _lnx___sigsetjmp
_lnx__setjmp:
_lnx___sigsetjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x9, sp
    str x9, [x0, #96]
    stp d8, d9, [x0, #104]
    stp d10, d11, [x0, #120]
    stp d12, d13, [x0, #136]
    stp d14, d15, [x0, #152]
    mov w0, #0
    ret

    .p2align 2
    .globl _lnx__longjmp
    .globl _lnx_siglongjmp
_lnx__longjmp:
_lnx_siglongjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x9, [x0, #96]
    mov sp, x9
    ldp d8, d9, [x0, #104]
    ldp d10, d11, [x0, #120]
    ldp d12, d13, [x0, #136]
    ldp d14, d15, [x0, #152]
    cmp w1, #0
    csinc w0, w1, wzr, ne
    ret

/* ------------------------------------------------------------------------------------------------------ */

    .p2align 2
    .globl _lnx_getcontext
_lnx_getcontext:
    str xzr, [x0, #184]          /* regs[0]: getcontext "returns" 0 when the context is resumed */
    stp x19, x20, [x0, #184 + 19 * 8]
    stp x21, x22, [x0, #184 + 21 * 8]
    stp x23, x24, [x0, #184 + 23 * 8]
    stp x25, x26, [x0, #184 + 25 * 8]
    stp x27, x28, [x0, #184 + 27 * 8]
    stp x29, x30, [x0, #184 + 29 * 8]
    mov x9, sp
    str x9, [x0, #432]
    str x30, [x0, #440]          /* pc: back to the caller */
    add x9, x0, #464             /* __reserved: d8-d15 */
    stp d8, d9, [x9, #0]
    stp d10, d11, [x9, #16]
    stp d12, d13, [x9, #32]
    stp d14, d15, [x9, #48]
    mov w0, #0
    ret

    .p2align 2
    .globl _lnx_setcontext
_lnx_setcontext:
    mov x9, x0
    add x10, x9, #464            /* __reserved: d8-d15 */
    ldp d8, d9, [x10, #0]
    ldp d10, d11, [x10, #16]
    ldp d12, d13, [x10, #32]
    ldp d14, d15, [x10, #48]
    ldp x19, x20, [x9, #184 + 19 * 8]
    ldp x21, x22, [x9, #184 + 21 * 8]
    ldp x23, x24, [x9, #184 + 23 * 8]
    ldp x25, x26, [x9, #184 + 25 * 8]
    ldp x27, x28, [x9, #184 + 27 * 8]
    ldp x29, x30, [x9, #184 + 29 * 8]
    ldr x10, [x9, #432]
    mov sp, x10
    ldr x16, [x9, #440]
    ldp x0, x1, [x9, #184]
    ldp x2, x3, [x9, #184 + 16]
    ldp x4, x5, [x9, #184 + 32]
    ldp x6, x7, [x9, #184 + 48]
    br x16

/* makecontext(ucp, func, argc, a0, a1, a2, a3, a4): up to five arguments, the ones Linux passes in registers.
 * The context starts func on uc_stack with lr at context_return, which resumes uc_link (in x19) or exits. */
    .p2align 2
    .globl _lnx_makecontext
_lnx_makecontext:
    ldr x9, [x0, #16]            /* uc_stack.ss_sp */
    ldr x10, [x0, #32]           /* uc_stack.ss_size */
    add x9, x9, x10
    and x9, x9, #~15
    str x9, [x0, #432]           /* sp */
    str x1, [x0, #440]           /* pc */
    adrp x10, context_return@PAGE
    add x10, x10, context_return@PAGEOFF
    str x10, [x0, #184 + 30 * 8] /* lr */
    str xzr, [x0, #184 + 29 * 8] /* fp: the end of the frame chain */
    ldr x10, [x0, #8]            /* uc_link */
    str x10, [x0, #184 + 19 * 8]
    stp x3, x4, [x0, #184]
    stp x5, x6, [x0, #184 + 16]
    str x7, [x0, #184 + 32]
    ret

    .p2align 2
context_return:
    cbz x19, 1f
    mov x0, x19
    b _lnx_setcontext
1:  mov w0, #0
    bl __exit                    /* _exit */

/* ------------------------------------------------------------------------------------------------------ */

/* VENTRY name, core, ngp: a variadic function with ngp named integer arguments, calling
 * core(named arguments..., struct lnx_va_list *) and returning what it returns */
.macro VENTRY name, core, ngp
    .p2align 2
    .globl _\name
_\name:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #224
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp q0, q1, [sp, #64]
    stp q2, q3, [sp, #96]
    stp q4, q5, [sp, #128]
    stp q6, q7, [sp, #160]
    add x9, x29, #16             /* __stack: the caller's stacked arguments */
    str x9, [sp, #192]
    add x9, sp, #64              /* __gr_top: the end of the saved x0-x7 */
    str x9, [sp, #200]
    add x9, sp, #192             /* __vr_top: the end of the saved q0-q7 */
    str x9, [sp, #208]
    mov w9, #(\ngp * 8 - 64)     /* __gr_offs: the first unnamed register */
    str w9, [sp, #216]
    mov w9, #-128                /* __vr_offs: no named floating-point arguments */
    str w9, [sp, #220]
    add x\ngp, sp, #192
    bl _\core
    mov sp, x29
    ldp x29, x30, [sp], #16
    ret
.endm

    VENTRY lnx_printf, lnx_vprintf_core, 1
    VENTRY lnx_fprintf, lnx_vfprintf, 2
    VENTRY lnx_sprintf, lnx_vsprintf_core, 2
    VENTRY lnx_snprintf, lnx_vsnprintf, 3
    VENTRY lnx_sscanf, lnx_vsscanf, 2
    VENTRY lnx_swprintf, lnx_vswprintf, 3
    VENTRY lnx_syslog, lnx_vsyslog, 2
