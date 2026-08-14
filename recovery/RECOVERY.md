# Fork recovery notes (2026-08-14)

## What happened
`build-wasm-tcc.sh` (in sh2runtime) treats its default `TINYCC` path
(`/root/src/tinycc-wasm`) as DISPOSABLE: run WITHOUT `TINYCC_DIR`, it does
`rm -rf "$TINYCC"` and re-clones the remote (origin/mob = 726e659).  The
fork had 51 local commits on top (corpus PASS 92 → 109 → 110 + the
uncommitted computed-goto attempt) that were never pushed — all destroyed.

**Always run the build script with `TINYCC_DIR=/root/src/tinycc-wasm`.**

## What was recovered
- `replay_engine.py` + `fork-ops.json`: the session transcripts
  (`/root/.pi/agent/sessions/--root-src-sh2runtime--/*.jsonl`, sessions
  2026-08-03T06-06, 08-09T18-06, 08-13T08-26, 08-13T15-30) contain EVERY
  edit/python/cp/git operation applied to the fork.  The engine replays
  them onto the 726e659 base.  It gets ~80% there but does NOT converge
  to a compilable state — the sessions' own failed instrumentation
  asserts and git-checkout experiments leave intermediate states that
  only exact execution reproduces.  The remaining blockers are the
  w_layout segment rework (ops ~160-190 in fork-ops.json) and the
  arg-materialization experiments.
- `ggoto-*.diff`: the uncommitted computed-goto attempt (working tree vs
  commit 3fa1fb6b) — the final wasm32-gen.c/tccgen.c state.
- Commit 99e60d8: the small standalone fixes re-applied onto 726e659
  (sret struct returns, i64 return pair store, code-buffer grow, data
  base, NULL guards) — corpus 45 PASS (from ~36).

## To continue
1. Replay /tmp/replay_engine.py with fork-ops.json on a pristine
   726e659 clone (or the committed 99e60d8 state).
2. Fix the remaining ops by hand — the w_layout segment rework is in
   the 09 session ops ~137-161 + 13a session ops (WasmLabel.seg,
   resolve-by-segment, w_br_depth, gjmp_addr).
3. The 13b session (PASS 105/110) added struct-by-value args (caller
   temp copy + sp reserve, callee byref), VLA, signed-LEB, switch split
   cap, fn-ptr resolution, atomics — all described in the commit
   messages inside fork-ops.json's GIT ops and WASM_CORPUS.md history.

## Test runner
`tests/wasm-corpus/run-corpus.mjs` (recreated from the session) — run
with `TCC_PATH=/tmp/tcc-rb/build-wasm32/tcc TCC_INC=/root/src/tinycc-wasm/include node tests/wasm-corpus/run-corpus.mjs`
(corpus = tests/tests2, runtime = sh2runtime's src/c-runtime.js).
