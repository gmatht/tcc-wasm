/*
 *  WebAssembly (wasm32) code generator for TCC
 *
 *  A minimal but complete backend that emits a self-contained wasm
 *  module.  Design notes:
 *
 *  - "Registers" are shadow-memory slots in the function frame
 *    (reg_ofs[r] = -(8*(r+1))).  Every value lives in memory, so the
 *    wasm operand stack is only used transiently inside a single
 *    operation.  This keeps the operand stack empty at control-flow
 *    points, which is what makes the state-machine codegen below
 *    valid wasm.
 *
 *  - Control flow is compiled to a pc-based state machine:  every
 *    block ends with `pc = L; br $dispatch`, conditional edges are
 *    `if (cond) pc = L end`, and a `br_table` at the top of the
 *    dispatch loop selects the next block.  Blocks are laid out in
 *    reverse creation order inside nested (block $L0 (block $L1 ...))
 *    so that br_table can target them (wasm can only branch to
 *    enclosing labels).  This handles arbitrary C control flow
 *    (goto included) without any structured-CFG analysis.
 *
 *  - Undefined functions become wasm imports (module "env", by name),
 *    so libc-ish helpers can be provided by the embedding runtime.
 *    fd_write/proc_exit are imported from wasi_snapshot_preview1.
 *
 *  - Struct returns use the standard sret pointer convention
 *    (gfunc_sret returns 0).  Varargs, VLAs and computed gotos are
 *    not supported yet and raise tcc_error.
 *
 *  - Output: a single wasm module written by wasm_output_file()
 *    (see tccelf.c).  `tcc -c foo.c -o foo.wasm`.
 *
 *  Known limitations: no function pointers (call_indirect), no
 *  varargs, no struct-by-value args, no long-double (mapped to
 *  double), 64-bit div/rem need __divdi3-style imports.
 *
 *  Copyright (c) 2024  the tinysh/j.cmd project
 *  Licensed under the GNU Lesser General Public License v2.1
 */

#ifdef TARGET_DEFS_ONLY

/* "registers" — NB_REGS shadow-memory slots in the frame */
#define NB_REGS 8

#define RC_INT   (1 << 0)
#define RC_FLOAT (1 << 1)
#define RC_R(x)  (1 << (2 + (x)))   /* x = 0..7 */
#define RC_F(x)  (1 << (10 + (x)))

#define RC_IRET  (RC_R(0))  /* int return slot */
#define RC_IRE2  (RC_R(1))  /* int 2nd return slot (long long high word) */
#define RC_FRET  (RC_F(1))  /* float return slot */

#define REG_IRET 0
#define REG_IRE2 1
#define REG_FRET 1

#define PTR_SIZE 4
#define LDOUBLE_SIZE 8
#define LDOUBLE_ALIGN 8
#define MAX_ALIGN 16

#define PROMOTE_RET

#else
#define USING_GLOBALS
#include "tcc.h"
#include <assert.h>

ST_DATA const char * const target_machine_defs =
    "__wasm\0"
    "__wasm32\0"
    "__wasm32__ 1\0"
    "__wasi__ 1\0"
    ;

/* "registers" are shadow-memory slots; every slot is usable for
   integers and floats alike (the class tags only matter to tccgen's
   register allocator bookkeeping) */
ST_DATA const int reg_classes[NB_REGS] = {
    RC_INT | RC_FLOAT | RC_R(0) | RC_F(0),
    RC_INT | RC_FLOAT | RC_R(1) | RC_F(1),
    RC_INT | RC_FLOAT | RC_R(2) | RC_F(2),
    RC_INT | RC_FLOAT | RC_R(3) | RC_F(3),
    RC_INT | RC_FLOAT | RC_R(4) | RC_F(4),
    RC_INT | RC_FLOAT | RC_R(5) | RC_F(5),
    RC_INT | RC_FLOAT | RC_R(6) | RC_F(6),
    RC_INT | RC_FLOAT | RC_R(7) | RC_F(7),
};

#if defined(CONFIG_TCC_BCHECK)
ST_DATA int func_bound_add_epilog;
#endif

/* ---------------------------------------------------------------- */
/* wasm opcodes */

#define W_UNREACHABLE 0x00
#define W_NOP         0x01
#define W_BLOCK       0x02
#define W_LOOP        0x03
#define W_IF          0x04
#define W_ELSE        0x05
#define W_END         0x0b
#define W_BR          0x0c
#define W_BR_IF       0x0d
#define W_BR_TABLE    0x0e
#define W_CALL        0x10
#define W_LOCAL_GET   0x20
#define W_LOCAL_SET   0x21
#define W_LOCAL_TEE   0x22
#define W_GLOBAL_GET  0x23
#define W_GLOBAL_SET  0x24
#define W_I32_LOAD    0x28
#define W_I64_LOAD    0x29
#define W_F32_LOAD    0x2a
#define W_F64_LOAD    0x2b
#define W_I32_LOAD8_S 0x2c
#define W_I32_LOAD8_U 0x2d
#define W_I32_LOAD16_S 0x2e
#define W_I32_LOAD16_U 0x2f
#define W_I64_LOAD8_S 0x30
#define W_I64_LOAD8_U 0x31
#define W_I64_LOAD16_S 0x32
#define W_I64_LOAD16_U 0x33
#define W_I64_LOAD32_S 0x34
#define W_I64_LOAD32_U 0x35
#define W_I32_STORE    0x36
#define W_I64_STORE    0x37
#define W_F32_STORE    0x38
#define W_F64_STORE    0x39
#define W_I32_STORE8   0x3a
#define W_I32_STORE16  0x3b
#define W_I64_STORE8   0x3c
#define W_I64_STORE16  0x3d
#define W_I64_STORE32  0x3e
#define W_I32_CONST    0x41
#define W_I64_CONST    0x42
#define W_F32_CONST    0x43
#define W_F64_CONST    0x44
#define W_I32_EQZ      0x45
#define W_I32_EQ       0x46
#define W_I32_NE       0x47
#define W_I32_LT_S     0x48
#define W_I32_LT_U     0x49
#define W_I32_GT_S     0x4a
#define W_I32_GT_U     0x4b
#define W_I32_LE_S     0x4c
#define W_I32_LE_U     0x4d
#define W_I32_GE_S     0x4e
#define W_I32_GE_U     0x4f
#define W_I64_EQ       0x51
#define W_I64_NE       0x52
#define W_I64_LT_S     0x53
#define W_I64_LT_U     0x54
#define W_I64_GT_S     0x55
#define W_I64_GT_U     0x56
#define W_I64_LE_S     0x57
#define W_I64_LE_U     0x58
#define W_I64_GE_S     0x59
#define W_I64_GE_U     0x5a
#define W_F32_EQ       0x5b
#define W_F32_NE       0x5c
#define W_F32_LT       0x5d
#define W_F32_GT       0x5e
#define W_F32_LE       0x5f
#define W_F32_GE       0x60
#define W_F64_EQ       0x61
#define W_F64_NE       0x62
#define W_F64_LT       0x63
#define W_F64_GT       0x64
#define W_F64_LE       0x65
#define W_F64_GE       0x66
#define W_I32_ADD      0x6a
#define W_I32_SUB      0x6b
#define W_I32_MUL      0x6c
#define W_I32_DIV_S    0x6d
#define W_I32_DIV_U    0x6e
#define W_I32_REM_S    0x6f
#define W_I32_REM_U    0x70
#define W_I32_AND      0x71
#define W_I32_OR       0x72
#define W_I32_XOR      0x73
#define W_I32_SHL      0x74
#define W_I32_SHR_S    0x75
#define W_I32_SHR_U    0x76
#define W_I64_ADD      0x7c
#define W_I64_SUB      0x7d
#define W_I64_MUL      0x7e
#define W_I64_DIV_S    0x7f
#define W_I64_DIV_U    0x80
#define W_I64_REM_S    0x81
#define W_I64_REM_U    0x82
#define W_I64_AND      0x83
#define W_I64_OR       0x84
#define W_I64_XOR      0x85
#define W_I64_SHL      0x86
#define W_I64_SHR_S    0x87
#define W_I64_SHR_U    0x88
#define W_F32_NEG      0x8c
#define W_F64_NEG      0x8d
#define W_F32_ADD      0x92
#define W_F32_SUB      0x93
#define W_F32_MUL      0x94
#define W_F32_DIV      0x95
#define W_F64_ADD      0xa0
#define W_F64_SUB      0xa1
#define W_F64_MUL      0xa2
#define W_F64_DIV      0xa3
#define W_I32_WRAP     0xa7
#define W_I32_TRUNC_F32_S 0xa8
#define W_I32_TRUNC_F32_U 0xa9
#define W_I32_TRUNC_F64_S 0xaa
#define W_CALL_INDIRECT 0x11
#define W_I32_TRUNC_F64_U 0xab
#define W_I64_EXTEND_S 0xac
#define W_I64_EXTEND_U 0xad
#define W_I64_TRUNC_F32_S 0xae
#define W_I64_TRUNC_F32_U 0xaf
#define W_I64_TRUNC_F64_S 0xb0
#define W_I64_TRUNC_F64_U 0xb1
#define W_F32_CONVERT_I32_S 0xb2
#define W_F32_CONVERT_I32_U 0xb3
#define W_F32_CONVERT_I64_S 0xb4
#define W_F32_CONVERT_I64_U 0xb5
#define W_F32_DEMOTE     0xb6
#define W_F64_CONVERT_I32_S 0xb7
#define W_F64_CONVERT_I32_U 0xb8
#define W_F64_CONVERT_I64_S 0xb9
#define W_F64_CONVERT_I64_U 0xba
#define W_F64_PROMOTE    0xbb
#define W_I32_EXTEND8_S  0xc0
#define W_I32_EXTEND16_S 0xc1

#define VAL_I32 0x7f
#define VAL_I64 0x7e
#define VAL_F32 0x7d
#define VAL_F64 0x7c
#define BLOCKTYPE_EMPTY 0x40

/* ---------------------------------------------------------------- */
/* module-level state (ONE_SOURCE build: shared across "TUs") */

typedef struct WasmSig {
    char *sig;           /* valtype bytes, NUL terminated; '\0' = void result */
    int nparams, nresults;
} WasmSig;

typedef struct WasmFuncRef {
    char *name;          /* symbol name */
    Sym *sym;            /* symbol pointer (for identity checks) */
    int sig;             /* signature index */
    int defined;         /* 1 = has a body */
    int order;           /* body emission order (defined funcs) */
    int import;          /* 1 = import (called but never defined) */
} WasmFuncRef;

typedef struct WasmPatch {
    int ofs;             /* offset in the function body */
    Sym *sym;            /* referenced symbol (NULL for frame-size) */
    char *name;          /* C function name, strdup'd at patch time
                            (get_tok_str is unsafe at output time) */
    int kind;            /* 0 = function index, 1 = data address,
                            2 = function value (table slot) */
    int dsec, dofs;      /* data symbol: section index + offset (kind 1),
                            captured at patch time (sym->c is not stable) */
    int sig;             /* call signature (kind 0; per-call for varargs) */
} WasmPatch;

typedef struct WasmImport {
    char *name;          /* C symbol name (emitted as env."$"+name) */
    int sig;
} WasmImport;

/* per-function state */
typedef struct WasmSeg { int start, end, edge; } WasmSeg;
typedef struct WasmEdge { int kind, op, label, chain_next, seg; } WasmEdge;
typedef struct WasmLabel { int pos; int sub; int seg; } WasmLabel;
typedef struct WasmBlk { int fs, ls; } WasmBlk;

typedef struct WasmFunc {
    char *name;
    int sig;
    int fidx;            /* final function index (imports first) */
    unsigned char *code; /* growable instruction buffer */
    int csize;
    int npatches, npatch_alloc;
    WasmPatch *patches;
    WasmSeg *segs;       int nsegs, seg_alloc;
    WasmEdge *edges;     int nedges, edge_alloc;
    WasmLabel *labels;   int nlabels, label_alloc;
    WasmBlk *blks;       int nblks, blk_alloc;
    int *inspos;         int nins, ins_alloc;
    int cur_seg, cur_blk;
    int *posblks;        int nposblks, posblk_alloc;  /* (pos,block) pairs */
    int nparams;         /* wasm params */
    int nlocals;         /* wasm locals (fp, pc) */
    int has_sret;
    int order;           /* body emission order */
    unsigned char *final; int flen;       /* laid-out body */
    int *sub_starts, *sub_ends; int nsubs_out;  /* position -> sub map
                                                   (for &&label values) */
    int *lblpos; int nlblpos, lblpos_alloc;    /* &&label positions in
                                                  this function (recorded
                                                  at gsym_addr(0, ind)) */
} WasmFunc;

static WasmSig *wasm_sigs;   static int wasm_nsigs, wasm_sig_alloc;
static WasmFuncRef *wasm_funcs; static int wasm_nfuncs, wasm_func_alloc;
static WasmImport *wasm_imports; static int wasm_nimports, wasm_import_alloc;
static void *wa_grow(void *p, int *alloc, int need, int esz);

/* function-value table: names of functions whose ADDRESS is taken
   (atexit(fn), fn-pointer compares), in element-section order — the
   value of a kind-2 patch is the slot in this list. */
static char **wasm_fv; static int wasm_nfv, wasm_fv_alloc;
/* constructor/destructor functions (__attribute((constructor/destructor))) */
static char **wasm_ctors; static int wasm_nctors, wasm_ctors_alloc;
static char **wasm_dtors; static int wasm_ndtors, wasm_dtors_alloc;
/* signatures used by call_indirect (function pointers) — must be in
   the type section even though no direct function has them */
static int *wasm_indir_sigs; static int wasm_nindir, wasm_indir_alloc;

static void w_indir_sig(int sig)
{
    int i;
    for (i = 0; i < wasm_nindir; i++)
        if (wasm_indir_sigs[i] == sig)
            return;
    wasm_indir_sigs = wa_grow(wasm_indir_sigs, &wasm_indir_alloc,
                              wasm_nindir + 1, sizeof(int));
    wasm_indir_sigs[wasm_nindir++] = sig;
}

/* a WEAK function reference (__attribute__((weak)) — the GOT() checks
   in 104_inline) resolves to 0/NULL: it has no table slot.  A STRONG
   extern (fprintf in 42) gets a slot. */
/* the effective symbol name: an __asm__("z7") renamed extern resolves
   under its asm name (129_scopes' extern struct xx7 y __asm__("z7")
   must reach the z7 definition) */
static const char *w_sym_name(Sym *sym)
{
    if (!sym)
        return NULL;
    if (sym->asm_label)
        return get_tok_str(sym->asm_label, NULL);
    return get_tok_str(sym->v, NULL);
}

static int w_sym_weak(Sym *sym)
{
    ElfSym *es;
    if (!sym)
        return 0;
    /* the weak attribute lives on the Sym (the GOT() refs never got a
       symtab entry — c == 0) and/or in the symtab binding */
    if (sym->a.weak)
        return 1;
    if (!sym->c)
        return 0;
    if (sym->c < 0 ||
        sym->c >= (int)(symtab_section->data_offset / sizeof(ElfSym)))
        return 0;
    es = elfsym(sym);
    if (!es)
        return 0;
    return ELFW(ST_BIND)(es->st_info) == STB_WEAK;
}

/* a weak reference WITHOUT a local definition resolves to 0/NULL (no
   table slot); a weak ref that HAS a strong definition in this module
   resolves to the function (104_inline's extern_* GOT() checks). */
static int w_sym_weak_undef(Sym *sym, const char *nm)
{
    int k;
    if (!w_sym_weak(sym) || !nm)
        return 0;
    for (k = 0; k < wasm_nfuncs; k++)
        if (wasm_funcs[k].defined && !strcmp(wasm_funcs[k].name, nm)) {
            /* a local (static) definition is a DIFFERENT symbol than
               the extern weak ref — no slot (104's static_func prints
               0); a global (extern) definition resolves the ref.  The
               syms are freed at output — the symtab (stable) decides */
            int i;
            for (i = 0; i < (int)(symtab_section->data_offset / sizeof(ElfSym)); i++) {
                ElfSym *es = &((ElfSym *)symtab_section->data)[i];
                char *en = &((char *)symtab_section->link->data)[es->st_name];
                if (es->st_shndx == text_section->sh_num &&
                    ELFW(ST_TYPE)(es->st_info) == STT_FUNC &&
                    !strcmp(en, nm))
                    return ELFW(ST_BIND)(es->st_info) == STB_LOCAL;
            }
            return 0;
        }
    return 1;
}

static int w_fv_find(const char *nm)
{
    int i;
    for (i = 0; i < wasm_nfv; i++)
        if (!strcmp(wasm_fv[i], nm))
            return i;
    return -1;
}

static int w_fv_slot(const char *nm)
{
    int i;
    for (i = 0; i < wasm_nfv; i++)
        if (!strcmp(wasm_fv[i], nm))
            return i;
    wasm_fv = wa_grow(wasm_fv, &wasm_fv_alloc, wasm_nfv + 1, sizeof(char *));
    wasm_fv[wasm_nfv] = tcc_strdup(nm);
    return wasm_nfv++;
}

static WasmFunc *wasm_cf;    /* current function */
static WasmFunc **wasm_func_list = NULL;
static int wasm_nfunc_list = 0, wasm_func_list_alloc = 0;
static int wasm_ndef;        /* defined funcs emitted so far */
static int w_park_cursor;    /* next free parking slot (frame words below the body) */
#define W_BLK_SPLITS 128   /* max label split positions per block (14 merged a
                                 20-case switch's extra handlers into the first
                                 sub — case N ran case N-1's body) */
static int w_last_cmp_r, w_last_cmp_a, w_last_cmp_b;  /* last deferred compare's operands */
static int wasm_nimp;        /* imports so far: 2 (fd_write, proc_exit) + n */
static int wasm_text_size;
static int wasm_data_size;

/* wasm value type for a C type */
static int w_typeof(CType *t)
{
    switch (t->t & VT_BTYPE) {
    case VT_FLOAT:  return VAL_F32;
    case VT_DOUBLE: return VAL_F64;
    case VT_LLONG:  return VAL_I64;
    default:        return VAL_I32;
    }
}

static int reg_ofs(int r)
{
    return -(8 * (r + 1));
}

/* locals: params..., fp, pc, scratch_i32, scratch_f32, scratch_f64 */
#define W_FP_LOCAL  (wasm_cf->nparams)
#define W_PC_LOCAL  (wasm_cf->nparams + 1)
#define W_SCRATCH_LOCAL (wasm_cf->nparams + 2)
#define W_SCRATCH_F32 (wasm_cf->nparams + 3)
#define W_SCRATCH_F64 (wasm_cf->nparams + 4)
#define W_SCRATCH_I64 (wasm_cf->nparams + 5)

/* ---------------------------------------------------------------- */
/* growable arrays */

static void *wa_grow(void *p, int *alloc, int need, int esz)
{
    if (need > *alloc) {
        int na = *alloc ? *alloc * 2 : 16;
        while (na < need)
            na *= 2;
        p = tcc_realloc(p, (size_t)na * esz);
        *alloc = na;
    }
    return p;
}

/* ---------------------------------------------------------------- */
/* instruction emission */

static void w_vdbg(const char *where)
{
}

ST_FUNC void o(unsigned int c)
{
    /* placeholder for generic code that emits machine words */
    tcc_error("wasm: o() not supported");
}

ST_FUNC void g(int c)
{
    /* data-initializer evaluation outside any function: no code buffer
       (60_errors' `int i = i++;` crashed on the NULL wasm_cf) */
    if (!wasm_cf)
        return;
    if (nocode_wanted) {
        /* fill the suppressed region with NOPs: the layout slices the
           raw code by POSITION, and the stale bytes left by the
           early-return (from an earlier function's exit) leaked into
           the emitted module (03_struct's leading unreachables) */
        if (wasm_cf->csize <= ind)
            wasm_cf->code = wa_grow(wasm_cf->code, &wasm_cf->csize,
                                    ind + 1, 1);
        wasm_cf->code[ind++] = 0x01;
        return;
    }
    if (wasm_cf->csize <= ind) {
        /* grow when the buffer is FULL (<=, not ==: under nocode_wanted
           w_ins() advances ind without emitting, so after a dead-code
           stretch csize < ind and the next real byte would overflow) */
        wasm_cf->code = wa_grow(wasm_cf->code, &wasm_cf->csize,
                                ind + 1, 1);
    }
    wasm_cf->code[ind++] = c;
}

static void w_ins(int op)
{
    /* record instruction start position, then emit the opcode */
    if (!wasm_cf)
        return;   /* data-initializer evaluation outside any function */
    if (nocode_wanted) {
        g(0x01);
        return;
    }
    wasm_cf->inspos = wa_grow(wasm_cf->inspos, &wasm_cf->ins_alloc,
                              wasm_cf->nins + 1, sizeof(int));
    wasm_cf->inspos[wasm_cf->nins++] = ind;
    g(op);
}

static void w_u32(unsigned v)
{
    /* LEB128 unsigned */
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v)
            b |= 0x80;
        g(b);
    } while (v);
}

static void w_s32(int v)
{
    unsigned char b;
    int more;
    do {
        b = v & 0x7f;
        v >>= 7;
        more = !((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)));
        if (more)
            b |= 0x80;
        g(b);
    } while (more);
}

static void w_i32_const_patch_slot_dummy(int kind, Sym *sym);

/* is the symbol a function (its address is a code/table pointer)? */
static int w_sym_is_func(Sym *sym)
{
    return sym && (sym->type.t & VT_BTYPE) == VT_FUNC;
}

/* emit i32.const with a 5-byte patchable slot; record the patch */
static void w_i32_patch(int kind, Sym *sym)
{
    int i, slot;
    if (nocode_wanted) {
        w_i32_const_patch_slot_dummy(kind, sym);
        return;
    }

    w_ins(W_I32_CONST);
    slot = ind;
    for (i = 0; i < 4; i++)
        g(0x80);
    g(0x00);   /* 5-byte slot: 4 continuation bytes + terminator */
    wasm_cf->patches = wa_grow(wasm_cf->patches, &wasm_cf->npatch_alloc,
                               wasm_cf->npatches + 1, sizeof(WasmPatch));
    wasm_cf->patches[wasm_cf->npatches].ofs = slot;
    wasm_cf->patches[wasm_cf->npatches].sym = sym;
    wasm_cf->patches[wasm_cf->npatches].name =
        sym ? tcc_strdup(w_sym_name(sym)) : NULL;
    wasm_cf->patches[wasm_cf->npatches].kind = kind;
    wasm_cf->patches[wasm_cf->npatches].sig = 0;
    wasm_cf->patches[wasm_cf->npatches].dsec = 0;
    wasm_cf->patches[wasm_cf->npatches].dofs = 0;
    if (kind == 1 && sym && sym->c) {
        ElfSym *es = elfsym(sym);
        wasm_cf->patches[wasm_cf->npatches].dsec = es->st_shndx;
        wasm_cf->patches[wasm_cf->npatches].dofs = es->st_value;
    }
    wasm_cf->npatches++;
}

static void w_i32_const_patch_slot_dummy(int kind, Sym *sym)
{
    /* under nocode_wanted the slot is not emitted; nothing to do */
}

static void w_i32_const(int v)
{
    w_ins(W_I32_CONST);
    w_s32(v);
}

static void w_local_get(int i) { w_ins(W_LOCAL_GET); w_u32(i); }
static void w_local_set(int i) { w_ins(W_LOCAL_SET); w_u32(i); }
static void w_local_tee(int i) { w_ins(W_LOCAL_TEE); w_u32(i); }
static void w_global_get(void) { w_ins(W_GLOBAL_GET); w_u32(0); }
static void w_global_set(void) { w_ins(W_GLOBAL_SET); w_u32(0); }

static void w_call(Sym *sym, int sig)
{
    /* call with a 5-byte patchable function index */
    int i, slot;
    if (nocode_wanted)
        return;
    w_ins(W_CALL);
    slot = ind;
    for (i = 0; i < 4; i++)
        g(0x80);
    g(0x00);   /* 5-byte slot */

    wasm_cf->patches = wa_grow(wasm_cf->patches, &wasm_cf->npatch_alloc,
                               wasm_cf->npatches + 1, sizeof(WasmPatch));
    wasm_cf->patches[wasm_cf->npatches].ofs = slot;
    wasm_cf->patches[wasm_cf->npatches].sym = sym;
    wasm_cf->patches[wasm_cf->npatches].name = sym ? tcc_strdup(w_sym_name(sym)) : NULL;
    wasm_cf->patches[wasm_cf->npatches].kind = 0;
    wasm_cf->patches[wasm_cf->npatches].sig = sig;
    wasm_cf->npatches++;
}

/* memory ops: memop(al, off) */
static void w_memop(int op, int al, int off)
{
    w_ins(op);
    w_u32(al);
    w_u32(off);
}

/* memory.copy (bulk-memory): [dst, src, len] on the wasm stack, copies
   len bytes, overlap-safe (memmove semantics).  memidx 0/0 = memory 0. */
static void w_mem_copy(int len)
{
    w_i32_const(len);
    w_ins(0xfc);
    g(0x0a);
    g(0x00);
    g(0x00);
}

static void w_br(int depth) { w_ins(W_BR); w_u32(depth); }
static void w_br_if(int depth) { w_ins(W_BR_IF); w_u32(depth); }

static void w_block(int bt) { w_ins(W_BLOCK); g(bt); }
static void w_loop(int bt)  { w_ins(W_LOOP); g(bt); }
static void w_if(int bt)    { w_ins(W_IF); g(bt); }
static void w_else(void)    { w_ins(W_ELSE); }
static void w_end(void)     { w_ins(W_END); }
static void w_unreachable(void) { w_ins(W_UNREACHABLE); }

/* load/store helpers: value on stack below address */
static void w_store(int op, int size, int align, int off)
{
    w_memop(op, align, off);
}
static void w_load(int op, int align, int off)
{
    w_memop(op, align, off);
}

/* ---------------------------------------------------------------- */
static void w_layout(void);

/* frame slots */

static int w_slot_ofs(int r)
{
    return reg_ofs(r);
}

static void w_emit_slot_addr(int r)
{
    w_local_get(wasm_cf->nparams);          /* fp */
    w_i32_const(w_slot_ofs(r));
    w_ins(W_I32_ADD);
}

/* load the value of slot r onto the wasm stack */
static void w_emit_slot_load(int r, int bt)
{
    w_emit_slot_addr(r);
    switch (bt) {
    case VT_FLOAT:  w_load(W_F32_LOAD, 2, 0); break;
    case VT_DOUBLE:
    case VT_LDOUBLE: w_load(W_F64_LOAD, 3, 0); break;   /* LDOUBLE_SIZE=8 */
    default:        w_load(W_I32_LOAD, 2, 0); break;
    }
}

/* store the wasm-stack value into slot r.
   wasm stores take the address FIRST then the value, so park the
   value in a scratch local, push the address, then reload it. */
static void w_emit_slot_store(int r, int bt)
{
    int scr = (bt == VT_FLOAT) ? W_SCRATCH_F32
            : (bt == VT_DOUBLE || bt == VT_LDOUBLE) ? W_SCRATCH_F64
            : W_SCRATCH_LOCAL;
    w_local_set(scr);
    w_emit_slot_addr(r);
    w_local_get(scr);
    switch (bt) {
    case VT_FLOAT:  w_store(W_F32_STORE, 4, 2, 0); break;
    case VT_DOUBLE:
    case VT_LDOUBLE: w_store(W_F64_STORE, 8, 3, 0); break;   /* LDOUBLE_SIZE=8 */
    default:        w_store(W_I32_STORE, 4, 2, 0); break;
    }
}

/* ---------------------------------------------------------------- */
/* blocks / segments / edges / labels */

#define EDGE_COND   0
#define EDGE_UNCOND 1
#define EDGE_TERM   2

static int w_new_label(void)
{
    int i = wasm_cf->nlabels++;
    wasm_cf->labels = wa_grow(wasm_cf->labels, &wasm_cf->label_alloc,
                              wasm_cf->nlabels, sizeof(WasmLabel));
    wasm_cf->labels[i].pos = -1;
    wasm_cf->labels[i].sub = -1;
    wasm_cf->labels[i].seg = -1;
    return i;
}

static void w_close_seg(void)
{
    wasm_cf->segs[wasm_cf->cur_seg].end = ind;
}

static void w_open_seg(void)
{
    int i;
    if (wasm_cf->nsegs == 0) {
        wasm_cf->segs = wa_grow(NULL, &wasm_cf->seg_alloc, 1, sizeof(WasmSeg));
        wasm_cf->nsegs = 1;
        wasm_cf->segs[0].start = 0;
        wasm_cf->segs[0].end = 0;
        wasm_cf->segs[0].edge = -1;
        wasm_cf->cur_seg = 0;
        return;
    }
    wasm_cf->segs = wa_grow(wasm_cf->segs, &wasm_cf->seg_alloc,
                            wasm_cf->nsegs + 1, sizeof(WasmSeg));
    i = wasm_cf->nsegs++;
    wasm_cf->segs[i].start = ind;
    wasm_cf->segs[i].end = ind;
    wasm_cf->segs[i].edge = -1;
    wasm_cf->cur_seg = i;
    /* the current block extends to this new segment (no block exists
       while the prolog is being emitted) */
    if (wasm_cf->nblks > 0)
        wasm_cf->blks[wasm_cf->cur_blk].ls = i;
}

static void w_add_edge(int kind, int op, int label)
{
    int e = wasm_cf->nedges++;
    wasm_cf->edges = wa_grow(wasm_cf->edges, &wasm_cf->edge_alloc,
                             wasm_cf->nedges, sizeof(WasmEdge));
    w_close_seg();
    wasm_cf->segs[wasm_cf->cur_seg].edge = e;
    wasm_cf->edges[e].kind = kind;
    wasm_cf->edges[e].op = op;
    wasm_cf->edges[e].label = label;
    wasm_cf->edges[e].chain_next = -1;
    wasm_cf->edges[e].seg = wasm_cf->cur_seg;
    w_open_seg();
}

static void w_new_block(void)
{
    int b = wasm_cf->nblks++;
    wasm_cf->blks = wa_grow(wasm_cf->blks, &wasm_cf->blk_alloc,
                            wasm_cf->nblks, sizeof(WasmBlk));
    /* the segment just opened by the split belongs to the new block:
       pull the previous block's last segment back */
    if (wasm_cf->nblks > 1)
        wasm_cf->blks[wasm_cf->cur_blk].ls = wasm_cf->cur_seg - 1;
    wasm_cf->blks[b].fs = wasm_cf->cur_seg;
    wasm_cf->blks[b].ls = wasm_cf->cur_seg;
    wasm_cf->cur_blk = b;
    /* record (pos -> block) */
    wasm_cf->posblks = wa_grow(wasm_cf->posblks, &wasm_cf->posblk_alloc,
                               wasm_cf->nposblks + 1, sizeof(int) * 2);
    wasm_cf->posblks[wasm_cf->nposblks * 2] = ind;
    wasm_cf->posblks[wasm_cf->nposblks * 2 + 1] = b;
    wasm_cf->nposblks++;
}

/* jump chains use handles = edge_index + 1 (0 = empty chain, since
   tccgen uses 0 as the "no jump" sentinel) */
static void w_append_chain(int head, int tail)
{
    int e;
    if (head <= 0)
        return;
    e = head - 1;
    while (wasm_cf->edges[e].chain_next > 0)
        e = wasm_cf->edges[e].chain_next - 1;
    wasm_cf->edges[e].chain_next = tail;
}

/* find the block containing a code position */
static int w_block_at_pos(int pos)
{
    int lo = 0, hi = wasm_cf->nposblks - 1, best = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (wasm_cf->posblks[mid * 2] <= pos) {
            best = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return wasm_cf->posblks[best * 2 + 1];
}

/* ---------------------------------------------------------------- */
/* backend interface */

/* materialize a comparison into 0/1 on the wasm stack */
static void w_emit_cmp(int op, int a, int b)
{
    int o;
    switch (op) {
    case TOK_EQ:  o = W_I32_EQ;  break;
    case TOK_NE:  o = W_I32_NE;  break;
    case TOK_LT:  o = W_I32_LT_S; break;
    case TOK_ULT: o = W_I32_LT_U; break;
    case TOK_GT:  o = W_I32_GT_S; break;
    case TOK_UGT: o = W_I32_GT_U; break;
    case TOK_LE:  o = W_I32_LE_S; break;
    case TOK_ULE: o = W_I32_LE_U; break;
    case TOK_GE:  o = W_I32_GE_S; break;
    case TOK_UGE: o = W_I32_GE_U; break;
    default: o = W_I32_NE; break;
    }
    w_emit_slot_load(a, VT_INT);
    w_emit_slot_load(b, VT_INT);
    w_ins(o);
}

/* emit the address of an lvalue (SValue) onto the wasm stack */
static void w_emit_addr(SValue *sv)
{
    int v = sv->r & VT_VALMASK;
    int fc = sv->c.i;
    if (sv->r & VT_LVAL) {
        if (v == VT_LOCAL) {
            w_local_get(wasm_cf->nparams);
            w_i32_const(fc);
            w_ins(W_I32_ADD);
        } else if (v == VT_LLOCAL) {
            w_local_get(wasm_cf->nparams);
            w_i32_const(fc);
            w_ins(W_I32_ADD);
            w_load(W_I32_LOAD, 2, 0);
        } else if (v == VT_CONST && (sv->r & VT_SYM)) {
            w_i32_patch(w_sym_is_func(sv->sym) ? 2 : 1, sv->sym);
            if (fc)
                w_i32_const(fc), w_ins(W_I32_ADD);   /* member offset */
        } else if (v == VT_CONST) {
            w_i32_const(fc);
        } else if (v < VT_CONST) {
            /* address held in a register slot */
            w_emit_slot_load(v, VT_INT);
        } else if (v == VT_CMP || v == VT_JMP || v == (VT_JMP | 1)) {
            tcc_error("wasm: lvalue of comparison");
        } else {
            tcc_error("wasm: unimp addr (v=%d)", v);
        }
    } else {
        tcc_error("wasm: w_emit_addr on non-lvalue");
    }
}

/* emit the value of an lvalue (dereference) */
static void w_emit_deref(SValue *sv)
{
    int bt = sv->type.t & VT_BTYPE;
    int al, sz = type_size(&sv->type, &al);
    if (bt == VT_STRUCT)
        tcc_error("wasm: struct deref");
    w_emit_addr(sv);
    if (sz == 1)
        w_load((sv->type.t & VT_UNSIGNED) ? W_I32_LOAD8_U : W_I32_LOAD8_S, 0, 0);
    else if (sz == 2)
        w_load((sv->type.t & VT_UNSIGNED) ? W_I32_LOAD16_U : W_I32_LOAD16_S, 1, 0);
    else if (bt == VT_FLOAT)
        w_load(W_F32_LOAD, 2, 0);
    else if (bt == VT_DOUBLE || bt == VT_LDOUBLE)
        w_load(W_F64_LOAD, 3, 0);   /* LDOUBLE_SIZE=8 */
    else
        w_load(W_I32_LOAD, 2, 0);
}

static void w_emit_store(SValue *sv)
{
    int bt = sv->type.t & VT_BTYPE;
    int al, sz = type_size(&sv->type, &al);
    if (bt == VT_STRUCT) {
        /* v1: unrolled word copy, max 16 bytes */
        int i;
        w_emit_addr(sv);           /* dst */
        w_i32_const(sz);           /* (src = value below; need temp) */
        tcc_error("wasm: struct store");
        return;
    }
    if (sz == 1)
        w_store(W_I32_STORE8, 1, 0, 0);
    else if (sz == 2)
        w_store(W_I32_STORE16, 2, 1, 0);
    else if (bt == VT_FLOAT)
        w_store(W_F32_STORE, 4, 2, 0);
    else if (bt == VT_DOUBLE || bt == VT_LDOUBLE)
        w_store(W_F64_STORE, 8, 3, 0);
    else
        w_store(W_I32_STORE, 4, 2, 0);
}

ST_FUNC void load(int r, SValue *sv)
{
    int bt = sv->type.t & VT_BTYPE;
    int v = sv->r & VT_VALMASK;
    int fc = sv->c.i;

    if (v == VT_CMP) {
        /* deferred comparison: compute 0/1, store to slot r */
        int ca = sv->cmp_r & 0xff, cb = (sv->cmp_r >> 8) & 0xff;
        if (sv->cmp_r != w_last_cmp_r) {
            /* re-marked comparison (see gen_opi): use the last one */
            ca = w_last_cmp_a;
            cb = w_last_cmp_b;
        }
        w_emit_cmp(sv->cmp_op, ca, cb);
        w_emit_slot_store(r, VT_INT);
        return;
    }
    if (v == VT_JMP || v == (VT_JMP | 1)) {
        /* jump-list value: r = inv if the chain jumps, else !inv */
        int inv = v & 1, t = sv->c.i;
        int l_true, l_false, l_merge;
        /* close current block with fallthrough to l_false; chain -> l_true */
        l_true = w_new_label();
        l_false = w_new_label();
        l_merge = w_new_label();
        /* resolve chain t to l_true */
        while (t > 0) {
            int e = t - 1;
            wasm_cf->edges[e].label = l_true;
            t = wasm_cf->edges[e].chain_next;
        }
        /* current block: fallthrough edge -> l_false */
        w_add_edge(EDGE_UNCOND, 0, l_false);
        /* anchor each label to its code position + segment (w_layout
           resolves labels by segment — a bare pos is ambiguous when
           several segments start at one position) */
        wasm_cf->labels[l_false].pos = ind;
        wasm_cf->labels[l_false].seg = -1;   /* pos-containment only */
        /* l_false block: r = inv (the chain did NOT jump — the value
           is the non-inverted test result) */
        w_i32_const(inv);
        w_emit_slot_store(r, VT_INT);
        w_add_edge(EDGE_UNCOND, 0, l_merge);
        /* l_true block: r = !inv (the chain jumped — the value is the
           inverted test result) */
        wasm_cf->labels[l_true].pos = ind;
        wasm_cf->labels[l_true].seg = -1;
        w_i32_const(inv ^ 1);
        w_emit_slot_store(r, VT_INT);
        w_add_edge(EDGE_UNCOND, 0, l_merge);
        /* merge: the code that follows */
        wasm_cf->labels[l_merge].pos = ind;
        wasm_cf->labels[l_merge].seg = -1;
        return;
    }
    if (sv->r & VT_LVAL) {
        w_emit_deref(sv);
        w_emit_slot_store(r, bt);
        return;
    }
    /* plain value */
    if (v == VT_CONST) {
        if (sv->r & VT_SYM) {
            w_i32_patch(w_sym_is_func(sv->sym) ? 2 : 1, sv->sym);
            if (fc)
                w_i32_const(fc), w_ins(W_I32_ADD);   /* member offset */
            w_emit_slot_store(r, VT_INT);
        } else if (bt == VT_FLOAT) {
            union { float f; unsigned u; } u;
            u.f = sv->c.f;
            w_i32_const(u.u);
            w_emit_slot_store(r, VT_INT);   /* the bits ARE the f32 */
        } else if (bt == VT_DOUBLE || bt == VT_LDOUBLE) {
            union { double d; unsigned u[2]; } u;
            /* a long double constant lives in c.ld (host width), NOT in
               c.d — on hosts where long double != double the low bytes
               of the 80/128-bit representation are not the double
               value, so reading c.d gave garbage (12.34l printed
               -3.2e+26).  LDOUBLE_SIZE=8 on wasm32: truncate the value. */
            u.d = (bt == VT_LDOUBLE) ? (double)sv->c.ld : sv->c.d;
            w_i32_const(u.u[0]);
            w_emit_slot_store(r, VT_INT);
            /* high word at slot r + 4 (the second half of the slot's
               8-byte interval, so a single F64 load at slot r reads
               both words back) */
            w_emit_slot_addr(r);
            w_i32_const(4);
            w_ins(W_I32_ADD);
            w_i32_const(u.u[1]);
            w_ins(W_I32_STORE); w_u32(2); w_u32(0);
        } else {
            w_i32_const(fc);
            w_emit_slot_store(r, VT_INT);
        }
        return;
    }
    if (v == VT_LOCAL) {
        w_local_get(wasm_cf->nparams);
        w_i32_const(fc);
        w_ins(W_I32_ADD);
        w_emit_slot_store(r, VT_INT);
        return;
    }
    if (v == VT_LLOCAL) {
        w_local_get(wasm_cf->nparams);
        w_i32_const(fc);
        w_ins(W_I32_ADD);
        w_load(W_I32_LOAD, 2, 0);
        w_emit_slot_store(r, VT_INT);
        return;
    }
    if (v < VT_CONST) {
        /* move from slot v to slot r */
        w_emit_slot_load(v, bt);
        w_emit_slot_store(r, bt);
        return;
    }
    tcc_error("wasm: unimp load (v=%d)", v);
}

ST_FUNC void store(int r, SValue *sv)
{
    int bt = sv->type.t & VT_BTYPE;
    if (bt == VT_STRUCT) {
        tcc_error("wasm: struct store");
    }
    if (sv->r & VT_LVAL) {
        /* sv is the lvalue; value from slot r */
        w_emit_addr(sv);
        w_emit_slot_load(r, bt);
        w_emit_store(sv);
    } else {
        tcc_error("wasm: store to non-lvalue");
    }
}

/* find a function reference by symbol pointer, adding an import if
   unknown.  The name is captured here (tcc_state is valid during
   compilation; get_tok_str is unsafe at output time). */
static int w_func_ref(Sym *sym)
{
    int i;
    for (i = 0; i < wasm_nfuncs; i++)
        if (wasm_funcs[i].sym == sym)
            return i;
    wasm_funcs = wa_grow(wasm_funcs, &wasm_func_alloc,
                         wasm_nfuncs + 1, sizeof(WasmFuncRef));
    wasm_funcs[wasm_nfuncs].name = tcc_strdup(get_tok_str(sym->v, NULL));
    wasm_funcs[wasm_nfuncs].sym = sym;
    wasm_funcs[wasm_nfuncs].sig = -1;
    wasm_funcs[wasm_nfuncs].defined = 0;
    wasm_funcs[wasm_nfuncs].import = 1;
    wasm_funcs[wasm_nfuncs].order = -1;
    return wasm_nfuncs++;
}

/* mark a function as defined at gfunc_prolog; returns its ref index */
static int w_func_defined(Sym *func_sym, int sig)
{
    int i = w_func_ref(func_sym);
    const char *nm;
    wasm_funcs[i].defined = 1;
    wasm_funcs[i].import = 0;
    wasm_funcs[i].sig = sig;
    wasm_funcs[i].order = wasm_ndef++;
    if (func_sym->type.ref && func_sym->type.ref->f.func_ctor) {
        nm = wasm_funcs[i].name;
        wasm_ctors = wa_grow(wasm_ctors, &wasm_ctors_alloc,
                             wasm_nctors + 1, sizeof(char *));
        wasm_ctors[wasm_nctors++] = tcc_strdup(nm);
    }
    if (func_sym->type.ref && func_sym->type.ref->f.func_dtor) {
        nm = wasm_funcs[i].name;
        wasm_dtors = wa_grow(wasm_dtors, &wasm_dtors_alloc,
                             wasm_ndtors + 1, sizeof(char *));
        wasm_dtors[wasm_ndtors++] = tcc_strdup(nm);
    }
    return i;
}

/* signature management: build "i32 i32 ... : result" string */
static int w_sig_get(int nparams, const unsigned char *params, int nres, int res)
{
    char buf[256], *p = buf;
    int i;
    for (i = 0; i < nparams; i++)
        *p++ = (char)params[i];
    *p++ = ':';
    *p++ = (char)(nres ? res : 0);
    *p = 0;
    for (i = 0; i < wasm_nsigs; i++)
        if (!strcmp(wasm_sigs[i].sig, buf))
            return i;
    wasm_sigs = wa_grow(wasm_sigs, &wasm_sig_alloc,
                        wasm_nsigs + 1, sizeof(WasmSig));
    wasm_sigs[wasm_nsigs].sig = tcc_strdup(buf);
    wasm_sigs[wasm_nsigs].nparams = nparams;
    wasm_sigs[wasm_nsigs].nresults = nres;
    return wasm_nsigs++;
}

static void w_sig_from_type(CType *func_type, int *psig, int *pnparams)
{
    Sym *s;
    unsigned char params[64];
    int n = 0, res, nres;
    int variadic = 0;
    if (func_type->ref->f.func_type == FUNC_ELLIPSIS)
        variadic = 1;
    if ((func_type->ref->type.t & VT_BTYPE) == VT_STRUCT) {
        params[n++] = VAL_I32;   /* sret pointer */
    }
    for (s = func_type->ref->next; s; s = s->next) {
        if (n >= 60)
            tcc_error("wasm: too many params");
        if ((s->type.t & VT_BTYPE) == VT_STRUCT) {
            params[n++] = VAL_I32;   /* by-pointer (v1: caller copies) */
        } else {
            params[n++] = w_typeof(&s->type);
        }
    }
    (void)variadic;   /* ellipsis: the signature carries the fixed params
                         only — the per-call varargs are added by
                         w_call_sig() at the call site */
    res = w_typeof(&func_type->ref->type);
    nres = ((func_type->ref->type.t & VT_BTYPE) == VT_VOID) ? 0 : 1;
    *pnparams = n;
    *psig = w_sig_get(n, params, nres, res);
}

/* ---------------------------------------------------------------- */
/* gfunc_prolog / epilog */

static int w_func_sig;    /* signature index of current function */
static int w_ret_bt;      /* return value type */
static int w_frame_patch; /* offset of frame-size slot in prolog */
static int w_body_start;  /* code position where the body begins */

ST_FUNC void gfunc_prolog(Sym *func_sym)
{
    CType *func_type = &func_sym->type;
    int i, align, size, param_count;
    Sym *sym;
    CType *type;
    int nwasmparams;

    wasm_cf = tcc_mallocz(sizeof(WasmFunc));
    wasm_cf->name = tcc_strdup(get_tok_str(func_sym->v, NULL));
    w_sig_from_type(func_type, &w_func_sig, &nwasmparams);
    wasm_cf->sig = w_func_sig;
    wasm_cf->nparams = nwasmparams;
    wasm_cf->nlocals = 6;   /* fp, pc, scratch_i32/f32/f64/i64 */
    wasm_cf->has_sret = ((func_type->ref->type.t & VT_BTYPE) == VT_STRUCT);
    {
        int ref = w_func_defined(func_sym, w_func_sig);
        wasm_cf->order = wasm_funcs[ref].order;
    }
    w_ret_bt = func_vt.t & VT_BTYPE;

    /* register file + locals */
    loc = -(NB_REGS * 8);
    w_park_cursor = ((-loc + 15) & ~15) >> 3;
    ind = 0;
    w_open_seg();

    /* prolog: fp = sp (frame TOP); sp = fp - frame (frame BOTTOM).
       Locals live at negative offsets from fp, i.e. INSIDE the frame,
       so a callee's frame (allocated below our sp) never collides. */
    w_global_get();
    w_local_tee(W_FP_LOCAL);                /* fp = sp */
    w_ins(W_I32_CONST);
    w_frame_patch = ind;
    for (i = 0; i < 4; i++) {
        g(0x80);
    }
    g(0x00);   /* 5-byte slot */
    w_ins(W_I32_SUB);
    w_global_set();                         /* sp = fp - frame */

    /* sret param (if any): store to func_vc */
    if (wasm_cf->has_sret) {
        loc = (loc - 8) & -8;
        func_vc = loc;
        w_local_get(wasm_cf->nparams);
        w_i32_const(loc);
        w_ins(W_I32_ADD);
        w_local_get(0);
        w_store(W_I32_STORE, 4, 2, 0);
    }

    /* define parameters */
    param_count = 0;
    for (sym = func_type->ref->next; sym; sym = sym->next) {
        int byref = 0;
        type = &sym->type;
        size = type_size(type, &align);
        if ((type->t & VT_BTYPE) == VT_STRUCT) {
            /* structs arrive by POINTER (the caller copied the value to
               a temp): the param home holds the pointer, so member
               access must deref through it — force VT_LLOCAL even for
               small structs (byref=0 would leave the pointer in the
               struct's home and s.a would read the pointer's bytes) */
            byref = 1;
        }
        if (size > 8) {
            type = &char_pointer_type;
            size = align = byref = 4;
        }
        loc = (loc - size) & -align;
        gfunc_set_param(sym, loc, byref);
        /* stores take the address first, then the value */
        if ((sym->type.t & VT_BTYPE) == VT_STRUCT) {
            /* passed by pointer: the param is the pointer */
            w_local_get(wasm_cf->nparams);
            w_i32_const(loc);
            w_ins(W_I32_ADD);
            w_local_get(param_count);
            w_store(W_I32_STORE, 4, 2, 0);
        } else {
            int vt = w_typeof(type);
            w_local_get(wasm_cf->nparams);
            w_i32_const(loc);
            w_ins(W_I32_ADD);
            w_local_get(param_count);
            if (vt == VAL_F32)
                w_store(W_F32_STORE, 4, 2, 0);
            else if (vt == VAL_F64)
                w_store(W_F64_STORE, 8, 3, 0);
            else if (vt == VAL_I64) {
                /* the outer code has already pushed [addr, param]; park
                   the param in the i64 scratch, store the low word to the
                   pending addr, then the high word at loc+4 (little-
                   endian var home) — never push the param again, or the
                   outer [addr, param] dangles and unbalances the stack */
                w_local_set(W_SCRATCH_I64);              /* [] (param parked) */
                w_local_get(W_SCRATCH_I64);
                w_ins(W_I32_WRAP);
                w_store(W_I32_STORE, 4, 2, 0);           /* low → pending addr */
                w_local_get(wasm_cf->nparams);           /* fp */
                w_i32_const(loc + 4);
                w_ins(W_I32_ADD);
                w_local_get(W_SCRATCH_I64);
                w_ins(W_I64_CONST); w_u32(32);
                w_ins(W_I64_SHR_U);
                w_ins(W_I32_WRAP);
                w_store(W_I32_STORE, 4, 2, 0);           /* high → loc+4 */
            } else
                w_store(W_I32_STORE, 4, 2, 0);
        }
        param_count++;
    }
    /* block 0 (the body) starts after the prolog+params */
    w_body_start = ind;
    w_open_seg();
    w_new_block();
}

ST_FUNC int gfunc_sret(CType *vt, int variadic, CType *ret,
                       int *ret_align, int *regsize)
{
    /* always return structs via the sret pointer (v1) */
    *ret_align = 1;
    *regsize = 8;
    return 0;
}

ST_FUNC void arch_transfer_ret_regs(int aftercall)
{
    tcc_error("wasm: mixed struct return unsupported");
}

ST_FUNC void gfunc_epilog(void)
{
    /* return value (if any) from the return slot, then the term edge;
       the whole function body is laid out by w_layout() */
    int ret_bt = w_ret_bt, frame, slot, v, b, i;
    /* pop the frame: sp = fp */
    w_local_get(W_FP_LOCAL);
    w_global_set();
    if (ret_bt != VT_VOID) {
        int reg = (ret_bt == VT_FLOAT || ret_bt == VT_DOUBLE)
                  ? REG_FRET : REG_IRET;
        if (ret_bt == VT_LLONG) {
            /* two-word return: low + high<<32 */
            w_emit_slot_load(REG_IRET, VT_INT);
            w_ins(W_I64_EXTEND_U);
            w_emit_slot_load(REG_IRE2, VT_INT);
            w_ins(W_I64_EXTEND_U);
            w_ins(W_I64_CONST);
            w_u32(32);
            w_ins(W_I64_SHL);
            w_ins(W_I64_OR);
        } else {
            w_emit_slot_load(reg, ret_bt);
        }
    }
    w_add_edge(EDGE_TERM, 0, -1);
    /* frame size */
    frame = (-loc + 15) & ~15;
    /* patch the frame-size slot (5-byte LEB, continuation bits forced) */
    slot = w_frame_patch;
    v = frame;
    for (i = 0; i < 5; i++) {
        b = v & 0x7f;
        v >>= 7;
        wasm_cf->code[slot++] = (i == 4) ? b : (b | 0x80);
    }
    w_layout();
    wasm_func_list = wa_grow(wasm_func_list, &wasm_func_list_alloc,
                             wasm_nfunc_list + 1, sizeof(WasmFunc *));
    wasm_func_list[wasm_nfunc_list++] = wasm_cf;
}

/* ---------------------------------------------------------------- */
/* jumps */

ST_FUNC int gjmp(int t)
{
    int l = w_new_label();
    int e;
    w_add_edge(EDGE_UNCOND, 0, l);
    e = wasm_cf->nedges;   /* handle of the new edge */
    if (nocode_wanted & 0xFFFF)
        /* suppressed jump inside a genuinely-dead region (NOEVAL
           counting — a dead statement-expression's loop/return): mark
           the edge so w_layout skips it and the region falls through
           instead of looping or jumping into dead bytes. */
        wasm_cf->edges[e - 1].op = -1;
    if (t > 0)
        w_append_chain(t, e);
    return t > 0 ? t : e;
}

ST_FUNC int gjmp_cond(int op, int t)
{
    int l = w_new_label();
    int ins0 = wasm_cf->nins;   /* real (non-suppressed) ops so far */
    /* materialize the comparison: vtop is VT_CMP */
    if (vtop->r == VT_CMP) {
        int ca = vtop->cmp_r & 0xff, cb = (vtop->cmp_r >> 8) & 0xff;
        if (vtop->cmp_r != w_last_cmp_r) {
            /* re-marked comparison (see gen_opi): tccgen's 64-bit
               compare decomposition sets VT_CMP(NE) without cmp_r
               (x86's flags are the source of truth); use the last
               recorded operands — otherwise the switch's case ranges
               compare garbage registers (118_switch matched the wrong
               range) */
            ca = w_last_cmp_a;
            cb = w_last_cmp_b;
        }
        w_emit_cmp(op, ca, cb);
    } else {
        /* plain value vs zero */
        int bt = vtop->type.t & VT_BTYPE;
        if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST) {
            /* constant: jump depends on value */
            w_i32_const(vtop->c.i != 0);
        } else {
            if (vtop->r & VT_LVAL) {
                w_emit_deref(vtop);
                w_emit_slot_store(6, bt);
                w_emit_slot_load(6, bt);
            } else if ((vtop->r & VT_VALMASK) < VT_CONST) {
                w_emit_slot_load(vtop->r & VT_VALMASK, bt);
            } else {
                w_i32_const(0);
            }
        }
        if (op == TOK_EQ || op == TOK_NE) {
            /* value is 0/1 already for EQ/NE tests */
            w_ins(W_I32_EQZ);   /* !value */
            w_i32_const(1);
            w_ins(W_I32_XOR);
        } else if (op == TOK_LT || op == TOK_ULT || op == TOK_GT || op == TOK_UGT
                || op == TOK_LE || op == TOK_ULE || op == TOK_GE || op == TOK_UGE) {
            /* compare value with zero */
            w_ins(op == TOK_LT ? W_I32_LT_S : op == TOK_ULT ? W_I32_LT_U :
                  op == TOK_GT ? W_I32_GT_S : op == TOK_UGT ? W_I32_GT_U :
                  op == TOK_LE ? W_I32_LE_S : op == TOK_ULE ? W_I32_LE_U :
                  op == TOK_GE ? W_I32_GE_S : W_I32_GE_U);
        } else {
            w_i32_const(0);
            w_ins(W_I32_NE);
        }
    }
    w_add_edge(EDGE_COND, op, l);
    if (wasm_cf->nins == ins0)
        /* the whole condition was suppressed under nocode_wanted: the
           edge's cond bytes are NOPs (no stack value), so emitting the
           'if (cond)' wrapper would be an invalid 'if' with an empty
           stack. Mark the edge; w_layout skips it (fall through). */
        wasm_cf->edges[wasm_cf->nedges - 1].op = -1;
    if (t > 0)
        w_append_chain(t, wasm_cf->nedges);
    return t > 0 ? t : wasm_cf->nedges;
}

ST_FUNC int gjmp_append(int n, int t)
{
    if (n <= 0)
        return t;
    if (t > 0)
        w_append_chain(n, t);
    return n;
}

/* signed LEB128: wasm's i32.const decodes the value as SIGNED, so a
   pc target >= 64 (bit 6 set in the last byte) emitted with an
   unsigned-style LEB decodes NEGATIVE and the br_table default fires
   in any function with more than ~64 subs. */
static void w_body_leb(unsigned char *body, int *blen, int *balloc, unsigned int v)
{
    /* unsigned LEB (the br depths/counts can exceed 127 when many
       labels or a big switch split a block — 93_integer_promotion) */
    do {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if (v)
            b |= 0x80;
        body = wa_grow(body, balloc, *blen + 1, 1);
        body[(*blen)++] = b;
    } while (v);
}

static void w_body_sleb(unsigned char *body, int *blen, int *balloc, int v)
{
    int more = 1, neg = v < 0;
    while (more) {
        unsigned char b = v & 0x7f;
        v >>= 7;
        if ((neg && v == -1 && !(b & 0x40)) ||
            (!neg && v == 0 && !(b & 0x40)))
            more = 0;
        else
            b |= 0x80;
        body = wa_grow(body, balloc, *blen + 1, 1);
        body[(*blen)++] = b;
    }
}

/* the segment containing a code position: the closed segment whose
   range covers pos, or the current (still-open) segment for the
   current position, or the earliest segment starting at pos (an
   edge-only boundary). */
static int w_seg_at_pos(int pos)
{
    int i;
    for (i = 0; i < wasm_cf->nsegs; i++)
        if (pos >= wasm_cf->segs[i].start && pos < wasm_cf->segs[i].end)
            return i;
    for (i = 0; i < wasm_cf->nsegs; i++)
        if (pos == wasm_cf->segs[i].start)
            return i;
    return wasm_cf->cur_seg;
}

ST_FUNC void gjmp_addr(int a)
{
    /* unconditional jump to a code position (usually BACKWARD — the
       loop restart): the label's segment is the one containing the
       TARGET position, not the current emission point */
    int l = w_new_label();
    wasm_cf->labels[l].pos = a;
    wasm_cf->labels[l].seg = w_seg_at_pos(a);
    /* a loop back-edge inside a genuinely-dead (NOEVAL) region must not
       fire — mark it -1 so w_layout skips it and the dead code falls
       through (87_dead_code's dead while(1) looped forever); CODE_OFF
       alone is not a dead region (switch implicit breaks must fire) */
    w_add_edge(EDGE_UNCOND, (nocode_wanted & 0xFFFF) ? -1 : 0, l);
}

ST_FUNC void gsym_addr(int t, int a)
{
    /* resolve the jump chain t to position a; close the current block
       with a fallthrough to the next one (the code continues there) */
    if (!wasm_cf)
        return;   /* data-initializer / preprocessor evaluation (the
                     tccgen's gsym also calls us for the empty chain) */
    if (t == 0) {
        /* an address-taken label (&&lbl — the tccgen calls us with an
           empty chain): record its position so the data-reloc &&label
           VALUES resolve to THIS function's subs (the code positions
           are per-function, so the owner must be known) */
        wasm_cf->lblpos = wa_grow(wasm_cf->lblpos, &wasm_cf->lblpos_alloc,
                                  wasm_cf->nlblpos + 1, sizeof(int));
        wasm_cf->lblpos[wasm_cf->nlblpos++] = a;
    }
    int l = w_new_label();
    wasm_cf->labels[l].pos = a;
    /* a == ind (the usual gsym): the target code follows, in the next
       segment.  a != ind (a past/future position — backward/forward
       chain resolution): use the segment containing a. */
    wasm_cf->labels[l].seg = (a == ind) ? wasm_cf->cur_seg + 1
                                        : w_seg_at_pos(a);
    while (t > 0) {
        int e = t - 1;
        wasm_cf->edges[e].label = l;
        t = wasm_cf->edges[e].chain_next;
    }
    if (a == ind) {
        /* resolution to the current position: fall through.  The edge
           is a block-boundary MARKER (not a real jump) — mark it -1 so
           w_layout skips it and the sub's implicit fallthrough (pc =
           next) runs instead; a real continue/break chain resolved to
           the same position still fires (its chain edges are separate
           and stay op=0). */
        w_add_edge(EDGE_UNCOND, -1, l);
        w_new_block();
    }
}

/* gsym() is generic in tccgen.c (calls gsym_addr(t, ind)) */

/* ---------------------------------------------------------------- */
/* gen_opi / gen_opl / gen_opf */

static int w_carry_slot = 7;   /* dedicated slot for add-carry/borrow */

ST_FUNC void gen_opi(int op)
{
    int a, b, d, d2, o;
    if ((vtop->r & (VT_VALMASK | VT_LVAL | VT_SYM)) == VT_CONST &&
        op != TOK_ADDC1 && op != TOK_ADDC2 && op != TOK_SUBC1 &&
        op != TOK_SUBC2 && op != TOK_UMULL) {
        /* constant folding happens in tccgen; handle only the trivial
           const-with-const case here defensively */
    }
    switch (op) {
    case TOK_ADDC1:
    case TOK_SUBC1:
        /* low word op; store carry/borrow in the carry slot */
        gv2(RC_INT, RC_INT);
        a = vtop[-1].r; b = vtop[0].r;
        d = get_reg(RC_INT);   /* while a/b are still on the vstack
                                   (busy) — after vtop -= 2 they are
                                   "free" and get_reg would recycle one,
                                   aliasing d with a source that the
                                   carry step below re-reads */
        vtop -= 2;
        vtop++;
        vtop[0].r = d;
        w_emit_slot_load(a, VT_INT);
        w_emit_slot_load(b, VT_INT);
        w_ins(op == TOK_ADDC1 ? W_I32_ADD : W_I32_SUB);
        w_emit_slot_store(d, VT_INT);
        /* carry: (u32)sum < (u32)a  (borrow: a < b) */
        w_emit_slot_load(a, VT_INT);
        w_emit_slot_load(b, VT_INT);
        w_ins(op == TOK_ADDC1 ? W_I32_LT_U : W_I32_GT_U);
        w_emit_slot_store(w_carry_slot, VT_INT);
        vtop[0].r = d;
        return;
    case TOK_ADDC2:
    case TOK_SUBC2:
        gv2(RC_INT, RC_INT);
        a = vtop[-1].r; b = vtop[0].r;
        vtop -= 2;
        d = get_reg(RC_INT);
        vtop++;
        w_emit_slot_load(a, VT_INT);
        w_emit_slot_load(b, VT_INT);
        w_ins(op == TOK_ADDC2 ? W_I32_ADD : W_I32_SUB);
        w_emit_slot_load(w_carry_slot, VT_INT);
        w_ins(op == TOK_ADDC2 ? W_I32_ADD : W_I32_SUB);
        w_emit_slot_store(d, VT_INT);
        vtop[0].r = d;
        return;
    case TOK_UMULL:
        /* 32x32 -> 64: result in slots d (low) and d2 (high) — d2 is a
           REAL register (get_reg allocates the second word
           independently; d+1 may be busy — the i64 multiply of two
           small values silently corrupted the high result slot) */
        gv2(RC_INT, RC_INT);
        a = vtop[-1].r; b = vtop[0].r;
        d = get_reg(RC_INT);   /* while a/b are on the vstack (busy) —
                                   the high-word step re-reads a and b
                                   after the low store, and a recycled
                                   d == a would read the low result */
        vtop -= 2;
        vtop++;
        vtop[0].r = d;             /* mark d busy so d2 is distinct */
        d2 = get_reg(RC_INT);
        vtop[0].r2 = d2;
        w_emit_slot_load(a, VT_INT);
        w_ins(W_I64_EXTEND_U);
        w_emit_slot_load(b, VT_INT);
        w_ins(W_I64_EXTEND_U);
        w_ins(W_I64_MUL);
        /* low word */
        w_ins(W_I32_WRAP);
        w_emit_slot_store(d, VT_INT);
        /* high word: shr 32 */
        w_emit_slot_load(a, VT_INT);
        w_ins(W_I64_EXTEND_U);
        w_emit_slot_load(b, VT_INT);
        w_ins(W_I64_EXTEND_U);
        w_ins(W_I64_MUL);
        w_ins(W_I64_CONST);
        w_u32(32);
        w_ins(W_I64_SHR_U);
        w_ins(W_I32_WRAP);
        w_emit_slot_store(d2, VT_INT);
        return;
    default:
        break;
    }
    if (op >= TOK_ULT && op <= TOK_GT) {
        /* deferred comparison: set VT_CMP with the slot pair.
           The result replaces the first operand (vtop++ like the
           arithmetic path below). */
        gv2(RC_INT, RC_INT);
        a = vtop[-1].r; b = vtop[0].r;
        vtop -= 2;
        vtop++;
        vset_VT_CMP(op);
        vtop[0].cmp_r = a | (b << 8);
        /* remember the operands: tccgen's 64-bit compare decomposition
           re-marks a value as VT_CMP(NE) via vset_VT_CMP without
           setting cmp_r (x86's CPU flags are the source of truth there;
           the re-mark means "the last comparison") */
        w_last_cmp_a = a;
        w_last_cmp_b = b;
        w_last_cmp_r = a | (b << 8);
        return;
    }
    gv2(RC_INT, RC_INT);
    a = vtop[-1].r; b = vtop[0].r;
    vtop -= 2;
    d = get_reg(RC_INT);
    vtop++;
    w_emit_slot_load(a, VT_INT);
    w_emit_slot_load(b, VT_INT);
    switch (op) {
    case '+':  o = W_I32_ADD; break;
    case '-':  o = W_I32_SUB; break;
    case '*':  o = W_I32_MUL; break;
    case '/':  o = W_I32_DIV_S; break;
    case TOK_PDIV: o = W_I32_DIV_S; break;
    case TOK_UDIV: o = W_I32_DIV_U; break;
    case '%':  o = W_I32_REM_S; break;
    case TOK_UMOD: o = W_I32_REM_U; break;
    case '&':  o = W_I32_AND; break;
    case '|':  o = W_I32_OR; break;
    case '^':  o = W_I32_XOR; break;
    case TOK_SHL: o = W_I32_SHL; break;
    case TOK_SHR: o = W_I32_SHR_U; break;
    case TOK_SAR: o = W_I32_SHR_S; break;
    default:
        tcc_error("wasm: unimp gen_opi(%s)", get_tok_str(op, NULL));
        return;
    }
    w_ins(o);
    w_emit_slot_store(d, VT_INT);
    vtop[0].r = d;
}

ST_FUNC void gen_opl(int op)
{
    int a, b, a2, b2, d, d2, o;
    if (op >= TOK_ULT && op <= TOK_GT) {
        /* 64-bit compares are decomposed by tccgen (gen_opl) using
           gvtst/gjmp — we never get here for compares */
        tcc_error("wasm: unimp gen_opl compare");
    }
    switch (op) {
    case '/': case TOK_PDIV: case TOK_UDIV:
    case '%': case TOK_UMOD:
        /* tccgen routes these to __divdi3-style helpers (imports) */
        tcc_error("wasm: 64-bit div needs helper import");
        return;
    default:
        break;
    }
    gv2(RC_INT, RC_INT);
    a = vtop[-1].r; a2 = vtop[-1].r2;   /* actual high registers — get_reg
                                           allocates the second word
                                           independently, NOT a+1 */
    b = vtop[0].r; b2 = vtop[0].r2;
    d = get_reg(RC_INT);   /* while a/a2/b/b2 are on the vstack (busy) */
    vtop -= 2;
    vtop++;
    vtop[0].r = d;             /* mark d busy so d2 is distinct */
    d2 = get_reg(RC_INT);
    vtop[0].r2 = d2;
    /* combine low words into i64, op, split back */
    switch (op) {
    case '+': o = W_I64_ADD; break;
    case '-': o = W_I64_SUB; break;
    case '*': o = W_I64_MUL; break;
    case '&': o = W_I64_AND; break;
    case '|': o = W_I64_OR; break;
    case '^': o = W_I64_XOR; break;
    case TOK_SHL: o = W_I64_SHL; break;
    case TOK_SHR: o = W_I64_SHR_U; break;
    case TOK_SAR: o = W_I64_SHR_S; break;
    default:
        tcc_error("wasm: unimp gen_opl(%s)", get_tok_str(op, NULL));
        return;
    }
    /* low: (i64)a, high: (i64)b<<32 | low — combine both operands */
    w_emit_slot_load(a, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_emit_slot_load(a2, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHL);
    w_ins(W_I64_OR);
    w_emit_slot_load(b, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_emit_slot_load(b2, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHL);
    w_ins(W_I64_OR);
    w_ins(o);
    /* split: low = wrap, high = shr 32 */
    w_ins(W_I32_WRAP);
    w_emit_slot_store(d, VT_INT);
    w_emit_slot_load(a, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_emit_slot_load(a2, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHL);
    w_ins(W_I64_OR);
    w_emit_slot_load(b, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_emit_slot_load(b2, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHL);
    w_ins(W_I64_OR);
    w_ins(o);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHR_U);
    w_ins(W_I32_WRAP);
    w_emit_slot_store(d2, VT_INT);
}

ST_FUNC void gen_opf(int op)
{
    int a, b, d, dbl, o, cmp;
    gv2(RC_FLOAT, RC_FLOAT);
    dbl = (vtop->type.t & (VT_BTYPE | VT_LONG)) == VT_DOUBLE ||
          (vtop->type.t & VT_BTYPE) == VT_LDOUBLE;   /* LDOUBLE_SIZE=8 */
    a = vtop[-1].r; b = vtop[0].r;
    vtop--;
    if (op >= TOK_ULT && op <= TOK_GT) {
        /* float comparisons: compute 0/1 immediately (like riscv) */
        w_emit_slot_load(a, dbl ? VT_DOUBLE : VT_FLOAT);
        w_emit_slot_load(b, dbl ? VT_DOUBLE : VT_FLOAT);
        switch (op) {
        case TOK_EQ:  cmp = dbl ? W_F64_EQ  : W_F32_EQ;  break;
        case TOK_NE:  cmp = dbl ? W_F64_NE  : W_F32_NE;  break;
        case TOK_LT:  cmp = dbl ? W_F64_LT  : W_F32_LT;  break;
        case TOK_GT:  cmp = dbl ? W_F64_GT  : W_F32_GT;  break;
        case TOK_LE:  cmp = dbl ? W_F64_LE  : W_F32_LE;  break;
        case TOK_GE:  cmp = dbl ? W_F64_GE  : W_F32_GE;  break;
        default: tcc_error("wasm: float cmp %s", get_tok_str(op, NULL)); return;
        }
        w_ins(cmp);
        /* store 0/1 into an int slot and leave it as a plain register
           value: tccgen's gvtst will convert it to VT_CMP against an
           actual zero constant (we have no zero register, so a
           deferred cmp_r = d|0<<8 would read slot 0 = REG_IRET) */
        d = get_reg(RC_INT);
        w_emit_slot_store(d, VT_INT);
        vtop[0].r = d;
        vtop[0].type.t = VT_INT;
        return;
    }
    d = get_reg(RC_FLOAT);
    w_emit_slot_load(a, dbl ? VT_DOUBLE : VT_FLOAT);
    w_emit_slot_load(b, dbl ? VT_DOUBLE : VT_FLOAT);
    switch (op) {
    case '+': o = dbl ? W_F64_ADD : W_F32_ADD; break;
    case '-': o = dbl ? W_F64_SUB : W_F32_SUB; break;
    case '*': o = dbl ? W_F64_MUL : W_F32_MUL; break;
    case '/': o = dbl ? W_F64_DIV : W_F32_DIV; break;
    default: tcc_error("wasm: unimp gen_opf(%s)", get_tok_str(op, NULL)); return;
    }
    w_ins(o);
    w_emit_slot_store(d, dbl ? VT_DOUBLE : VT_FLOAT);
    vtop[0].r = d;
}

ST_FUNC void gen_cvt_csti(int t)
{
    /* value is in a register slot; mask/sign-extend in place */
    int r = gv(RC_INT), bt = t & VT_BTYPE;
    if (bt == VT_SHORT) {
        if (t & VT_UNSIGNED) {
            w_emit_slot_load(r, VT_INT);
            w_i32_const(0xffff);
            w_ins(W_I32_AND);
            w_emit_slot_store(r, VT_INT);
        } else {
            w_emit_slot_load(r, VT_INT);
            w_ins(W_I32_EXTEND16_S);
            w_emit_slot_store(r, VT_INT);
        }
    } else if (bt == VT_BYTE || bt == VT_BOOL) {
        if (t & VT_UNSIGNED) {
            w_emit_slot_load(r, VT_INT);
            w_i32_const(0xff);
            w_ins(W_I32_AND);
            w_emit_slot_store(r, VT_INT);
        } else {
            w_emit_slot_load(r, VT_INT);
            w_ins(W_I32_EXTEND8_S);
            w_emit_slot_store(r, VT_INT);
        }
    } else {
        w_emit_slot_load(r, VT_INT);
        w_i32_const(0xffffffff);
        w_ins(W_I32_AND);
        w_emit_slot_store(r, VT_INT);
    }
}

ST_FUNC void gen_cvt_itof(int t)
{
    int r = gv(RC_INT), d;
    vtop--;
    d = get_reg(RC_FLOAT);
    vtop++;
    w_emit_slot_load(r, VT_INT);
    if (vtop->type.t & VT_UNSIGNED)
        w_ins(t == VT_DOUBLE ? W_F64_CONVERT_I32_U : W_F32_CONVERT_I32_U);
    else
        w_ins(t == VT_DOUBLE ? W_F64_CONVERT_I32_S : W_F32_CONVERT_I32_S);
    w_emit_slot_store(d, t == VT_DOUBLE ? VT_DOUBLE : VT_FLOAT);
    vtop[0].r = d;
}

ST_FUNC void gen_cvt_ftoi(int t)
{
    int ft = vtop->type.t & VT_BTYPE;
    int r = gv(RC_FLOAT), d;
    vtop--;
    d = get_reg(RC_INT);
    vtop++;
    w_emit_slot_load(r, (ft == VT_DOUBLE || ft == VT_LDOUBLE) ? VT_DOUBLE : VT_FLOAT);
    if (t & VT_UNSIGNED)
        w_ins((ft == VT_DOUBLE || ft == VT_LDOUBLE) ? W_I32_TRUNC_F64_U : W_I32_TRUNC_F32_U);
    else
        w_ins((ft == VT_DOUBLE || ft == VT_LDOUBLE) ? W_I32_TRUNC_F64_S : W_I32_TRUNC_F32_S);
    w_emit_slot_store(d, VT_INT);
    vtop[0].r = d;
}

ST_FUNC void gen_cvt_ftof(int dt)
{
    int st = vtop->type.t & VT_BTYPE;
    int r = gv(RC_FLOAT), d;
    dt &= VT_BTYPE;
    if (st == dt)
        return;
    vtop--;
    d = get_reg(RC_FLOAT);
    vtop++;
    w_emit_slot_load(r, st == VT_DOUBLE ? VT_DOUBLE : VT_FLOAT);
    w_ins(dt == VT_DOUBLE ? W_F64_PROMOTE : W_F32_DEMOTE);
    w_emit_slot_store(d, dt == VT_DOUBLE ? VT_DOUBLE : VT_FLOAT);
    vtop[0].r = d;
}

ST_FUNC void gen_cvt_sxtw(void)
{
    tcc_error("wasm: sxtw unused");
}

/* register an env import (name, sig); one entry per distinct pair */
static int w_import_get(const char *name, int sig)
{
    int i;
    for (i = 0; i < wasm_nimports; i++)
        if (wasm_imports[i].sig == sig &&
            !strcmp(wasm_imports[i].name, name))
            return i;
    wasm_imports = wa_grow(wasm_imports, &wasm_import_alloc,
                           wasm_nimports + 1, sizeof(WasmImport));
    wasm_imports[wasm_nimports].name = tcc_strdup(name);
    wasm_imports[wasm_nimports].sig = sig;
    return wasm_nimports++;
}

/* signature for a call: declared fixed params plus, for variadic
   functions, this call's actual (promoted) vararg types.  Default
   promotions (float→double etc.) are applied by the frontend
   (gfunc_param_typed) before we get here. */
static int w_call_sig(CType *ft, int nb_args, int *pnparams, int *pvariadic,
                   int *pnfixed)
{
    Sym *s;
    unsigned char params[64];
    int n = 0, res, nres, variadic;
    int n_fixed = 0, i;

    variadic = (ft->ref->f.func_type == FUNC_ELLIPSIS ||
                ft->ref->f.func_type == FUNC_OLD);
    if ((ft->ref->type.t & VT_BTYPE) == VT_STRUCT)
        params[n++] = VAL_I32;   /* sret pointer */
    for (s = ft->ref->next; s; s = s->next) {
        if (n >= 60)
            tcc_error("wasm: too many params");
        if ((s->type.t & VT_BTYPE) == VT_STRUCT)
            params[n++] = VAL_I32;   /* by-pointer (v1: caller copies) */
        else
            params[n++] = w_typeof(&s->type);
        n_fixed++;
    }
    if (variadic) {
        for (i = n_fixed; i < nb_args; i++) {
            SValue *sv = &vtop[1 + i - nb_args];
            int bt = sv->type.t & VT_BTYPE;
            if (n >= 60)
                tcc_error("wasm: too many params");
            if (bt == VT_STRUCT) {
                /* small struct vararg: pass the first 4 bytes as an i32
                   value (the env helpers like $__atomic_store_4 expect
                   the VALUE, not a pointer) */
                int al, sz = type_size(&sv->type, &al);
                if (sz > 4)
                    tcc_error("wasm: struct vararg larger than 4 bytes");
                params[n++] = VAL_I32;
            } else if (bt == VT_LLONG)
                params[n++] = VAL_I64;
            else if (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE)
                params[n++] = VAL_F64;
            else
                params[n++] = VAL_I32;
        }
    }
    res = w_typeof(&ft->ref->type);
    nres = ((ft->ref->type.t & VT_BTYPE) == VT_VOID) ? 0 : 1;
    *pnparams = n;
    *pvariadic = variadic;
    *pnfixed = n_fixed;
    return w_sig_get(n, params, nres, res);
}
/* ---------------------------------------------------------------- */
/* calls */

ST_FUNC void gcall_or_jmp(int docall)
{
    if (!docall)
        tcc_error("wasm: computed goto / tail call unsupported");
    tcc_error("wasm: gcall_or_jmp");
}

ST_FUNC void gfunc_call(int nb_args)
{
    int i, rt, ret_bt, res_slot;
    int *slots;
    CType *ft;
    Sym *sym;
    int sig, nparams, variadic, nfixed;
    int bt;
    int has_struct_arg = 0;
    int park_reserve = 0;

    ft = &vtop[-nb_args].type;
    sym = vtop[-nb_args].sym;
    if (!sym || (vtop[-nb_args].r & (VT_VALMASK | VT_LVAL | VT_SYM)) !=
                (VT_CONST | VT_SYM)) {
        /* indirect call through a function pointer: the callee value is
           a table index (a function-value slot).  Materialize the args
           into the parking slots, then push the callee index and emit
           call_indirect with the per-call signature's typeidx. */
        int callee_r;
        sig = w_call_sig(ft, nb_args, &nparams, &variadic, &nfixed);
        w_indir_sig(sig);
        save_regs(nb_args + 1);
        slots = tcc_malloc(nb_args * sizeof(int));
        {
            int frame8 = ((-loc + 15) & ~15) >> 3;
            int base = w_park_cursor < frame8 ? frame8 : w_park_cursor;
            int cur = base;
            /* park the callee pointer too (last slot) */
            vrotb(nb_args + 1);
            load(base + nb_args, vtop);
            callee_r = base + nb_args;
            vrott(nb_args + 1);
            for (i = 0; i < nb_args; i++) {
                vrotb(i + 1);
                bt = vtop->type.t & VT_BTYPE;
                slots[nb_args - 1 - i] = cur;
                if (bt == VT_STRUCT)
                    tcc_error("wasm: struct fn-ptr arg unsupported");
                else if (bt == VT_LLONG)
                    tcc_error("wasm: i64 fn-ptr arg unsupported");
                else {
                    load(cur, vtop);
                    cur++;
                }
                vrott(i + 1);
            }
            w_park_cursor = cur;
        }
        /* result address (for the store) goes below the args */
        ret_bt = ft->ref->type.t & VT_BTYPE;
        res_slot = (ret_bt == VT_FLOAT || ret_bt == VT_DOUBLE || ret_bt == VT_LDOUBLE) ? REG_FRET : REG_IRET;
        if (ret_bt != VT_STRUCT)
            w_emit_slot_addr(res_slot);
        /* push args in order (on top of the address) */
        for (i = 0; i < nb_args; i++) {
            SValue *sv = &vtop[1 + i - nb_args];
            int bt = sv->type.t & VT_BTYPE;
            w_emit_slot_load(slots[i], (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE) ? bt : VT_INT);
        }
        /* the callee's table index (the parked function value) */
        w_emit_slot_load(callee_r, VT_INT);
        w_ins(W_CALL_INDIRECT);
        /* 5-byte patchable type-index slot (kind 3, resolved at output) */
        {
            int tslot = ind, t;
            for (t = 0; t < 4; t++)
                g(0x80);
            g(0x00);
            wasm_cf->patches = wa_grow(wasm_cf->patches,
                &wasm_cf->npatch_alloc, wasm_cf->npatches + 1,
                sizeof(WasmPatch));
            wasm_cf->patches[wasm_cf->npatches].ofs = tslot;
            wasm_cf->patches[wasm_cf->npatches].sym = NULL;
            wasm_cf->patches[wasm_cf->npatches].name = NULL;
            wasm_cf->patches[wasm_cf->npatches].kind = 3;  /* indir typeidx */
            wasm_cf->patches[wasm_cf->npatches].sig = sig;
            wasm_cf->npatches++;
        }
        w_u32(0);                             /* table 0 */
        /* store the result */
        if (ret_bt == VT_VOID) {
        } else if (ret_bt == VT_STRUCT) {
        } else if (ret_bt == VT_FLOAT) {
            w_store(W_F32_STORE, 4, 2, 0);
        } else if (ret_bt == VT_DOUBLE || ret_bt == VT_LDOUBLE) {
            w_store(W_F64_STORE, 8, 3, 0);
        } else if (ret_bt == VT_LLONG) {
            w_local_set(W_SCRATCH_I64);
            w_local_get(W_SCRATCH_I64);
            w_ins(W_I32_WRAP);
            w_store(W_I32_STORE, 4, 2, 0);
            w_emit_slot_addr(REG_IRE2);
            w_local_get(W_SCRATCH_I64);
            w_ins(W_I64_CONST); w_u32(32);
            w_ins(W_I64_SHR_U);
            w_ins(W_I32_WRAP);
            w_store(W_I32_STORE, 4, 2, 0);
        } else {
            w_store(W_I32_STORE, 4, 2, 0);
        }
        tcc_free(slots);
        vtop -= nb_args + 1;
        return;
    }
    sig = w_call_sig(ft, nb_args, &nparams, &variadic, &nfixed);
    /* Register the env import (name, sig).  Variadic calls get one import
       per distinct signature — wasm allows same-name imports with
       different types, and the shell's JS runtime takes (...args).
       Functions defined later in this TU shadow the import at output. */
    w_import_get(get_tok_str(sym->v, NULL), sig);
    /* spill live register values (e.g. a previous call's result in the
       shared return slot) so the callee can clobber them */
    save_regs(nb_args + 1);

    slots = tcc_malloc(nb_args * sizeof(int));
    {
        /* the parking region must sit BELOW the frame; locals are
           allocated during the body, so re-derive the frame bottom at
           every call (the prolog cursor is stale once a local exists) */
        int frame8 = ((-loc + 15) & ~15) >> 3;
        int base = w_park_cursor < frame8 ? frame8 : w_park_cursor;
        int cur = base;
        for (i = 0; i < nb_args; i++) {
            vrotb(i + 1);
            /* the parked value is vtop[0] after the rotation — its OWN
               type decides the parking width (the sv of the fixed-arg
               loop can differ: the args are processed bottom-up) */
            bt = vtop->type.t & VT_BTYPE;
            slots[nb_args - 1 - i] = cur;
            if (bt == VT_STRUCT && i >= nfixed && variadic) {
                /* small struct VARARG: pass the first 4 bytes as an
                   i32 VALUE (the env helpers expect the value, not a
                   pointer — w_call_sig used VAL_I32 for this arg) */
                int al, sz = type_size(&vtop->type, &al);
                if (sz > 4)
                    tcc_error("wasm: struct vararg larger than 4 bytes");
                if (!(vtop->r & VT_LVAL))
                    tcc_error("wasm: struct vararg not an lvalue");
                w_emit_addr(vtop);
                w_load(W_I32_LOAD, 2, 0);
                w_emit_slot_store(cur, VT_INT);
                has_struct_arg = 1;
                cur++;
            } else if (bt == VT_STRUCT) {
                /* by-value struct arg: copy to a parking temp
                   (memory.copy), park the temp's address as the i32
                   arg — the callee stores the pointer in the param
                   home and member access derefs through it */
                int al, sz = type_size(&vtop->type, &al);
                int words = (sz + 7) >> 3;
                /* the copy runs UP from the base for sz bytes, so the
                   base must sit a full words*8 BELOW the parking slot
                   (fp-8*(cur+1)) or the copy overwrites the address
                   parked there (structs > 8 bytes) */
                int temp_ofs = -(8 * (cur + 1)) - (words << 3);
                if (!(vtop->r & VT_LVAL))
                    tcc_error("wasm: struct arg not an lvalue");
                w_local_get(wasm_cf->nparams);
                w_i32_const(temp_ofs);
                w_ins(W_I32_ADD);                      /* [dst] */
                w_emit_addr(vtop);                     /* [dst, src] */
                w_mem_copy(sz);                        /* [dst,src,len] */
                w_local_get(wasm_cf->nparams);
                w_i32_const(temp_ofs);
                w_ins(W_I32_ADD);                      /* [temp addr] */
                w_emit_slot_store(cur, VT_INT);
                has_struct_arg = 1;
                cur += words + 1;
            } else if (bt == VT_LLONG) {
                /* i64 = two 4-byte words: low at slot, high at slot+1 */
                if (vtop->r & VT_LVAL) {
                    /* the value lives in memory: park both words */
                    w_emit_addr(vtop);
                    w_load(W_I32_LOAD, 2, 0);
                    w_emit_slot_store(cur, VT_INT);
                    w_emit_addr(vtop);
                    w_i32_const(4);
                    w_ins(W_I32_ADD);
                    w_load(W_I32_LOAD, 2, 0);
                    w_emit_slot_store(cur + 1, VT_INT);
                } else if ((vtop->r & (VT_VALMASK | VT_SYM)) == VT_CONST) {
                    /* numeric 64-bit constant: park both words directly */
                    uint64_t cv = (uint64_t)vtop->c.i;
                    w_emit_slot_addr(cur);
                    w_i32_const((int)(cv & 0xffffffffu));
                    w_ins(W_I32_STORE); w_u32(2); w_u32(0);
                    w_emit_slot_addr(cur + 1);
                    w_i32_const((int)(cv >> 32));
                    w_ins(W_I32_STORE); w_u32(2); w_u32(0);
                } else if (vtop->r & VT_SYM) {
                    /* an address constant: low word = the address */
                    w_i32_patch(w_sym_is_func(vtop->sym) ? 2 : 1, vtop->sym);
                    w_emit_slot_store(cur, VT_INT);
                    w_emit_slot_addr(cur + 1);
                    w_i32_const(0);
                    w_ins(W_I32_STORE); w_u32(2); w_u32(0);
                } else if ((vtop->r & VT_VALMASK) < VT_CONST) {
                    /* register pair: r = low, r2 = high */
                    w_emit_slot_load(vtop->r & VT_VALMASK, VT_INT);
                    w_emit_slot_store(cur, VT_INT);
                    w_emit_slot_load(vtop->r2 & VT_VALMASK, VT_INT);
                    w_emit_slot_store(cur + 1, VT_INT);
                } else {
                    tcc_error("wasm: unimp i64 arg park (r=0x%x)", vtop->r);
                }
                cur += 2;
            } else {
                load(cur, vtop);
                cur++;
            }
            vrott(i + 1);
        }
        w_park_cursor = cur;
        if (has_struct_arg) {
            /* the struct temps live below the frame, i.e. below the
               caller's sp — but the callee's frame is allocated below
               sp at the CALL, so without protection the callee (and
               its own inner-call parking) clobbers the temps while
               still dereferencing them.  Reserve the whole parking
               depth by lowering sp (x86-64's sub rsp for struct args):
               the callee's frame then starts below the temps. */
            park_reserve = cur << 3;
            w_global_get();
            w_i32_const(-park_reserve);
            w_ins(W_I32_ADD);
            w_global_set();
        }
    }
    /* result address (for the store) goes below the args */
    ret_bt = ft->ref->type.t & VT_BTYPE;
    res_slot = (ret_bt == VT_FLOAT || ret_bt == VT_DOUBLE || ret_bt == VT_LDOUBLE) ? REG_FRET : REG_IRET;
    if (ret_bt != VT_STRUCT)
        w_emit_slot_addr(res_slot);
    /* push args in order (on top of the address) */
    for (i = 0; i < nb_args; i++) {
        SValue *sv = &vtop[1 + i - nb_args];
        int bt = sv->type.t & VT_BTYPE;
        if (bt == VT_LLONG) {
            /* i64: combine the low/high words from the parking slots
               (low at slots[i], high at slots[i]+1) */
            w_emit_slot_addr(slots[i]);
            w_load(W_I32_LOAD, 2, 0);
            w_ins(W_I64_EXTEND_U);
            w_emit_slot_addr(slots[i] + 1);
            w_load(W_I32_LOAD, 2, 0);
            w_ins(W_I64_EXTEND_U);
            w_ins(W_I64_CONST); w_u32(32);
            w_ins(W_I64_SHL);
            w_ins(W_I64_OR);
        } else
            w_emit_slot_load(slots[i], (bt == VT_FLOAT || bt == VT_DOUBLE || bt == VT_LDOUBLE) ? bt : VT_INT);
    }
    w_call(sym, sig);
    /* store the result */
    if (ret_bt == VT_VOID) {
        /* nothing on the stack */
    } else if (ret_bt == VT_STRUCT) {
        /* sret: the result was written via the sret pointer by the callee;
           the frontend keeps the result as the local the pointer pointed
           to — no register store needed */
    } else if (ret_bt == VT_FLOAT) {
        w_store(W_F32_STORE, 4, 2, 0);
    } else if (ret_bt == VT_DOUBLE) {
        w_store(W_F64_STORE, 8, 3, 0);
    } else if (ret_bt == VT_LLONG) {
        /* pair store: the i64 result (on the wasm stack above the
           result-slot address) splits into the two 4-byte return
           slots (REG_IRET low, REG_IRE2 high).  An 8-byte store at
           slot 0 would miss slot 1 — the register slots sit 8 bytes
           apart (reg_ofs(r) = -8*(r+1)). */
        w_local_set(W_SCRATCH_I64);
        w_local_get(W_SCRATCH_I64);
        w_ins(W_I32_WRAP);
        w_store(W_I32_STORE, 4, 2, 0);          /* low  -> slot 0 */
        w_emit_slot_addr(REG_IRE2);
        w_local_get(W_SCRATCH_I64);
        w_ins(W_I64_CONST); w_u32(32);
        w_ins(W_I64_SHR_U);
        w_ins(W_I32_WRAP);
        w_store(W_I32_STORE, 4, 2, 0);          /* high -> slot 1 */
    } else {
        w_store(W_I32_STORE, 4, 2, 0);
    }
    if (has_struct_arg) {
        /* release the reserved depth: sp += park_reserve */
        w_global_get();
        w_i32_const(park_reserve);
        w_ins(W_I32_ADD);
        w_global_set();
    }
    tcc_free(slots);
    vtop -= nb_args + 1;
}

ST_FUNC void ggoto(void)
{
    /* computed goto (GCC extension): vtop = the label's address
       (&&lbl).  A CONSTANT label (goto *&&lbl) is a direct pc-set to
       the label's sub (patched at layout — the label may be defined
       later); a runtime value (goto *gostring[i]) is dispatched by
       setting pc to the loaded sub index and branching back to the
       dispatcher. */
    if (!wasm_cf)
        return;
    if (nocode_wanted & 0xFFFF) {
        vtop--;
        return;
    }
    if ((vtop->r & VT_VALMASK) == VT_CONST && vtop->sym) {
        /* constant: the label's sub index is a kind-4 patch (captured
           to dsec = -2 at layout, when the label's jind is final) */
        w_i32_patch(4, vtop->sym);
        vtop--;
        w_local_set(W_PC_LOCAL);
    } else {
        /* runtime: the label value (a sub index) becomes the pc */
        int r = gv(RC_INT);
        vtop--;
        w_emit_slot_load(r, VT_INT);
        w_local_set(W_PC_LOCAL);
    }
    w_add_edge(EDGE_UNCOND, -2, w_new_label());
    w_new_block();
}

/* ---------------------------------------------------------------- */
/* stubs */

ST_FUNC void gen_va_start(void)
{
    tcc_error("wasm: varargs unsupported");
}

ST_FUNC void gen_va_arg(CType *t)
{
    tcc_error("wasm: varargs unsupported");
}

ST_FUNC void gen_fill_nops(int bytes)
{
    tcc_error("wasm: alignment requested (%d bytes)", bytes);
}

ST_FUNC void gen_clear_cache(void)
{
}

ST_FUNC void gen_increment_tcov(SValue *sv)
{
}

ST_FUNC void gen_vla_sp_save(int addr)
{
    /* [fp+addr] = sp — the restore point (or the VLA's base after an
       alloc: gen_vla_alloc lowers sp, then this saves the new sp) */
    w_local_get(wasm_cf->nparams);
    w_i32_const(addr);
    w_ins(W_I32_ADD);
    w_global_get();
    w_store(W_I32_STORE, 4, 2, 0);
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
    /* sp = [fp+addr] */
    w_local_get(wasm_cf->nparams);
    w_i32_const(addr);
    w_ins(W_I32_ADD);
    w_load(W_I32_LOAD, 2, 0);
    w_global_set();
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    /* vtop = the allocation size (bytes): sp -= size, aligned down.
       The new sp IS the VLA's base — the frontend's following
       gen_vla_sp_save stores it into the VLA's local slot. */
    int r = gv(RC_INT);
    int a = align > 8 ? align : 8;
    w_global_get();
    w_emit_slot_load(r, VT_INT);
    w_ins(W_I32_SUB);
    if (a > 1) {
        w_i32_const(-a);
        w_ins(W_I32_AND);
    }
    w_global_set();
    vpop();
}

ST_FUNC void gen_bounds_call(int v)
{
    tcc_error("wasm: bounds checking unsupported");
}

ST_FUNC void gen_bounds_prolog(void)
{
}

ST_FUNC void gen_bounds_epilog(void)
{
}

ST_FUNC void gen_bounds_check(void)
{
}

/* ---------------------------------------------------------------- */
/* layout: the pc state machine                                     */

typedef struct WPart { int seg, start, end; } WPart;
typedef struct WSub {
    int blk, start, end;
    WPart *parts; int nparts, part_alloc;
} WSub;

static void w_layout(void)
{
    int nb = wasm_cf->nblks, i, j, k;
    WSub *subs = NULL; int nsubs = 0, sub_alloc = 0;
    int *cnt = NULL;
    unsigned char *body = NULL; int blen = 0, balloc = 0;
    int ret_valtype;

    /* 1. collect split positions per block (from labels) */
    cnt = tcc_mallocz(nb * sizeof(int) * (W_BLK_SPLITS + 1));
    for (i = 0; i < wasm_cf->nlabels; i++) {
        int pos = wasm_cf->labels[i].pos;
        if (pos >= 0) {
            int b = w_block_at_pos(pos);
            int c = cnt[b * (W_BLK_SPLITS + 1)], m, dup = 0;
            for (m = 0; m < c; m++)
                if (cnt[b * (W_BLK_SPLITS + 1) + 1 + m] == pos)
                    dup = 1;
            if (!dup && c < W_BLK_SPLITS)
                cnt[b * (W_BLK_SPLITS + 1) + 1 + (cnt[b * (W_BLK_SPLITS + 1)]++)] = pos;
        }
    }
    /* also split at &&label positions: each address-taken label must be
       a sub boundary so its VALUE (the sub index, the pc dispatch) is
       resolvable.  The kind-4 patches (goto *&&lbl) carry the label
       sym; the data/rodata relocs reference labels whose symtab
       entries land in this function's code range. */
    {
        int p2;
        for (p2 = 0; p2 < wasm_cf->npatches; p2++) {
            WasmPatch *pp = &wasm_cf->patches[p2];
            if (pp->kind == 4 && pp->sym && pp->sym->jind > 0) {
                int pos = pp->sym->jind;
                if (pos < ind) {
                    int b = w_block_at_pos(pos);
                    int c = cnt[b * (W_BLK_SPLITS + 1)], m, dup = 0;
                    for (m = 0; m < c; m++)
                        if (cnt[b * (W_BLK_SPLITS + 1) + 1 + m] == pos)
                            dup = 1;
                    if (!dup && c < W_BLK_SPLITS)
                        cnt[b * (W_BLK_SPLITS + 1) + 1 + (cnt[b * (W_BLK_SPLITS + 1)]++)] = pos;
                }
            }
        }
    }
    for (i = 0; i < nb; i++) {
        int c = cnt[i * (W_BLK_SPLITS + 1)];
        for (j = 1; j < c; j++)
            for (k = j; k > 0 && cnt[i * (W_BLK_SPLITS + 1) + 1 + k - 1] > cnt[i * (W_BLK_SPLITS + 1) + 1 + k]; k--) {
                int t = cnt[i * (W_BLK_SPLITS + 1) + 1 + k - 1];
                cnt[i * (W_BLK_SPLITS + 1) + 1 + k - 1] = cnt[i * (W_BLK_SPLITS + 1) + 1 + k];
                cnt[i * (W_BLK_SPLITS + 1) + 1 + k] = t;
            }
    }

    /* 2. build sub-blocks (split each block at its positions) */
    for (i = 0; i < nb; i++) {
        int seg, si = 0, c = cnt[i * (W_BLK_SPLITS + 1)];
        subs = wa_grow(subs, &sub_alloc, nsubs + 1, sizeof(WSub));
        memset(&subs[nsubs], 0, sizeof(WSub));
        subs[nsubs].blk = i;
        subs[nsubs].start = wasm_cf->segs[wasm_cf->blks[i].fs].start;
        nsubs++;
        for (seg = wasm_cf->blks[i].fs; seg <= wasm_cf->blks[i].ls; seg++) {
            int pos = wasm_cf->segs[seg].start;
            int segend = wasm_cf->segs[seg].end;
            while (si < c && cnt[i * (W_BLK_SPLITS + 1) + 1 + si] < segend) {
                int p = cnt[i * (W_BLK_SPLITS + 1) + 1 + si];
                if (p > pos) {
                    WSub *s = &subs[nsubs - 1];
                    s->parts = wa_grow(s->parts, &s->part_alloc,
                                       s->nparts + 1, sizeof(WPart));
                    s->parts[s->nparts].seg = seg;
                    s->parts[s->nparts].start = pos;
                    s->parts[s->nparts].end = p;
                    s->nparts++;
                }
                subs = wa_grow(subs, &sub_alloc, nsubs + 1, sizeof(WSub));
                memset(&subs[nsubs], 0, sizeof(WSub));
                subs[nsubs].blk = i;
                subs[nsubs].start = p;
                nsubs++;
                pos = p;
                si++;
            }
            if (pos < segend ||
                (pos == segend && wasm_cf->segs[seg].edge >= 0)) {
                /* include empty segments that carry an edge (e.g. the
                   edge-only segment of a plain gjmp) so their jump is
                   not lost */
                WSub *s = &subs[nsubs - 1];
                s->parts = wa_grow(s->parts, &s->part_alloc,
                                   s->nparts + 1, sizeof(WPart));
                s->parts[s->nparts].seg = seg;
                s->parts[s->nparts].start = pos;
                s->parts[s->nparts].end = segend;
                s->nparts++;
            }
        }
        /* set end positions */
        for (seg = 0; seg < nsubs; seg++)
            if (subs[seg].blk == i) {
                int last = subs[seg].nparts - 1;
                subs[seg].end = last >= 0 ? subs[seg].parts[last].end
                                          : subs[seg].start;
            }
    }
    /* fix: end of each sub = end of its last part (done above) */

    /* 3. resolve labels to sub-blocks.  The first pass maps the label to
       the sub whose range CONTAINS pos; the second handles pos == a
       sub's START when that sub's range is empty (an edge-only sub like
       the loop back-edge — [266,266) — containment skips it, but the
       label at 266 must reach the back-edge, not the exit block that
       merely follows it: without this the COND edge's fallthrough
       jumped to the exit and loops exited after one iteration). */
    for (i = 0; i < wasm_cf->nlabels; i++) {
        int pos = wasm_cf->labels[i].pos;
        wasm_cf->labels[i].sub = -1;
        if (pos < 0)
            continue;
        for (j = nsubs - 1; j >= 0; j--)
            if (pos >= subs[j].start && pos < subs[j].end) {
                wasm_cf->labels[i].sub = j;
                break;
            }
        /* resolve by the label's SEGMENT first, but ACCEPT the result
           only when the sub's code range covers the label's POSITION —
           the seg heuristic (cur_seg+1 for gsym, seg-at-pos for the
           backward jumps) is right for the multi-segment boundaries
           (45_empty_for's back-edge vs exit both at 266) but can
           overshoot when no edge intervenes (05_array's exit label —
           seg 7 points at 472 while the label sits at 441).  A
           mismatched sub is discarded and the position containment
           used instead. */
        if (wasm_cf->labels[i].seg >= 0) {
            for (j = 0; j < nsubs; j++)
                for (k = 0; k < subs[j].nparts; k++)
                    if (subs[j].parts[k].seg == wasm_cf->labels[i].seg) {
                        if (pos >= subs[j].start && pos < subs[j].end) {
                            wasm_cf->labels[i].sub = j;
                            break;
                        }
                        /* sub starts at pos (an edge-only boundary sub) */
                        if (pos == subs[j].start) {
                            wasm_cf->labels[i].sub = j;
                            break;
                        }
                    }
        }
        if (wasm_cf->labels[i].sub < 0)
            for (j = nsubs - 1; j >= 0; j--)
                if (pos >= subs[j].start && pos < subs[j].end) {
                    wasm_cf->labels[i].sub = j;
                    break;
                }
        if (wasm_cf->labels[i].sub < 0) {
            /* pos == the end of the last sub of a block: use it */
            for (j = nsubs - 1; j >= 0; j--)
                if (pos >= subs[j].start && pos <= subs[j].end &&
                    (j + 1 >= nsubs || subs[j + 1].blk != subs[j].blk)) {
                    wasm_cf->labels[i].sub = j;
                    break;
                }
        }        if (wasm_cf->labels[i].sub < 0)
            tcc_error("wasm: internal: unresolved label at %d", pos);
    }

    /* capture the &&label patches: at LAYOUT time the label sym's jind
       is its final code position (the sym is freed/reused by output
       time).  The VALUE resolves to the SUB INDEX of the code at that
       position (the pc dispatch is by sub).  kind-1 patches with a
       label sym (c == 0 for code-only &&refs, or a text-section
       symtab entry for address-taken labels). */
    for (i = 0; i < wasm_cf->npatches; i++) {
        WasmPatch *pp = &wasm_cf->patches[i];
        if (pp->kind == 4 && pp->sym && pp->sym->jind > 0) {
            /* a constant computed goto (goto *&&lbl): the label's
               position (jind > 0 — strings/data syms never set it) */
            pp->dofs = pp->sym->jind;
            pp->dsec = -2;
        } else if (pp->kind == 1 && pp->sym && pp->sym->jind > 0 &&
            pp->sym->c > 0 &&
            pp->sym->c < (int)(symtab_section->data_offset / sizeof(ElfSym)) &&
            elfsym(pp->sym)->st_shndx == text_section->sh_num) {
            pp->dofs = pp->sym->jind;
            pp->dsec = -2;
        }
    }

    /* 4. emit the final body */    ret_valtype = w_typeof(&func_vt);
    if ((func_vt.t & VT_BTYPE) == VT_VOID)
        ret_valtype = 0;
    if ((func_vt.t & VT_BTYPE) == VT_LLONG)
        ret_valtype = VAL_I64;

    /* prolog bytes */
    for (i = 0; i < w_body_start; i++) {
        body = wa_grow(body, &balloc, blen + 1, 1);
        body[blen++] = wasm_cf->code[i];
    }
    /* pc = 0 */
    body = wa_grow(body, &balloc, blen + 8, 1);
    body[blen++] = W_I32_CONST; body[blen++] = 0;
    body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
    /* block $exit (result T) / loop / switch */
    body = wa_grow(body, &balloc, blen + 8, 1);
    body[blen++] = W_BLOCK;
    body[blen++] = ret_valtype ? ret_valtype : BLOCKTYPE_EMPTY;
    body[blen++] = W_LOOP; body[blen++] = BLOCKTYPE_EMPTY;
    body[blen++] = W_BLOCK; body[blen++] = BLOCKTYPE_EMPTY;
    /* nested pads */
    for (i = 0; i < nsubs; i++) {
        body = wa_grow(body, &balloc, blen + 2, 1);
        body[blen++] = W_BLOCK; body[blen++] = BLOCKTYPE_EMPTY;
    }
    /* br_table: local.get $pc; br_table (labels, default=switch).
       The count and the target depths are unsigned LEBs (nsubs can
       exceed 127 when a big switch or many labels split a block —
       93_integer_promotion's br_table count overflowed a single byte
       and the validator read a huge count). */
    body = wa_grow(body, &balloc, blen + 8 + nsubs * 2, 1);
    body[blen++] = W_LOCAL_GET; body[blen++] = W_PC_LOCAL;
    body[blen++] = W_BR_TABLE;
    {
        int v = nsubs;
        do {
            unsigned char b = v & 0x7f;
            v >>= 7;
            if (v)
                b |= 0x80;
            body = wa_grow(body, &balloc, blen + 1, 1);
            body[blen++] = b;
        } while (v);
    }
    for (i = 0; i < nsubs; i++) {
        int d = nsubs - 1 - i;
        do {
            unsigned char b = d & 0x7f;
            d >>= 7;
            if (d)
                b |= 0x80;
            body = wa_grow(body, &balloc, blen + 1, 1);
            body[blen++] = b;
        } while (d);
    }
    {
        int v = nsubs;
        do {
            unsigned char b = v & 0x7f;
            v >>= 7;
            if (v)
                b |= 0x80;
            body = wa_grow(body, &balloc, blen + 1, 1);
            body[blen++] = b;
        } while (v);
    }

    /* sub-block bodies in reverse order */
    {
        /* exact code-buffer offset -> final offset map (the layout
           reorders sub-blocks, so this must be exact, not derived) */
        typedef struct { int cs, ce, fs; } RMap;
        RMap *rmap = NULL;
        int nrmap = 0, rmap_alloc = 0;
        for (i = nsubs - 1; i >= 0; i--) {
        WSub *s = &subs[i];
        int last_has_edge = 0;
        body = wa_grow(body, &balloc, blen + 1, 1);
        body[blen++] = W_END;   /* close pad i */
        for (k = 0; k < s->nparts; k++) {
            int seg = s->parts[k].seg;
            int pos = s->parts[k].start, e = s->parts[k].end;
            if (e > pos) {
                rmap = wa_grow(rmap, &rmap_alloc, nrmap + 1, sizeof(RMap));
                rmap[nrmap].cs = pos;
                rmap[nrmap].ce = e;
                rmap[nrmap].fs = blen;
                nrmap++;
            }
            while (pos < e) {
                body = wa_grow(body, &balloc, blen + 1, 1);
                body[blen++] = wasm_cf->code[pos++];
            }
            if (e == wasm_cf->segs[seg].end && wasm_cf->segs[seg].edge >= 0) {
                WasmEdge *ed = &wasm_cf->edges[wasm_cf->segs[seg].edge];
                int target = ed->label >= 0 ? wasm_cf->labels[ed->label].sub : -1;
                int v;
                if (ed->op == -2) {
                    /* computed goto (ggoto): the pc was ALREADY set to
                       the runtime label value — just br back to the
                       dispatcher.  Checked BEFORE the nocode-suppressed
                       skip: the edge carries a DUMMY label (never
                       resolved) and the pc-set must not be emitted. */
                    body = wa_grow(body, &balloc, blen + 4, 1);
                    body[blen++] = W_BR; w_body_leb(body, &blen, &balloc, i + 1);
                    if (k == s->nparts - 1)
                        last_has_edge = 1;
                    continue;
                }
                if (ed->op == -1) {
                    /* suppressed under nocode_wanted (gjmp/gjmp_cond
                       marked it): the region is dead — emit nothing and
                       fall through to the next sub in creation order.
                       For a COND edge this also skips the 'if (cond)'
                       wrapper that would otherwise pop a stack value the
                       suppressed cond never pushed (invalid wasm). */
                    continue;
                }
                if (ed->kind == EDGE_COND) {
                    /* if (cond) { pc = L; br $dispatch } end — the br
                       leaves the current sub so the fall-through path
                       (the code after the edge) runs only when the
                       condition is false */
                    body = wa_grow(body, &balloc, blen + 20, 1);
                    body[blen++] = W_IF; body[blen++] = BLOCKTYPE_EMPTY;
                    body[blen++] = W_I32_CONST;
                    w_body_sleb(body, &blen, &balloc, target);
                    body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
                    /* the br sits inside the if: one level deeper */
                    body[blen++] = W_BR; w_body_leb(body, &blen, &balloc, i + 2);
                    body[blen++] = W_END;
                } else if (ed->kind == EDGE_UNCOND) {
                    body = wa_grow(body, &balloc, blen + 16, 1);
                    body[blen++] = W_I32_CONST;
                    w_body_sleb(body, &blen, &balloc, target);
                    body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
                    body[blen++] = W_BR; w_body_leb(body, &blen, &balloc, i + 1);
                } else {
                    body = wa_grow(body, &balloc, blen + 4, 1);
                    body[blen++] = W_BR; w_body_leb(body, &blen, &balloc, i + 2);
                }
                /* only uncond/term edges end the sub-block; a cond edge
                   leaves a fallthrough */
                if (k == s->nparts - 1 && ed->kind != EDGE_COND)
                    last_has_edge = 1;
            }
        }
        if (!last_has_edge) {
            /* implicit fallthrough to the next sub (creation order) */
            int next = i + 1;
            int v = next < nsubs ? next : 0;
            body = wa_grow(body, &balloc, blen + 16, 1);
            body[blen++] = W_I32_CONST;
            w_body_sleb(body, &blen, &balloc, v);
            body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
            body[blen++] = W_BR; w_body_leb(body, &blen, &balloc, i + 1);
        }
        }
        /* remap patch offsets via the exact range map */
        for (i = 0; i < wasm_cf->npatches; i++) {
            WasmPatch *p = &wasm_cf->patches[i];
            if (p->ofs >= w_body_start) {
                int r, ok = 0;
                for (r = 0; r < nrmap; r++) {
                    if (p->ofs >= rmap[r].cs && p->ofs < rmap[r].ce) {
                        p->ofs = rmap[r].fs + (p->ofs - rmap[r].cs);
                        ok = 1;
                        break;
                    }
                }
                if (!ok)
                    tcc_error("wasm: patch outside mapped code");
            }
        }
        tcc_free(rmap);
    }
    /* close switch / loop / exit / function */
    body = wa_grow(body, &balloc, blen + 8, 1);
    body[blen++] = W_UNREACHABLE;
    body[blen++] = W_END;   /* switch */
    body[blen++] = W_END;   /* dispatch */
    body[blen++] = W_UNREACHABLE;
    body[blen++] = W_END;   /* exit */
    body[blen++] = W_END;   /* function */

    wasm_cf->final = body;
    wasm_cf->flen = blen;
    /* position -> sub map (for &&label VALUES: the pc dispatch) */
    wasm_cf->sub_starts = tcc_malloc(nsubs * sizeof(int));
    wasm_cf->sub_ends = tcc_malloc(nsubs * sizeof(int));
    wasm_cf->nsubs_out = nsubs;
    for (i = 0; i < nsubs; i++) {
        wasm_cf->sub_starts[i] = subs[i].start;
        wasm_cf->sub_ends[i] = subs[i].end;
    }
    for (i = 0; i < nsubs; i++)
        tcc_free(subs[i].parts);
    tcc_free(subs);
    tcc_free(cnt);
}

/* ---------------------------------------------------------------- */
/* module output                                                    */

static WasmFunc *w_func_by_order(int order)
{
    int i;
    for (i = 0; i < wasm_nfunc_list; i++)
        if (wasm_func_list[i]->order == order)
            return wasm_func_list[i];
    return NULL;
}

/* sig -> type-section index mapping (built at output time) */
static int *w_sig_ti_used = NULL;
static int w_sig_ti_n = 0;
static int w_sig_typeidx_of(int sig)
{
    int i;
    for (i = 0; i < w_sig_ti_n; i++)
        if (w_sig_ti_used[i] == sig)
            return i;
    tcc_error("wasm: internal: sig not in type section");
    return 0;
}

/* resolve the linear-memory address of a data symbol */
/* section pointers must survive to output time (tcc_state is unset there) */
static Section *w_sec_data, *w_sec_rodata, *w_sec_bss;

static int w_data_addr_of(int dsec, int dofs)
{
    int base = 0;
    if (dsec == w_sec_rodata->sh_num)
        base = w_sec_data->data_offset;
    else if (dsec == w_sec_bss->sh_num)
        base = w_sec_data->data_offset + w_sec_rodata->data_offset;
    return base + dofs;
}

typedef struct WOut {
    unsigned char *data; int len, alloc;
} WOut;

static void wo_init(WOut *o) { o->data = NULL; o->len = o->alloc = 0; }
static void wo_b(WOut *o, int c)
{
    o->data = wa_grow(o->data, &o->alloc, o->len + 1, 1);
    o->data[o->len++] = c;
}
static void wo_leb(WOut *o, unsigned v)
{
    do { unsigned char b = v & 0x7f; v >>= 7;
         if (v) b |= 0x80; wo_b(o, b); } while (v);
}
static void w_sleb_out(WOut *o, int v)
{
    unsigned char b;
    int more;
    do {
        b = v & 0x7f;
        v >>= 7;
        more = !((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)));
        if (more)
            b |= 0x80;
        wo_b(o, b);
    } while (more);
}
static void wo_str(WOut *o, const char *s)
{
    int n = strlen(s);
    wo_leb(o, n);
    while (n--)
        wo_b(o, *s++);
}
static void wo_sec(WOut *o, int id, WOut *p)
{
    int i;
    wo_b(o, id);
    wo_leb(o, p->len);
    for (i = 0; i < p->len; i++)
        wo_b(o, p->data[i]);
}

static void add_used(int *used, int *pnused, int s)
{
    int m;
    for (m = 0; m < *pnused; m++)
        if (used[m] == s)
            return;
    used[(*pnused)++] = s;
}

/* resolve a section's relocations in the output data: function symbols
   become table slots (fv + 1), data symbols become linear addresses */
static void w_apply_data_relocs(TCCState *s1, Section *sec, unsigned char *out)
{
    Section *sr = sec->reloc;
    int i;
    for (i = 0; i < (int)(sr->data_offset / sizeof(ElfW_Rel)); i++) {
        ElfW_Rel *rel = &((ElfW_Rel *)sr->data)[i];
        int si = ELFW(R_SYM)(rel->r_info);
        ElfW(Sym) *es = &((ElfW(Sym) *)symtab_section->data)[si];
        unsigned char *p = out + rel->r_offset;
        char *nm = &((char *)symtab_section->link->data)[es->st_name];
        int v = 0;
        if (ELFW(ST_TYPE)(es->st_info) == STT_FUNC
            || es->st_shndx == SHN_ABS) {
            /* function address — the table slot */
            int fv = w_fv_find(nm);
            v = (fv >= 0) ? fv + 1 : 0;
        } else if (es->st_shndx == text_section->sh_num) {
            /* a &&label: the value is the SUB INDEX of the code at
               st_value, resolved in the OWNING function (the one that
               recorded the label position at gsym_addr(0, ind)) — the
               code positions are per-function, so the owner must be
               known to avoid false matches) */
            int f2, r2, found = 0;
            for (f2 = 0; f2 < wasm_nfuncs && !found; f2++) {
                WasmFunc *wfl = w_func_by_order(wasm_funcs[f2].order);
                if (!wfl)
                    continue;
                for (r2 = 0; r2 < wfl->nlblpos; r2++)
                    if (wfl->lblpos[r2] == es->st_value) {
                        /* owner found — resolve in its sub map */
                        int r3;
                        for (r3 = 0; r3 < wfl->nsubs_out; r3++)
                            if (es->st_value >= wfl->sub_starts[r3] &&
                                es->st_value < wfl->sub_ends[r3]) {
                                v = r3;
                                break;
                            }
                        found = 1;
                        break;
                    }
            }
        } else {
            v = w_data_addr_of(es->st_shndx, es->st_value);
        }
        p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
        p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
    }
}

ST_FUNC int wasm_output_file(TCCState *s, const char *filename)
{
    WOut out = {0}, sec = {0};
    FILE *f;
    int i, j, nimports, npages, data_end, stack_top;
    int *func_idx, *imp_idx;
    int main_idx = -1, start_idx = -1;
    int ndef = 0, nused = 0, used[512];
    int sig_fdwrite, sig_procexit, sig_start;
    int k;
    unsigned char p4[4] = { VAL_I32, VAL_I32, VAL_I32, VAL_I32 };

    sig_fdwrite = w_sig_get(4, p4, 0, 0);
    sig_procexit = w_sig_get(1, p4, 0, 0);
    sig_start = w_sig_get(0, p4, 0, 0);
    if (getenv("WASM_FUNC_DBG"))
        for (i = 0; i < wasm_nfuncs; i++)
            fprintf(stderr, "F: %s defined=%d import=%d order=%d\n",
                    wasm_funcs[i].name, wasm_funcs[i].defined,
                    wasm_funcs[i].import, wasm_funcs[i].order);
    tcc_enter_state(s);   /* make the section macros resolve correctly */
    /* collect function-value slots: kind-2 patches (address-of-function
       in code — a fn passed as an argument) + data/rodata relocs
       (global fn-ptr initializers like int (*f)(int) = &fred) */
    for (i = 0; i < wasm_nfuncs; i++) {
        if (!wasm_funcs[i].defined)
            continue;
        {
            WasmFunc *wf = w_func_by_order(wasm_funcs[i].order);
            if (!wf)
                continue;
            for (j = 0; j < wf->npatches; j++)
                if (wf->patches[j].kind == 2 && wf->patches[j].name) {
                    if (getenv("WASM_WK_DBG")) {
                        Sym *ss = wf->patches[j].sym;
                        fprintf(stderr, "WK %s sym=%p c=%d bind=%d\n",
                                wf->patches[j].name, (void*)ss,
                                ss ? ss->c : -1,
                                (ss && ss->c > 0 && ss->c < (int)(symtab_section->data_offset/sizeof(ElfSym)))
                                    ? ELFW(ST_BIND)(elfsym(ss)->st_info) : -1);
                    }
                    if (!w_sym_weak_undef(wf->patches[j].sym,
                                           wf->patches[j].name))
                        w_fv_slot(wf->patches[j].name);
                }
        }
    }
    for (i = 0; i < wasm_ndtors; i++)
        w_fv_slot(wasm_dtors[i]);   /* atexit registration needs a slot */
    {
        Section *sr;
        int secs[2]; secs[0] = data_section->sh_num; secs[1] = rodata_section->sh_num;
        for (k = 0; k < 2; k++) {
            sr = (secs[k] == data_section->sh_num) ? data_section->reloc
                                                   : rodata_section->reloc;
            if (!sr)
                continue;
            for (i = 0; i < (int)(sr->data_offset / sizeof(ElfW_Rel)); i++) {
                ElfW_Rel *rel = &((ElfW_Rel *)sr->data)[i];
                int si = ELFW(R_SYM)(rel->r_info);
                ElfW(Sym) *es = &((ElfW(Sym) *)symtab_section->data)[si];
                char *nm = &((char *)symtab_section->link->data)[es->st_name];
                if (ELFW(ST_TYPE)(es->st_info) == STT_FUNC ||
                    es->st_shndx == SHN_ABS)
                    w_fv_slot(nm);
            }
        }
    }


    w_sec_data = data_section;
    w_sec_rodata = rodata_section;
    w_sec_bss = bss_section;

    /* pre-pass: extern functions referenced only by address (fn-ptr
       initializers like int (*fp)(...) = &extern_fn, or &sprintf passed
       as an argument) must be env imports so they have a function
       index — the call_indirect table entry needs one.  The signature
       is the LAST indirect-call signature: the extern's call was the
       most recent call_indirect to register its per-call sig (variadic
       calls include the actual varargs). */
    {
        /* collect the extern fn-ptr names: data/rodata relocs (global
           fn-ptr initializers, &extern_fn) + kind-2 patches (a fn
           address passed as an argument in code).  Each gets an env
           import (the call_indirect table entry needs a function
           index).  The signature is the LAST indirect-call signature:
           the extern's call was the most recent call_indirect to
           register its per-call sig (variadic calls include the actual
           varargs). */
        char **want = NULL; int nwant = 0, walloc = 0;
        Section *sr;
        int secs[2]; secs[0] = data_section->sh_num; secs[1] = rodata_section->sh_num;
        for (k = 0; k < 2; k++) {
            sr = (secs[k] == data_section->sh_num) ? data_section->reloc
                                                   : rodata_section->reloc;
            if (!sr)
                continue;
            for (i = 0; i < (int)(sr->data_offset / sizeof(ElfW_Rel)); i++) {
                ElfW_Rel *rel = &((ElfW_Rel *)sr->data)[i];
                int si = ELFW(R_SYM)(rel->r_info);
                ElfW(Sym) *es = &((ElfW(Sym) *)symtab_section->data)[si];
                if (es->st_shndx != SHN_UNDEF ||
                    ELFW(ST_TYPE)(es->st_info) != STT_FUNC ||
                    ELFW(ST_BIND)(es->st_info) == STB_WEAK)
                    continue;
                want = wa_grow(want, &walloc, nwant + 1, sizeof(char *));
                want[nwant++] = &((char *)symtab_section->link->data)[es->st_name];
            }
        }
        for (i = 0; i < wasm_nfuncs; i++) {
            WasmFunc *wf = wasm_funcs[i].defined
                           ? w_func_by_order(wasm_funcs[i].order) : NULL;
            if (!wf)
                continue;
            for (j = 0; j < wf->npatches; j++)
                if (wf->patches[j].kind == 2 && wf->patches[j].name &&
                    !w_sym_weak_undef(wf->patches[j].sym,
                                      wf->patches[j].name)) {
                    int isdef = 0;
                    for (k = 0; k < wasm_nfuncs; k++)
                        if (wasm_funcs[k].defined &&
                            !strcmp(wasm_funcs[k].name, wf->patches[j].name)) {
                            isdef = 1;
                            break;
                        }
                    if (!isdef) {
                        want = wa_grow(want, &walloc, nwant + 1, sizeof(char *));
                        want[nwant++] = wf->patches[j].name;
                    }
                }
        }
        for (i = 0; i < nwant; i++) {
            const char *nm = want[i];
            int found = 0, q2;
            for (q2 = 0; q2 < wasm_nimports; q2++)
                if (!strcmp(wasm_imports[q2].name, nm)) {
                    found = 1;
                    break;
                }
            if (found)
                continue;
            /* the extern's table slot (its fv index) is popped by ITS
               call_indirect — the sigs register in call order, so the
               extern's fv index maps to its call's per-call sig */
            int fvi = w_fv_find(nm);
            int sig = 0;
            if (wasm_nindir > 0) {
                if (fvi >= 0 && fvi < wasm_nindir)
                    sig = wasm_indir_sigs[fvi];
                else
                    sig = wasm_indir_sigs[wasm_nindir - 1];
            }
            w_import_get(nm, sig);
        }
    }

    func_idx = tcc_mallocz(wasm_nfuncs * sizeof(int));
    imp_idx = tcc_mallocz(wasm_nimports * sizeof(int));
    nimports = 2;
    for (i = 0; i < wasm_nimports; i++) {
        int defined = 0;
        for (k = 0; k < wasm_nfuncs; k++)
            if (wasm_funcs[k].defined &&
                !strcmp(wasm_funcs[k].name, wasm_imports[i].name)) {
                defined = 1;
                break;
            }
        imp_idx[i] = defined ? -1 : nimports++;
        if (getenv("WASM_FUNC_DBG") && defined)
            fprintf(stderr, "IMP: %s -> defined (dropped)\n", wasm_imports[i].name);
    }
    for (i = 0; i < wasm_nfuncs; i++) {
        if (wasm_funcs[i].defined) {
            func_idx[i] = nimports + wasm_funcs[i].order;
            ndef++;
            if (!strcmp(wasm_funcs[i].name, "main"))
                main_idx = func_idx[i];
        }
    }
    if (main_idx >= 0)
        start_idx = nimports + ndef;

    /* ---- type section ---- */
    wo_init(&sec);
    nused = 0;
    for (i = 0; i < wasm_nfuncs; i++)
        if (wasm_funcs[i].sig >= 0)
            add_used(used, &nused, wasm_funcs[i].sig);
    for (i = 0; i < wasm_nimports; i++)
        add_used(used, &nused, wasm_imports[i].sig);
    if (main_idx >= 0)
        add_used(used, &nused, sig_start);
    for (i = 0; i < wasm_nindir; i++)
        add_used(used, &nused, wasm_indir_sigs[i]);
    add_used(used, &nused, sig_fdwrite);
    add_used(used, &nused, sig_procexit);
    wo_leb(&sec, nused);
    for (k = 0; k < nused; k++) {
        WasmSig *sg = &wasm_sigs[used[k]];
        const char *sp = sg->sig;
        int nparams = 0;
        while (*sp && *sp != ':') { nparams++; sp++; }
        wo_b(&sec, 0x60);
        wo_leb(&sec, nparams);
        sp = sg->sig;
        while (*sp && *sp != ':')
            wo_b(&sec, (unsigned char)*sp++);
        if (*sp == ':' && sp[1]) {
            wo_b(&sec, 1);
            wo_b(&sec, (unsigned char)sp[1]);
        } else {
            wo_b(&sec, 0);
        }
    }
    w_sig_ti_used = used;
    w_sig_ti_n = nused;
    wo_sec(&out, 1, &sec);

    /* ---- import section ---- */
    wo_init(&sec);
    wo_leb(&sec, nimports);
    wo_str(&sec, "wasi_snapshot_preview1");
    wo_str(&sec, "fd_write");
    wo_b(&sec, 0); wo_leb(&sec, w_sig_typeidx_of(sig_fdwrite));
    wo_str(&sec, "wasi_snapshot_preview1");
    wo_str(&sec, "proc_exit");
    wo_b(&sec, 0); wo_leb(&sec, w_sig_typeidx_of(sig_procexit));
    for (i = 0; i < wasm_nimports; i++) {
        if (imp_idx[i] < 0)
            continue;   /* defined in this module — no import */
        {
            char nm[300];
            snprintf(nm, sizeof nm, "$%s", wasm_imports[i].name);
            wo_str(&sec, "env");
            wo_str(&sec, nm);
            wo_b(&sec, 0);
            wo_leb(&sec, w_sig_typeidx_of(wasm_imports[i].sig));
        }
    }
    wo_sec(&out, 2, &sec);

    /* ---- function section ---- */
    wo_init(&sec);
    {
        int need_ctors = (wasm_nctors > 0 ? 1 : 0) + (wasm_ndtors > 0 ? 1 : 0);
        wo_leb(&sec, ndef + (main_idx >= 0 ? 1 : 0) + need_ctors);
        for (i = 0; i < wasm_nfuncs; i++)
            if (wasm_funcs[i].defined)
                wo_leb(&sec, w_sig_typeidx_of(wasm_funcs[i].sig));
        if (main_idx >= 0)
            wo_leb(&sec, w_sig_typeidx_of(sig_start));
        for (i = 0; i < need_ctors; i++)
            wo_leb(&sec, w_sig_typeidx_of(sig_start));
    }
    wo_sec(&out, 3, &sec);

    /* ---- table section ---- */
    if (wasm_nfv > 0 || wasm_nindir > 0) {
        /* (table N funcref) — needed by call_indirect even when no
           function ADDRESS is taken (a NULL call) */
        wo_init(&sec);
        wo_leb(&sec, 1);
        wo_b(&sec, 0x70); wo_b(&sec, 0x00);   /* funcref */
        wo_leb(&sec, wasm_nfv + 1);           /* slot 0 = NULL */
        wo_sec(&out, 4, &sec);
    }
    /* ---- memory section ---- */
            rodata_section ? (int)rodata_section->data_offset : -1,
    data_end = (data_section->data_offset + rodata_section->data_offset
                + bss_section->data_offset + 15) & ~15;
    /* the stack grows down from the top of memory; the frame grows UP
       from sp (locals at fp+offset).  119_random_stuff's 256KB struct
       by-value copies park ~1MB below sp AND its frame sits ~720KB
       above it — give the memory 2MB total headroom so both fit */
    npages = (data_end + 0x200000 + 0xffff) >> 16;
    if (npages < 16)
        npages = 16;
    wo_init(&sec);
    wo_leb(&sec, 1);
    wo_leb(&sec, 0);
    wo_leb(&sec, npages);
    wo_sec(&out, 5, &sec);

    /* ---- global section: __stack_pointer ---- */
    stack_top = npages << 16;
    wo_init(&sec);
    wo_leb(&sec, 1);
    wo_b(&sec, VAL_I32); wo_b(&sec, 1);
    wo_b(&sec, W_I32_CONST);
    w_sleb_out(&sec, stack_top);
    wo_b(&sec, W_END);
    wo_sec(&out, 6, &sec);

    /* ---- export section ---- */
    wo_init(&sec);
    {
        int need_ctors = (wasm_nctors > 0 ? 1 : 0) + (wasm_ndtors > 0 ? 1 : 0);
        int nexp = 1 + (main_idx >= 0 ? 2 : 0) + ((wasm_nfv > 0 || wasm_nindir > 0) ? 1 : 0)
                 + need_ctors;
        for (i = 0; i < wasm_nfuncs; i++)
            if (wasm_funcs[i].defined && strcmp(wasm_funcs[i].name, "main"))
                nexp++;
        wo_leb(&sec, nexp);
    }
    wo_str(&sec, "memory");
    wo_b(&sec, 2); wo_leb(&sec, 0);
    if (wasm_nfv > 0 || wasm_nindir > 0) {
        /* the function-value table — the env runtime resolves fn values
           (atexit handlers, fn pointers) through it */
        wo_str(&sec, "__indirect_function_table");
        wo_b(&sec, 1); wo_leb(&sec, 0);
    }
    if (main_idx >= 0) {
        wo_str(&sec, "main");
        wo_b(&sec, 0); wo_leb(&sec, main_idx);
        wo_str(&sec, "_start");
        wo_b(&sec, 0); wo_leb(&sec, start_idx);
    }
    if (wasm_nctors > 0) {
        wo_str(&sec, "__wasm_call_ctors");
        wo_b(&sec, 0);
        wo_leb(&sec, nimports + ndef + (main_idx >= 0 ? 1 : 0));
    }
    if (wasm_ndtors > 0) {
        wo_str(&sec, "__wasm_call_fini");
        wo_b(&sec, 0);
        wo_leb(&sec, nimports + ndef + (main_idx >= 0 ? 1 : 0)
                      + (wasm_nctors > 0 ? 1 : 0));
    }
    for (i = 0; i < wasm_nfuncs; i++)
        if (wasm_funcs[i].defined && strcmp(wasm_funcs[i].name, "main")) {
            wo_str(&sec, wasm_funcs[i].name);
            wo_b(&sec, 0); wo_leb(&sec, func_idx[i]);
        }
    wo_sec(&out, 7, &sec);

    /* ---- element section: the function-value table contents ---- */
    if (wasm_nfv > 0) {
        wo_init(&sec);
        wo_leb(&sec, 1);
        wo_leb(&sec, 0);                        /* table 0 */
        wo_b(&sec, W_I32_CONST); wo_leb(&sec, 1);  /* offset 1 (slot 0 = NULL) */
        wo_b(&sec, W_END);
        wo_leb(&sec, wasm_nfv);
        for (i = 0; i < wasm_nfv; i++) {
            int fi = -1;
            for (k = 0; k < wasm_nfuncs; k++)
                if (wasm_funcs[k].defined &&
                    !strcmp(wasm_funcs[k].name, wasm_fv[i])) {
                    fi = func_idx[k];
                    break;
                }
            if (fi < 0) {
                /* an extern function (env import) — the table needs a
                   function index; imports precede defined funcs.  The
                   import's sig must match the call that pops this slot
                   (the fv-index -> indir-sig mapping: sigs register in
                   call order). */
                int want_sig = (i < wasm_nindir) ? wasm_indir_sigs[i] : -1;
                for (k = 0; k < wasm_nimports; k++)
                    if (imp_idx[k] >= 0 &&
                        (want_sig < 0 || wasm_imports[k].sig == want_sig) &&
                        !strcmp(wasm_imports[k].name, wasm_fv[i])) {
                        fi = imp_idx[k];
                        break;
                    }
            }
            wo_leb(&sec, fi >= 0 ? fi : 0);
        }
        wo_sec(&out, 9, &sec);
    }



    /* ---- code section ---- */
    wo_init(&sec);
    {
        int need_ctors = (wasm_nctors > 0 ? 1 : 0) + (wasm_ndtors > 0 ? 1 : 0);
        wo_leb(&sec, ndef + (main_idx >= 0 ? 1 : 0) + need_ctors);
    }
    for (i = 0; i < wasm_nfuncs; i++) {
        if (wasm_funcs[i].defined) {
            WasmFunc *wf = w_func_by_order(wasm_funcs[i].order);
            if (wf && wf->final) {
                for (j = 0; j < wf->npatches; j++) {
                    WasmPatch *p = &wf->patches[j];
                    int v = 0;
                    if (p->kind == 0 && p->sym) {
                        const char *nm = p->name;
                        for (k = 0; k < wasm_nfuncs; k++)
                            if (wasm_funcs[k].defined &&
                                !strcmp(wasm_funcs[k].name, nm))
                                break;
                        if (k < wasm_nfuncs)
                            v = func_idx[k];
                        else {
                            for (k = 0; k < wasm_nimports; k++)
                                if (imp_idx[k] >= 0 &&
                                    wasm_imports[k].sig == p->sig &&
                                    !strcmp(wasm_imports[k].name, nm))
                                    break;
                            if (k < wasm_nimports)
                                v = imp_idx[k];
                            else
                                tcc_error("wasm: undefined function");
                        }
                    } else if (p->kind == 4) {
                        /* a constant computed goto: same as the -2 case */
                        int rr;
                        v = 0;
                        for (rr = 0; rr < wf->nsubs_out; rr++)
                            if (p->dofs >= wf->sub_starts[rr] &&
                                p->dofs < wf->sub_ends[rr]) {
                                v = rr;
                                break;
                            }
                    } else if (p->kind == 1) {
                        if (p->dsec == -2) {
                            /* a &&label: the sub index of the code at
                               dofs (the pc dispatch value) */
                            int rr;
                            v = 0;
                            for (rr = 0; rr < wf->nsubs_out; rr++)
                                if (p->dofs >= wf->sub_starts[rr] &&
                                    p->dofs < wf->sub_ends[rr]) {
                                    v = rr;
                                    break;
                                }
                        } else if (p->dsec == SHN_UNDEF && p->name) {
                            /* an undefined extern data symbol (incl.
                               an __asm__-renamed one): resolve to the
                               DEFINED symbol's data address */
                            int s2;
                            v = 0;
                            for (s2 = 0;
                                 s2 < (int)(symtab_section->data_offset / sizeof(ElfSym));
                                 s2++) {
                                ElfSym *es2 = &((ElfSym *)symtab_section->data)[s2];
                                char *en2 = &((char *)symtab_section->link->data)[es2->st_name];
                                if (es2->st_shndx != SHN_UNDEF &&
                                    !strcmp(en2, p->name)) {
                                    v = w_data_addr_of(es2->st_shndx, es2->st_value);
                                    break;
                                }
                            }
                        } else {
                            v = w_data_addr_of(p->dsec, p->dofs);
                        }
                    } else if (p->kind == 2 && p->name) {
                        if (w_sym_weak_undef(p->sym, p->name)) {
                            /* a weak reference (GOT(): the inline/static
                               functions have no address) — NULL */
                            v = 0;
                        } else {
                            int fv = w_fv_find(p->name);
                            v = (fv >= 0) ? fv + 1 : 0;  /* 1-based slot */
                        }
                    } else if (p->kind == 3) {
                        v = w_sig_typeidx_of(p->sig); /* call_indirect type */
                    }
                    if (p->ofs + 5 <= wf->flen) {
                        int slot = p->ofs, t;
                        unsigned int u = (unsigned int)v;
                        for (t = 0; t < 5; t++) {
                            unsigned char b = u & 0x7f;
                            u >>= 7;
                            wf->final[slot++] = (t == 4) ? b : (b | 0x80);
                        }
                    }
                }
                /* locals: 3x i32 (fp, pc, scratch_i32), 1x f32, 1x f64,
                   1x i64 (scratch_i64) */
                wo_leb(&sec, wf->flen + 9);
                wo_b(&sec, 0x04);           /* 4 groups */
                wo_b(&sec, 0x03); wo_b(&sec, VAL_I32);
                wo_b(&sec, 0x01); wo_b(&sec, VAL_F32);
                wo_b(&sec, 0x01); wo_b(&sec, VAL_F64);
                wo_b(&sec, 0x01); wo_b(&sec, VAL_I64);
                for (j = 0; j < wf->flen; j++)
                    wo_b(&sec, wf->final[j]);
            }
        }
    }
    if (main_idx >= 0) {
        /* _start: push argc/argv (main's params, 0-filled — the wasm
           runner calls _start with no WASI args), call main, call
           proc_exit, unreachable, end */
        int bl = 0;
        unsigned char body[32];
        int main_sig = -1, np = 0;
        for (i = 0; i < wasm_nfuncs; i++)
            if (wasm_funcs[i].defined && !strcmp(wasm_funcs[i].name, "main"))
                main_sig = wasm_funcs[i].sig;
        if (main_sig >= 0 && main_sig < wasm_nsigs)
            np = wasm_sigs[main_sig].nparams;
        for (i = 0; i < np; i++) {
            body[bl++] = W_I32_CONST;
            body[bl++] = 0;
        }
        body[bl++] = W_CALL;
        body[bl++] = main_idx;             /* LEB (main_idx < 128 for v1) */
        if (main_sig >= 0 && main_sig < wasm_nsigs &&
            wasm_sigs[main_sig].nresults == 0) {
            /* a void main leaves no exit code — proc_exit needs one
               (137_funcall_struct_args's void main produced an invalid
               module: call with an empty stack) */
            body[bl++] = W_I32_CONST;
            body[bl++] = 0;
        }
        body[bl++] = W_CALL;
        body[bl++] = 1;                    /* proc_exit */
        body[bl++] = W_UNREACHABLE;
        body[bl++] = W_END;
        wo_b(&sec, bl + 1);
        wo_b(&sec, 0x00);                  /* no locals */
        for (i = 0; i < bl; i++)
            wo_b(&sec, body[i]);
    }
    if (wasm_nctors > 0) {
        /* __wasm_call_ctors: run the constructor functions in order */
        int bl = 0;
        unsigned char body[128];
        for (i = 0; i < wasm_nctors; i++) {
            int fi = -1;
            for (k = 0; k < wasm_nfuncs; k++)
                if (wasm_funcs[k].defined &&
                    !strcmp(wasm_funcs[k].name, wasm_ctors[i])) {
                    fi = func_idx[k];
                    break;
                }
            if (fi < 0)
                continue;
            body[bl++] = W_CALL;
            body[bl++] = fi;
        }
        body[bl++] = W_END;
        wo_b(&sec, bl + 1);
        wo_b(&sec, 0x00);                  /* no locals */
        for (i = 0; i < bl; i++)
            wo_b(&sec, body[i]);
    }
    if (wasm_ndtors > 0) {
        /* __wasm_call_fini: run the destructor functions in order (the
           runner calls it after main, BEFORE the atexit flush, matching
           the native ordering where .fini_array runs first) */
        int bl = 0;
        unsigned char body[128];
        for (i = 0; i < wasm_ndtors; i++) {
            int fi = -1;
            for (k = 0; k < wasm_nfuncs; k++)
                if (wasm_funcs[k].defined &&
                    !strcmp(wasm_funcs[k].name, wasm_dtors[i])) {
                    fi = func_idx[k];
                    break;
                }
            if (fi < 0)
                continue;
            body[bl++] = W_CALL;
            body[bl++] = fi;
        }
        body[bl++] = W_END;
        wo_b(&sec, bl + 1);
        wo_b(&sec, 0x00);                  /* no locals */
        for (i = 0; i < bl; i++)
            wo_b(&sec, body[i]);
    }
    wo_sec(&out, 10, &sec);

    /* ---- data section ---- */
    /* resolve data/rodata relocations: an address-of-function in a
       global initializer (int (*f)(int) = &fred) is a table slot, and
       an address-of-data is the linear-memory address */
    {
        unsigned char *dbuf = tcc_malloc(data_section->data_offset
                        + rodata_section->data_offset + bss_section->data_offset);
        memcpy(dbuf, data_section->data, data_section->data_offset);
        memcpy(dbuf + data_section->data_offset, rodata_section->data,
               rodata_section->data_offset);
        memset(dbuf + data_section->data_offset + rodata_section->data_offset,
               0, bss_section->data_offset);
        if (data_section->reloc)
            w_apply_data_relocs(s, data_section, dbuf);
        if (rodata_section->reloc)
            w_apply_data_relocs(s, rodata_section,
                                dbuf + data_section->data_offset);
        wo_init(&sec);
        wo_leb(&sec, 1);
        wo_b(&sec, 0);                     /* active, memory 0 */
        wo_b(&sec, W_I32_CONST); wo_leb(&sec, 0);
        wo_b(&sec, W_END);
        wo_leb(&sec, data_section->data_offset + rodata_section->data_offset
                     + bss_section->data_offset);
        for (i = 0; i < data_section->data_offset + rodata_section->data_offset
                         + bss_section->data_offset; i++)
            wo_b(&sec, dbuf[i]);
        tcc_free(dbuf);
    }
    wo_sec(&out, 11, &sec);

    /* ---- write the file ---- */
    f = fopen(filename, "wb");
    if (!f) {
        tcc_error_noabort("could not write '%s'", filename);
        tcc_free(func_idx);
        tcc_free(imp_idx);
        return -1;
    }
    fwrite("\0asm", 1, 4, f);
    fwrite("\1\0\0\0", 1, 4, f);
    fwrite(out.data, 1, out.len, f);
    fclose(f);
    tcc_free(func_idx);
    tcc_free(imp_idx);
    return 0;
}

#endif /* TARGET_DEFS_ONLY */
