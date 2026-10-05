#!/usr/bin/env python3
"""Generate the libtsql C++ AST from Microsoft SqlScriptDOM's Ast.xml.

Inputs (read-only, from a SqlScriptDOM checkout):
  SqlScriptDom/Parser/TSql/Ast.xml          AST classes, members, interfaces
  SqlScriptDom/**/*.cs                      C# enum declarations used by members
  SqlScriptDom/Parser/TSql/TSqlTokenTypes.g token vocabulary (ANTLR 2 numbering)

    astgen.py --ssd DIR --ssd-commit SHA --partial-dir DIR --out-include DIR --out-src DIR [--chunks N]

Run by the build (src/ast/CMakeLists.txt); writes only into the two output directories, and
rewrites a file only when its content changes. Outputs:
  <out-include>/token_types.hpp           enum class TSqlTokenType
  <out-include>/enums.hpp                 member enums (+ flags operators)
  <out-include>/nodes.hpp                 interfaces, node structs
  <out-include>/fragment_type_id.hpp      FragmentTypeId
  <out-include>/visitor_members.inc, concrete_visitor_members.inc
                                          Visit/ExplicitVisit declarations
  <out-src>/enums.cpp                     ToString() per enum (.NET Enum.ToString)
  <out-src>/dump.cpp                      DumpJson member walkers
  <out-src>/visitor_<n>.cpp               TSqlFragmentVisitor / TSqlConcreteFragmentVisitor
  <out-src>/accept_<n>.cpp                Accept / AcceptChildren per node
(<n> = 0 .. N-1, exactly N files each, so the build knows the file list in advance.)
The headers are included as "tsql/ast/generated/<name>", so <out-include> must end in
tsql/ast/generated below an include root.

Hand-written C# partial-class members live in <partial-dir>/<Class>.inc (the repo's
include/tsql/ast/partial); the generator #includes such a file inside the struct body when it exists.

Semantics mirror SqlScriptDOM's tools/AstGen (AstXmlReaderWithOrder.cs,
TypeMemberDescription.cs): IsStreamAnalyticsExtension members are dropped, interface
members are copied into implementing classes, fragment-typed members whose
GenerateUpdatePositionInfoCall is not "false" get a setter that calls UpdateTokenInfo,
CollectionFirstItem members are views over the "<Name>s" collection.
"""

import argparse
import os
import re
import xml.etree.ElementTree as ET

AST_REL = 'SqlScriptDom/Parser/TSql/Ast.xml'
TOKENS_REL = 'SqlScriptDom/Parser/TSql/TSqlTokenTypes.g'
NS = '::tsql::ast::'
# C# primitive member types -> member kind (TypeMemberDescription._primitiveTypes plus spellings used in Ast.xml).
PRIMITIVES = {'bool': 'bool', 'Boolean': 'bool', 'int': 'int', 'Int32': 'int', 'string': 'string'}
# Same list AstGen uses to switch off UpdateTokenInfo calls.
NO_UPDATE_TYPES = {'bool', 'bool?', 'int', 'int?', 'string'}
# Enums referenced by hand-written partial members rather than by Ast.xml members.
EXTRA_ENUMS = ['LiteralType']
# Translation units per generated visitor/Accept source (they dominate tsql_ast compile time).
VISITOR_CHUNKS = 4


def cs_bool(value, default=False):
    """Convert.ToBoolean semantics: trimmed, case-insensitive."""
    if value is None:
        return default
    v = value.strip().lower()
    if v not in ('true', 'false'):
        raise SystemExit(f'astgen: bad boolean {value!r}')
    return v == 'true'


# --------------------------------------------------------------------------- model

class Member:
    def __init__(self, el, owner):
        self.name = el.get('Name')
        raw = el.get('Type').strip()
        self.nullable = raw.endswith('?')
        self.base_type = raw.rstrip('?')
        self.collection = cs_bool(el.get('Collection'))
        self.first_item = cs_bool(el.get('CollectionFirstItem'))
        self.update = cs_bool(el.get('GenerateUpdatePositionInfoCall'), True) and raw not in NO_UPDATE_TYPES
        self.owner = owner           # class or interface name that declared it
        self.interface = None        # interface name if copied from an interface
        self.kind = None             # 'fragment' | 'bool' | 'int' | 'string' | 'enum'


class AstClass:
    def __init__(self, el):
        self.name = el.get('Name')
        self.base = (el.get('Base') or '').strip() or None
        self.abstract = cs_bool(el.get('Abstract'))
        self.members = []      # in dump order (own members, interface members at <Implements>)
        self.interfaces = []
        # AcceptChildren order, as tools/AstGen builds it: <Member>/<InheritedMember>/<InheritedClass>
        # in document order, then interface members (AddInterfaceMembersToClasses appends them).
        # Entries: ('member', Member) | ('inherited_member', name, container) | ('inherited_class', name, update)
        self.accept = []


def load_ast(path):
    root = ET.parse(path).getroot()
    interface_els = {el.get('Name'): el.findall('Member') for el in root.findall('Interface')}
    interfaces = {name: [Member(m, name) for m in els] for name, els in interface_els.items()}
    classes = {}
    for el in root.findall('Class'):
        c = AstClass(el)
        implemented = []
        for child in el:
            if child.tag in ('Member', 'InheritedMember', 'InheritedClass') and \
                    cs_bool(child.get('IsStreamAnalyticsExtension')):
                continue
            if child.tag == 'Member':
                m = Member(child, c.name)
                c.members.append(m)
                c.accept.append(('member', m))
            elif child.tag == 'InheritedMember':
                c.accept.append(('inherited_member', child.get('Name').strip(), child.get('ContainerClass').strip()))
            elif child.tag == 'InheritedClass':
                c.accept.append(('inherited_class', child.get('Name').strip(),
                                 cs_bool(child.get('GenerateUpdatePositionInfoCall'), True)))
            elif child.tag == 'Implements':
                iname = child.get('Interface')
                c.interfaces.append(iname)
                for im_el in interface_els[iname]:
                    m = Member(im_el, c.name)
                    m.interface = iname
                    c.members.append(m)
                    implemented.append(m)
        c.accept += [('member', m) for m in implemented]
        if c.name in classes:
            raise SystemExit(f'astgen: duplicate class {c.name}')
        classes[c.name] = c
    return classes, interfaces


# --------------------------------------------------------------------------- C# enums

def read_cs(path):
    data = open(path, 'rb').read()
    if data[:2] in (b'\xff\xfe', b'\xfe\xff'):
        return data.decode('utf-16')
    return data.decode('utf-8-sig')


def strip_comments(s):
    """Drop comments and preprocessor lines (#region, #pragma, ...)."""
    s = re.sub(r'/\*.*?\*/', ' ', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    return re.sub(r'^\s*#[^\n]*', '', s, flags=re.M)


ENUM_RE = re.compile(r'((?:\[[^\]]*\]\s*)*)(?:public|internal)\s+enum\s+(\w+)\s*(?::\s*(\w+))?\s*\{([^}]*)\}')


def parse_cs_enums(ssd):
    """Return {name: [decl, ...]} where decl = dict(file, outer, flags, underlying, entries)."""
    found = {}
    base = os.path.join(ssd, 'SqlScriptDom')
    for dirpath, _, files in os.walk(base):
        for fn in files:
            if not fn.endswith('.cs'):
                continue
            path = os.path.join(dirpath, fn)
            text = strip_comments(read_cs(path))
            for m in ENUM_RE.finditer(text):
                attrs, name, underlying, body = m.groups()
                outer = enclosing_class(text, m.start())
                flags = re.search(r'\bFlags(Attribute)?\b', attrs) is not None
                entries = []
                nxt = 0
                values = {}
                for item in body.split(','):
                    item = re.sub(r'\[[^\]]*\]', '', item).strip()
                    if not item:
                        continue
                    if '=' in item:
                        ename, expr = (' '.join(x.split()) for x in item.split('=', 1))
                        val = eval_cs_expr(expr, values)
                    else:
                        ename, val = item, nxt
                    values[ename] = val
                    entries.append((ename, val))
                    nxt = val + 1
                found.setdefault(name, []).append(dict(
                    file=os.path.relpath(path, ssd), outer=outer, flags=flags,
                    underlying=underlying or 'int', entries=entries))
    return found


def enclosing_class(text, pos):
    depth = 0
    i = pos
    while i > 0:
        i -= 1
        ch = text[i]
        if ch == '}':
            depth += 1
        elif ch == '{':
            if depth == 0:
                head = text[max(0, i - 300):i]
                m = re.search(r'\b(class|struct|namespace)\s+([\w.]+)[^{;]*$', head)
                if m and m.group(1) != 'namespace':
                    return m.group(2)
                return None
            depth -= 1
    return None


def eval_cs_expr(expr, values):
    py = re.sub(r'\b0[xX]([0-9a-fA-F]+)[uUlL]*\b', lambda m: str(int(m.group(1), 16)), expr)
    py = re.sub(r'\b(\d+)[uUlL]+\b', r'\1', py)
    py = re.sub(r'\b[A-Za-z_]\w*\b', lambda m: str(values[m.group(0)]), py)
    if not re.fullmatch(r'[\d\s|&^~<>()+\-*]+', py):
        raise SystemExit(f'astgen: unsupported enum expression {expr!r}')
    return int(eval(py, {'__builtins__': {}}))


def to_int32(v):
    v &= 0xFFFFFFFF
    return v - (1 << 32) if v & 0x80000000 else v


# --------------------------------------------------------------------------- tokens

def parse_token_types(path):
    """ANTLR 2 numbering as produced by SqlScriptDOM's build: tokens{} then lexer rules
    from 4 upwards; EOF=1 renamed EndOfFile; NULL_TREE_LOOKAHEAD=3 rewritten to None=0."""
    g = strip_comments(open(path, encoding='utf-8-sig').read())
    m = re.search(r'\btokens\s*\{(.*?)\}', g, re.S)
    names = [re.match(r'\s*(\w+)', e).group(1) for e in m.group(1).split(';') if e.strip()]
    rest = g[m.end():]
    names += re.findall(r'^\s*(?:protected\s+)?([A-Z]\w*)\s*:', rest, re.M)
    entries = [('None', 0), ('EndOfFile', 1)]
    entries += [(n, i + 4) for i, n in enumerate(names)]
    return entries


# --------------------------------------------------------------------------- emit helpers

def header(src, what):
    return (f'// Ported from Microsoft SqlScriptDOM (MIT) @ {COMMIT}: {src}\n'
            f'// GENERATED by tools/astgen/astgen.py ({what}). do not edit; the build regenerates it.\n')


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = None
    if os.path.exists(path):
        old = open(path, encoding='utf-8').read()
    if old != text:
        with open(path, 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)


def topo(classes):
    out, seen = [], set()

    def visit(n):
        if n in seen:
            return
        c = classes[n]
        if c.base and c.base != 'TSqlFragment':
            if c.base not in classes:
                raise SystemExit(f'astgen: {n} has unknown base {c.base}')
            visit(c.base)
        seen.add(n)
        out.append(n)

    for n in classes:
        visit(n)
    return out


# --------------------------------------------------------------------------- visitor

def find_inherited(classes, owner, name, container):
    """CompleteInheritedMembers: copy type/collection/update flags from the container's member
    (case-insensitive lookups; the last matching member wins, as in the C# loop)."""
    ancestors = []
    base = classes[owner].base or 'TSqlFragment'
    while base in classes:
        ancestors.append(base)
        base = classes[base].base or 'TSqlFragment'
    found = None
    for a in ancestors:
        if a.lower() == container.lower():
            for m in classes[a].members:
                if m.name.lower() == name.lower():
                    found = m
    if found is None:
        # C# would leave an untyped member and emit code that does not compile.
        raise SystemExit(f'astgen: {owner}: InheritedMember {container}.{name} not found')
    return found


def emit_visitor(classes, out_inc, out_src):
    src = 'SqlScriptDom/Parser/TSql/Ast.xml via tools/AstGen'

    # Visitor declarations, included inside the hand-written classes in tsql/ast/visitor.hpp.
    v = [header(src, 'TSqlFragmentVisitor members'), '// Included inside class TSqlFragmentVisitor.\n']
    for name in classes:
        v.append(f'virtual void Visit({NS}{name}* node);\n')
        v.append(f'virtual void ExplicitVisit({NS}{name}* node);\n')
    write(os.path.join(out_inc, 'visitor_members.inc'), ''.join(v))

    cv = [header(src, 'TSqlConcreteFragmentVisitor members'),
          '// Included inside class TSqlConcreteFragmentVisitor: abstract classes are sealed.\n']
    for name, c in classes.items():
        if c.abstract:
            cv.append(f'void Visit({NS}{name}* node) final;\n')
            cv.append(f'void ExplicitVisit({NS}{name}* node) final;\n')
    write(os.path.join(out_inc, 'concrete_visitor_members.inc'), ''.join(cv))

    # Calls name the overload by exact type through a member pointer instead of letting the
    # compiler run overload resolution over ~1,070 candidates per call (that alone cost ~6 s of
    # compile time). Dispatch stays virtual, as in C#.
    visitor_prologue = ('#include "tsql/ast/ast.hpp"\n\n'
                        '#define TSQL_VISIT(T, node) \\\n'
                        '    (this->*static_cast<void (TSqlFragmentVisitor::*)(::tsql::ast::T*)>'
                        '(&TSqlFragmentVisitor::Visit))(node)\n\n')
    vis = []  # one block per class: GenerateVisitMethod / GenerateExplicitVisitMethod
    for name, c in classes.items():
        # GenerateVisitBaseTypes: nearest base first, the synthesized TSqlFragment last.
        bases = []
        cur = name
        while True:
            b = classes[cur].base or 'TSqlFragment'
            bases.append(b)
            if b not in classes:
                break
            cur = b
        block = [f'void TSqlFragmentVisitor::Visit({NS}{name}* node) {{\n'
                 f'    if (!VisitBaseType()) TSQL_VISIT(TSqlFragment, node);\n}}\n\n',
                 f'void TSqlFragmentVisitor::ExplicitVisit({NS}{name}* node) {{\n    if (VisitBaseType()) {{\n']
        block += [f'        TSQL_VISIT({b}, node);\n' for b in bases]
        block.append(f'    }}\n    TSQL_VISIT({name}, node);\n    node->AcceptChildren(this);\n}}\n\n')
        if c.abstract:  # GenerateVisitMethodsForConcreteVisitor
            block.append(f'void TSqlConcreteFragmentVisitor::Visit({NS}{name}* node) {{ TSqlFragmentVisitor::Visit(node); }}\n'
                         f'void TSqlConcreteFragmentVisitor::ExplicitVisit({NS}{name}* node) '
                         f'{{ TSqlFragmentVisitor::ExplicitVisit(node); }}\n\n')
        vis.append(''.join(block))

    accept_prologue = ('#include <cstddef>\n\n#include "tsql/ast/ast.hpp"\n\n'
                       '#define TSQL_EXPLICIT_VISIT(T) \\\n'
                       '    (visitor->*static_cast<void (TSqlFragmentVisitor::*)(::tsql::ast::T*)>'
                       '(&TSqlFragmentVisitor::ExplicitVisit))(this)\n\n')
    acc = []  # one block per class: GenerateAcceptMethod / GenerateAcceptChildrenMethod
    for name, c in classes.items():
        block = []
        if not c.abstract:
            block.append(f'void {name}::Accept({NS}TSqlFragmentVisitor* visitor) {{\n'
                         f'    if (visitor != nullptr) TSQL_EXPLICIT_VISIT({name});\n}}\n\n')
        base = c.base or 'TSqlFragment'
        body = []
        inherited = False  # any InheritedMember/InheritedClass suppresses the trailing base call
        for entry in c.accept:
            if entry[0] == 'inherited_class':
                inherited = True
                if entry[2] and entry[1].lower() == base.lower():
                    body.append(f'    {NS}{base}::AcceptChildren(visitor);\n')
                continue
            if entry[0] == 'inherited_member':
                inherited = True
                m = find_inherited(classes, name, entry[1], entry[2])
            else:
                m = entry[1]
            if not m.update:  # GenerateUpdatePositionInfoCall=false and primitives are not visited
                continue
            if m.kind != 'fragment':
                raise SystemExit(f'astgen: {name}.{m.name}: visiting a non-fragment member')
            if m.collection:
                body.append(f'    for (std::size_t i = 0, count = this->{m.name}.size(); i < count; ++i) '
                            f'this->{m.name}[i]->Accept(visitor);\n')
            else:
                getter = f'this->{m.name}()' if m.first_item else f'this->{m.name}'
                body.append(f'    if ({getter} != nullptr) {getter}->Accept(visitor);\n')
        if not inherited:
            body.append(f'    {NS}{base}::AcceptChildren(visitor);\n')
        if not body:
            body.append('    (void)visitor;\n')
        block.append(f'void {name}::AcceptChildren({NS}TSqlFragmentVisitor* visitor) {{\n{"".join(body)}}}\n\n')
        acc.append(''.join(block))

    files = write_chunks(out_src, 'visitor', 'TSqlFragmentVisitor, TSqlConcreteFragmentVisitor', visitor_prologue, vis)
    files += write_chunks(out_src, 'accept', 'Accept, AcceptChildren', accept_prologue, acc)
    return files


def write_chunks(out_src, stem, what, prologue, blocks):
    """Spread `blocks` over exactly VISITOR_CHUNKS translation units of similar size (parallel
    builds): a block goes to the part its start offset falls in."""
    total = sum(len(b) for b in blocks)
    chunks = [[] for _ in range(VISITOR_CHUNKS)]
    pos = 0
    for b in blocks:
        chunks[min(VISITOR_CHUNKS - 1, pos * VISITOR_CHUNKS // max(total, 1))].append(b)
        pos += len(b)
    files = []
    for i, chunk in enumerate(chunks):
        name = f'{stem}_{i}.cpp'
        write(os.path.join(out_src, name),
              header('SqlScriptDom/Parser/TSql/Ast.xml via tools/AstGen', f'{what}, part {i + 1}')
              + prologue + 'namespace tsql::ast {\n\n' + ''.join(chunk) + '}  // namespace tsql::ast\n')
        files.append(name)
    return files


# --------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--ssd', required=True, help='SqlScriptDOM source tree')
    ap.add_argument('--ssd-commit', required=True, help='its commit (short form), stamped into the headers')
    ap.add_argument('--partial-dir', required=True, help='hand-written partial-class members (<Class>.inc)')
    ap.add_argument('--out-include', required=True, help='output directory for headers (.../tsql/ast/generated)')
    ap.add_argument('--out-src', required=True, help='output directory for .cpp files')
    ap.add_argument('--chunks', type=int, default=4, help='visitor/accept translation units each')
    args = ap.parse_args()

    global COMMIT, VISITOR_CHUNKS
    COMMIT = args.ssd_commit
    VISITOR_CHUNKS = args.chunks

    classes, interfaces = load_ast(os.path.join(args.ssd, AST_REL))
    cs_enums = parse_cs_enums(args.ssd)

    # ---- resolve member kinds and collect enums
    enum_names = []

    def resolve(m):
        t = m.base_type
        if t in classes:
            m.kind = 'fragment'
        elif t in PRIMITIVES:
            m.kind = PRIMITIVES[t]
        else:
            m.kind = 'enum'
            if t not in enum_names:
                enum_names.append(t)
        if m.collection and m.kind != 'fragment':
            raise SystemExit(f'astgen: non-fragment collection {m.owner}.{m.name}')
        if m.update and m.kind != 'fragment':
            raise SystemExit(f'astgen: UpdateTokenInfo on non-fragment {m.owner}.{m.name}')

    for c in classes.values():
        for m in c.members:
            resolve(m)
    for ms in interfaces.values():
        for m in ms:
            resolve(m)
    for e in EXTRA_ENUMS:
        if e not in enum_names:
            enum_names.append(e)

    enums = []  # (cs_name, cpp_qualified, decl)
    for cs_name in enum_names:
        if '.' in cs_name:
            outer, inner = cs_name.split('.', 1)
            cands = [d for d in cs_enums.get(inner, []) if d['outer'] == outer]
        else:
            outer, inner = None, cs_name
            cands = [d for d in cs_enums.get(inner, []) if d['outer'] is None]
        if len(cands) != 1:
            raise SystemExit(f'astgen: cannot resolve enum {cs_name}: {[d["file"] for d in cands]}')
        d = cands[0]
        if d['underlying'] not in ('int', 'Int32'):
            raise SystemExit(f'astgen: enum {cs_name} has underlying {d["underlying"]}')
        enums.append((cs_name, outer, inner, d))
    enum_cpp = {cs: (NS + (o + '::' if o else '') + i) for cs, o, i, _ in enums}

    def cpp_value_type(m):
        if m.kind == 'fragment':
            return f'{NS}{m.base_type}*'
        if m.kind == 'bool':
            t = 'bool'
        elif m.kind == 'int':
            t = 'std::int32_t'
        elif m.kind == 'string':
            return 'std::optional<std::string>'
        else:
            t = enum_cpp[m.base_type]
        return f'std::optional<{t}>' if m.nullable else t

    def cpp_field_type(m):
        if m.collection:
            return f'std::vector<{NS}{m.base_type}*>'
        return cpp_value_type(m)

    def field_init(m):
        if m.collection or m.kind == 'string' or m.nullable:
            return ''
        if m.kind == 'fragment':
            return ' = nullptr'
        if m.kind == 'bool':
            return ' = false'
        if m.kind == 'int':
            return ' = 0'
        return '{}'

    out_inc = args.out_include
    out_src = args.out_src
    partial_dir = args.partial_dir

    # ---- token types
    tokens = parse_token_types(os.path.join(args.ssd, TOKENS_REL))
    t = [header(TOKENS_REL, 'TSqlTokenType'), '#pragma once\n\n#include <cstdint>\n#include <string>\n\n',
         'namespace tsql::ast {\n\n',
         '/// Token ids, identical to SqlScriptDOM\'s TSqlTokenType (ANTLR 2 numbering of TSqlTokenTypes.g).\n',
         'enum class TSqlTokenType : std::int32_t {\n']
    for n, v in tokens:
        t.append(f'    {n} = {v},\n')
    t.append('};\n\n/// .NET Enum.ToString() text.\nstd::string ToString(TSqlTokenType value);\n\n}  // namespace tsql::ast\n')
    write(os.path.join(out_inc, 'token_types.hpp'), ''.join(t))

    # ---- enums header
    h = [header('SqlScriptDom/Parser/TSql/*.cs (enum declarations)', 'member enums'),
         '#pragma once\n\n#include <cstdint>\n#include <string>\n\n', 'namespace tsql::ast {\n\n']
    nested = {}
    for cs, outer, inner, d in enums:
        if outer:
            nested.setdefault(outer, []).append((cs, inner, d))

    def emit_enum(inner, d, indent):
        lines = [f'{indent}// {d["file"]}{" [Flags]" if d["flags"] else ""}\n',
                 f'{indent}enum class {inner} : std::int32_t {{\n']
        for en, ev in d['entries']:
            lines.append(f'{indent}    {en} = {to_int32(ev)},\n')
        lines.append(f'{indent}}};\n')
        return lines

    for cs, outer, inner, d in enums:
        if outer:
            continue
        h += emit_enum(inner, d, '')
        h.append('\n')
    for outer, items in nested.items():
        h.append(f'/// C# class {outer} (only the enums used by the AST are ported).\n')
        h.append(f'struct {outer} {{\n')
        for cs, inner, d in items:
            h += emit_enum(inner, d, '    ')
        h.append('};\n\n')
    for cs, outer, inner, d in enums:
        q = enum_cpp[cs]
        if d['flags']:
            for op in ('|', '&', '^'):
                h.append(f'constexpr {q} operator{op}({q} a, {q} b) {{ return static_cast<{q}>(static_cast<std::int32_t>(a) {op} static_cast<std::int32_t>(b)); }}\n')
                h.append(f'constexpr {q}& operator{op}=({q}& a, {q} b) {{ return a = a {op} b; }}\n')
            h.append(f'constexpr {q} operator~({q} a) {{ return static_cast<{q}>(~static_cast<std::int32_t>(a)); }}\n')
    h.append('\n// .NET Enum.ToString() text ("A, B" for flag combinations, the number for undefined values).\n')
    for cs, outer, inner, d in enums:
        h.append(f'std::string ToString({enum_cpp[cs]} value);\n')
    h.append('\n}  // namespace tsql::ast\n')
    write(os.path.join(out_inc, 'enums.hpp'), ''.join(h))

    # ---- enums.cpp
    s = [header('SqlScriptDom/Parser/TSql/*.cs (enum declarations)', 'enum ToString'),
         '#include "tsql/ast/ast.hpp"\n#include "enum_format.hpp"\n\n', 'namespace tsql::ast {\n\n']

    def emit_tostring(qname, entries, flags, idx):
        # .NET sorts enum values as unsigned for lookup and flag formatting.
        srt = sorted(((to_int32(v) & 0xFFFFFFFF, n) for n, v in entries), key=lambda x: x[0])
        s.append(f'static const detail::EnumEntry kEnum{idx}[] = {{\n')
        for v, n in srt:
            s.append(f'    {{{v}u, "{n}"}},\n')
        s.append('};\n')
        fn = 'FormatFlagsEnum' if flags else 'FormatEnum'
        s.append(f'std::string ToString({qname} value) {{ return detail::{fn}(kEnum{idx}, sizeof(kEnum{idx}) / sizeof(kEnum{idx}[0]), static_cast<std::int32_t>(value)); }}\n\n')

    emit_tostring(NS + 'TSqlTokenType', tokens, False, 0)
    for i, (cs, outer, inner, d) in enumerate(enums, 1):
        emit_tostring(enum_cpp[cs], d['entries'], d['flags'], i)
    s.append('}  // namespace tsql::ast\n')
    write(os.path.join(out_src, 'enums.cpp'), ''.join(s))

    # ---- fragment_type_id.hpp (needed by fragment.hpp)
    f = [header(AST_REL, 'FragmentTypeId'), '#pragma once\n\n#include <cstdint>\n\nnamespace tsql::ast {\n\n',
         '/// One id per Ast.xml class (abstract ones included), in Ast.xml order.\n',
         'enum class FragmentTypeId : std::uint16_t {\n']
    for name in classes:
        f.append(f'    {name},\n')
    f.append('};\n\n}  // namespace tsql::ast\n')
    write(os.path.join(out_inc, 'fragment_type_id.hpp'), ''.join(f))

    # ---- nodes.hpp
    order = topo(classes)
    n = [header(AST_REL, 'AST node structs'),
         '#pragma once\n\n#include <cstdint>\n#include <optional>\n#include <string>\n#include <utility>\n#include <vector>\n\n',
         '#include "tsql/ast/fragment.hpp"\n#include "tsql/ast/generated/enums.hpp"\n\n',
         'namespace tsql::ast {\n\n']
    for name in classes:
        n.append(f'struct {name};\n')
    n.append('\n')
    n.append('// Ast.xml <Interface>s: grammar actions set these members through the interface.\n')
    for iname, ms in interfaces.items():
        n.append(f'struct {iname} {{\n    virtual ~{iname}() = default;\n')
        for m in ms:
            n.append(f'    virtual {cpp_value_type(m)} get_{m.name}() const = 0;\n')
            n.append(f'    virtual void set_{m.name}({cpp_value_type(m)} value) = 0;\n')
        n.append('};\n\n')

    deferred = []
    for name in order:
        c = classes[name]
        bases = [NS + (c.base or 'TSqlFragment')] + [NS + i for i in c.interfaces]
        n.append(f'// Ast.xml: {name}{" (abstract)" if c.abstract else ""}\n')
        n.append(f'struct {name} : {", ".join("public " + b for b in bases)} {{\n')
        n.append(f'    static constexpr {NS}FragmentTypeId kTypeId = {NS}FragmentTypeId::{name};\n')
        if not c.abstract:
            n.append(f'    {NS}FragmentTypeId TypeId() const override {{ return kTypeId; }}\n')
            n.append(f'    const char* TypeName() const override {{ return "{name}"; }}\n')
            n.append(f'    void Accept({NS}TSqlFragmentVisitor* visitor) override;\n')
        n.append(f'    void AcceptChildren({NS}TSqlFragmentVisitor* visitor) override;\n')
        for m in c.members:
            vt = cpp_value_type(m)
            ov = ' override' if m.interface else ''
            if m.first_item:
                coll = m.name + 's'
                n.append(f'    /// CollectionFirstItem: view over {coll}[0].\n')
                n.append(f'    {vt} {m.name}() const {{ return {coll}.empty() ? nullptr : {coll}[0]; }}\n')
                n.append(f'    void set_{m.name}({vt} value) {{ if ({coll}.empty()) {coll}.push_back(value); else {coll}[0] = value; }}\n')
                continue
            n.append(f'    {cpp_field_type(m)} {m.name}{field_init(m)};\n')
            if m.collection:
                continue
            if m.update:
                # Defined after all structs: the argument must convert to TSqlFragment*.
                n.append(f'    void set_{m.name}({vt} value){ov};\n')
                deferred.append(f'inline void {name}::set_{m.name}({vt} value) {{ UpdateTokenInfo(value); {m.name} = value; }}\n')
            else:
                move = m.kind == 'string' or m.nullable
                body = f'{m.name} = std::move(value);' if move else f'{m.name} = value;'
                n.append(f'    void set_{m.name}({vt} value){ov} {{ {body} }}\n')
            if m.interface:
                n.append(f'    {vt} get_{m.name}() const override {{ return {m.name}; }}\n')
        if os.path.exists(os.path.join(partial_dir, name + '.inc')):
            n.append(f'    // Hand-written C# partial-class members.\n#include "tsql/ast/partial/{name}.inc"\n')
        n.append('};\n\n')
    n.append('// Setters that mirror the C# property setters\' UpdateTokenInfo(value) call.\n')
    n += deferred
    n.append('\n}  // namespace tsql::ast\n')
    write(os.path.join(out_inc, 'nodes.hpp'), ''.join(n))

    # ---- dump.cpp
    d = [header(AST_REL, 'DumpJson member walkers'),
         '#include "tsql/ast/ast.hpp"\n#include "json_writer.hpp"\n\n',
         'namespace tsql::ast::detail {\n\nnamespace {\n\n']
    for name in order:
        d.append(f'void M_{name}(JsonWriter& w, const {name}& n);\n')
    d.append('\n')
    for name in order:
        c = classes[name]
        d.append(f'void M_{name}(JsonWriter& w, const {name}& n) {{\n')
        if c.base and c.base != 'TSqlFragment':
            d.append(f'    M_{c.base}(w, n);\n')
        if not c.members:
            d.append('    (void)w;\n    (void)n;\n')
        for m in c.members:
            acc = f'n.{m.name}()' if m.first_item else f'n.{m.name}'
            if m.collection:
                call = f'w.Collection("{m.name}", {acc});'
            elif m.kind == 'fragment':
                call = f'w.Fragment("{m.name}", {acc});'
            elif m.kind == 'enum':
                call = f'w.Enum("{m.name}", {acc});'
            else:
                call = f'w.Value("{m.name}", {acc});'
            d.append(f'    {call}\n')
        d.append('}\n\n')
    d.append('}  // namespace\n\n')
    d.append('void DumpMembers(JsonWriter& w, const TSqlFragment& n) {\n    switch (n.TypeId()) {\n')
    for name in classes:
        if classes[name].abstract:
            continue
        d.append(f'    case FragmentTypeId::{name}: M_{name}(w, static_cast<const {name}&>(n)); return;\n')
    d.append('    default: return;\n    }\n}\n\n}  // namespace tsql::ast::detail\n')
    write(os.path.join(out_src, 'dump.cpp'), ''.join(d))

    # ---- visitor (tools/AstGen: GenerateGenericVisitor, GenerateConcreteVisitor,
    #      ClassDescription.GenerateAcceptMethod/GenerateAcceptChildrenMethod)
    emit_visitor(classes, out_inc, out_src)

    concrete = sum(1 for c in classes.values() if not c.abstract)
    print(f'astgen: {len(classes)} classes ({concrete} concrete), {len(interfaces)} interfaces, '
          f'{sum(len(c.members) for c in classes.values())} members, {len(enums)} enums, {len(tokens)} token types')


COMMIT = 'unknown'

if __name__ == '__main__':
    main()
