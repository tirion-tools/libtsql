"""Type knowledge the C#->C++ action translator needs: Ast.xml classes/members, enum names
(AST-side from the generated header, parser-side from SqlScriptDOM's C# sources), static
classes and the token vocabulary."""
import os
import re
import xml.etree.ElementTree as ET


def read_text(path):
    raw = open(path, 'rb').read()
    if raw.startswith(b'\xff\xfe') or raw.startswith(b'\xfe\xff'):
        return raw.decode('utf-16')
    return raw.decode('utf-8-sig')


class Member:
    def __init__(self, name, type_, collection, first_item, update):
        self.name, self.type, self.collection, self.first_item, self.update = name, type_, collection, first_item, update


class AstClass:
    def __init__(self, name, base, abstract):
        self.name, self.base, self.abstract = name, base, abstract
        self.members = {}
        self.interfaces = []


# C#-computed properties / methods hand-ported by AstGen (include/tsql/ast/partial/*.inc).
# name -> {class: kind}; accessed as obj->Name() in C++.
PARTIAL_PROPS = {
    'BaseIdentifier': 'frag:Identifier', 'SchemaIdentifier': 'frag:Identifier',
    'DatabaseIdentifier': 'frag:Identifier', 'ServerIdentifier': 'frag:Identifier',
    'ChildIdentifier': 'frag:Identifier', 'LiteralType': 'enum:LiteralType',
}
# Class-specific computed properties (only when the receiver class is known).
PARTIAL_CLASS_PROPS = {
    ('IdentifierOrValueExpression', 'Value'): 'optstr',
    ('MultiPartIdentifier', 'Count'): 'int',
}


class Model:
    def __init__(self, ssd, ast_enums_hpp):
        """ssd: SqlScriptDOM source tree; ast_enums_hpp: the AST's generated enums.hpp (astgen)."""
        self.ssd = ssd
        self.classes = {}
        self.interfaces = {}
        self._load_ast(os.path.join(ssd, 'SqlScriptDom/Parser/TSql/Ast.xml'))
        self.ast_enums = self._load_ast_enums(ast_enums_hpp)
        self.cs_enums = self._scan_cs_enums()
        self.parser_enums = {}   # filled on demand: name -> (underlying, [(member, value)], flags)
        self.member_index = {}   # member name -> list of Member
        for c in self.classes.values():
            for m in c.members.values():
                self.member_index.setdefault(m.name, []).append(m)

    # ------------------------------------------------------------ Ast.xml
    def _load_ast(self, path):
        root = ET.parse(path).getroot()
        for el in root:
            if el.tag == 'Class':
                c = AstClass(el.get('Name'), el.get('Base'), el.get('Abstract') == 'true')
                for m in el:
                    if m.tag == 'Member':
                        mem = Member(m.get('Name'), m.get('Type'), m.get('Collection') == 'true',
                                     m.get('CollectionFirstItem') == 'true',
                                     m.get('GenerateUpdatePositionInfoCall') != 'false')
                        c.members[mem.name] = mem
                    elif m.tag == 'Implements':
                        c.interfaces.append(m.get('Interface') or m.get('Name'))
                self.classes[c.name] = c
            elif el.tag == 'Interface':
                ms = {}
                for m in el:
                    if m.tag == 'Member':
                        ms[m.get('Name')] = Member(m.get('Name'), m.get('Type'), False, False, True)
                self.interfaces[el.get('Name')] = ms
        # interface members are copied into implementing classes
        for c in self.classes.values():
            for i in c.interfaces:
                for n, m in self.interfaces.get(i, {}).items():
                    c.members.setdefault(n, m)

    def is_ast_class(self, name):
        return name in self.classes or name == 'TSqlFragment' or name in self.interfaces

    def lookup_member(self, cls, name):
        """Member `name` visible from class `cls` (walks the Base chain)."""
        c = self.classes.get(cls)
        while c is not None:
            if name in c.members:
                return c.members[name]
            c = self.classes.get(c.base) if c.base else None
        if cls in self.interfaces and name in self.interfaces[cls]:
            return self.interfaces[cls][name]
        return None

    def is_subclass(self, cls, base):
        c = self.classes.get(cls)
        while c is not None:
            if c.name == base:
                return True
            c = self.classes.get(c.base) if c.base else None
        return base == 'TSqlFragment'

    def member_kind(self, m):
        t = m.type
        if m.collection:
            return 'vec:' + t
        if t in ('string',):
            return 'optstr'
        if t in ('bool', 'Boolean'):
            return 'bool'
        if t in ('int', 'Int32'):
            return 'int'
        if t.endswith('?'):
            return 'opt:' + t[:-1]
        if self.is_ast_class(t):
            return 'frag:' + t
        return 'enum:' + t

    # ------------------------------------------------------------ enums
    def _load_ast_enums(self, path):
        names = set()
        if os.path.exists(path):
            txt = open(path).read()
            for m in re.finditer(r'enum class (\w+)', txt):
                names.add(m.group(1))
        return names

    def _scan_cs_enums(self):
        found = {}
        base = os.path.join(self.ssd, 'SqlScriptDom')
        rx = re.compile(r'((?:\[[^\]]*\]\s*)*)(?:public|internal)\s+enum\s+(\w+)\s*(?::\s*(\w+))?\s*\{([^}]*)\}')
        for dp, _, fs in os.walk(base):
            for f in fs:
                if not f.endswith('.cs'):
                    continue
                txt = read_text(os.path.join(dp, f))
                txt = re.sub(r'//[^\n]*', '', txt)
                txt = re.sub(r'/\*.*?\*/', '', txt, flags=re.S)
                for m in rx.finditer(txt):
                    line_start = txt.rfind('\n', 0, m.start()) + 1
                    indent = len(txt[line_start:m.start()].expandtabs(4)) - len(txt[line_start:m.start()].expandtabs(4).lstrip())
                    found.setdefault(m.group(2), []).append(
                        dict(attrs=m.group(1), underlying=m.group(3), body=m.group(4), nested=indent > 4,
                             file=os.path.relpath(os.path.join(dp, f), self.ssd)))
        return found

    def enum_kind(self, name):
        """'ast' | 'parser' | None"""
        if name in self.ast_enums:
            return 'ast'
        if name in self.cs_enums and not all(d['nested'] for d in self.cs_enums[name]):
            return 'parser'
        return None

    def use_parser_enum(self, name):
        self.parser_enums[name] = self.cs_enums[name][0]
