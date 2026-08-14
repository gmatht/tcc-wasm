// Hang-safe variant of the fork's corpus runner: each test's wasm runs in
// a worker thread with a wall-clock timeout, so a runaway module can't
// hang the whole suite. Compile stays in the main thread (tcc via execFileSync).
import { readFileSync, readdirSync, writeFileSync } from "node:fs";
import { execFileSync, spawnSync } from "node:child_process";
import { Worker } from "node:worker_threads";
import { createCRuntime, CExit } from "/root/src/sh2runtime/src/c-runtime.js";

const TCC = process.env.TCC_PATH || "/root/src/tinycc-wasm/build-wasm32/tcc";
const INC = process.env.TCC_INC || "/tmp/tcc-rb/include";
const WASI_INC = "/usr/include/wasm32-wasi";
const CORPUS = "/root/src/tinycc-wasm/tests/tests2";
const OUT = "/tmp/tcc-corpus";
const RUN_TIMEOUT_MS = 8000;

const WORKER_SRC = `
const { parentPort, workerData } = require("node:worker_threads");
const fs = require("node:fs");
(async () => {
const { createCRuntime, CExit } = await import("/root/src/sh2runtime/src/c-runtime.js");
const { wasmPath, args } = workerData;
const CORPUS = "/root/src/tinycc-wasm/tests/tests2";
const fds = new Map();
const bytes = fs.readFileSync(wasmPath);
let module;
try { module = new WebAssembly.Module(bytes); }
catch (e) { parentPort.postMessage({ kind: "COMPILE-OUT", note: e.message.slice(0, 70) }); return; }
const imports = WebAssembly.Module.imports(module);
const memRef = { memory: null };
const tabRef = { table: null };
let stdout = "";
const envRt = createCRuntime({
  getMem: () => new Uint8Array(memRef.memory.buffer),
  memory: () => memRef.memory,
  out: (t) => { stdout += t; },
  err: (t) => { stdout += t; },
  table: () => tabRef.table,
});
const want = new Set(imports.filter(i => i.module === "env").map(i => i.name));
const env = {};
for (const [k, v] of Object.entries(envRt)) if (want.has(k)) env[k] = v;
{
  const mem8 = () => new Uint8Array(memRef.memory.buffer);
  const rstr = (p) => { const m = mem8(); let e = p; while (e < m.length && m[e]) e++; return Buffer.from(m.subarray(p, e)).toString("utf8"); };
  const fileDir = "/tmp/tcc-corpus-fs";
  try { fs.mkdirSync(fileDir, { recursive: true }); } catch {}
  const modeOf = (mode) => { const m = rstr(mode); if (m.includes("w")) return "w"; if (m.includes("a")) return "a"; return "r"; };
  const f = (k, fn) => { if (want.has(k)) env[k] = fn; };
  f("$fopen", (path, mode) => {
    const name = rstr(path).split("/").pop();
    const m = modeOf(mode);
    const cands = m === "r" ? [fileDir + "/" + name, CORPUS + "/" + name] : [fileDir + "/" + name];
    for (const c of cands) { try { const fd = fs.openSync(c, m); fds.set(fd, c); return fd; } catch {} }
    return 0;
  });
  f("$fwrite", (ptr, size, nmemb, stream) => { try { const n = size * nmemb; return fs.writeSync(stream, mem8().subarray(ptr, ptr + n)); } catch { return 0; } });
  f("$fread", (ptr, size, nmemb, stream) => { try { const n = size * nmemb; return fs.readSync(stream, mem8().subarray(ptr, ptr + n), 0, n, null); } catch { return 0; } });
  f("$fclose", (fd) => { if (fd < 3) return 0; try { fs.closeSync(fd); } catch {} return 0; });
  f("$fgetc", (fd) => { try { const b = Buffer.alloc(1); const n = fs.readSync(fd, b, 0, 1, null); return n ? b[0] : -1; } catch { return -1; } });
  f("$getc", (fd) => { try { const b = Buffer.alloc(1); const n = fs.readSync(fd, b, 0, 1, null); return n ? b[0] : -1; } catch { return -1; } });
  f("$fputs", (s, fd) => { try { const b = Buffer.from(rstr(s)); fs.writeSync(fd, b); return b.length; } catch { return -1; } });
  f("$fputc", (c, fd) => { try { fs.writeSync(fd, Buffer.from([c & 255])); return c & 255; } catch { return -1; } });
  f("$fgets", (p, n, fd) => { try { const m = mem8(); let i = 0; while (i < n - 1) { const b = Buffer.alloc(1); const r = fs.readSync(fd, b, 0, 1, null); if (!r) break; m[p + i] = b[0]; i++; if (b[0] === 10) break; } if (i === 0) return 0; m[p + i] = 0; return p; } catch { return 0; } });
  f("$remove", (path) => { try { fs.unlinkSync(fileDir + "/" + rstr(path).split("/").pop()); return 0; } catch { return -1; } });
  f("$rename", (a, b) => { try { fs.renameSync(fileDir + "/" + rstr(a).split("/").pop(), fileDir + "/" + rstr(b).split("/").pop()); return 0; } catch { return -1; } });
  f("$fflush", () => 0);
  f("$fseek", () => 0);
  f("$ftell", () => 0);
  f("$feof", () => 0);
}
const impObj = { env };
if (imports.some(i => i.module === "wasi_snapshot_preview1")) {
  impObj.wasi_snapshot_preview1 = {
    proc_exit: (code) => { throw new CExit(code); },
    fd_write: () => 0, fd_close: () => 0, fd_seek: () => 0, fd_read: () => 0,
  };
}
let exitCode = 0;
let instance = null;
try {
  instance = new WebAssembly.Instance(module, impObj);
  memRef.memory = instance.exports.memory;
  if (instance.exports.__indirect_function_table) tabRef.table = instance.exports.__indirect_function_table;
  if (instance.exports.__wasm_call_ctors) instance.exports.__wasm_call_ctors();
  if (instance.exports._start && args.length === 0) {
    instance.exports._start();
  } else if (instance.exports.main) {
    const argstr = [wasmPath, ...args];
    const bytes = argstr.map(a => Buffer.byteLength(a) + 1);
    const total = bytes.reduce((x, y) => x + y, 0) + (argstr.length + 1) * 4;
    memRef.memory.grow(Math.ceil((total + 64) / 65536));
    const mem = new Uint8Array(memRef.memory.buffer);
    const base = memRef.memory.buffer.byteLength - total - 32;
    let p = base;
    const ptrs = [];
    for (let ai = 0; ai < argstr.length; ai++) {
      ptrs.push(p);
      mem.set(Buffer.from(argstr[ai], "utf8"), p);
      p += bytes[ai];
    }
    const argv = p;
    for (let i = 0; i < ptrs.length; i++) {
      mem.set([ptrs[i] & 255, (ptrs[i] >> 8) & 255, (ptrs[i] >> 16) & 255, (ptrs[i] >> 24) & 255], p + i * 4);
    }
    instance.exports.main(argstr.length, argv);
  }
} catch (e) {
  if (e instanceof CExit) { exitCode = e.code; }
  else if (e instanceof WebAssembly.RuntimeError) { parentPort.postMessage({ kind: "RUN-CRASH", note: String(e).slice(0, 70) }); return; }
  else { parentPort.postMessage({ kind: "RUN-ERR", note: String(e).slice(0, 70) }); return; }
}
try { if (instance.exports.__wasm_call_fini) instance.exports.__wasm_call_fini(); } catch (e) {}
try { if (envRt.flushAtexit) envRt.flushAtexit(exitCode); } catch (e) {}
parentPort.postMessage({ kind: "DONE", stdout, exitCode });
})();
`;

function runWasm(wasmPath, args) {
  return new Promise((resolve) => {
    const w = new Worker(WORKER_SRC, { eval: true, type: "commonjs", workerData: { wasmPath, args: args || [] } });
    const timer = setTimeout(() => { w.terminate(); resolve({ kind: "RUN-TIMEOUT", note: "infinite loop" }); }, RUN_TIMEOUT_MS);
    w.on("message", (m) => { clearTimeout(timer); w.terminate(); resolve(m); });
    w.on("error", (e) => { clearTimeout(timer); resolve({ kind: "RUN-ERR", note: String(e && e.stack || e).slice(0, 160) }); });
  });
}

function dtSections(cFile) {
  const src = readFileSync(cFile, "utf8");
  const names = [];
  for (const m of src.matchAll(/defined\s+(test_[A-Za-z0-9_]+)/g))
    if (!names.includes(m[1])) names.push(m[1]);
  return names;
}
function runDt(cFile) {
  const name = cFile.replace(/\.c$/, "").split("/").pop();
  const names = dtSections(cFile);
  let out = "";
  let ok = true;
  for (let ni = 0; ni < names.length; ni++) {
    const tn = names[ni];
    const wasm = `${OUT}/${name}-dt.wasm`;
    const sr = spawnSync(TCC, ["-nostdinc", `-I${INC}`, `-I${WASI_INC}`, "-D_WASI_EMULATED_MMAN", `-D${tn}`, cFile, "-o", wasm], { encoding: "utf8" });
    if (ni > 0) out += "\n";
    out += `[${tn}]\n`;
    const err = (sr.stderr || "").replaceAll(CORPUS + "/", "");
    if (sr.status !== 0) {
      out += err.replace(/\n\n+$/, "\n");
      continue;
    }
    if (err.trim())
      out += err;
    let mod;
    try { mod = new WebAssembly.Module(readFileSync(wasm)); }
    catch (e) {
      out += String(e).replaceAll(CORPUS + "/", "").split("\n")[0] + "\n";
      continue;
    }
    const imports = WebAssembly.Module.imports(mod);
    const memRef = { memory: null };
    const tabRef = { table: null };
    const envRt = createCRuntime({ getMem: () => new Uint8Array(memRef.memory.buffer), memory: () => memRef.memory, out: t => out += t, err: t => out += t, table: () => tabRef.table });
    const want = new Set(imports.filter(i => i.module === "env").map(i => i.name));
    const env = {};
    for (const [k, v] of Object.entries(envRt)) if (want.has(k)) env[k] = v;
    const impObj = { env };
    if (imports.some(i => i.module === "wasi_snapshot_preview1"))
      impObj.wasi_snapshot_preview1 = { proc_exit: c => { throw new CExit(c); }, fd_write: () => 0, fd_close: () => 0, fd_seek: () => 0, fd_read: () => 0 };
    let exitCode = 0;
    let inst = null;
    try {
      inst = new WebAssembly.Instance(mod, impObj);
      memRef.memory = inst.exports.memory;
      if (inst.exports.__indirect_function_table) tabRef.table = inst.exports.__indirect_function_table;
      if (inst.exports.__wasm_call_ctors) inst.exports.__wasm_call_ctors();
      if (inst.exports._start) inst.exports._start();
      else if (inst.exports.main) inst.exports.main();
    } catch (e) {
      if (e instanceof CExit) exitCode = e.code;
      else { out += String(e).replaceAll(CORPUS + "/", "") + "\n"; continue; }
    }
    try { if (inst.exports.__wasm_call_fini) inst.exports.__wasm_call_fini(); } catch (e) {}
    try { if (envRt.flushAtexit) envRt.flushAtexit(exitCode); } catch (e) {}

    if (exitCode !== 0) out += `[returns ${exitCode}]\n`;
  }
  return out;
}

const COMPANIONS = {
  "104_inline": ["104+_inline.c"],
  "120_alias": ["120+_alias.c"],
};
const ARGS = {
  "31_args": ["arg1", "arg2", "arg3", "arg4", "arg5"],
  "46_grep": ["[^* ]*[:a:d: ]+\\:\\*-/: $", "46_grep.c"],
};

function runOne(cFile, expect) {
  const name = cFile.replace(/\.c$/, "");
  const base = name.split("/").pop();
  const wasm = `${OUT}/${base.replace(/\.c$/, "")}.wasm`;
  const sources = [base + ".c", ...(COMPANIONS[base] || [])];
  const args = ARGS[base] || [];
  const sr = spawnSync(TCC, ["-nostdinc", `-I${INC}`, `-I${WASI_INC}`, "-D_WASI_EMULATED_MMAN", ...sources, "-o", wasm], { encoding: "utf8", cwd: CORPUS });
  if (sr.status !== 0)
    return { kind: "REFUSE", note: (sr.stderr || sr.stdout || "").split("\n").filter(Boolean).pop()?.slice(0, 70) };
  const compWarn = (sr.stderr || "").replaceAll(CORPUS + "/", "");
  return { kind: "RAN", wasm, args, compWarn };
}

const SKIP = new Set([
  "34_array_assignment", "85_asm-outside-function", "98_al_ax_extend",
  "99_fastcall", "127_asm_goto", "138_arm64_encoding", "139_arm64_errors",
  "140_arm64_extasm", "141_riscv_asm", "145_winarm64_interlocked",
  "112_backtrace", "113_btdll", "114_bound_signal", "115_bound_setjmp",
  "116_bound_setjmp2", "117_builtins", "126_bound_global", "132_bound_test",
  "106_versym", "124_atomic_counter", "144_tls", "146_tls_extern",
  "95_bitfields", "95_bitfields_ms",
  "149_end_copy_reloc", "151_dso_linker_symbol",
  "148_linker_symbols", "150_linker_boundaries",
]);
const DT_TESTS = new Set(["60_errors_and_warnings", "96_nodata_wanted", "125_atomic_misc", "128_run_atexit"]);
const files = readdirSync(CORPUS).filter(f => f.endsWith(".c") && f !== "42test.h" && !SKIP.has(f.replace(/\.c$/, "")));
const rows = [];
for (const f of files) {
  let expect = "";
  try { expect = readFileSync(`${CORPUS}/${f.replace(/\.c$/, ".expect")}`, "utf8"); }
  catch { rows.push({ f, kind: "NO-EXPECT", note: "no .expect file" }); continue; }
  if (DT_TESTS.has(f.replace(/\.c$/, ""))) {
    const out = runDt(`${CORPUS}/${f}`);
    const norm = (x) => x.split("\n").map(l => l.replace(/ +$/, "")).join("\n");
    const got = norm(out.replace(/\n$/, "")), wantOut = norm(expect.replace(/\n$/, ""));
    rows.push({ f, kind: got === wantOut ? "PASS" : "WRONG", note: got === wantOut ? "" : `dt: got ${got.length} want ${wantOut.length} chars` });
    continue;
  }
  const c = runOne(`${CORPUS}/${f}`, expect);
  if (c.kind !== "RAN") { rows.push({ f, ...c }); continue; }
  const r = await runWasm(c.wasm, c.args);
  if (r.kind === "DONE") {
    const norm = (x) => x.split("\n").map(l => l.replace(/ +$/, "")).join("\n");
    const warn = /: warning:/.test(expect) ? c.compWarn : "";
    const got = norm((warn + r.stdout).replace(/\n$/, ""));
    const wantOut = norm(expect.replace(/\n$/, ""));
    if (got === wantOut) rows.push({ f, kind: "PASS" });
    else {
      const gl = got.split("\n"), wl = wantOut.split("\n");
      let d = 0; while (d < Math.min(gl.length, wl.length) && gl[d] === wl[d]) d++;
      rows.push({ f, kind: "WRONG", note: `line ${d + 1}: got "${(gl[d]||"").slice(0,40)}" want "${(wl[d]||"").slice(0,40)}"` });
    }
  } else {
    rows.push({ f, ...r });
  }
}
const counts = {};
for (const r of rows) counts[r.kind] = (counts[r.kind] || 0) + 1;
console.log(`\n=== tcc wasm32 × tests2 corpus (${rows.length} tests) ===`);
console.log(Object.entries(counts).map(([k, v]) => `${k}: ${v}`).join("  "));
console.log("─".repeat(70));
for (const r of rows) {
  if (r.kind !== "PASS")
    console.log(`${(r.kind + "      ").slice(0, 12)} ${r.f}${r.note ? "  — " + r.note : ""}`);
}
writeFileSync("/tmp/tcc-corpus-report.json", JSON.stringify(rows, null, 1));
