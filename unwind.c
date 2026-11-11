/* Linux module host: the C++ unwinder (libgcc_s's _Unwind_* functions), for exceptions thrown in Linux libraries.
 *
 * A Linux library carries its own C++ runtime (__cxa_throw, the personality routine) and imports only the unwinder.
 * Unwinding walks the Linux frames from the thrower up, with the DWARF call frame information in each library's
 * .eh_frame (found through its .eh_frame_hdr), in two phases as the Itanium C++ ABI describes: a search for a frame
 * whose personality routine handles the exception, then a walk to it running cleanups, ending with a jump into the
 * landing pad with the frame's registers restored. A frame no library describes (the host's own, or Windows code)
 * ends the walk: the exception is not caught, and the C++ runtime calls std::terminate as it would on Linux. */
#include <windows.h>
#include <stdint.h>
#include <string.h>

#include "elfload.h"
#include "libc.h"
#include "process.h"

/* DWARF's x86_64 register numbers: rax rdx rcx rbx rsi rdi rbp rsp r8-r15, and 16 the return address */
#define NREGS 17
#define REG_SP 7
#define REG_RA 16

enum { URC_NO_REASON = 0, URC_FOREIGN_EXCEPTION_CAUGHT = 1, URC_FATAL_PHASE2_ERROR = 2, URC_FATAL_PHASE1_ERROR = 3,
       URC_END_OF_STACK = 5, URC_HANDLER_FOUND = 6, URC_INSTALL_CONTEXT = 7, URC_CONTINUE_UNWIND = 8 };
enum { UA_SEARCH_PHASE = 1, UA_CLEANUP_PHASE = 2, UA_HANDLER_FRAME = 4, UA_END_OF_STACK = 16 };

struct uw_exception
{
    uint64_t exception_class;
    void (SYSV *cleanup)( int reason, struct uw_exception *exc );
    uint64_t private_1, private_2;  /* the stop function of a forced unwind; the handler frame's CFA */
};

struct uw_context
{
    uint64_t reg[NREGS];
    uint64_t cfa;
    uint64_t func_start;
    const uint8_t *lsda;
    uint64_t personality;
};

typedef int (SYSV *personality_fn)( int version, int actions, uint64_t exception_class, struct uw_exception *exc,
                                    struct uw_context *context );

/* ---- DWARF encodings ---- */

enum { PE_ABSPTR = 0, PE_ULEB128 = 1, PE_UDATA2 = 2, PE_UDATA4 = 3, PE_UDATA8 = 4, PE_SLEB128 = 9, PE_SDATA2 = 0xa,
       PE_SDATA4 = 0xb, PE_SDATA8 = 0xc, PE_PCREL = 0x10, PE_DATAREL = 0x30, PE_INDIRECT = 0x80, PE_OMIT = 0xff };

static uint64_t read_uleb( const uint8_t **p )
{
    uint64_t value = 0;
    unsigned int shift = 0;
    uint8_t byte;

    do
    {
        byte = *(*p)++;
        if (shift < 64) value |= (uint64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    return value;
}

static int64_t read_sleb( const uint8_t **p )
{
    int64_t value = 0;
    unsigned int shift = 0;
    uint8_t byte;

    do
    {
        byte = *(*p)++;
        if (shift < 64) value |= (int64_t)(byte & 0x7f) << shift;
        shift += 7;
    } while (byte & 0x80);
    if (shift < 64 && (byte & 0x40)) value |= -((int64_t)1 << shift);
    return value;
}

/* an encoded pointer; datarel is relative to base (the .eh_frame_hdr) */
static uint64_t read_encoded( const uint8_t **p, uint8_t encoding, uint64_t base )
{
    const uint8_t *start = *p;
    uint64_t value;

    if (encoding == PE_OMIT) return 0;
    switch (encoding & 0x0f)
    {
    case PE_ABSPTR: case PE_UDATA8: case PE_SDATA8: memcpy( &value, *p, 8 ); *p += 8; break;
    case PE_ULEB128: value = read_uleb( p ); break;
    case PE_SLEB128: value = read_sleb( p ); break;
    case PE_UDATA2: { uint16_t v; memcpy( &v, *p, 2 ); *p += 2; value = v; break; }
    case PE_SDATA2: { int16_t v; memcpy( &v, *p, 2 ); *p += 2; value = v; break; }
    case PE_UDATA4: { uint32_t v; memcpy( &v, *p, 4 ); *p += 4; value = v; break; }
    case PE_SDATA4: { int32_t v; memcpy( &v, *p, 4 ); *p += 4; value = v; break; }
    default: return 0;
    }
    if (!value) return 0;
    switch (encoding & 0x70)
    {
    case PE_PCREL: value += (uint64_t)start; break;
    case PE_DATAREL: value += base; break;
    }
    if (encoding & PE_INDIRECT) value = *(const uint64_t *)value;
    return value;
}

/* ---- call frame information ---- */

struct cie
{
    const uint8_t *instructions, *end;
    uint64_t code_align;
    int64_t data_align;
    unsigned int ra_reg;
    uint8_t fde_encoding, lsda_encoding;
    uint64_t personality;
    int has_augmentation_data;
};

struct fde
{
    struct cie cie;
    uint64_t pc_begin, pc_end;
    const uint8_t *lsda;
    const uint8_t *instructions, *end;
};

/* the record at p: its contents and the end; NULL at the terminator */
static const uint8_t *record( const uint8_t *p, const uint8_t **end )
{
    uint64_t length = *(const uint32_t *)p;

    p += 4;
    if (!length) return NULL;
    if (length == 0xffffffff)
    {
        length = *(const uint64_t *)p;
        p += 8;
    }
    *end = p + length;
    return p;
}

static int parse_cie( const uint8_t *p, struct cie *cie )
{
    const uint8_t *end, *aug;
    uint8_t version;

    memset( cie, 0, sizeof(*cie) );
    cie->fde_encoding = PE_ABSPTR;
    cie->lsda_encoding = PE_OMIT;
    if (!(p = record( p, &end )) || *(const uint32_t *)p) return 0;  /* a CIE's id is 0 in .eh_frame */
    p += 4;
    version = *p++;
    aug = p;
    p += strlen( (const char *)p ) + 1;
    if (aug[0] == 'e' && aug[1] == 'h') p += 8;
    cie->code_align = read_uleb( &p );
    cie->data_align = read_sleb( &p );
    cie->ra_reg = version == 1 ? *p++ : (unsigned int)read_uleb( &p );
    if (*aug == 'z')
    {
        uint64_t len = read_uleb( &p );
        const uint8_t *data_end = p + len;

        cie->has_augmentation_data = 1;
        for (aug++; *aug; aug++)
        {
            switch (*aug)
            {
            case 'L': cie->lsda_encoding = *p++; break;
            case 'R': cie->fde_encoding = *p++; break;
            case 'P':
            {
                uint8_t encoding = *p++;

                cie->personality = read_encoded( &p, encoding, 0 );
                break;
            }
            default: break;  /* 'S' (a signal frame), 'B': no data */
            }
        }
        p = data_end;
    }
    cie->instructions = p;
    cie->end = end;
    return 1;
}

static int parse_fde( const uint8_t *p, struct fde *fde )
{
    const uint8_t *end, *id_field;
    uint32_t cie_offset;

    if (!(p = record( p, &end ))) return 0;
    id_field = p;
    cie_offset = *(const uint32_t *)p;
    if (!cie_offset) return 0;  /* a CIE */
    p += 4;
    if (!parse_cie( id_field - cie_offset, &fde->cie )) return 0;
    fde->pc_begin = read_encoded( &p, fde->cie.fde_encoding, 0 );
    fde->pc_end = fde->pc_begin + read_encoded( &p, fde->cie.fde_encoding & 0x0f, 0 );
    fde->lsda = NULL;
    if (fde->cie.has_augmentation_data)
    {
        uint64_t len = read_uleb( &p );
        const uint8_t *data = p;

        if (fde->cie.lsda_encoding != PE_OMIT)
            fde->lsda = (const uint8_t *)read_encoded( &data, fde->cie.lsda_encoding, 0 );
        p += len;
    }
    fde->instructions = p;
    fde->end = end;
    return 1;
}

/* the FDE covering pc, through the binary search table of the .eh_frame_hdr of the library holding it */
static int find_fde( uint64_t pc, struct fde *fde )
{
    struct elf_module *module = elf_find_address( (const void *)pc );
    const uint8_t *hdr, *p, *eh_frame;
    uint64_t count;

    if (!module || !(hdr = elf_eh_frame_hdr( module )) || hdr[0] != 1) return 0;
    p = hdr + 4;
    eh_frame = (const uint8_t *)read_encoded( &p, hdr[1], (uint64_t)hdr );
    count = read_encoded( &p, hdr[2], (uint64_t)hdr );
    if (hdr[3] == (PE_DATAREL | PE_SDATA4) && count)
    {
        const int32_t *table = (const int32_t *)p;
        uint64_t low = 0, high = count;

        while (high - low > 1)
        {
            uint64_t mid = (low + high) / 2;

            if ((uint64_t)hdr + table[mid * 2] <= pc) low = mid;
            else high = mid;
        }
        if (!parse_fde( hdr + table[low * 2 + 1], fde )) return 0;
        return pc >= fde->pc_begin && pc < fde->pc_end;
    }
    /* no table: every record in .eh_frame */
    for (p = eh_frame; p; )
    {
        const uint8_t *end;

        if (!record( p, &end )) break;
        if (parse_fde( p, fde ) && pc >= fde->pc_begin && pc < fde->pc_end) return 1;
        p = end;
    }
    return 0;
}

/* ---- the rows of the CFA table ---- */

enum rule { RULE_SAME, RULE_OFFSET, RULE_VAL_OFFSET, RULE_REGISTER, RULE_EXPRESSION, RULE_VAL_EXPRESSION,
            RULE_UNDEFINED };

struct row
{
    unsigned int cfa_reg;
    int64_t cfa_offset;
    const uint8_t *cfa_expression;
    struct { enum rule rule; int64_t value; const uint8_t *expression; } reg[NREGS];
};

/* a DWARF expression, as CFI uses them (the PLT's CFA, mostly) */
static uint64_t evaluate( const uint8_t *p, const struct uw_context *context, uint64_t initial, int push_initial )
{
    uint64_t stack[64];
    int sp = 0;
    uint64_t len = read_uleb( &p );
    const uint8_t *end = p + len;

    if (push_initial) stack[sp++] = initial;
    while (p < end && sp < 63)
    {
        uint8_t op = *p++;
        uint64_t a, b;

        if (op >= 0x30 && op <= 0x4f) { stack[sp++] = op - 0x30; continue; }  /* DW_OP_lit<n> */
        if (op >= 0x50 && op <= 0x6f) { stack[sp++] = op - 0x50 < NREGS ? context->reg[op - 0x50] : 0; continue; }
        if (op >= 0x70 && op <= 0x8f)  /* DW_OP_breg<n> */
        {
            int64_t offset = read_sleb( &p );

            stack[sp++] = (op - 0x70 < NREGS ? context->reg[op - 0x70] : 0) + offset;
            continue;
        }
        switch (op)
        {
        case 0x03: memcpy( &a, p, 8 ); p += 8; stack[sp++] = a; break;                    /* addr */
        case 0x06: if (sp) stack[sp - 1] = *(const uint64_t *)stack[sp - 1]; break;       /* deref */
        case 0x08: stack[sp++] = *p++; break;                                              /* const1u */
        case 0x09: stack[sp++] = (int8_t)*p++; break;                                      /* const1s */
        case 0x0a: stack[sp++] = *(const uint16_t *)p; p += 2; break;                      /* const2u */
        case 0x0b: stack[sp++] = *(const int16_t *)p; p += 2; break;                       /* const2s */
        case 0x0c: stack[sp++] = *(const uint32_t *)p; p += 4; break;                      /* const4u */
        case 0x0d: stack[sp++] = *(const int32_t *)p; p += 4; break;                       /* const4s */
        case 0x0e: case 0x0f: memcpy( &a, p, 8 ); p += 8; stack[sp++] = a; break;          /* const8 */
        case 0x10: stack[sp++] = read_uleb( &p ); break;                                   /* constu */
        case 0x11: stack[sp++] = read_sleb( &p ); break;                                   /* consts */
        case 0x12: if (sp) { stack[sp] = stack[sp - 1]; sp++; } break;                     /* dup */
        case 0x13: if (sp) sp--; break;                                                    /* drop */
        case 0x14: if (sp > 1) { stack[sp] = stack[sp - 2]; sp++; } break;                 /* over */
        case 0x16: if (sp > 1) { a = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = a; } break;
        case 0x23: if (sp) stack[sp - 1] += read_uleb( &p ); break;                        /* plus_uconst */
        case 0x92: { uint64_t r = read_uleb( &p ); int64_t o = read_sleb( &p );           /* bregx */
                     stack[sp++] = (r < NREGS ? context->reg[r] : 0) + o; break; }
        case 0x96: break;                                                                  /* nop */
        default:
            if (sp < 2) return 0;
            b = stack[--sp];
            a = stack[sp - 1];
            switch (op)
            {
            case 0x1a: a &= b; break;
            case 0x1b: a = b ? (uint64_t)((int64_t)a / (int64_t)b) : 0; break;
            case 0x1c: a -= b; break;
            case 0x1d: a = b ? a % b : 0; break;
            case 0x1e: a *= b; break;
            case 0x21: a |= b; break;
            case 0x22: a += b; break;
            case 0x24: a <<= b; break;
            case 0x25: a >>= b; break;
            case 0x26: a = (uint64_t)((int64_t)a >> b); break;
            case 0x27: a ^= b; break;
            case 0x29: a = a == b; break;
            case 0x2a: a = (int64_t)a >= (int64_t)b; break;
            case 0x2b: a = (int64_t)a > (int64_t)b; break;
            case 0x2c: a = (int64_t)a <= (int64_t)b; break;
            case 0x2d: a = (int64_t)a < (int64_t)b; break;
            case 0x2e: a = a != b; break;
            default:
                linuxhost_trace( "unwind: DWARF expression op %#x not handled\n", op );
                return 0;
            }
            stack[sp - 1] = a;
            break;
        }
    }
    return sp ? stack[sp - 1] : 0;
}

/* runs CFA instructions until the row covering pc (or to the end when pc is ~0) */
static void run_cfi( const uint8_t *p, const uint8_t *end, const struct fde *fde, uint64_t pc, struct row *row,
                     const struct row *initial )
{
    struct row saved[8];
    unsigned int depth = 0;
    uint64_t loc = fde->pc_begin;
    const struct cie *cie = &fde->cie;

    while (p < end)
    {
        uint8_t op = *p++;
        uint64_t reg, delta = 0;

        switch (op >> 6)
        {
        case 1: delta = (op & 0x3f) * cie->code_align; goto advance;
        case 2:
            reg = op & 0x3f;
            if (reg < NREGS) { row->reg[reg].rule = RULE_OFFSET; row->reg[reg].value = read_uleb( &p ) * cie->data_align; }
            else read_uleb( &p );
            continue;
        case 3:
            reg = op & 0x3f;
            if (reg < NREGS && initial) row->reg[reg] = initial->reg[reg];
            continue;
        }
        switch (op)
        {
        case 0x00: break;
        case 0x01: loc = read_encoded( &p, cie->fde_encoding, 0 ); if (loc > pc) return; break;
        case 0x02: delta = *p++ * cie->code_align; goto advance;
        case 0x03: delta = *(const uint16_t *)p * cie->code_align; p += 2; goto advance;
        case 0x04: delta = *(const uint32_t *)p * cie->code_align; p += 4; goto advance;
        case 0x05:
            reg = read_uleb( &p );
            delta = read_uleb( &p );
            if (reg < NREGS) { row->reg[reg].rule = RULE_OFFSET; row->reg[reg].value = delta * cie->data_align; }
            break;
        case 0x06: reg = read_uleb( &p ); if (reg < NREGS && initial) row->reg[reg] = initial->reg[reg]; break;
        case 0x07: reg = read_uleb( &p ); if (reg < NREGS) row->reg[reg].rule = RULE_UNDEFINED; break;
        case 0x08: reg = read_uleb( &p ); if (reg < NREGS) row->reg[reg].rule = RULE_SAME; break;
        case 0x09:
            reg = read_uleb( &p );
            delta = read_uleb( &p );
            if (reg < NREGS) { row->reg[reg].rule = RULE_REGISTER; row->reg[reg].value = delta; }
            break;
        case 0x0a: if (depth < ARRAY_SIZE(saved)) saved[depth++] = *row; break;
        case 0x0b:
            if (depth)
            {
                /* the CFA is not part of the remembered state */
                unsigned int cfa_reg = row->cfa_reg;
                int64_t cfa_offset = row->cfa_offset;
                const uint8_t *cfa_expression = row->cfa_expression;

                *row = saved[--depth];
                row->cfa_reg = cfa_reg;
                row->cfa_offset = cfa_offset;
                row->cfa_expression = cfa_expression;
            }
            break;
        case 0x0c: row->cfa_reg = read_uleb( &p ); row->cfa_offset = read_uleb( &p ); row->cfa_expression = NULL; break;
        case 0x0d: row->cfa_reg = read_uleb( &p ); row->cfa_expression = NULL; break;
        case 0x0e: row->cfa_offset = read_uleb( &p ); break;
        case 0x0f:
        {
            uint64_t n;

            row->cfa_expression = p;  /* the expression's length, then its bytes */
            n = read_uleb( &p );
            p += n;
            break;
        }
        case 0x10: case 0x16:
        {
            const uint8_t *expression;
            uint64_t n;

            reg = read_uleb( &p );
            expression = p;
            n = read_uleb( &p );
            p += n;
            if (reg < NREGS)
            {
                row->reg[reg].rule = op == 0x10 ? RULE_EXPRESSION : RULE_VAL_EXPRESSION;
                row->reg[reg].expression = expression;
            }
            break;
        }
        case 0x11: case 0x15:
            reg = read_uleb( &p );
            {
                int64_t offset = read_sleb( &p ) * cie->data_align;

                if (reg < NREGS)
                {
                    row->reg[reg].rule = op == 0x11 ? RULE_OFFSET : RULE_VAL_OFFSET;
                    row->reg[reg].value = offset;
                }
            }
            break;
        case 0x12: row->cfa_reg = read_uleb( &p ); row->cfa_offset = read_sleb( &p ) * cie->data_align;
                   row->cfa_expression = NULL; break;
        case 0x13: row->cfa_offset = read_sleb( &p ) * cie->data_align; break;
        case 0x14:
            reg = read_uleb( &p );
            delta = read_uleb( &p );
            if (reg < NREGS) { row->reg[reg].rule = RULE_VAL_OFFSET; row->reg[reg].value = delta * cie->data_align; }
            break;
        case 0x2e: read_uleb( &p ); break;  /* GNU_args_size */
        case 0x2f:
            reg = read_uleb( &p );
            delta = read_uleb( &p );
            if (reg < NREGS) { row->reg[reg].rule = RULE_OFFSET; row->reg[reg].value = -(int64_t)(delta * cie->data_align); }
            break;
        default:
            linuxhost_trace( "unwind: CFA instruction %#x not handled\n", op );
            return;
        }
        continue;
advance:
        if (loc + delta > pc) return;
        loc += delta;
    }
}

/* the frame at context->reg[REG_RA]: its FDE, CFA and the personality data; 0 if no library describes it */
static int frame_info( struct uw_context *context, struct row *row )
{
    struct fde fde;
    struct row initial;
    uint64_t pc = context->reg[REG_RA] - 1;  /* a return address: the call is before it */

    if (!context->reg[REG_RA] || !find_fde( pc, &fde )) return 0;
    memset( row, 0, sizeof(*row) );
    run_cfi( fde.cie.instructions, fde.cie.end, &fde, ~0ull, row, NULL );
    initial = *row;
    run_cfi( fde.instructions, fde.end, &fde, pc, row, &initial );
    if (fde.cie.ra_reg != REG_RA) return 0;  /* always 16 on x86_64 */
    context->cfa = row->cfa_expression ? evaluate( row->cfa_expression, context, 0, 0 )
                                       : context->reg[row->cfa_reg < NREGS ? row->cfa_reg : REG_SP] + row->cfa_offset;
    context->func_start = fde.pc_begin;
    context->lsda = fde.lsda;
    context->personality = fde.cie.personality;
    return 1;
}

/* to the caller of the frame frame_info described */
static int step( struct uw_context *context, const struct row *row )
{
    uint64_t old[NREGS], cfa = context->cfa;
    unsigned int i;

    memcpy( old, context->reg, sizeof(old) );
    for (i = 0; i < NREGS; i++)
    {
        switch (row->reg[i].rule)
        {
        case RULE_SAME: break;
        case RULE_UNDEFINED: if (i == REG_RA) context->reg[i] = 0; break;
        case RULE_OFFSET: context->reg[i] = *(const uint64_t *)(cfa + row->reg[i].value); break;
        case RULE_VAL_OFFSET: context->reg[i] = cfa + row->reg[i].value; break;
        case RULE_REGISTER: context->reg[i] = row->reg[i].value < NREGS ? old[row->reg[i].value] : 0; break;
        case RULE_EXPRESSION:
        {
            struct uw_context tmp = *context;

            memcpy( tmp.reg, old, sizeof(old) );
            context->reg[i] = *(const uint64_t *)evaluate( row->reg[i].expression, &tmp, cfa, 1 );
            break;
        }
        case RULE_VAL_EXPRESSION:
        {
            struct uw_context tmp = *context;

            memcpy( tmp.reg, old, sizeof(old) );
            context->reg[i] = evaluate( row->reg[i].expression, &tmp, cfa, 1 );
            break;
        }
        }
    }
    if (row->reg[REG_SP].rule == RULE_SAME) context->reg[REG_SP] = cfa;
    return context->reg[REG_RA] != 0;
}

/* ---- the two phases ---- */

/* restores a frame's registers and continues at its landing pad: rdi holds the context */
__asm__( ".text\n"
         "uw_install:\n"
         "    movq 0x18(%rdi), %rbx\n"
         "    movq 0x30(%rdi), %rbp\n"
         "    movq 0x60(%rdi), %r12\n"
         "    movq 0x68(%rdi), %r13\n"
         "    movq 0x70(%rdi), %r14\n"
         "    movq 0x78(%rdi), %r15\n"
         "    movq 0x00(%rdi), %rax\n"
         "    movq 0x08(%rdi), %rdx\n"
         "    movq 0x10(%rdi), %rcx\n"
         "    movq 0x20(%rdi), %rsi\n"
         "    movq 0x38(%rdi), %rsp\n"
         "    pushq 0x80(%rdi)\n"
         "    movq 0x28(%rdi), %rdi\n"
         "    ret\n" );
SYSV void __attribute__((noreturn)) uw_install( struct uw_context *context );

static int phase2( struct uw_exception *exc, struct uw_context *context )
{
    struct row row;

    for (;;)
    {
        int actions = UA_CLEANUP_PHASE, ret;

        if (!frame_info( context, &row )) return URC_FATAL_PHASE2_ERROR;
        if (context->cfa == exc->private_2) actions |= UA_HANDLER_FRAME;
        if (context->personality)
        {
            ret = ((personality_fn)context->personality)( 1, actions, exc->exception_class, exc, context );
            if (ret == URC_INSTALL_CONTEXT)
            {
                linuxhost_trace( "unwind: landing pad %#llx\n", (unsigned long long)context->reg[REG_RA] );
                uw_install( context );
            }
            if (ret != URC_CONTINUE_UNWIND) return URC_FATAL_PHASE2_ERROR;
        }
        if (actions & UA_HANDLER_FRAME) return URC_FATAL_PHASE2_ERROR;  /* the handler's frame did not take it */
        if (!step( context, &row )) return URC_FATAL_PHASE2_ERROR;
    }
}

/* the register state at the caller of an entry point below: regs[] as DWARF numbers them, at its return address */
static void init_context( struct uw_context *context, const uint64_t *regs )
{
    memset( context, 0, sizeof(*context) );
    memcpy( context->reg, regs, sizeof(context->reg) );
}

SYSV int uw_raise( struct uw_exception *exc, void *unused, const uint64_t *regs )
{
    struct uw_context context, search;
    struct row row;

    init_context( &context, regs );
    search = context;
    linuxhost_trace( "unwind: exception %#llx thrown at %#llx\n", (unsigned long long)exc->exception_class,
                     (unsigned long long)regs[REG_RA] );
    for (;;)  /* phase 1: find the handler */
    {
        int ret;

        if (!frame_info( &search, &row ))
        {
            linuxhost_trace( "unwind: nothing catches it\n" );
            return URC_END_OF_STACK;
        }
        if (search.personality)
        {
            ret = ((personality_fn)search.personality)( 1, UA_SEARCH_PHASE, exc->exception_class, exc, &search );
            if (ret == URC_HANDLER_FOUND) break;
            if (ret != URC_CONTINUE_UNWIND) return URC_FATAL_PHASE1_ERROR;
        }
        if (!step( &search, &row ))
        {
            linuxhost_trace( "unwind: nothing catches it\n" );
            return URC_END_OF_STACK;
        }
    }
    exc->private_1 = 0;
    exc->private_2 = search.cfa;
    return phase2( exc, &context );
}

SYSV int uw_resume( struct uw_exception *exc, void *unused, const uint64_t *regs )
{
    struct uw_context context;
    int ret;

    init_context( &context, regs );
    ret = phase2( exc, &context );
    linuxhost_trace( "unwind: _Unwind_Resume failed (%d)\n", ret );
    ExitProcess( 71 );
}

SYSV int uw_resume_or_rethrow( struct uw_exception *exc, void *unused, const uint64_t *regs )
{
    if (exc->private_1) return uw_resume( exc, unused, regs );  /* a forced unwind continues */
    return uw_raise( exc, unused, regs );
}

typedef int (SYSV *trace_fn)( struct uw_context *context, void *arg );

SYSV int uw_backtrace( trace_fn callback, void *arg, const uint64_t *regs )
{
    struct uw_context context;
    struct row row;

    init_context( &context, regs );
    for (;;)
    {
        if (!frame_info( &context, &row )) return URC_END_OF_STACK;
        if (callback( &context, arg ) != URC_NO_REASON) return URC_FATAL_PHASE1_ERROR;
        if (!step( &context, &row )) return URC_END_OF_STACK;
    }
}

/* An entry point takes the caller's callee-saved registers, stack pointer and return address (regs, DWARF-numbered,
 * on its stack) and passes them to the C function as its third argument. */
#define CAPTURE_ENTRY(name, function) \
    __asm__( ".text\n" \
             #name ":\n" \
             "    subq $0x98, %rsp\n" \
             "    movq %rbx, 0x18(%rsp)\n" \
             "    movq %rbp, 0x30(%rsp)\n" \
             "    movq %r12, 0x60(%rsp)\n" \
             "    movq %r13, 0x68(%rsp)\n" \
             "    movq %r14, 0x70(%rsp)\n" \
             "    movq %r15, 0x78(%rsp)\n" \
             "    leaq 0xa0(%rsp), %rax\n" \
             "    movq %rax, 0x38(%rsp)\n" \
             "    movq 0x98(%rsp), %rax\n" \
             "    movq %rax, 0x80(%rsp)\n" \
             "    movq %rsp, %rdx\n" \
             "    call " #function "\n" \
             "    addq $0x98, %rsp\n" \
             "    ret\n" ); \
    extern void name(void)

CAPTURE_ENTRY( lx_Unwind_RaiseException, uw_raise );
CAPTURE_ENTRY( lx_Unwind_Resume, uw_resume );
CAPTURE_ENTRY( lx_Unwind_Resume_or_Rethrow, uw_resume_or_rethrow );
CAPTURE_ENTRY( lx_Unwind_Backtrace, uw_backtrace );

/* ---- what the personality routine asks of a frame ---- */

static SYSV void lx_Unwind_DeleteException( struct uw_exception *exc )
{
    if (exc->cleanup) exc->cleanup( URC_FOREIGN_EXCEPTION_CAUGHT, exc );
}

static SYSV uint64_t lx_Unwind_GetGR( struct uw_context *context, int reg ) { return reg < NREGS ? context->reg[reg] : 0; }
static SYSV void lx_Unwind_SetGR( struct uw_context *context, int reg, uint64_t value )
{
    if (reg < NREGS) context->reg[reg] = value;
}
static SYSV uint64_t lx_Unwind_GetIP( struct uw_context *context ) { return context->reg[REG_RA]; }
static SYSV uint64_t lx_Unwind_GetIPInfo( struct uw_context *context, int *before_insn )
{
    *before_insn = 0;
    return context->reg[REG_RA];
}
static SYSV void lx_Unwind_SetIP( struct uw_context *context, uint64_t ip ) { context->reg[REG_RA] = ip; }
static SYSV uint64_t lx_Unwind_GetCFA( struct uw_context *context ) { return context->cfa; }
static SYSV const uint8_t *lx_Unwind_GetLanguageSpecificData( struct uw_context *context ) { return context->lsda; }
static SYSV uint64_t lx_Unwind_GetRegionStart( struct uw_context *context ) { return context->func_start; }
static SYSV uint64_t lx_Unwind_GetDataRelBase( struct uw_context *context ) { return 0; }  /* unused on x86_64 */
static SYSV uint64_t lx_Unwind_GetTextRelBase( struct uw_context *context ) { return 0; }

static SYSV void *lx_Unwind_FindEnclosingFunction( void *pc )
{
    struct fde fde;

    return find_fde( (uint64_t)pc, &fde ) ? (void *)fde.pc_begin : NULL;
}

const struct libc_function linuxhost_unwind_functions[] =
{
    { "_Unwind_RaiseException", lx_Unwind_RaiseException }, { "_Unwind_Resume", lx_Unwind_Resume },
    { "_Unwind_Resume_or_Rethrow", lx_Unwind_Resume_or_Rethrow }, { "_Unwind_Backtrace", lx_Unwind_Backtrace },
    { "_Unwind_DeleteException", lx_Unwind_DeleteException }, { "_Unwind_GetGR", lx_Unwind_GetGR },
    { "_Unwind_SetGR", lx_Unwind_SetGR }, { "_Unwind_GetIP", lx_Unwind_GetIP }, { "_Unwind_GetIPInfo", lx_Unwind_GetIPInfo },
    { "_Unwind_SetIP", lx_Unwind_SetIP }, { "_Unwind_GetCFA", lx_Unwind_GetCFA },
    { "_Unwind_GetLanguageSpecificData", lx_Unwind_GetLanguageSpecificData },
    { "_Unwind_GetRegionStart", lx_Unwind_GetRegionStart }, { "_Unwind_GetDataRelBase", lx_Unwind_GetDataRelBase },
    { "_Unwind_GetTextRelBase", lx_Unwind_GetTextRelBase },
    { "_Unwind_FindEnclosingFunction", lx_Unwind_FindEnclosingFunction },
    { NULL, NULL }
};
