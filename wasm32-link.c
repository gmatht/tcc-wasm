/*
 *  wasm32 linker stubs.
 *
 *  The wasm backend never emits ELF relocations (it has its own
 *  patch mechanism inside wasm32-gen.c), so the arch-link interface
 *  is a set of no-ops.  wasm_output_file() writes the module directly
 *  from tccelf.c's tcc_output_file().
 */

#ifdef TARGET_DEFS_ONLY

/* dummy relocation/ELF constants: the wasm backend never emits ELF
   relocations, but the generic code references these (must be visible
   via tcc.h's TARGET_DEFS_ONLY include, like riscv64-link.c) */
#define EM_TCC_TARGET 0x7a73   /* fake e_machine ("zs") */
#define R_DATA_PTR  1
#define R_DATA_32   2
#define R_DATA_32DW 3
#define R_JMP_SLOT  4
#define R_GLOB_DAT  5
#define R_COPY      6
#define R_RELATIVE  7
#define R_SIZE      8
#define R_TYPE      9
#define R_INFO      10
#define R_NUM       11

#define ELF_START_ADDR 0x00010000
#define ELF_PAGE_SIZE 0x1000

#define PCRELATIVE_DLLPLT 1
#define RELOCATE_DLLPLT 1

#else
#define USING_GLOBALS
#include "tcc.h"

ST_FUNC int code_reloc(int reloc_type)
{
    return 0;
}

ST_FUNC int gotplt_entry_type(int reloc_type)
{
    return 0;
}

ST_FUNC unsigned create_plt_entry(TCCState *s1, unsigned got_offset,
                                  struct sym_attr *attr)
{
    return 0;
}

ST_FUNC void relocate_plt(TCCState *s1)
{
}

ST_FUNC void relocate(TCCState *s1, ElfW_Rel *rel, int type,
                      unsigned char *ptr, addr_t addr, addr_t val)
{
    tcc_error("wasm: unexpected relocation %d", type);
}
#endif
