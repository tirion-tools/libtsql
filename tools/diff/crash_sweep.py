#!/usr/bin/env python3
"""Deterministic crash sweep of tsql_dump over broken scripts:

    tools/diff/crash_sweep.py [--build DIR] [--version V]... [--jobs N]

Inputs: every unique SqlScriptDOM test script ($P/ssd/Test/SqlDom, P = workspace.P, deduplicated by content),
decoded like tsql_dump, split into rough T-SQL tokens (a word or one other non-space character). Variants:
the script cut after every TRUNC_EVERY-th token, and the script without every DELETE_EVERY-th token
(starting at DELETE_FIRST). 44,432 variants, the same for every version, written once to
$P/sweep/inputs. Each version's tsql_dump (default: every version the build has) parses them
in batches under $P/bin/capped when present, each tsql_dump limited to 2 GB of address space (a runaway
aborts on its own allocation, exit 134, instead of being OOM-killed; TSQL_DUMP_LIMIT replaces that prefix,
empty for sanitizer builds, which limit themselves with ASAN_OPTIONS=hard_rss_limit_mb; give them
abort_on_error=1 in ASAN_OPTIONS and UBSAN_OPTIONS too, or a report exits 1 like a parse error and is
not counted); when a batch dies of a signal (exit >= 128), the
first file without a dump is recorded and the batch resumes after it. Prints every crashing variant as
`<version> <exit> <file>`; exits 1 if there is any. About 6 minutes for all seven versions (4 jobs)."""
import argparse, glob, hashlib, os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

from workspace import P
CORPUS = P + '/ssd/Test/SqlDom'
WORK = P + '/sweep'
TRUNC_EVERY = 7
DELETE_EVERY = 13
DELETE_FIRST = 3
BATCH = 400
TOKEN = re.compile(r'\w+|[^\w\s]')


def decode(raw):
    if raw.startswith(b'\xef\xbb\xbf'):
        return raw[3:].decode('utf-8', 'replace')
    if raw[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return raw.decode('utf-16')
    return raw.decode('utf-8', 'replace')


def make_inputs():
    out = WORK + '/inputs'
    stamp = out + '/.done-%d-%d-%d' % (TRUNC_EVERY, DELETE_EVERY, DELETE_FIRST)
    if os.path.exists(stamp):
        return sorted(f for f in os.listdir(out) if f.endswith('.sql'))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    seen = set()
    names = []
    for path in sorted(glob.glob(CORPUS + '/**/*.sql', recursive=True)):
        raw = open(path, 'rb').read()
        digest = hashlib.sha1(raw).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        text = decode(raw)
        spans = [m.span() for m in TOKEN.finditer(text)]
        base = os.path.relpath(path, CORPUS).replace('/', '__')[:-4]
        variants = [('t%d' % k, text[:spans[k][1]]) for k in range(TRUNC_EVERY - 1, len(spans), TRUNC_EVERY)]
        variants += [('d%d' % k, text[:spans[k][0]] + text[spans[k][1]:])
                     for k in range(DELETE_FIRST, len(spans), DELETE_EVERY)]
        for tag, body in variants:
            name = '%s.%s.sql' % (base, tag)
            open(out + '/' + name, 'w', encoding='utf-8', newline='').write(body)
            names.append(name)
    open(stamp, 'w', encoding='utf-8').close()
    return sorted(names)


def run(cmd):
    """Exit status as a shell reports it: a process killed by signal N gives 128 + N."""
    rc = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode
    return 128 - rc if rc < 0 else rc


def check_batch(dump, cap, version, files):
    """tsql_dump writes each dump before reading the next file, so after a crash the first input
    without a dump is the culprit; the batch resumes after it."""
    crashes = []
    with tempfile.TemporaryDirectory(dir=WORK) as tmp:
        while files:
            rc = run(cap + [dump, '--version', version, tmp] + [WORK + '/inputs/' + f for f in files])
            if rc < 128:
                break
            done = set(os.listdir(tmp))
            first = next(i for i, f in enumerate(files) if f + '.json' not in done)
            crashes.append((version, rc, files[first]))
            files = files[first + 1:]
    return crashes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--build', default=P + '/build')
    ap.add_argument('--version', action='append')
    ap.add_argument('--jobs', type=int, default=4)
    a = ap.parse_args()
    dump = a.build + '/src/parser/tsql_dump'
    limit = os.environ.get('TSQL_DUMP_LIMIT', 'prlimit --as=%d' % (2 << 30)).split()
    cap = ([P + '/bin/capped'] if os.access(P + '/bin/capped', os.X_OK) else []) + limit
    versions = a.version or subprocess.run([dump, '--list-versions'], capture_output=True, encoding='utf-8',
                                           check=True).stdout.split()
    os.makedirs(WORK, exist_ok=True)
    files = make_inputs()
    print('%d variants x %d versions' % (len(files), len(versions)), flush=True)
    batches = [(v, files[i:i + BATCH]) for v in versions for i in range(0, len(files), BATCH)]
    crashes = []
    with ThreadPoolExecutor(a.jobs) as pool:
        for found in pool.map(lambda b: check_batch(dump, cap, b[0], b[1]), batches):
            for c in found:
                print('%s %d %s' % c, flush=True)
            crashes += found
    print('crashes: %d' % len(crashes))
    sys.exit(1 if crashes else 0)


if __name__ == '__main__':
    main()
