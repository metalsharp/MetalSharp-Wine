/* Linux module host, native: entry points Linux code reaches through its relocations.
 *
 * elf_tlsdesc_resolver: a TLS descriptor's function. x0 points to the descriptor; it returns in x0 the variable's
 * offset from the thread pointer (tpidr_el0) and preserves every other register, as the AArch64 TLS descriptor
 * ABI requires. The variable's address comes from elf_tlsdesc_address; the offset is taken against tpidr_el0 as
 * it is now, which on macOS is the CPU number, not a pointer.
 *
 * elf_unresolved_entry: where the stubs for unresolved imports jump, with the stub's number in x16. */

    .text
    .p2align 2
    .globl _elf_tlsdesc_resolver
_elf_tlsdesc_resolver:
    stp x29, x30, [sp, #-16]!
    mov x29, sp
    sub sp, sp, #544
    stp x1, x2, [sp, #0]
    stp x3, x4, [sp, #16]
    stp x5, x6, [sp, #32]
    stp x7, x8, [sp, #48]
    stp x9, x10, [sp, #64]
    stp x11, x12, [sp, #80]
    stp x13, x14, [sp, #96]
    stp x15, x16, [sp, #112]
    stp x17, x18, [sp, #128]
    stp q0, q1, [sp, #160]
    stp q2, q3, [sp, #192]
    stp q4, q5, [sp, #224]
    stp q6, q7, [sp, #256]
    stp q16, q17, [sp, #288]
    stp q18, q19, [sp, #320]
    stp q20, q21, [sp, #352]
    stp q22, q23, [sp, #384]
    stp q24, q25, [sp, #416]
    stp q26, q27, [sp, #448]
    stp q28, q29, [sp, #480]
    stp q30, q31, [sp, #512]

    ldr x0, [x0, #8]
    bl _elf_tlsdesc_address
    mrs x1, tpidr_el0
    sub x0, x0, x1

    ldp q30, q31, [sp, #512]
    ldp q28, q29, [sp, #480]
    ldp q26, q27, [sp, #448]
    ldp q24, q25, [sp, #416]
    ldp q22, q23, [sp, #384]
    ldp q20, q21, [sp, #352]
    ldp q18, q19, [sp, #320]
    ldp q16, q17, [sp, #288]
    ldp q6, q7, [sp, #256]
    ldp q4, q5, [sp, #224]
    ldp q2, q3, [sp, #192]
    ldp q0, q1, [sp, #160]
    ldp x17, x18, [sp, #128]
    ldp x15, x16, [sp, #112]
    ldp x13, x14, [sp, #96]
    ldp x11, x12, [sp, #80]
    ldp x9, x10, [sp, #64]
    ldp x7, x8, [sp, #48]
    ldp x5, x6, [sp, #32]
    ldp x3, x4, [sp, #16]
    ldp x1, x2, [sp, #0]
    add sp, sp, #544
    ldp x29, x30, [sp], #16
    ret

    .p2align 2
    .globl _elf_unresolved_entry
_elf_unresolved_entry:
    /* reached by a call, so sp is 16-byte aligned; the handler does not return */
    mov x0, x16
    bl _elf_unresolved_called
    brk #0
