"""Rule-based translation of SqlScriptDOM's C# grammar actions to C++.

Not a C# compiler: a token-level rewriter with light type tracking (kinds) so that member
access, collection and string operations, enums, casts and AST property setters come out
right. Anything it cannot translate becomes `G2TO4_UNTRANSLATED("why")` (a macro that does
not exist, so the C++ build fails until the action gets a hand override) and is counted.

Kinds: 'frag:C' AST node pointer, 'vec:C' std::vector<C*>, 'strvec', 'str' std::string,
'optstr' std::optional<std::string>, 'cstr' const char*, 'tok' antlr4::Token*, 'enum:E',
'opt:E', 'int', 'bool', 'exc' (exception object), 'null', 'unknown'.
"""
import re

from model import PARTIAL_CLASS_PROPS, PARTIAL_PROPS

CPP_PRIMS = {
    'bool': ('bool', 'bool'), 'Boolean': ('bool', 'bool'),
    'int': ('int', 'int'), 'Int32': ('int', 'int'),
    'long': ('int64_t', 'int'), 'Int64': ('int64_t', 'int'),
    'ulong': ('uint64_t', 'int'), 'UInt64': ('uint64_t', 'int'),
    'uint': ('uint32_t', 'int'), 'UInt32': ('uint32_t', 'int'),
    'double': ('double', 'int'), 'char': ('char', 'int'),
    # System.Decimal: the grammar only range-checks option values (multiples of 0.25 etc. are exact
    # in binary floating point)
    'decimal': ('double', 'dec'), 'Decimal': ('double', 'dec'),
    'string': ('std::string', 'str'), 'String': ('std::string', 'str'),
}

STRING_METHOD_KIND = {
    'Equals': 'bool', 'StartsWith': 'bool', 'EndsWith': 'bool', 'Contains': 'bool',
    'ToUpper': 'str', 'ToUpperInvariant': 'str', 'ToLower': 'str', 'ToLowerInvariant': 'str',
    'Substring': 'str', 'Trim': 'str', 'TrimEnd': 'str', 'TrimStart': 'str', 'Replace': 'str',
    'IndexOf': 'int', 'LastIndexOf': 'int', 'ToString': 'str', 'CompareTo': 'int',
}

# Parser-base helper functions with a known result kind.
FUNC_KINDS = {
    'LT': 'tok', 'LA': 'int', 'GetFirstToken': 'tok', 'NextTokenMatches': 'bool',
    'TryMatch': 'bool', 'GetEmptyIdentifier': 'frag:Identifier',
    'DecodeAsciiStringLiteral': 'str', 'DecodeUnicodeStringLiteral': 'str',
    'GetUnexpectedTokenErrorException': 'exc', 'GetUnexpectedTokenError': 'perr',
    'GetFaultTolerantUnexpectedTokenError': 'perr', 'ProcessTokenStreamRecognitionException': 'perr',
    'CreateParseError': 'perr', 'GetIncorrectSyntaxError': 'perr',
    'IdentifierOrValueExpression': 'frag:IdentifierOrValueExpression',
}

STATIC_STRING_CLASSES = {'CodeGenerationSupporter', 'TSqlParserResource'}
DOTNET_ENUMS = {'StringComparison', 'CultureInfo', 'NumberStyles'}
# TSql<ver>ParserInternal.X: token-type constants of the generated parser (any version)
GRAMMAR_CLASS_RE = re.compile(r'^TSql\w*ParserInternal$')
# TSql<ver>ParserBaseInternal.X(...): static helpers of a parser base (flattened into <G>ParserBase)
BASE_CLASS_RE = re.compile(r'^TSql\w*ParserBaseInternal$')
CS_KEYWORDS = {'if', 'else', 'return', 'throw', 'for', 'foreach', 'while', 'do', 'switch', 'case',
               'break', 'continue', 'default', 'new', 'in', 'is', 'as', 'ref', 'out', 'null', 'true',
               'false', 'this', 'typeof', 'try', 'catch', 'finally', 'goto', 'base', 'var', 'params'}


class CsTok:
    __slots__ = ('kind', 'text', 'pre')

    def __init__(self, kind, text, pre):
        self.kind, self.text, self.pre = kind, text, pre

    def __repr__(self):
        return '%s:%r' % (self.kind, self.text)


_OPS = ['<<=', '>>=', '==', '!=', '<=', '>=', '&&', '||', '++', '--', '+=', '-=', '*=', '/=', '%=',
        '|=', '&=', '^=', '=>', '??', '?.', '::', '<<', '>>']


def cs_tokenize(text):
    toks, i, n, pre = [], 0, len(text), ''
    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            j = i
            while j < n and text[j] in ' \t\r\n':
                j += 1
            pre += text[i:j]
            i = j
            continue
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            pre += text[i:j]
            i = j
            continue
        if text.startswith('/*', i):
            j = text.find('*/', i + 2) + 2
            pre += text[i:j]
            i = j
            continue
        if c == '@' and i + 1 < n and text[i + 1] == '"':
            j = i + 2
            while True:
                if text[j] == '"':
                    if j + 1 < n and text[j + 1] == '"':
                        j += 2
                        continue
                    break
                j += 1
            toks.append(CsTok('vstr', text[i:j + 1], pre))
            pre, i = '', j + 1
            continue
        if c == '"':
            j = i + 1
            while text[j] != '"':
                if text[j] == '\\':
                    j += 1
                j += 1
            toks.append(CsTok('str', text[i:j + 1], pre))
            pre, i = '', j + 1
            continue
        if c == "'":
            m = re.match(r"'(\\u[0-9a-fA-F]{4}|\\.|[^\\'])'", text[i:])
            toks.append(CsTok('chr', m.group(0), pre))
            pre, i = '', i + len(m.group(0))
            continue
        if c.isalpha() or c == '_' or (c == '@' and i + 1 < n and text[i + 1].isalpha()):
            m = re.match(r'@?[A-Za-z_][A-Za-z0-9_]*', text[i:])
            toks.append(CsTok('id', m.group(0).lstrip('@'), pre))
            pre, i = '', i + len(m.group(0))
            continue
        if c.isdigit():
            m = re.match(r'0[xX][0-9a-fA-F]+[uUlL]*|[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?[uUlLfFdDmM]*', text[i:])
            toks.append(CsTok('num', m.group(0), pre))
            pre, i = '', i + len(m.group(0))
            continue
        for op in _OPS:
            if text.startswith(op, i):
                toks.append(CsTok('op', op, pre))
                pre, i = '', i + len(op)
                break
        else:
            toks.append(CsTok('op', c, pre))
            pre, i = '', i + 1
    return toks, pre


class Untranslatable(Exception):
    pass


class Scope:
    """Variables visible to an action: name -> (kind, cpp_type); ctx names are referenced as
    $name inside predicates."""

    def __init__(self):
        self.vars = {}
        self.ctx = set()
        self.ctx_locals = set()   # init-block locals moved into the rule context (read by predicates)
        self.pred_alias = {}      # name -> text to use inside predicates
        self.pattern_vars = set()  # C# `x is T name` variables (hoisted)

    def add(self, name, kind, cpp=None, ctx=False):
        self.vars[name] = (kind, cpp)
        if ctx:
            self.ctx.add(name)

    def kind(self, name):
        v = self.vars.get(name)
        return v[0] if v else None

    def copy(self):
        s = Scope()
        s.vars = dict(self.vars)
        s.ctx = set(self.ctx)
        s.ctx_locals = set(self.ctx_locals)
        s.pred_alias = dict(self.pred_alias)
        s.pattern_vars = set(self.pattern_vars)
        return s


class Translator:
    def __init__(self, model, tokens, parser_class):
        self.model = model
        self.tokens = set(tokens)
        self.parser_class = parser_class
        self.static_classes_used = set()   # unknown static classes referenced (needs porting)
        self.helpers_used = set()
        self.parser_enums_used = set()

    # ---------------------------------------------------------------- types
    def is_type_name(self, name):
        m = self.model
        if name in ('System.Text.StringBuilder', 'StringBuilder', 'decimal'):
            return True
        if '.' in name:
            outer, inner = name.rsplit('.', 1)
            return '.' not in outer and inner in m.cs_enums
        return (name in CPP_PRIMS or name == 'var' or m.is_ast_class(name) or name in ('IToken', 'List', 'IList')
                or m.enum_kind(name) is not None)

    def cpp_type(self, cs):
        """C# type text -> (cpp type, kind)."""
        cs = cs.replace(' ', '')
        if cs in ('System.Text.StringBuilder', 'StringBuilder'):
            return 'std::string', 'sb'
        if cs.startswith('antlr.'):
            cs = cs[6:]
        if cs.endswith('[]'):
            et, ek = self.cpp_type(cs[:-2])
            return 'std::vector<%s>' % et, ('strvec' if ek == 'str' else 'vec:' + ek.split(':')[-1])
        if cs.endswith('?'):
            et, ek = self.cpp_type(cs[:-1])
            return 'std::optional<%s>' % et, 'opt:' + ek.split(':')[-1]
        m = re.match(r'(I?List)<(.*)>$', cs)
        if m:
            et, ek = self.cpp_type(m.group(2))
            if ek == 'str':
                return 'std::vector<std::string>', 'strvec'
            return 'std::vector<%s>' % et, 'vec:' + ek.split(':', 1)[-1]
        if cs == 'var':
            return 'auto', 'unknown'
        if cs in ('System.Text.StringBuilder', 'StringBuilder'):
            return 'std::string', 'sb'
        if cs in CPP_PRIMS:
            return CPP_PRIMS[cs]
        if cs == 'IToken':
            return 'antlr4::Token*', 'tok'
        if self.model.is_ast_class(cs):
            return 'ast::%s*' % cs, 'frag:' + cs
        if '.' in cs:
            outer, inner = cs.rsplit('.', 1)
            if '.' not in outer and inner in self.model.cs_enums:
                # enum nested in a C# class (e.g. SensitivityClassification.Rank); the hand-ported
                # class (src/parser/helpers) provides it
                self.static_classes_used.add(outer)
                return '%s::%s' % (outer, inner), 'enum:%s::%s' % (outer, inner)
        ek = self.model.enum_kind(cs)
        if ek == 'ast':
            return 'ast::' + cs, 'enum:' + cs
        if ek == 'parser':
            self.model.use_parser_enum(cs)
            self.parser_enums_used.add(cs)
            return cs, 'enum:' + cs
        raise Untranslatable('unknown C# type ' + cs)

    def enum_prefix(self, name):
        if '::' in name:
            return name
        ek = self.model.enum_kind(name)
        if ek == 'ast':
            return 'ast::' + name
        self.model.use_parser_enum(name)
        self.parser_enums_used.add(name)
        return name

    # ---------------------------------------------------------------- entry points
    def translate(self, text, scope, mode='action'):
        """mode: 'action' (statements; declarations are added to `scope`), 'pred' (expression in
        a sempred: rule-context names become $name), 'expr'."""
        toks, tail = cs_tokenize(text)
        sc = scope if mode in ('action', 'method') else scope.copy()
        hoisted = self.hoist_out_declarations(toks, sc) if mode in ('action', 'method') else ''
        st = _State(self, toks, sc, mode)
        out = st.seq(0, len(toks), stmt=(mode in ('action', 'method')))
        return hoisted + out + tail

    def hoist_out_declarations(self, toks, scope):
        """C# 7 `M(out T name)`: the variable is declared by the call. Its scope is the enclosing
        statement's block; it is declared at the start of the snippet instead (names must be unique
        in the snippet). Removes the type tokens from `toks`."""
        decls, seen = [], set()
        i = 0
        while i + 3 < len(toks):
            if toks[i].kind == 'id' and toks[i].text == 'out' and toks[i + 1].kind == 'id' and \
                    toks[i + 2].kind == 'id' and toks[i + 3].kind == 'op' and toks[i + 3].text in (')', ','):
                cstype, name = toks[i + 1].text, toks[i + 2].text
                if name in seen or name in scope.vars:
                    raise Untranslatable('out variable %s declared twice' % name)
                cpp, kind = self.cpp_type(cstype)
                seen.add(name)
                decls.append('%s %s{}; ' % (cpp, name))
                scope.add(name, kind, cpp)
                del toks[i + 1]
            elif toks[i].kind == 'id' and toks[i].text == 'is' and toks[i + 1].kind == 'id' and \
                    toks[i + 2].kind == 'id' and toks[i + 2].text not in CS_KEYWORDS and \
                    self.model.is_ast_class(toks[i + 1].text):
                # `x is T name`: declared by the pattern (C# 7), assigned where it is tested
                name = toks[i + 2].text
                if name in seen or name in scope.vars:
                    raise Untranslatable('pattern variable %s declared twice' % name)
                cpp, kind = self.cpp_type(toks[i + 1].text)
                seen.add(name)
                decls.append('%s %s{}; ' % (cpp, name))
                scope.add(name, kind, cpp)
                scope.pattern_vars.add(name)
            i += 1
        return ''.join(decls)

    def translate_rule_args(self, text, scope, callee_params):
        """Arguments of a rule invocation `rule[a, ref b]`; pointer params get &(...)."""
        toks, _ = cs_tokenize(text)
        st = _State(self, toks, scope.copy(), 'expr')
        parts = st.split_commas(0, len(toks))
        out = []
        for idx, (a, b) in enumerate(parts):
            mode = callee_params[idx][0] if idx < len(callee_params) else 'val'
            if a < b and toks[a].kind == 'id' and toks[a].text in ('ref', 'out'):
                a += 1
            s = st.seq(a, b).strip()
            if s == 'nullptr' and idx < len(callee_params) and callee_params[idx][1].cstype.endswith('?'):
                s = 'std::nullopt'
            if mode == 'ptr':
                s = '&(%s)' % s
            out.append(s)
        return ', '.join(out)


def _match(toks, i, open_, close):
    depth = 0
    for j in range(i, len(toks)):
        if toks[j].kind == 'op':
            if toks[j].text == open_:
                depth += 1
            elif toks[j].text == close:
                depth -= 1
                if depth == 0:
                    return j
    raise Untranslatable('unbalanced ' + open_)


class _State:
    def __init__(self, tr, toks, scope, mode):
        self.tr, self.t, self.scope, self.mode = tr, toks, scope, mode
        self.m = tr.model
        self.last_kind = None

    # -------------------------------------------------------------- helpers
    def tx(self, i):
        return self.t[i].text if i < len(self.t) else None

    def kd(self, i):
        return self.t[i].kind if i < len(self.t) else None

    def split_commas(self, a, b):
        parts, depth, start = [], 0, a
        for j in range(a, b):
            tk = self.t[j]
            if tk.kind == 'op' and tk.text in '([{':
                depth += 1
            elif tk.kind == 'op' and tk.text in ')]}':
                depth -= 1
            elif tk.kind == 'op' and tk.text == ',' and depth == 0:
                parts.append((start, j))
                start = j + 1
        if start < b or parts:
            parts.append((start, b))
        return parts

    def generic_end(self, i):
        """t[i] == '<': index of matching '>' if this is a generic argument list, else None."""
        depth = 0
        for j in range(i, min(len(self.t), i + 12)):
            x = self.t[j]
            if x.kind == 'op' and x.text == '<':
                depth += 1
            elif x.kind == 'op' and x.text == '>':
                depth -= 1
                if depth == 0:
                    return j
            elif x.kind == 'op' and x.text == '>>':
                depth -= 2
                if depth <= 0:
                    return j
            elif not (x.kind == 'id' or (x.kind == 'op' and x.text in ('.', ',', '[', ']', '?'))):
                return None
        return None

    def type_text(self, i):
        """Parses a C# type at i. Returns (text, next) or None."""
        if self.kd(i) != 'id':
            return None
        j = i
        txt = self.tx(j)
        j += 1
        while self.tx(j) == '.' and self.kd(j + 1) == 'id':
            txt += '.' + self.tx(j + 1)
            j += 2
        if self.tx(j) == '<':
            e = self.generic_end(j)
            if e is None:
                return None
            txt += ''.join(x.text for x in self.t[j:e + 1])
            j = e + 1
        if self.tx(j) == '[' and self.tx(j + 1) == ']':
            txt += '[]'
            j += 2
        if self.tx(j) == '?' and self.kd(j + 1) == 'id' and self.tx(j + 2) in ('=', ';', ','):
            txt += '?'
            j += 1
        return txt, j

    def is_decl(self, i):
        """Declaration `Type name (=|;|,)` at statement start."""
        r = self.type_text(i)
        if not r:
            return None
        txt, j = r
        base = re.sub(r'<.*', '', txt.replace('antlr.', ''))
        if base not in ('var', 'List', 'IList') and not self.tr.is_type_name(base.rstrip('[]?')):
            return None
        if self.kd(j) == 'id' and self.tx(j) not in CS_KEYWORDS and self.tx(j + 1) in ('=', ';', ',', ')'):
            return txt, j
        return None

    # -------------------------------------------------------------- statements
    def seq(self, a, b, stmt=False):
        out = []
        i = a
        prev_sig = None   # previous significant token text (for statement-start detection)
        pending_defaults = set()   # token indexes of switch-closing braces that need `default: break;`
        while i < b:
            tk = self.t[i]
            pre = tk.pre
            at_stmt = stmt and (prev_sig is None or prev_sig in ('{', '}', ';', 'else', ':') )
            if tk.kind == 'id' and at_stmt:
                d = self.is_decl(i)
                if d:
                    txt, j = d
                    s, i = self.declaration(txt, j, b)
                    out.append(pre + s)
                    prev_sig = ';'
                    continue
            if tk.kind == 'id' and tk.text == 'return' and self.mode == 'action':
                if self.tx(i + 1) == ';':
                    out.append(pre + 'return _localctx')
                    i += 1
                    prev_sig = 'return'
                    continue
                raise Untranslatable('return with value in action')
            if tk.kind == 'id' and tk.text == 'foreach':
                s, i = self.foreach(i, b)
                out.append(pre + s)
                prev_sig = ')'
                continue
            if tk.kind == 'id' and tk.text in ('typeof', 'lock', 'using', 'base', 'goto', 'params'):
                raise Untranslatable('C# construct ' + tk.text)
            if tk.kind == 'op' and tk.text == '??' and out and self.last_kind and i + 1 < b:
                left, lk = out.pop(), self.last_kind
                right, rk, i = self.chain(i + 1, b)
                if lk.startswith('opt:') or lk == 'optstr':
                    out.append('%s.value_or(%s)' % (left, right.strip()))
                else:
                    out.append('CsCoalesce(%s, %s)' % (left, right.strip()))
                self.last_kind = rk
                prev_sig = 'x'
                continue
            if tk.kind == 'op' and tk.text == '%' and self.last_kind == 'dec' and out:
                left = out.pop()
                right, rk, i = self.chain(i + 1, b)
                out.append('std::fmod(%s, %s)' % (left, right.strip()))   # System.Decimal remainder
                self.last_kind = 'dec'
                prev_sig = 'x'
                continue
            if tk.kind == 'op' and tk.text in ('??', '?.', '=>'):
                raise Untranslatable('C# operator ' + tk.text)
            if tk.kind == 'id' and tk.text == 'switch' and self.tx(i + 1) == '(':
                close = _match(self.t, i + 1, '(', ')')
                subject = self.seq(i + 2, close).strip()
                if self.last_kind in ('str', 'optstr', 'cstr') and self.tx(close + 1) == '{':
                    s2, i = self.string_switch(subject, close + 1, b)
                    out.append(pre + s2)
                    prev_sig = '}'
                    continue
                if self.last_kind and self.last_kind.startswith('opt:'):
                    subject = 'CsSwitchValue(%s)' % subject   # switch over a nullable enum
                out.append(pre + 'switch (' + subject + ')')
                # C# leaves unlisted enum values unhandled silently; say so in C++ (-Wswitch).
                if self.tx(close + 1) == '{':
                    body_end = _match(self.t, close + 1, '{', '}')
                    depth = 0
                    has_default = False
                    for k in range(close + 2, body_end):
                        x = self.tx(k)
                        depth += x == '{'
                        depth -= x == '}'
                        has_default |= depth == 0 and x == 'default'
                    if not has_default:
                        pending_defaults.add(body_end)
                i = close + 1
                prev_sig = ')'
                continue
            if tk.kind == 'op' and tk.text in ('==', '!=') and self.tx(i + 1) == 'null' and \
                    self.last_kind is not None and (self.last_kind.startswith('vec:') or self.last_kind == 'strvec'):
                # a C# list that is never null (AST collections, locals initialized with new)
                left = out.pop()
                out.append('%s(static_cast<void>(%s), %s)' % (pre, left.strip(), 'false' if tk.text == '==' else 'true'))
                i += 2
                prev_sig = 'null'
                continue
            if tk.kind == 'id' and tk.text in ('if', 'else', 'while', 'for', 'do', 'switch', 'case', 'break',
                                               'continue', 'throw', 'try', 'catch', 'finally', 'default'):
                out.append(pre + tk.text)
                prev_sig = tk.text
                i += 1
                continue
            if tk.kind == 'op' and tk.text in ('==', '!=') and self.tx(i + 1) == '0' and \
                    self.last_kind is not None and self.last_kind.startswith('enum:'):
                # C# compares any enum with 0; an enum class needs its zero value
                en = self.last_kind[5:]
                out.append(pre + tk.text + self.t[i + 1].pre + '%s{}' % self.tr.enum_prefix(en))
                i += 2
                prev_sig = '0'
                continue
            if tk.kind == 'op' and tk.text in ('==', '!=', '=') and self.tx(i + 1) == 'null' and \
                    self.last_kind is not None and (self.last_kind == 'optstr' or self.last_kind.startswith('opt:')):
                out.append(pre + tk.text + self.t[i + 1].pre + 'std::nullopt')
                i += 2
                prev_sig = 'null'
                continue
            if tk.kind in ('id', 'num', 'str', 'vstr', 'chr') or (tk.kind == 'op' and tk.text == '('):
                s, kind, i = self.chain(i, b)
                self.last_kind = kind
                out.append(pre + s)
                prev_sig = 'x'
                continue
            # plain operator / punctuation
            if tk.kind == 'op' and tk.text == '}' and i in pending_defaults:
                out.append(' default: break; ')
            out.append(pre + tk.text)
            prev_sig = tk.text
            self.last_kind = None if tk.text not in ('==', '!=') else self.last_kind
            i += 1
        return ''.join(out)

    def string_switch(self, subject, lb, b):
        """switch over a string -> if/else chain (C# case labels are string constants)."""
        rb = _match(self.t, lb, '{', '}')
        groups = []          # (labels, stmt_start, stmt_end); label None = default
        k = lb + 1
        cur = None
        depth = 0
        while k < rb:
            x = self.t[k]
            if depth == 0 and x.kind == 'id' and x.text in ('case', 'default'):
                if x.text == 'default':
                    lab, end = None, k + 1
                else:
                    end = self.expr_end(k + 1, rb, stops=(':',))
                    lab = self.seq(k + 1, end).strip()
                if self.tx(end) != ':':
                    raise Untranslatable('switch label')
                if cur is None or cur[1] != k:
                    cur = [[], end + 1, end + 1]
                    groups.append(cur)
                cur[0].append(lab)
                cur[1] = cur[2] = end + 1
                k = end + 1
                continue
            if x.kind == 'op' and x.text in '([{':
                depth += 1
            elif x.kind == 'op' and x.text in ')]}':
                depth -= 1
            k += 1
            if cur is not None:
                cur[2] = k
        # statements of a group run from its last label to the next group's first label
        for gi, g in enumerate(groups):
            g[2] = rb
            if gi + 1 < len(groups):
                # find first label token of next group
                k2 = g[1]
                d = 0
                while k2 < rb:
                    x = self.t[k2]
                    if d == 0 and x.kind == 'id' and x.text in ('case', 'default'):
                        break
                    if x.kind == 'op' and x.text in '([{':
                        d += 1
                    elif x.kind == 'op' and x.text in ')]}':
                        d -= 1
                    k2 += 1
                g[2] = k2
        parts = ['{ const std::string sw_(CsStr(%s).get()); ' % subject]
        first = True
        default = None
        for labels, a, e in groups:
            # drop a trailing `break;`
            toks = list(range(a, e))
            if e - a >= 2 and self.tx(e - 2) == 'break' and self.tx(e - 1) == ';':
                e -= 2
            body = self.seq(a, e, stmt=True)
            if None in labels:
                default = body
                continue
            cond = ' || '.join('sw_ == %s' % l for l in labels)
            parts.append('%sif (%s) {%s} ' % ('' if first else 'else ', cond, body))
            first = False
        if default is not None:
            parts.append(('else {%s} ' if not first else '{%s} ') % default)
        parts.append('}')
        return ''.join(parts), rb + 1

    def declaration(self, cstype, j, b):
        cpp, kind = self.tr.cpp_type(cstype)
        stmts = []
        while True:
            name = self.tx(j)
            j += 1
            rhs = None
            if self.tx(j) == '=':
                end = self.expr_end(j + 1, b, stops=(';', ','))
                rhs = self.seq(j + 1, end).strip()
                rk = self.last_kind
                if rhs == 'nullptr' and cpp.startswith('std::optional'):
                    rhs = 'std::nullopt'
                if cpp == 'auto' and rk and rk != 'unknown':
                    kind = rk
                j = end
            if name in self.scope.ctx_locals:
                # lives in the rule context (a predicate reads it); the prologue binds a reference
                stmts.append('%s = %s' % (name, rhs) if rhs is not None else '(void)%s' % name)
                self.scope.add(name, kind, cpp, ctx=True)
            else:
                # A C# collection local initialized from an existing collection
                # aliases it (reference type); a C++ vector would be a copy.
                alias = (rhs is not None and kind.split(':')[0] in ('vec', 'strvec')
                         and not rhs.startswith(cpp))
                stmts.append('%s%s %s%s' % (cpp, '&' if alias else '', name,
                                            (' = ' + rhs) if rhs is not None else '{}'))
                self.scope.add(name, kind, cpp)
            if self.tx(j) == ',':
                j += 1
                continue
            break
        return '; '.join(stmts), j

    def expr_end(self, i, b, stops):
        depth = 0
        while i < b:
            x = self.t[i]
            if x.kind == 'op':
                if x.text in '([{':
                    depth += 1
                elif x.text in ')]}':
                    if depth == 0:
                        return i
                    depth -= 1
                elif depth == 0 and x.text in stops:
                    return i
            i += 1
        return i

    def foreach(self, i, b):
        # foreach (Type name in expr)
        if self.tx(i + 1) != '(':
            raise Untranslatable('foreach syntax')
        close = _match(self.t, i + 1, '(', ')')
        r = self.type_text(i + 2)
        if not r:
            raise Untranslatable('foreach type')
        cstype, j = r
        name = self.tx(j)
        if self.tx(j + 1) != 'in':
            raise Untranslatable('foreach syntax')
        coll = self.seq(j + 2, close).strip()
        ck = self.last_kind or ''
        if cstype == 'var':
            cpp, kind = 'auto', ('frag:' + ck[4:] if ck.startswith('vec:') else 'unknown')
        else:
            cpp, kind = self.tr.cpp_type(cstype)
        self.scope.add(name, kind, cpp)
        return 'for (%s %s : %s)' % (cpp, name, coll), close + 1

    # -------------------------------------------------------------- expressions
    def args(self, i, b):
        """t[i] == '(' -> ('(a, b)', next index). ref/out markers are dropped (C++ references)."""
        close = _match(self.t, i, '(', ')')
        parts = self.split_commas(i + 1, close)
        out = []
        for a, e in parts:
            if a < e and self.kd(a) == 'id' and self.tx(a) in ('ref', 'out'):
                if self.tx(a) == 'out' and self.is_decl_inline(a + 1, e):
                    raise Untranslatable('out variable declaration')   # not hoisted (method mode)
                a += 1
            out.append(self.seq(a, e).strip())
        return '(' + ', '.join(out) + ')', close + 1

    def is_decl_inline(self, a, e):
        return e - a >= 2 and self.kd(a) == 'id' and self.kd(a + 1) == 'id'

    def is_cast(self, i, close):
        r = self.type_text(i + 1)
        if not r or r[1] != close:
            return None
        txt = r[0]
        base = re.sub(r'<.*', '', txt.replace('antlr.', '')).rstrip('[]?')
        if base in self.scope.vars or not self.tr.is_type_name(base) or base == 'var':
            return None
        nxt = self.t[close + 1] if close + 1 < len(self.t) else None
        if nxt is None:
            return None
        if nxt.kind in ('id', 'num', 'str', 'vstr', 'chr') and nxt.text not in ('is', 'as', 'in'):
            return txt
        if nxt.kind == 'op' and nxt.text in ('(', '!', '~'):
            return txt
        return None

    def chain(self, i, b):
        t = self.t[i]
        text, kind = None, 'unknown'
        if t.kind == 'op' and t.text == '(':
            close = _match(self.t, i, '(', ')')
            ct = self.is_cast(i, close)
            if ct:
                cpp, k = self.tr.cpp_type(ct)
                inner, ik, j = self.chain(close + 1, b)
                if k.startswith('frag:'):
                    text, kind = 'dynamic_cast<%s>(%s)' % (cpp, inner.strip()), k
                else:
                    text, kind = 'static_cast<%s>(%s)' % (cpp, inner.strip()), k
                i = j
            else:
                inner = self.seq(i + 1, close)
                kind = self.last_kind or 'unknown'
                text = '(' + inner + self.t[close].pre + ')'
                i = close + 1
        elif t.kind == 'str':
            text, kind = self.cs_string(t.text), 'cstr'
            i += 1
        elif t.kind == 'vstr':
            body = t.text[2:-1].replace('""', '"')
            text, kind = 'R"g2(%s)g2"' % body, 'cstr'
            i += 1
        elif t.kind == 'chr':
            text, kind = t.text, 'int'
            i += 1
        elif t.kind == 'num':
            v = t.text
            if re.search(r'[mM]$', v):
                v = v[:-1]   # decimal literal
            elif re.search(r'[uU][lL]$|[lL][uU]$', v):
                v = re.sub(r'[uUlL]+$', 'ULL', v)
            elif re.search(r'[lL]$', v):
                v = v[:-1] + 'LL'
            text, kind = v, 'int'
            i += 1
        else:
            text, kind, i = self.primary_id(i, b)
        return self.postfix(text, kind, i, b)

    @staticmethod
    def cs_string(s):
        body = s[1:-1]
        # \uXXXX in C# strings -> \u in C++ is a universal-character-name (UTF-8 encoded)
        return '"' + body + '"'

    def primary_id(self, i, b):
        name = self.tx(i)
        nxt = self.tx(i + 1)
        if name == 'null':
            return 'nullptr', 'null', i + 1
        if name in ('true', 'false'):
            return name, 'bool', i + 1
        if name == 'new':
            return self.new_expr(i, b)
        if name == 'this':
            if nxt == '.':
                return self.primary_id(i + 2, b)
            return 'this', 'unknown', i + 1
        if name == 'FragmentFactory' and nxt == '.' and self.tx(i + 2) == 'CreateFragment':
            return self.create_fragment(i + 2)
        if name == 'CreateFragment' and nxt == '<':
            return self.create_fragment(i)
        if name in self.scope.vars:
            k = self.scope.kind(name)
            if self.mode == 'pred' and name in self.scope.pred_alias:
                return self.scope.pred_alias[name], k, i + 1
            if self.mode == 'pred' and name in self.scope.ctx:
                return '\x01' + name, k, i + 1
            return name, k, i + 1
        if name == 'EOF':
            return 'antlr4::Token::EOF', 'int', i + 1
        if nxt == '<' and name not in self.scope.vars:
            e = self.generic_end(i + 1)
            if e is not None and self.tx(e + 1) == '(':
                # Method<T>(...): explicit type arguments; C++ deduces them
                a, j = self.args(e + 1, b)
                return name + a, FUNC_KINDS.get(name, 'unknown'), j
        if nxt == '(':
            a, j = self.args(i + 1, b)
            if name == 'LAST_NODE':
                raise Untranslatable('LAST_NODE')
            return name + a, FUNC_KINDS.get(name, 'unknown'), j
        if nxt == '.' and self.kd(i + 2) == 'id':
            return self.static_access(i, b)
        if name in self.tr.tokens:
            return name, 'int', i + 1
        if name in ('string', 'String') or self.m.is_ast_class(name) or self.m.enum_kind(name):
            raise Untranslatable('type name in expression: ' + name)
        return name, 'unknown', i + 1

    def create_fragment(self, i):
        # CreateFragment < T > ( )
        if self.tx(i + 1) != '<' or self.tx(i + 3) != '>' or self.tx(i + 4) != '(' or self.tx(i + 5) != ')':
            raise Untranslatable('CreateFragment syntax')
        cls = self.tx(i + 2)
        return 'CreateFragment<ast::%s>()' % cls, 'frag:' + cls, i + 6

    def static_access(self, i, b):
        name, mem = self.tx(i), self.tx(i + 2)
        j = i + 3
        if name in STATIC_STRING_CLASSES:
            self.tr.helpers_used.add(name)
            return '%s::%s' % (name, mem), 'cstr', j
        if name in DOTNET_ENUMS:
            return '%s::%s' % (name, mem), 'enum:' + name, j
        if name == 'TSqlTokenType':
            return 'ast::TSqlTokenType::' + mem, 'enum:TSqlTokenType', j
        if GRAMMAR_CLASS_RE.match(name):
            # token type constant; usable from the parser base too (same numbering as TSqlTokenType)
            if mem == 'EOF':
                return 'antlr4::Token::EOF', 'int', j
            return 'static_cast<size_t>(ast::TSqlTokenType::%s)' % mem, 'int', j
        if BASE_CLASS_RE.match(name):
            if self.tx(j) == '(':
                a, j2 = self.args(j, b)
                return mem + a, FUNC_KINDS.get(mem, 'unknown'), j2
            return mem, 'unknown', j
        if name in ('String', 'string'):
            if mem == 'Empty':
                return 'std::string()', 'str', j
            if self.tx(j) == '(':
                a, j2 = self.args(j, b)
                return 'String_%s%s' % (mem, a), ('bool' if mem in ('Equals', 'IsNullOrEmpty') else 'str'), j2
            raise Untranslatable('String.' + mem)
        if name == '_tokenSource' and mem == 'QuotedIdentifier' and self.tx(j) == '=':
            # TSqlWhitespaceTokenFilter.QuotedIdentifier setter (SET QUOTED_IDENTIFIER ON|OFF)
            end = self.expr_end(j + 1, b, stops=(';', ','))
            return 'SetQuotedIdentifier(%s)' % self.seq(j + 1, end).strip(), 'unknown', end
        if name == '_tokenSource' and mem == 'LastToken' and self.tx(j) == '.' and self.tx(j + 1) == 'Offset':
            # TSqlWhitespaceTokenFilter.LastToken: the furthest token fetched so far
            return 'LastTokenOffset()', 'int', j + 2
        if name == 'int':
            name = 'Int32'
        if name in ('Int32', 'Int64', 'UInt64', 'Math', 'Char', 'Decimal', 'Enum', 'Double'):
            if mem in ('MaxValue', 'MinValue'):
                ctype = {'Int32': 'int32_t', 'Int64': 'int64_t', 'UInt64': 'uint64_t'}.get(name)
                if not ctype:
                    raise Untranslatable(name + '.' + mem)
                return 'std::numeric_limits<%s>::%s()' % (ctype, 'max' if mem == 'MaxValue' else 'min'), 'int', j
            if self.tx(j) == '(':
                a, j2 = self.args(j, b)
                return '%s_%s%s' % (name, mem, a), 'unknown', j2
            raise Untranslatable(name + '.' + mem)
        if name == 'Debug':
            a, j2 = self.args(j, b)
            return 'assert' + a if mem == 'Assert' else '(void)0', 'unknown', j2
        ek = self.m.enum_kind(name)
        if ek:
            return '%s::%s' % (self.tr.enum_prefix(name), mem), 'enum:' + name, j
        if self.m.is_ast_class(name):
            return 'ast::%s::%s' % (name, mem), 'unknown', j
        # helper / other static classes: XHelper.Instance.Method(...)
        self.tr.static_classes_used.add(name)
        if mem in self.m.cs_enums and self.tx(j) == '.' and self.kd(j + 1) == 'id':
            # Outer.NestedEnum.Value
            return '%s::%s::%s' % (name, mem, self.tx(j + 1)), 'enum:%s::%s' % (name, mem), j + 2
        if self.tx(j) == '(':
            a, j2 = self.args(j, b)
            return '%s::%s%s' % (name, mem, a), FUNC_KINDS.get(mem, 'unknown'), j2
        if mem == 'Instance':
            return '%s::Instance()' % name, 'helper:' + name, j
        return '%s::%s' % (name, mem), 'unknown', j

    def new_expr(self, i, b):
        r = self.type_text(i + 1)
        if not r:
            raise Untranslatable('new expression')
        txt, j = r
        if txt.endswith('[]') or (self.tx(j) == '[' and self.tx(j + 1) == ']'):
            if self.tx(j) == '[':
                j += 2
            if self.tx(j) != '{':
                raise Untranslatable('array creation')
            close = _match(self.t, j, '{', '}')
            items = [self.seq(a, e).strip() for a, e in self.split_commas(j + 1, close)]
            cpp, kind = self.tr.cpp_type(txt if txt.endswith('[]') else txt + '[]')
            return '%s{%s}' % (cpp, ', '.join(items)), kind, close + 1
        if re.match(r'I?List<', txt):
            cpp, kind = self.tr.cpp_type(txt)
            if self.tx(j) != '(':
                raise Untranslatable('new List syntax')
            close = _match(self.t, j, '(', ')')
            if close != j + 1:
                raise Untranslatable('new List with arguments')
            return cpp + '()', kind, close + 1
        if txt in ('TSqlParseErrorException', 'ParseError'):
            a, j2 = self.args(j, b)
            return txt + a, ('exc' if txt == 'TSqlParseErrorException' else 'perr'), j2
        if txt == 'System.Text.StringBuilder' or txt == 'StringBuilder':
            if self.tx(j) != '(' or self.tx(j + 1) != ')':
                raise Untranslatable('StringBuilder with arguments')
            return 'std::string()', 'sb', j + 2
        if self.m.enum_kind(txt) and self.tx(j) == '(' and self.tx(j + 1) == ')':
            return '%s{}' % self.tr.enum_prefix(txt), 'enum:' + txt, j + 2   # default(enum)
        if self.m.is_ast_class(txt) and txt in self.m.classes and not self.m.classes[txt].abstract:
            # `new X() { A = b, ... }` bypasses the fragment factory: no token stream until a
            # setter adopts a child's (TSqlFragment.UpdateTokenInfo)
            if self.tx(j) == '(':
                if self.tx(j + 1) != ')':
                    raise Untranslatable('AST constructor with arguments')
                j += 2
            sets = []
            if self.tx(j) == '{':
                close = _match(self.t, j, '{', '}')
                for a, e in self.split_commas(j + 1, close):
                    if e - a < 3 or self.kd(a) != 'id' or self.tx(a + 1) != '=':
                        raise Untranslatable('object initializer syntax')
                    mem = self.m.lookup_member(txt, self.tx(a))
                    if mem is None or mem.collection:
                        raise Untranslatable('object initializer member ' + self.tx(a))
                    sets.append('n_->set_%s(%s); ' % (self.tx(a), self.seq(a + 2, e).strip()))
                j = close + 1
            return ('[&]() { auto* n_ = CreateFragment<ast::%s>(); n_->ScriptTokenStream = nullptr; %sreturn n_; }()'
                    % (txt, ''.join(sets))), 'frag:' + txt, j
        raise Untranslatable('new ' + txt)

    def postfix(self, text, kind, i, b):
        while i < b:
            x = self.tx(i)
            if x == '.' and self.kd(i + 1) == 'id':
                name = self.tx(i + 1)
                text, kind, i = self.member(text, kind, name, i + 2, b)
                continue
            if x == '[':
                close = _match(self.t, i, '[', ']')
                idx = self.seq(i + 1, close).strip()
                if kind.startswith('vec:'):
                    text, kind = '%s[%s]' % (text, idx), 'frag:' + kind[4:]
                elif kind == 'strvec':
                    text, kind = '%s[%s]' % (text, idx), 'str'
                elif kind == 'frag:MultiPartIdentifier':
                    text, kind = '(*%s)[%s]' % (text, idx), 'frag:Identifier'
                elif kind in ('str', 'optstr', 'cstr'):
                    text, kind = 'Str_At(%s, %s)' % (text, idx), 'int'
                else:
                    raise Untranslatable('indexing unknown kind ' + kind)
                i = close + 1
                continue
            if x in ('++', '--'):
                text += self.t[i].pre + x
                i += 1
                continue
            if x == 'is' and self.kd(i + 1) == 'id':
                r = self.type_text(i + 1)
                tt, j = r
                if self.kd(j) == 'id' and self.tx(j) not in CS_KEYWORDS:
                    if self.tx(j) not in self.scope.pattern_vars:
                        raise Untranslatable('is-pattern with declaration')
                    cpp, k = self.tr.cpp_type(tt)
                    text, kind = '((%s = dynamic_cast<%s>(%s)) != nullptr)' % (self.tx(j), cpp, text.strip()), 'bool'
                    i = j + 1
                    continue
                cpp, k = self.tr.cpp_type(tt)
                if not k.startswith('frag:'):
                    raise Untranslatable('is on non-AST type')
                text, kind = '(dynamic_cast<%s>(%s) != nullptr)' % (cpp, text.strip()), 'bool'
                i = j
                continue
            if x == 'as' and self.kd(i + 1) == 'id':
                tt, j = self.type_text(i + 1)
                cpp, k = self.tr.cpp_type(tt)
                text, kind = 'dynamic_cast<%s>(%s)' % (cpp, text.strip()), k
                i = j
                continue
            break
        return text, kind, i

    def member(self, text, kind, name, i, b):
        """text.name at t[i] (just after name)."""
        call = self.tx(i) == '('
        if kind == 'tokvecptr':
            if name == 'Count':
                return 'static_cast<int>(%s->size())' % text, 'int', i
            raise Untranslatable('token stream member ' + name)
        if kind == 'sb':
            if call and name == 'Append':
                a, j = self.args(i, b)
                return '%s += %s' % (text, a), 'unknown', j
            if call and name == 'ToString' and self.tx(i + 1) == ')':
                return text, 'str', i + 2
            raise Untranslatable('StringBuilder member ' + name)
        if kind.startswith('opt:'):
            if name == 'HasValue' and not call:
                return '%s.has_value()' % text, 'bool', i
            if name == 'Value' and not call:
                return '(*%s)' % text, kind[4:] if kind[4:] in ('bool', 'int') else 'enum:' + kind[4:], i
            raise Untranslatable('member %s on %s' % (name, kind))
        if kind.startswith('vec:') or kind == 'strvec':
            if name == 'Count':
                return 'static_cast<int>(%s.size())' % text, 'int', i
            if call:
                a, j = self.args(i, b)
                if name == 'Add':
                    return '%s.push_back%s' % (text, a), 'unknown', j
                if name == 'Clear':
                    return '%s.clear()' % text, 'unknown', j
                inner = a[1:-1]
                return 'Vec_%s(%s%s)' % (name, text, (', ' + inner) if inner else ''), \
                    ('bool' if name == 'Contains' else 'unknown'), j
            raise Untranslatable('collection member ' + name)
        if kind in ('str', 'optstr', 'cstr') or (kind == 'unknown' and name in STRING_METHOD_KIND and call):
            if name == 'Length' and not call:
                return 'Str_Length(%s)' % text, 'int', i
            if call:
                a, j = self.args(i, b)
                inner = a[1:-1]
                if name in ('ToUpper', 'ToUpperInvariant') and self.tx(j) in ('==', '!='):
                    # s.ToUpper[Invariant](...) == t: compared without building the upper-case copy
                    try:
                        right, rk, k = self.chain(j + 1, b)
                    except Untranslatable:
                        rk = None
                    if rk in ('str', 'cstr') and (k >= b or self.tx(k) in (')', '&&', '||', ';', ',', '?', ':')):
                        return '%sStr_UpperEquals(%s, %s)' % ('!' if self.tx(j) == '!=' else '', text, right.strip()), \
                            'bool', k
                return 'Str_%s(%s%s)' % (name, text, (', ' + inner) if inner else ''), \
                    STRING_METHOD_KIND.get(name, 'unknown'), j
            raise Untranslatable('string member ' + name)
        if kind == 'exc' or kind == 'perr':
            if name == 'token':
                # antlr.RecognitionException.token: ANTLR 2's error token (see ErrorToken)
                return 'ErrorToken(%s)' % text, 'tok', i
            if call:
                a, j = self.args(i, b)
                return '%s.%s%s' % (text, name, a), 'unknown', j
            return '%s.%s' % (text, name), ('perr' if name == 'ParseError' else 'unknown'), i
        if kind.startswith('helper:'):
            if call:
                a, j = self.args(i, b)
                return '%s.%s%s' % (text, name, a), 'unknown', j
            raise Untranslatable('helper member ' + name)
        if kind.startswith('enum:') or kind == 'int' or kind == 'bool':
            if name == 'ToString' and call:
                a, j = self.args(i, b)
                return 'ToString(%s)' % text, 'str', j
            raise Untranslatable('member %s on %s' % (name, kind))
        if kind == 'tok':
            if call:
                a, j = self.args(i, b)
                if name == 'getText':
                    return '%s->getText()' % text, 'str', j
                if name == 'getLine':
                    return 'static_cast<int>(%s->getLine())' % text, 'int', j
                if name == 'getColumn':
                    return 'static_cast<int>(%s->getCharPositionInLine())' % text, 'int', j
                raise Untranslatable('token method ' + name)
            if name == 'Type':
                return 'static_cast<int>(%s->getType())' % text, 'int', i
            raise Untranslatable('token member ' + name)
        # AST node (or unknown object)
        cls = kind[5:] if kind.startswith('frag:') else None
        if not call and name == 'ScriptTokenStream':
            return '%s->ScriptTokenStream' % text, 'tokvecptr', i
        if not call and name in ('StartOffset', 'FragmentLength', 'StartLine', 'StartColumn'):
            return '%s->%s()' % (text, name), 'int', i
        if call:
            a, j = self.args(i, b)
            return '%s->%s%s' % (text, name, a), 'unknown', j
        if cls and (cls, name) in PARTIAL_CLASS_PROPS:
            return '%s->%s()' % (text, name), PARTIAL_CLASS_PROPS[(cls, name)], i
        mem = self.m.lookup_member(cls, name) if cls else None
        if mem is None and name in PARTIAL_PROPS:
            return '%s->%s()' % (text, name), PARTIAL_PROPS[name], i
        if mem is None and cls and cls in self.m.classes:
            # not an Ast.xml member of the static type (a C# partial-class member, or a member of
            # a subclass reached through a looser static type in C#): needs a hand port
            if not any(self.m.lookup_member(c, name) for c in self.m.classes if self.m.is_subclass(c, cls)):
                raise Untranslatable('%s is not a member of %s' % (name, cls))
        if mem is None:
            cands = self.m.member_index.get(name, [])
            mem = cands[0] if cands else None
        if mem is None:
            if kind == 'unknown':
                return '%s->%s' % (text, name), 'unknown', i
            raise Untranslatable('member %s on %s' % (name, kind))
        mk = self.m.member_kind(mem)
        # assignment to a member -> setter (mirrors the C# property setter)
        if self.tx(i) == '=' and not mem.collection:
            end = self.expr_end(i + 1, b, stops=(';', ','))
            rhs = self.seq(i + 1, end).strip()
            if rhs == 'nullptr' and (mk == 'optstr' or mk.startswith('opt:')):
                rhs = 'std::nullopt'
            return '%s->set_%s(%s)' % (text, name, rhs), 'unknown', end
        if cls in self.m.interfaces:
            # Ast.xml interface members are virtual get_X()/set_X()
            return '%s->get_%s()' % (text, name), mk, i
        if mem.first_item:
            return '%s->%s()' % (text, name), mk, i
        return '%s->%s' % (text, name), mk, i
