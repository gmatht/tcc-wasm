# Plan: fix the remaining tcc-wasm test failures

Status 2026-08-14 (after the fork-destruction incident + recovery commits).

## Baseline

| Harness | Tests | PASS | failures |
|---|---|---|---|
| sh2runtime runner `__tcc-suite-test.mjs` (717KB tcc.wasm) | 130 | 70 | CCERR 36, DIFF 22, RUNERR 2 |
| fork corpus `tests/wasm-corpus/run-corpus.mjs` (726e659 + 99e60d8) | 122 | 45 | REFUSE 45, WRONG 10, RUN-CRASH 2, COMPILE-OUT 11, NO-EXPECT 9 |
| fork corpus, LOST state (51 commits, c4655816..3fa1fb6b) | 122 | 110 | reference target |

The runner uses a stale 717KB binary (fork state ~Aug 13 18:56); the fork's
current source is 726e659 + the small recovered fixes.  Both are behind the
lost 110 state.

## Failure taxonomy (fork corpus, root causes)

| # | Root cause | Tests | Where fixed (lost history) |
|---|---|---|---|
| 1 | indirect calls / function pointers unsupported (`wasm: indirect call`) | 07, 33, 42, 81, 82, 90, 129, 142 | 13b: call_indirect + funcref table + data relocs |
| 2 | struct by-value args unsupported | 73, 109, 121, 130, 135, 137 | 13b: caller temp copy + sp reserve, callee byref |
| 3 | i64 args / i64 varargs unsupported | 107, 110, 111, 118, 131, 134 | 09+13b: two-word parking, i64 varargs |
| 4 | VLA unsupported | 78, 79, 122, 123 | 13b: gen_vla_sp_save/restore/alloc |
| 5 | computed goto unsupported | 90, 119 | ggoto attempt (saved in recovery/) |
| 6 | alloca.h type conflict (wasi header vs tccdefs.h) | 30, 46, 97 | header guard (`#ifndef __TINYC__`) |
| 7 | COMPILE-OUT: emitter emits invalid wasm (stack/align/i64) | 27, 31, 39, 48, 54, 70, 86, 89, 94, 101, 104 | 09: code-buffer grow, data base, arg materialization; 13a: patch fixes |
| 8 | WRONG: codegen semantics | 06_case (switch), 16_nesting, 17_enum (vararg slot), 38 (array index), 45_empty_for (w_layout), 108 (ctor order), 60/125/128 (dt mode) | 09: w_layout/arg fixes; 13b: switch/signed-LEB |
| 9 | RUN-CRASH: traps | 03_struct (sret flow), 136 (atomics) | 13a (sret) + 13b (signed-LEB) |

The 717KB runner binary's 36 CCERR / 22 DIFF / 2 RUNERR are the same root
causes minus the harness artifacts below, plus config-gated tests.

## Track 0 — Harness fixes (sh2runtime runner; no compiler changes)

Legitimate fixes mirroring the fork corpus + upstream tests2/Makefile.
~ +15 PASS on the runner.

1. **Normalization**: port the fork corpus's `norm` (strip trailing spaces
   per line; strip ONE trailing newline from both sides before compare).
   The runner's `replace(/\s+$/, "\n")` collapses to one newline but does
   not strip it symmetrically → spurious 1-2 char diffs:
   71_macro_empty_arg ("17" vs "17\n"), likely 76 (1 char), 38 (4 chars).
2. **-dt emulation**: 60_errors_and_warnings, 96_nodata_wanted,
   125_atomic_misc, 128_run_atexit are `#if defined(test_x)` sectioned
   tests — without `-D test_x` they print nothing (0/18 chars).  Port the
   fork corpus's DT_TESTS logic (compile each section with -D test_x,
   print `[test_x]` markers, diff against .expect).
3. **SKIPs for config-gated tests** (upstream skips these for a build
   without CONFIG_TCC_BCHECK/backtrace/dll/tls/elf-linker):
   112_backtrace, 113_btdll, 114_bound_signal, 115_bound_setjmp,
   116_bound_setjmp2, 117_builtins, 126_bound_global, 144_tls,
   146_tls_extern, 148_linker_symbols, 149_end_copy_reloc,
   150_linker_boundaries, 95_bitfields (MS layout; 32-bit non-win skip).
   These are NOT compiler bugs.

Gate: runner 70 → ~90; no compiler touched.

## Track 1 — Reconstruct the lost fork work (corpus 45 → 110)

Use the replay kit (`recovery/replay_engine.py` + `recovery/fork-ops.json`
from the pi session transcripts).  The engine replays ~470 ops but does not
converge (the sessions' failed instrumentation asserts + git-checkout
experiments leave states only exact execution reproduces).  Hand-finish in
three milestones, rebuilding the native tcc + running the corpus after each:

1. **w_layout segment rework** (09 session ops ~137-161): `WasmLabel.seg`,
   resolve-by-segment (pos==sub-start pass BEFORE containment; the loop
   back-edge's empty-range sub must win over the exit block), `w_br_depth`,
   `gjmp_addr` edge semantics, VT_JMP anchors by position.  Unlocks the
   13a session's 11 remaining edits.  → corpus ~60-70 (COMPILE-OUT/WRONG
   mostly gone).
2. **13a session** (sret struct returns; WasmPatch dsec/dofs/name init;
   resolve_common_syms; undefined-data-address re-fetch): → corpus ~92
   (the c4655816 state — 03_struct, 102_alignas, 129_scopes pass).
3. **13b session** (struct by-value args, signed-LEB pc targets, switch
   split cap, small-struct varargs, fn-ptr funcref table + call_indirect +
   data relocs, VLA, i64 fixed+vararg args, alias/__asm__ resolution,
   label addresses, atomic varargs): → corpus ~109-110.

Gate: corpus count monotonic; never regress 87_dead_code/101_cleanup (the
previous ggoto attempt did).

## Track 2 — Remaining gaps (beyond 110)

1. **computed goto** (90, 119): re-land the saved ggoto diff but guard the
   tccgen `gsym` change so the block split happens only for address-taken
   labels (the unguarded version broke 87_dead_code).
2. **struct varargs** (73_arm64): pointer-based vararg ABI (caller packs
   varargs in the parking area; hidden pointer param; callee prolog stores
   it; va_start reads it) — touches w_call_sig, gfunc_call, prolog, and
   c-runtime printf family.
3. **alloca.h** (30, 46, 97): guard wasi's alloca.h with `#ifndef __TINYC__`
   (tccdefs.h predeclares `void *alloca(__SIZE_TYPE__)`) — 30-min fix.
4. **big-struct headroom + variadic fn-ptr** (119): memory growth for
   struct big_struct a[262144]; per-sig imports for variadic fn-ptr calls.

## Track 3 — Ship to the runner

1. Rebuild tcc.wasm: `TINYCC_DIR=/root/src/tinycc-wasm ./build-wasm-tcc.sh`
   (the footgun is fixed — never run it without TINYCC_DIR).
2. Re-run `__tcc-suite-test.mjs`: expect ~110 corpus PASS minus the
   config-gated skips → ~95-100 runner PASS.
3. Rebuild tcc-include.dat if headers change (alloca.h guard).

## Risks

- The 09-session w_layout rework is intricate; the replay base diverges.
  Validate after each op group by compiling the native tcc — never commit
  a non-compiling state.
- The ggoto re-land can regress 87/101 — keep the guard narrow.
- The vararg ABI change touches c-runtime.js (a separate repo) — run the
  full corpus + the shell's printf tests together.
- The concurrent mimecroft session commits to sh2runtime — coordinate
  commits (small, focused).
