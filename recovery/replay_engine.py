#!/usr/bin/env python3
"""Replay the fork's file modifications from session transcripts.

Reads /tmp/fork-ops.json (list of [tag, kind, payload...]) and replays
them against the pristine repo files, modeling git stash/checkout/commit
and /tmp file copies.  The final state is written back to the repo.
"""
import json, os, re, sys, subprocess, shutil, traceback

REPO = "/root/src/tinycc-wasm"
SANDBOX = "/tmp/fork-replay"
TMPD = "/tmp/fork-tmp"          # our private /tmp mirror
os.makedirs(SANDBOX, exist_ok=True)
os.makedirs(TMPD, exist_ok=True)

# files we track
TRACK = ["wasm32-gen.c", "tccgen.c", "tcc.h", "tccelf.c", "include/stddef.h"]

# load pristine repo files into the sandbox
for t in TRACK:
    src = os.path.join(REPO, t)
    if os.path.exists(src):
        shutil.copy(src, os.path.join(SANDBOX, os.path.basename(t)))
    else:
        # empty placeholder (created later)
        open(os.path.join(SANDBOX, os.path.basename(t)), "w").write("")

def read_file(name):
    # name may be 'wasm32-gen.c' (sandbox) or '/tmp/...' (TMPD)
    if name.startswith("/tmp/"):
        p = os.path.join(TMPD, name[5:])
    elif "/" in name:
        p = os.path.join(SANDBOX, os.path.basename(name))
    else:
        p = os.path.join(SANDBOX, name)
    try:
        return open(p, encoding="utf-8", errors="replace").read()
    except FileNotFoundError:
        return None

def write_file(name, content):
    if name.startswith("/tmp/"):
        p = os.path.join(TMPD, name[5:])
    else:
        p = os.path.join(SANDBOX, os.path.basename(name))
    if os.path.isdir(p):
        return
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True)
    open(p, "w", encoding="utf-8").write(content)

# HEAD snapshot per tracked file (committed state)
HEAD = {}
for t in TRACK:
    c = read_file(os.path.basename(t))
    if c is not None:
        HEAD[os.path.basename(t)] = c

stash_stack = []   # stack of dicts name->content

def current_files():
    return {os.path.basename(t): read_file(os.path.basename(t)) for t in TRACK}

def apply_edit(path, old, new):
    name = os.path.basename(path)
    s = read_file(name)
    if s is None:
        return "MISSING"
    n = s.count(old)
    if n == 1:
        write_file(name, s.replace(old, new, 1))
        return "OK"
    return f"n={n}"

def run_python(code):
    # write the code to a file, run in SANDBOX cwd with real /tmp for /tmp paths
    script = os.path.join(TMPD, "_script.py")
    open(script, "w").write(code)
    # point /tmp reads at TMPD: run with HOME-like redirection?  Scripts use
    # literal '/tmp/...' paths -> we pre-create them in real /tmp.
    env = dict(os.environ)
    try:
        r = subprocess.run([sys.executable, script], cwd=SANDBOX, env=env,
                           capture_output=True, text=True, timeout=60)
    except Exception as e:
        return f"PY-EXC {e}"
    if r.returncode != 0:
        return f"PY-ERR rc={r.returncode}: {r.stderr[:300]}"
    return "PY-OK"

def sync_tmp_mirror():
    # ignore write-protected files
    import stat as _st
    # copy TMPD files into real /tmp so scripts reading /tmp/... work
    for fn in os.listdir(TMPD):
        src = os.path.join(TMPD, fn)
        dst = os.path.join("/tmp", fn)
        if os.path.isfile(src) and not fn.startswith("_"):
            try:
                shutil.copy(src, dst)
            except PermissionError:
                pass

def cp(src, dst):
    content = read_file(src)
    if content is None:
        return f"CP-MISSING {src}"
    write_file(dst, content)
    return "CP-OK"

# ---- op processors ----
def process_command(cmd):
    """Process a compound bash command (cd && A && B | python3 - <<PYEOF ...).
       Returns a list of result strings."""
    results = []
    if re.search(r"cd /tmp/tcc-pristine", cmd):
        return ["SKIP-pristine-copy"]
    if re.search(r"cd /tmp/pristine-build", cmd):
        return ["SKIP-pristine-copy"]
    # extract heredoc python first
    m = re.search(r"<<'PYEOF'\n(.*?)\nPYEOF", cmd, re.S)
    heredoc = m.group(1) if m else None
    # strip the heredoc body from the command for segment splitting
    cmd_noh = re.sub(r"<<'PYEOF'\n.*?\nPYEOF", "<<'PYEOF'", cmd, flags=re.S)
    # split into segments on && / ; / | / newlines (top level)
    segs = re.split(r"\s*(?:&&|;|\||\n)\s*", cmd_noh)
    for seg in segs:
        seg = seg.strip()
        if not seg:
            continue
        if seg.startswith("cd "):
            continue
        if re.match(r"(make|\./tcc|node|echo|grep|sed -n|cat |ls |head|tail|wc |git log|git show|git diff|git status|git rev|timeout|python3 -c|rm |mkdir|cp /tmp/.*\.c wasm|./build-wasm32)", seg):
            if seg.startswith("cp ") and re.search(r"wasm32-gen\.c|tccgen\.c", seg):
                results.append("CP-SKIP? " + seg[:80])
            continue
        if seg == "python3 - <<'PYEOF'" or seg.startswith("python3 -"):
            if heredoc is not None:
                sync_tmp_mirror()
                r = run_python(heredoc)
                results.append(r)
                # copy any real-/tmp files the script may have created back
                for fn in os.listdir("/tmp"):
                    p = os.path.join("/tmp", fn)
                    if os.path.isfile(p) and (fn.endswith(".c") or fn.endswith(".wasm") or fn.endswith(".py") or fn.startswith("w")):
                        try:
                            shutil.copy(p, os.path.join(TMPD, fn))
                        except Exception:
                            pass
                heredoc = None
            else:
                results.append("PY-NOHEREDOC")
            continue
        if seg.startswith("cp "):
            mm = re.match(r"cp (\S+) (\S+)", seg)
            if mm:
                results.append(cp(mm.group(1), mm.group(2)))
            continue
        if seg.startswith("git "):
            results.append(process_git(seg))
            continue
        if seg.startswith("sed -i"):
            results.append("SED-UNHANDLED " + seg[:60])
            continue
        results.append("UNHANDLED " + seg[:80])
    return results

def process_git(seg):
    if re.match(r"git stash -q\b", seg):
        stash_stack.append(current_files())
        for t in TRACK:
            write_file(os.path.basename(t), HEAD[os.path.basename(t)])
        return "STASH"
    if re.match(r"git stash pop", seg):
        if stash_stack:
            for k, v in stash_stack.pop().items():
                write_file(k, v)
        return "STASH-POP"
    def do_checkout(files):
        out = []
        for f in files:
            b = os.path.basename(f)
            if b in HEAD:
                write_file(b, HEAD[b])
                out.append(b)
        return "CHECKOUT " + " ".join(out)
    mco = re.match(r"git checkout -q --? (.*)", seg)
    if mco:
        files = mco.group(1).split()
        # strip junk like 2>/dev/null pieces
        files = [f for f in files if f.endswith(".c") or f.endswith(".h")]
        return do_checkout(files)
    mco2 = re.match(r"git checkout -q (HEAD~\d+|HEAD) --? (.*)", seg)
    if mco2:
        return do_checkout(mco2.group(2).split())
    if re.match(r"git reset -q\b", seg):
        return "RESET"
    if re.match(r"git add", seg):
        return "ADD"
    if re.match(r"git commit", seg):
        for t in TRACK:
            HEAD[os.path.basename(t)] = read_file(os.path.basename(t))
        return "COMMIT"
    return "GIT-UNHANDLED " + seg[:60]

# ---- main ----
ops = json.load(open("/tmp/fork-ops.json"))
report = open("/tmp/replay-report.txt", "w")
failcount = 0
for idx, op in enumerate(ops):
    tag, kind = op[0], op[1]
    if kind == "EDIT":
        _, _, path, old, new = op
        if tag == "03":
            # the 03 session's edits ARE the committed 726e659 content — the
            # clone already has them (re-applying duplicates structs etc.)
            continue
        r = apply_edit(path, old, new)
        if r != "OK":
            failcount += 1
            report.write(f"[{idx}] {tag} EDIT {os.path.basename(path)} {r}: {old[:60]!r}\n")
    elif kind == "WRITE":
        _, _, path, content = op
        write_file(path, content)
    elif kind == "CAT":
        _, _, path, content = op
        write_file(path, content)
    elif kind in ("PY", "CP", "GIT", "SED"):
        _, _, cmd, code = op
        sync_tmp_mirror()
        for r in process_command(cmd):
            if r.startswith("PY-ERR") or r.startswith("PY-EXC") or r.startswith("CP-MISSING"):
                failcount += 1
                report.write(f"[{idx}] {tag} {r}\n")
    if (idx+1) % 50 == 0:
        print(f"  ...{idx+1}/{len(ops)} (fails so far {failcount})", file=sys.stderr)

report.close()
print(f"done. fails={failcount}")
print("final sizes:")
for t in TRACK:
    p = os.path.join(SANDBOX, os.path.basename(t))
    if os.path.exists(p):
        print(" ", t, os.path.getsize(p))
