#ifndef _ALLOCA_H
#define _ALLOCA_H
/* tcc's tccdefs.h predeclares `void *alloca(__SIZE_TYPE__)` — wasi's
   alloca.h declares `void *alloca(size_t)` and the two differ on wasm32
   (unsigned int vs unsigned long), so including wasi's after tccdefs is
   an incompatible re-declaration error.  This header shadows wasi's
   (the corpus runner searches the fork include dir FIRST). */
#ifdef __TINYC__
void *alloca(__SIZE_TYPE__);
#else
#include <stddef.h>
void *alloca(size_t);
#endif
#endif
