#!/usr/bin/env python3
"""Per-version gate corpora from the source-built oracle's full-corpus dumps
($P/oracle-<V>, P = workspace.P, SqlScriptDOM @ eaf3a6e, `oracle dump --parser <V>` + `oracle classify`):

    tools/diff/mk_heldout_all.py --version TSql160 [--version ...]   (default: every version)

writes, for each version V:
    <V>-ok-list.txt / <V>-err-list.txt   unique test scripts V's oracle parses without / with errors
    heldout-<V>/                         each top-level statement of the ok scripts as its own .sql
                                         file named by the SHA-1 of its text (deduplicated)
    hoa-<V>-list.txt, hoa-<V>-origin.json
    hoa-<V>-oracle/                      heldout-<V> dumped with V's oracle

Statements are cut out by the oracle's StartOffset/FragmentLength (UTF-16 units)."""
import argparse, hashlib, json, os, shutil, subprocess

from workspace import P
ORACLE = P + '/build-OracleSrc/bin/Release/net10.0/oracle'
VERSIONS = ['TSql130', 'TSql140', 'TSql150', 'TSql160', 'TSql170', 'TSql180', 'TSqlFabricDW']


def read_script(path):
    raw = open(path, 'rb').read()
    if raw.startswith(b'\xef\xbb\xbf'):
        return raw[3:].decode('utf-8', 'replace')
    if raw.startswith(b'\xff\xfe') or raw.startswith(b'\xfe\xff'):
        return raw.decode('utf-16')
    return raw.decode('utf-8', 'replace')


def build(version):
    dumps = '%s/oracle-%s' % (P, version)
    idx = json.load(open(dumps + '/index.json', encoding='utf-8'))
    if not idx['parser'].startswith(version + 'Parser('):
        raise SystemExit('%s/index.json was made by %s' % (dumps, idx['parser']))
    ok = sorted(e['path'] for e in idx['entries'] if e['ok'])
    err = sorted(e['path'] for e in idx['entries'] if not e['ok'])
    open('%s/%s-ok-list.txt' % (P, version), 'w', encoding='utf-8', newline='\n').write(''.join(p + '\n' for p in ok))
    open('%s/%s-err-list.txt' % (P, version), 'w', encoding='utf-8', newline='\n').write(''.join(p + '\n' for p in err))
    out = '%s/heldout-%s' % (P, version)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    seen = {}
    n_stmts = 0
    for e in idx['entries']:
        if not e['ok']:
            continue
        u16 = read_script(e['source']).encode('utf-16-le')
        tree = json.load(open(dumps + '/' + e['path'] + '.json', encoding='utf-8'))['tree']
        for b in tree['Batches']:
            for s in b['Statements']:
                first, last, off, length = s['$pos']
                if off < 0 or length <= 0:
                    continue
                n_stmts += 1
                stmt = u16[2 * off:2 * (off + length)].decode('utf-16-le')
                h = hashlib.sha1(stmt.encode('utf-8')).hexdigest()[:12]
                if h in seen:
                    continue
                seen[h] = (e['path'], s['$type'])
                open('%s/%s.sql' % (out, h), 'w', encoding='utf-8', newline='').write(stmt)
    open('%s/hoa-%s-list.txt' % (P, version), 'w', encoding='utf-8', newline='\n').write(''.join('%s.sql\n' % h for h in sorted(seen)))
    json.dump(seen, open('%s/hoa-%s-origin.json' % (P, version), 'w', encoding='utf-8'), indent=0, sort_keys=True)
    odir = '%s/hoa-%s-oracle' % (P, version)
    shutil.rmtree(odir, ignore_errors=True)
    subprocess.run([ORACLE, 'dump', '--parser', version, '--jobs', '8', odir, out], check=True,
                   stdout=subprocess.DEVNULL)
    print('%s: ok %d err %d statements %d unique %d' % (version, len(ok), len(err), n_stmts, len(seen)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--version', action='append', choices=VERSIONS)
    for v in ap.parse_args().version or VERSIONS:
        build(v)


if __name__ == '__main__':
    main()
