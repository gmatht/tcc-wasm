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
#define W_F32_ADD      0x93
#define W_F32_SUB      0x94
#define W_F32_MUL      0x95
#define W_F32_DIV      0x96
#define W_F64_ADD      0xa0
#define W_F64_SUB      0xa1
#define W_F64_MUL      0xa2
#define W_F64_DIV      0xa3
#define W_I32_WRAP     0xa7
#define W_I32_TRUNC_F32_S 0xa8
#define W_I32_TRUNC_F32_U 0xa9
#define W_I32_TRUNC_F64_S 0xaa
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
    int kind;            /* 0 = function index, 1 = data address */
    int dsec, dofs;      /* data symbol: section index + offset (kind 1),
                            captured at patch time (sym->c is not stable) */
} WasmPatch;

/* per-function state */
typedef struct WasmSeg { int start, end, edge; } WasmSeg;
typedef struct WasmEdge { int kind, op, label, chain_next, seg; } WasmEdge;
typedef struct WasmLabel { int pos; int sub; } WasmLabel;
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
} WasmFunc;

static WasmSig *wasm_sigs;   static int wasm_nsigs, wasm_sig_alloc;
static WasmFuncRef *wasm_funcs; static int wasm_nfuncs, wasm_func_alloc;
static WasmFunc *wasm_cf;    /* current function */
static WasmFunc **wasm_func_list = NULL;
static int wasm_nfunc_list = 0, wasm_func_list_alloc = 0;
static int wasm_ndef;        /* defined funcs emitted so far */
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
    if (nocode_wanted)
        return;
    if (wasm_cf->csize == ind) {
        wasm_cf->code = wa_grow(wasm_cf->code, &wasm_cf->csize,
                                ind + 1, 1);
        wasm_cf->csize = ind + 1;
    }
    wasm_cf->code[ind++] = c;
}

static void w_ins(int op)
{
    /* record instruction start position, then emit the opcode */
    if (nocode_wanted) {
        ind++;
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
    wasm_cf->patches[wasm_cf->npatches].kind = kind;
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

static void w_call(Sym *sym)
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
    wasm_cf->patches[wasm_cf->npatches].kind = 0;
    wasm_cf->npatches++;
}

/* memory ops: memop(al, off) */
static void w_memop(int op, int al, int off)
{
    w_ins(op);
    w_u32(al);
    w_u32(off);
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
    case VT_DOUBLE: w_load(W_F64_LOAD, 3, 0); break;
    default:        w_load(W_I32_LOAD, 2, 0); break;
    }
}

/* store the wasm-stack value into slot r.
   wasm stores take the address FIRST then the value, so park the
   value in a scratch local, push the address, then reload it. */
static void w_emit_slot_store(int r, int bt)
{
    int scr = (bt == VT_FLOAT) ? W_SCRATCH_F32
            : (bt == VT_DOUBLE) ? W_SCRATCH_F64
            : W_SCRATCH_LOCAL;
    w_local_set(scr);
    w_emit_slot_addr(r);
    w_local_get(scr);
    switch (bt) {
    case VT_FLOAT:  w_store(W_F32_STORE, 4, 2, 0); break;
    case VT_DOUBLE: w_store(W_F64_STORE, 8, 3, 0); break;
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
            w_i32_patch(1, sv->sym);
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
    else if (bt == VT_DOUBLE)
        w_load(W_F64_LOAD, 3, 0);
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
    else if (bt == VT_DOUBLE)
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
        w_emit_cmp(sv->cmp_op, sv->cmp_r & 0xff, (sv->cmp_r >> 8) & 0xff);
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
        /* l_false block: r = !inv */
        w_i32_const(inv ^ 1);
        w_emit_slot_store(r, VT_INT);
        w_add_edge(EDGE_UNCOND, 0, l_merge);
        /* l_true block: r = inv */
        w_i32_const(inv);
        w_emit_slot_store(r, VT_INT);
        w_add_edge(EDGE_UNCOND, 0, l_merge);
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
            w_i32_patch(1, sv->sym);
            w_emit_slot_store(r, VT_INT);
        } else if (bt == VT_FLOAT) {
            union { float f; unsigned u; } u;
            u.f = sv->c.f;
            w_i32_const(u.u);
            w_emit_slot_store(r, VT_FLOAT);
        } else if (bt == VT_DOUBLE) {
            union { double d; unsigned u[2]; } u;
            u.d = sv->c.d;
            w_i32_const(u.u[0]);
            w_emit_slot_store(r, VT_INT);
            tcc_error("wasm: double const needs 64-bit slot");
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
    wasm_funcs[i].defined = 1;
    wasm_funcs[i].import = 0;
    wasm_funcs[i].sig = sig;
    wasm_funcs[i].order = wasm_ndef++;
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
    (void)variadic;
    if (variadic)
        tcc_error("wasm: varargs not supported yet");
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
    wasm_cf->nlocals = 5;   /* fp, pc, scratch_i32/f32/f64 */
    wasm_cf->has_sret = ((func_type->ref->type.t & VT_BTYPE) == VT_STRUCT);
    {
        int ref = w_func_defined(func_sym, w_func_sig);
        wasm_cf->order = wasm_funcs[ref].order;
    }
    w_ret_bt = func_vt.t & VT_BTYPE;

    /* register file + locals */
    loc = -(NB_REGS * 8);
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
                w_store(W_I64_STORE, 8, 3, 0);
                tcc_error("wasm: i64 param store needs pair");
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
    if (t > 0)
        w_append_chain(t, e);
    return t > 0 ? t : e;
}

ST_FUNC int gjmp_cond(int op, int t)
{
    int l = w_new_label();
    /* materialize the comparison: vtop is VT_CMP */
    if (vtop->r == VT_CMP) {
        w_emit_cmp(op, vtop->cmp_r & 0xff, (vtop->cmp_r >> 8) & 0xff);
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

ST_FUNC void gjmp_addr(int a)
{
    /* unconditional jump to a code position */
    int l = w_new_label();
    wasm_cf->labels[l].pos = a;
    w_add_edge(EDGE_UNCOND, 0, l);
}

ST_FUNC void gsym_addr(int t, int a)
{
    /* resolve the jump chain t to position a; close the current block
       with a fallthrough to the next one (the code continues there) */
    int l = w_new_label();
    wasm_cf->labels[l].pos = a;
    while (t > 0) {
        int e = t - 1;
        wasm_cf->edges[e].label = l;
        t = wasm_cf->edges[e].chain_next;
    }
    if (a == ind) {
        /* resolution to the current position: fall through */
        w_add_edge(EDGE_UNCOND, 0, l);
        w_new_block();
    }
}

/* gsym() is generic in tccgen.c (calls gsym_addr(t, ind)) */

/* ---------------------------------------------------------------- */
/* gen_opi / gen_opl / gen_opf */

static int w_carry_slot = 7;   /* dedicated slot for add-carry/borrow */

ST_FUNC void gen_opi(int op)
{
    int a, b, d, o;
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
        vtop -= 2;
        d = get_reg(RC_INT);
        vtop++;
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
        /* 32x32 -> 64: result in slots d (low) and d2 (high) */
        gv2(RC_INT, RC_INT);
        a = vtop[-1].r; b = vtop[0].r;
        vtop -= 2;
        d = get_reg(RC_INT);
        vtop++;
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
        w_emit_slot_store(d + 1, VT_INT);
        vtop[0].r = d;
        vtop[0].r2 = d + 1;
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
    int a, b, d, o;
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
    a = vtop[-1].r; b = vtop[0].r;
    vtop -= 2;
    d = get_reg(RC_INT);
    vtop++;
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
    w_emit_slot_load(a + 1, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHL);
    w_ins(W_I64_OR);
    w_emit_slot_load(b, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_emit_slot_load(b + 1, VT_INT);
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
    w_emit_slot_load(a + 1, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_ins(W_I64_CONST);
    w_u32(32);
    w_ins(W_I64_SHL);
    w_ins(W_I64_OR);
    w_emit_slot_load(b, VT_INT);
    w_ins(W_I64_EXTEND_U);
    w_emit_slot_load(b + 1, VT_INT);
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
    w_emit_slot_store(d + 1, VT_INT);
    vtop[0].r = d;
    vtop[0].r2 = d + 1;
}

ST_FUNC void gen_opf(int op)
{
    int a, b, d, dbl, o, cmp;
    gv2(RC_FLOAT, RC_FLOAT);
    dbl = (vtop->type.t & VT_BTYPE) == VT_DOUBLE;
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
    w_emit_slot_load(r, ft == VT_DOUBLE ? VT_DOUBLE : VT_FLOAT);
    if (t & VT_UNSIGNED)
        w_ins(ft == VT_DOUBLE ? W_I32_TRUNC_F64_U : W_I32_TRUNC_F32_U);
    else
        w_ins(ft == VT_DOUBLE ? W_I32_TRUNC_F64_S : W_I32_TRUNC_F32_S);
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
    int i, r, rt, ret_bt, res_slot;
    int *slots;
    CType *ft;
    Sym *sym;
    int sig, nparams;

    ft = &vtop[-nb_args].type;
    sym = vtop[-nb_args].sym;
    if (!sym || (vtop[-nb_args].r & (VT_VALMASK | VT_LVAL | VT_SYM)) !=
                (VT_CONST | VT_SYM)) {
        tcc_error("wasm: indirect call (function pointer) unsupported");
    }
    w_sig_from_type(ft, &sig, &nparams);
    {
        int r = w_func_ref(sym);
        wasm_funcs[r].sig = sig;
    }
    /* spill live register values (e.g. a previous call's result in the
       shared return slot) so the callee can clobber them */
    save_regs(nb_args + 1);

    slots = tcc_malloc(nb_args * sizeof(int));
    /* materialize args into slots (reverse order: bring each arg to
       the top of the value stack, gv it, then restore) */
    for (i = 0; i < nb_args; i++) {
        SValue *sv = &vtop[1 + i - nb_args];
        int bt = sv->type.t & VT_BTYPE;
        if (bt == VT_STRUCT)
            tcc_error("wasm: struct by-value args unsupported");
        vrotb(i + 1);
        gv((bt == VT_FLOAT || bt == VT_DOUBLE) ? RC_FLOAT : RC_INT);
        slots[nb_args - 1 - i] = vtop->r;
        vrott(i + 1);
    }
    /* result address (for the store) goes below the args */
    ret_bt = ft->ref->type.t & VT_BTYPE;
    res_slot = (ret_bt == VT_FLOAT || ret_bt == VT_DOUBLE) ? REG_FRET : REG_IRET;
    if (ret_bt != VT_STRUCT)
        w_emit_slot_addr(res_slot);
    /* push args in order (on top of the address) */
    for (i = 0; i < nb_args; i++) {
        SValue *sv = &vtop[1 + i - nb_args];
        int bt = sv->type.t & VT_BTYPE;
        w_emit_slot_load(slots[i], (bt == VT_FLOAT || bt == VT_DOUBLE) ? bt : VT_INT);
    }
    w_call(sym);
    /* store the result */
    if (ret_bt == VT_VOID) {
        /* nothing on the stack */
    } else if (ret_bt == VT_STRUCT) {
        /* sret: the result is written via the sret pointer by the callee */
        tcc_error("wasm: struct return call unsupported");
    } else if (ret_bt == VT_FLOAT) {
        w_store(W_F32_STORE, 4, 2, 0);
    } else if (ret_bt == VT_DOUBLE) {
        w_store(W_F64_STORE, 8, 3, 0);
    } else if (ret_bt == VT_LLONG) {
        w_store(W_I64_STORE, 8, 3, 0);
        tcc_error("wasm: i64 return needs pair store");
    } else {
        w_store(W_I32_STORE, 4, 2, 0);
    }
    tcc_free(slots);
    vtop -= nb_args + 1;
}

ST_FUNC void ggoto(void)
{
    tcc_error("wasm: computed goto unsupported");
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
    tcc_error("wasm: VLA unsupported");
}

ST_FUNC void gen_vla_sp_restore(int addr)
{
    tcc_error("wasm: VLA unsupported");
}

ST_FUNC void gen_vla_alloc(CType *type, int align)
{
    tcc_error("wasm: VLA unsupported");
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
    cnt = tcc_mallocz(nb * sizeof(int) * 16);
    for (i = 0; i < wasm_cf->nlabels; i++) {
        int pos = wasm_cf->labels[i].pos;
        if (pos >= 0) {
            int b = w_block_at_pos(pos);
            int c = cnt[b * 16], m, dup = 0;
            for (m = 0; m < c; m++)
                if (cnt[b * 16 + 1 + m] == pos)
                    dup = 1;
            if (!dup && c < 14)
                cnt[b * 16 + 1 + (cnt[b * 16]++)] = pos;
        }
    }
    for (i = 0; i < nb; i++) {
        int c = cnt[i * 16];
        for (j = 1; j < c; j++)
            for (k = j; k > 0 && cnt[i * 16 + 1 + k - 1] > cnt[i * 16 + 1 + k]; k--) {
                int t = cnt[i * 16 + 1 + k - 1];
                cnt[i * 16 + 1 + k - 1] = cnt[i * 16 + 1 + k];
                cnt[i * 16 + 1 + k] = t;
            }
    }

    /* 2. build sub-blocks (split each block at its positions) */
    for (i = 0; i < nb; i++) {
        int seg, si = 0, c = cnt[i * 16];
        subs = wa_grow(subs, &sub_alloc, nsubs + 1, sizeof(WSub));
        memset(&subs[nsubs], 0, sizeof(WSub));
        subs[nsubs].blk = i;
        subs[nsubs].start = wasm_cf->segs[wasm_cf->blks[i].fs].start;
        nsubs++;
        for (seg = wasm_cf->blks[i].fs; seg <= wasm_cf->blks[i].ls; seg++) {
            int pos = wasm_cf->segs[seg].start;
            int segend = wasm_cf->segs[seg].end;
            while (si < c && cnt[i * 16 + 1 + si] < segend) {
                int p = cnt[i * 16 + 1 + si];
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

    /* 3. resolve labels to sub-blocks */
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
        if (wasm_cf->labels[i].sub < 0) {
            /* pos == the end of the last sub of a block: use it */
            for (j = nsubs - 1; j >= 0; j--)
                if (pos >= subs[j].start && pos <= subs[j].end &&
                    (j + 1 >= nsubs || subs[j + 1].blk != subs[j].blk)) {
                    wasm_cf->labels[i].sub = j;
                    break;
                }
        }
        if (wasm_cf->labels[i].sub < 0)
            tcc_error("wasm: internal: unresolved label at %d", pos);
    }

    /* 4. emit the final body */
    ret_valtype = w_typeof(&func_vt);
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
    /* br_table: local.get $pc; br_table (labels, default=switch) */
    body = wa_grow(body, &balloc, blen + 8 + nsubs, 1);
    body[blen++] = W_LOCAL_GET; body[blen++] = W_PC_LOCAL;
    body[blen++] = W_BR_TABLE;
    body[blen++] = nsubs;
    for (i = 0; i < nsubs; i++) {
        int d = nsubs - 1 - i;
        body[blen++] = d;
    }
    body[blen++] = nsubs;

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
                if (ed->kind == EDGE_COND) {
                    /* if (cond) { pc = L; br $dispatch } end — the br
                       leaves the current sub so the fall-through path
                       (the code after the edge) runs only when the
                       condition is false */
                    body = wa_grow(body, &balloc, blen + 20, 1);
                    body[blen++] = W_IF; body[blen++] = BLOCKTYPE_EMPTY;
                    body[blen++] = W_I32_CONST;
                    v = target;
                    do { unsigned char b = v & 0x7f; v >>= 7;
                         if (v) b |= 0x80; body[blen++] = b; } while (v);
                    body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
                    /* the br sits inside the if: one level deeper */
                    body[blen++] = W_BR; body[blen++] = i + 2;
                    body[blen++] = W_END;
                } else if (ed->kind == EDGE_UNCOND) {
                    body = wa_grow(body, &balloc, blen + 16, 1);
                    body[blen++] = W_I32_CONST;
                    v = target;
                    do { unsigned char b = v & 0x7f; v >>= 7;
                         if (v) b |= 0x80; body[blen++] = b; } while (v);
                    body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
                    body[blen++] = W_BR; body[blen++] = i + 1;
                } else {
                    body = wa_grow(body, &balloc, blen + 4, 1);
                    body[blen++] = W_BR; body[blen++] = i + 2;
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
            do { unsigned char b = v & 0x7f; v >>= 7;
                 if (v) b |= 0x80; body[blen++] = b; } while (v);
            body[blen++] = W_LOCAL_SET; body[blen++] = W_PC_LOCAL;
            body[blen++] = W_BR; body[blen++] = i + 1;
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

ST_FUNC int wasm_output_file(TCCState *s, const char *filename)
{
    WOut out = {0}, sec = {0};
    FILE *f;
    int i, j, nimports, npages, data_end, stack_top;
    int *func_idx;
    int main_idx = -1, start_idx = -1;
    int ndef = 0, nused = 0, used[512];
    int sig_fdwrite, sig_procexit, sig_start;
    int k;
    unsigned char p4[4] = { VAL_I32, VAL_I32, VAL_I32, VAL_I32 };

    sig_fdwrite = w_sig_get(4, p4, 0, 0);
    sig_procexit = w_sig_get(1, p4, 0, 0);
    sig_start = w_sig_get(0, p4, 0, 0);
    tcc_enter_state(s);   /* make the section macros resolve correctly */
    w_sec_data = data_section;
    w_sec_rodata = rodata_section;
    w_sec_bss = bss_section;

    func_idx = tcc_mallocz(wasm_nfuncs * sizeof(int));
    nimports = 2;
    for (i = 0; i < wasm_nfuncs; i++)
        if (wasm_funcs[i].import)
            func_idx[i] = nimports++;
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
    {
        int m;
        int add_used(int s) {
            for (m = 0; m < nused; m++)
                if (used[m] == s)
                    return;
            used[nused++] = s;
        }
        for (i = 0; i < wasm_nfuncs; i++)
            if (wasm_funcs[i].sig >= 0)
                add_used(wasm_funcs[i].sig);
        if (main_idx >= 0)
            add_used(sig_start);
        add_used(sig_fdwrite);
        add_used(sig_procexit);
    }
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
    for (i = 0; i < wasm_nfuncs; i++) {
        if (wasm_funcs[i].import) {
            wo_str(&sec, "env");
            wo_str(&sec, wasm_funcs[i].name);
            wo_b(&sec, 0);
            wo_leb(&sec, w_sig_typeidx_of(wasm_funcs[i].sig));
        }
    }
    wo_sec(&out, 2, &sec);

    /* ---- function section ---- */
    wo_init(&sec);
    wo_leb(&sec, ndef + (main_idx >= 0 ? 1 : 0));
    for (i = 0; i < wasm_nfuncs; i++)
        if (wasm_funcs[i].defined)
            wo_leb(&sec, w_sig_typeidx_of(wasm_funcs[i].sig));
    if (main_idx >= 0)
        wo_leb(&sec, w_sig_typeidx_of(sig_start));
    wo_sec(&out, 3, &sec);

    /* ---- memory section ---- */
            rodata_section ? (int)rodata_section->data_offset : -1,
    data_end = (data_section->data_offset + rodata_section->data_offset
                + bss_section->data_offset + 15) & ~15;
    npages = (data_end + 0x20000 + 0xffff) >> 16;
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
        int nexp = 1 + (main_idx >= 0 ? 2 : 0);
        for (i = 0; i < wasm_nfuncs; i++)
            if (wasm_funcs[i].defined && strcmp(wasm_funcs[i].name, "main"))
                nexp++;
        wo_leb(&sec, nexp);
    }
    wo_str(&sec, "memory");
    wo_b(&sec, 2); wo_leb(&sec, 0);
    if (main_idx >= 0) {
        wo_str(&sec, "main");
        wo_b(&sec, 0); wo_leb(&sec, main_idx);
        wo_str(&sec, "_start");
        wo_b(&sec, 0); wo_leb(&sec, start_idx);
    }
    for (i = 0; i < wasm_nfuncs; i++)
        if (wasm_funcs[i].defined && strcmp(wasm_funcs[i].name, "main")) {
            wo_str(&sec, wasm_funcs[i].name);
            wo_b(&sec, 0); wo_leb(&sec, func_idx[i]);
        }
    wo_sec(&out, 7, &sec);

    /* ---- code section ---- */
    wo_init(&sec);
    wo_leb(&sec, ndef + (main_idx >= 0 ? 1 : 0));
    for (i = 0; i < wasm_nfuncs; i++) {
        if (wasm_funcs[i].defined) {
            WasmFunc *wf = w_func_by_order(wasm_funcs[i].order);
            if (wf && wf->final) {
                for (j = 0; j < wf->npatches; j++) {
                    WasmPatch *p = &wf->patches[j];
                    int v = 0;
                    if (p->kind == 0 && p->sym) {
                        for (k = 0; k < wasm_nfuncs; k++)
                            if (wasm_funcs[k].sym == p->sym)
                                break;
                        if (k < wasm_nfuncs)
                            v = func_idx[k];
                        else
                            tcc_error("wasm: undefined function");
                    } else if (p->kind == 1) {
                        v = w_data_addr_of(p->dsec, p->dofs);
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
                /* locals: 3x i32 (fp, pc, scratch_i32), 1x f32, 1x f64 */
                wo_leb(&sec, wf->flen + 7);
                wo_b(&sec, 0x03);           /* 3 groups */
                wo_b(&sec, 0x03); wo_b(&sec, VAL_I32);
                wo_b(&sec, 0x01); wo_b(&sec, VAL_F32);
                wo_b(&sec, 0x01); wo_b(&sec, VAL_F64);
                for (j = 0; j < wf->flen; j++)
                    wo_b(&sec, wf->final[j]);
            }
        }
    }
    if (main_idx >= 0) {
        /* _start: call main; call proc_exit; unreachable; end */
        int bl = 0;
        unsigned char body[32];
        body[bl++] = W_CALL;
        body[bl++] = main_idx;             /* LEB (main_idx < 128 for v1) */
        body[bl++] = W_CALL;
        body[bl++] = 1;                    /* proc_exit */
        body[bl++] = W_UNREACHABLE;
        body[bl++] = W_END;
        wo_b(&sec, bl + 1);
        wo_b(&sec, 0x00);                  /* no locals */
        for (i = 0; i < bl; i++)
            wo_b(&sec, body[i]);
    }
    wo_sec(&out, 10, &sec);

    /* ---- data section ---- */
    wo_init(&sec);
    wo_leb(&sec, 1);
    wo_b(&sec, 0);                     /* active, memory 0 */
    wo_b(&sec, W_I32_CONST); wo_leb(&sec, 0);
    wo_b(&sec, W_END);
    wo_leb(&sec, data_section->data_offset + rodata_section->data_offset
                 + bss_section->data_offset);
    for (i = 0; i < data_section->data_offset; i++)
        wo_b(&sec, data_section->data[i]);
    for (i = 0; i < rodata_section->data_offset; i++)
        wo_b(&sec, rodata_section->data[i]);
    for (i = 0; i < bss_section->data_offset; i++)
        wo_b(&sec, bss_section->data[i]);
    wo_sec(&out, 11, &sec);

    /* ---- write the file ---- */
    f = fopen(filename, "wb");
    if (!f) {
        tcc_error_noabort("could not write '%s'", filename);
        tcc_free(func_idx);
        return -1;
    }
    fwrite("\0asm", 1, 4, f);
    fwrite("\1\0\0\0", 1, 4, f);
    fwrite(out.data, 1, out.len, f);
    fclose(f);
    tcc_free(func_idx);
    return 0;
}

#endif /* TARGET_DEFS_ONLY */
