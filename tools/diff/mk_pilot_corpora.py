#!/usr/bin/env python3
"""TSql170 pilot gate corpora derived from the NuGet oracle's full-corpus dumps ($P/oracle-dumps, P = workspace.P,
`oracle dump` + `oracle classify`, ScriptDom 180.117.0, TSql170Parser):

    tools/diff/mk_pilot_corpora.py

writes into $P:
    select-rel.txt              select-corpus.txt relative to ssd/Test/SqlDom
    heldout/, ho-list.txt       every SelectStatement of the ok scripts outside the select corpus, as its own
                                .sql named by the SHA-1 of its text (deduplicated)
    bench-big.sql               heldout joined by GO, 8 times (427 KB); bigdir/ + big-list.txt hold a copy
    bench-big-err.sql           bench-big.sql + a broken statement at the end; bench-big-err-start.sql: at the
                                start; bigerr/ + bigerr-list.txt hold both
    mut/, mut-list.txt          3 mutants (token deleted, duplicated, inserted) of each heldout statement with 3+
                                tokens; random.Random(1234)

These reproduce the original pilot's ad hoc generators exactly, including their quirks (heldout reads scripts
with universal newlines while cutting by the oracle's UTF-16 offsets), so the recorded floors still apply.
The oracle dumps of these corpora are made by setup-workspace.sh."""
import glob, hashlib, json, os, random, re, shutil

from workspace import P
R = P + '/ssd/Test/SqlDom'


def fresh(path):
    shutil.rmtree(path, ignore_errors=True)
    os.makedirs(path)


def write_list(path, names):
    open(path, 'w', encoding='utf-8', newline='\n').write(''.join(n + '\n' for n in names))


def select_rel():
    corpus = [l.rstrip('\n') for l in open(P + '/oracle-dumps/select-corpus.txt', encoding='utf-8')]
    write_list(P + '/select-rel.txt', [c[len(R) + 1:] if c.startswith(R + '/') else c for c in corpus])


def heldout():
    idx = json.load(open(P + '/oracle-dumps/index.json', encoding='utf-8'))
    sel = set(l.strip() for l in open(P + '/select-rel.txt', encoding='utf-8'))
    out = P + '/heldout'
    fresh(out)
    enc_map = {'utf-8': 'utf-8', 'utf-8-bom': 'utf-8-sig', 'utf-16-bom': 'utf-16'}
    seen = set()
    for e in idx['entries']:
        if not e['ok'] or e['path'] in sel or 'SelectStatement' not in e['statementTypes']:
            continue
        enc = enc_map.get(e['encoding'])
        if not enc:
            continue
        u16 = open(e['source'], encoding=enc).read().encode('utf-16-le')
        d = json.load(open('%s/oracle-dumps/%s.json' % (P, e['path']), encoding='utf-8'))
        for b in d['tree']['Batches']:
            for s in b['Statements']:
                if s['$type'] != 'SelectStatement':
                    continue
                st, ln = s['$pos'][2], s['$pos'][3]
                frag = u16[st * 2:(st + ln) * 2].decode('utf-16-le')
                h = hashlib.sha1(frag.encode()).hexdigest()[:12]
                if h in seen:
                    continue
                seen.add(h)
                open('%s/%s.sql' % (out, h), 'w', encoding='utf-8', newline='').write(frag)
    write_list(P + '/ho-list.txt', sorted(os.listdir(out)))
    return len(seen)


def big():
    parts = [open(f, encoding='utf-8').read() for f in sorted(glob.glob(P + '/heldout/*.sql'))]
    one = '\nGO\n'.join(parts) + '\n'
    open(P + '/bench-big.sql', 'w', encoding='utf-8', newline='').write(one * 8)
    text = open(P + '/bench-big.sql', 'rb').read()
    open(P + '/bench-big-err.sql', 'wb').write(text + b'\nSELECT FROM WHERE\n')
    open(P + '/bench-big-err-start.sql', 'wb').write(b'SELECT FROM WHERE\nGO\n' + text)
    fresh(P + '/bigdir')
    shutil.copy(P + '/bench-big.sql', P + '/bigdir/')
    write_list(P + '/big-list.txt', ['bench-big.sql'])
    fresh(P + '/bigerr')
    for name in ('bench-big-err.sql', 'bench-big-err-start.sql'):
        shutil.copy(P + '/' + name, P + '/bigerr/')
    write_list(P + '/bigerr-list.txt', sorted(os.listdir(P + '/bigerr')))
    return len(text)


def mut():
    out = P + '/mut'
    fresh(out)
    rnd = random.Random(1234)
    inserts = [',', 'FROM', 'SELECT', '(', ')', 'WHERE', 'AS', '.', '+', 'ON', 'JOIN', "'x'"]
    for f in sorted(glob.glob(P + '/heldout/*.sql')):
        text = open(f, encoding='utf-8').read()
        spans = [m.span() for m in re.finditer(r'\S+', text)]
        if len(spans) < 3:
            continue
        base = os.path.basename(f)[:-4]
        for kind in ('del', 'dup', 'ins'):
            a, b = spans[rnd.randrange(len(spans))]
            if kind == 'del':
                t = text[:a] + text[b:]
            elif kind == 'dup':
                t = text[:b] + ' ' + text[a:b] + text[b:]
            else:
                t = text[:a] + rnd.choice(inserts) + ' ' + text[a:]
            open('%s/%s_%s.sql' % (out, base, kind), 'w', encoding='utf-8', newline='').write(t)
    names = sorted(os.listdir(out))
    write_list(P + '/mut-list.txt', names)
    return len(names)


def main():
    select_rel()
    n_held = heldout()
    n_big = big()
    n_mut = mut()
    print('heldout %d statements, bench-big.sql %d bytes, mut %d scripts' % (n_held, n_big, n_mut))


if __name__ == '__main__':
    main()
