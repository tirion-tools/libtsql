#!/usr/bin/env python3
"""Porting aid: first-draft C++ for SqlScriptDOM parser-base methods (TSql<ver>ParserBaseInternal.cs).

    python3 tools/g2to4/portbase.py --ssd DIR --ast-enums BUILD/.../enums.hpp --grammar TSql170 NAME... > draft.txt

Finds each NAME along the base-class chain of <grammar>ParserBaseInternal (most derived first),
adds the base methods those bodies call (transitively, skipping names already declared in the
hand-written src/parser/<class>.h of any class of the chain), and translates bodies with the
grammar-action translator (cs2cpp.py, 'method' mode). Output: declarations, then definitions, each
tagged with its C# origin and qualified with the C++ class mirroring the defining C# class
(TSql90ParserBaseInternal -> TSql90ParserBase); bodies it cannot translate carry G2TO4_UNTRANSLATED
and the C# text. Static fields the bodies read are listed on stderr: they need hand ports. The
drafts are meant to be pasted into the hand-written bases and fixed there.
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import convert  # noqa: E402
from cs2cpp import Scope, Translator, Untranslatable  # noqa: E402
from model import Model, read_text  # noqa: E402

METHOD_RE = re.compile(r'^\s*(?:\[[^\]]*\]\s*)*(?:new\s+)?(protected|internal|public|private)\s+'
                       r'(?:static\s+|override\s+|virtual\s+|new\s+)*([\w<>\[\],\.\?]+)\s+(\w+)\s*(<[^>]*>)?\s*\(')
FIELD_RE = re.compile(r'^\s*(?:protected|internal|public|private)?\s*(?:static\s+|readonly\s+|const\s+)+'
                      r'([\w<>\[\],\.\? ]+?)\s+(\w+)\s*(=|;)')


def base_chain(ssd, grammar):
    d = os.path.join(ssd, 'SqlScriptDom/Parser/TSql')
    chain, cls = [], grammar + 'ParserBaseInternal'
    while cls.startswith('TSql'):
        chain.append(cls)
        text = read_text(os.path.join(d, cls + '.cs'))
        m = re.search(r'class\s+%s\s*:\s*(\w+)' % cls, text)
        cls = m.group(1) if m else ''
    return chain


def scan(ssd, chain):
    d = os.path.join(ssd, 'SqlScriptDom/Parser/TSql')
    methods, fields = {}, {}
    for cls in chain:
        lines = read_text(os.path.join(d, cls + '.cs')).splitlines()
        i = 0
        while i < len(lines):
            m = METHOD_RE.match(lines[i])
            if m:
                depth, started = 0, False
                for j in range(i, len(lines)):
                    depth += lines[j].count('{') - lines[j].count('}')
                    started = started or '{' in lines[j]
                    if started and depth == 0:
                        break
                methods.setdefault(m.group(3), []).append((cls, i + 1, '\n'.join(lines[i:j + 1])))
                i = j + 1
                continue
            f = FIELD_RE.match(lines[i])
            if f:
                fields.setdefault(f.group(2), (cls, i + 1))
            i += 1
    return methods, fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ssd', required=True, help='SqlScriptDOM source tree')
    ap.add_argument('--ast-enums', required=True, help="the AST's generated enums.hpp (in the build tree)")
    ap.add_argument('--grammar', default='TSql170')
    ap.add_argument('names', nargs='+')
    a = ap.parse_args()
    repo = convert.REPO
    chain = base_chain(a.ssd, a.grammar)
    methods, fields = scan(a.ssd, chain)
    declared = set()
    for cls in chain:
        hdr_path = os.path.join(repo, 'src/parser/%s.h' % cls.replace('Internal', ''))
        if os.path.exists(hdr_path):
            declared |= set(re.findall(r'\b(\w+)\s*\(', read_text(hdr_path)))
    work = [n for n in a.names if n in methods and n not in declared]
    picked, used_fields = {}, set()
    while work:
        n = work.pop()
        if n in picked:
            continue
        picked[n] = methods[n]
        for _, _, text in methods[n]:
            body = text.split('{', 1)[1] if '{' in text else ''
            for c in set(re.findall(r'(?<![\.\w])(\w+)\s*(?:<[^>()]*>)?\s*\(', body)):
                if c in methods and c not in picked and c not in declared:
                    work.append(c)
            used_fields |= {w for w in re.findall(r'(?<![\.\w])(\w+)\b', body) if w in fields}

    model = Model(a.ssd, a.ast_enums)
    vocab, _ = convert.token_vocab(a.ssd)
    tr = Translator(model, {n for n, _ in vocab}, a.grammar + 'Parser')
    decls, defs, seen = [], [], set()
    for name in sorted(picked):
        for cls, line, text in picked[name]:
            m = re.match(r'\s*(?:\[[^\]]*\]\s*)*(?:new\s+)?(?:protected|internal|public|private)\s+(?:static\s+|override\s+|'
                         r'virtual\s+|new\s+)*([\w<>\[\],\.\?]+)\s+(\w+)\s*(<[^>]*>)?\s*\((.*?)\)\s*\{', text, re.S)
            if not m:
                print('signature not understood: %s (%s:%d)' % (name, cls, line), file=sys.stderr)
                continue
            ret, generic, params = m.group(1), m.group(3), m.group(4)
            body = text[m.end():text.rindex('}')]
            sc, cps = Scope(), []
            try:
                for p in (convert.split_top(params) if params.strip() else []):
                    w = re.sub(r'\s*=\s*.*$', '', p).split()
                    ref = w[0] in ('ref', 'out')
                    w = w[1:] if ref or w[0] == 'params' else w
                    ty, pn = ' '.join(w[:-1]), w[-1]
                    ct, kind = (ty, 'unknown') if generic and ty in generic else tr.cpp_type(ty)
                    sc.add(pn, kind, ct)
                    if ref:
                        cps.append('%s& %s' % (ct, pn))
                    elif ct.startswith('ast::') or ct.endswith('*'):
                        cps.append('%s %s' % (ct, pn))
                    elif ct.startswith('std::vector'):
                        cps.append('const %s& %s' % (ct, pn))
                    elif ct == 'std::string':
                        cps.append('CsStr %s' % pn)
                    else:
                        cps.append('%s %s' % (ct, pn))
                rct = 'void' if ret == 'void' else (ret if generic and ret in generic else tr.cpp_type(ret)[0])
            except Untranslatable as ex:
                print('signature of %s (%s:%d): %s' % (name, cls, line, ex), file=sys.stderr)
                continue
            sig = '(%s)' % ', '.join(cps)
            if name + sig in seen:
                continue
            seen.add(name + sig)
            tmpl = 'template <%s> ' % ', '.join('class ' + g.strip() for g in generic[1:-1].split(',')) if generic else ''
            try:
                out, note = tr.translate(body, sc, 'method'), ''
            except Untranslatable as ex:
                out = '\n    G2TO4_UNTRANSLATED("%s"); /* C#:\n%s */\n' % (ex, body.replace('*/', '* /'))
                note = ' NEEDS HAND PORT'
            cs_lines = len([x for x in text.splitlines() if x.strip() and not x.strip().startswith('//')])
            decls.append('    %s%s %s%s;   // %s.cs:%d' % (tmpl, rct, name, sig, cls, line))
            defs.append('// %s.cs:%d (%d C# lines)%s\n%s%s %s::%s%s {%s}\n'
                        % (cls, line, cs_lines, note, tmpl, rct, cls.replace('Internal', ''), name, sig, out))
    print('// ---- declarations\n' + '\n'.join(decls) + '\n\n// ---- definitions\n' + '\n'.join(defs))
    for f in sorted(used_fields):
        print('static field needs a hand port: %s (%s:%d)' % ((f,) + fields[f]), file=sys.stderr)
    missing = [n for n in a.names if n not in methods and n not in declared]
    if missing:
        print('not found in %s: %s' % (' -> '.join(chain), ' '.join(missing)), file=sys.stderr)


if __name__ == '__main__':
    main()
