"""Reader for ANTLR 2.7 grammar files (the subset SqlScriptDOM's TSql*.g use).

Produces a small object model: Grammar -> GrammarClass (parser/lexer) -> Rule ->
Block -> Alt -> elements. Comments are dropped; action text is kept verbatim.
"""
import hashlib
import re

# ---------------------------------------------------------------- tokenizer


class Tok:
    __slots__ = ("kind", "text", "line")

    def __init__(self, kind, text, line):
        self.kind, self.text, self.line = kind, text, line

    def __repr__(self):
        return "Tok(%s,%r,%d)" % (self.kind, self.text, self.line)


def _skip_csharp_balanced(src, i, open_ch, close_ch):
    """src[i] == open_ch. Returns index just past the matching close_ch.
    Skips C# strings, verbatim strings, chars and comments inside."""
    depth = 0
    n = len(src)
    while i < n:
        c = src[i]
        if c == open_ch:
            depth += 1
            i += 1
        elif c == close_ch:
            depth -= 1
            i += 1
            if depth == 0:
                return i
        elif c == '@' and i + 1 < n and src[i + 1] == '"':
            i += 2
            while i < n:
                if src[i] == '"':
                    if i + 1 < n and src[i + 1] == '"':
                        i += 2
                        continue
                    i += 1
                    break
                i += 1
        elif c == '"':
            i += 1
            while i < n and src[i] != '"':
                if src[i] == '\\':
                    i += 1
                i += 1
            i += 1
        elif c == "'":
            # char literal ('x', '\n', '\u0000'); be conservative
            m = re.match(r"'(\\u[0-9a-fA-F]{4}|\\.|[^\\'])'", src[i:])
            if m:
                i += len(m.group(0))
            else:
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '/':
            while i < n and src[i] != '\n':
                i += 1
        elif c == '/' and i + 1 < n and src[i + 1] == '*':
            j = src.find('*/', i + 2)
            i = n if j < 0 else j + 2
        else:
            i += 1
    raise SyntaxError("unbalanced %s at end of input" % open_ch)


_PUNCT = ["=>", "..", ":", ";", "|", "(", ")", "*", "+", "?", "~", "!", "^", "=", ",", ".", "#"]


def tokenize(src):
    toks = []
    i, n, line = 0, len(src), 1
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1
            i += 1
            continue
        if c in ' \t\r\f':
            i += 1
            continue
        if src.startswith('//', i):
            while i < n and src[i] != '\n':
                i += 1
            continue
        if src.startswith('/*', i):
            j = src.find('*/', i + 2)
            line += src.count('\n', i, j)
            i = j + 2
            continue
        start_line = line
        if c == '{':
            j = _skip_csharp_balanced(src, i, '{', '}')
            text = src[i + 1:j - 1]
            line += src.count('\n', i, j)
            # semantic predicate?
            k = j
            while k < n and src[k] in ' \t\r\n':
                k += 1
            if k < n and src[k] == '?' and not src.startswith('?=', k):
                line += src.count('\n', j, k)
                toks.append(Tok('SEMPRED', text, start_line))
                i = k + 1
            else:
                toks.append(Tok('ACTION', text, start_line))
                i = j
            continue
        if c == '[':
            j = _skip_csharp_balanced(src, i, '[', ']')
            toks.append(Tok('ARG', src[i + 1:j - 1], start_line))
            line += src.count('\n', i, j)
            i = j
            continue
        if c == '"':
            j = i + 1
            while src[j] != '"':
                if src[j] == '\\':
                    j += 1
                j += 1
            toks.append(Tok('STRING', src[i:j + 1], start_line))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while src[j] != "'":
                if src[j] == '\\':
                    j += 1
                j += 1
            toks.append(Tok('CHAR', src[i:j + 1], start_line))
            i = j + 1
            continue
        if c.isalpha() or c == '_':
            m = re.match(r'[A-Za-z_][A-Za-z0-9_]*', src[i:])
            toks.append(Tok('ID', m.group(0), start_line))
            i += len(m.group(0))
            continue
        if c.isdigit():
            m = re.match(r'[0-9]+', src[i:])
            toks.append(Tok('INT', m.group(0), start_line))
            i += len(m.group(0))
            continue
        for p in _PUNCT:
            if src.startswith(p, i):
                toks.append(Tok(p, p, start_line))
                i += len(p)
                break
        else:
            raise SyntaxError("unexpected char %r at line %d" % (c, line))
    toks.append(Tok('EOF', '', line))
    return toks


# ---------------------------------------------------------------- model


class Node:
    pass


class TokenRef(Node):
    def __init__(self, name, label=None, args=None):
        self.name, self.label, self.args = name, label, args


class RuleRef(Node):
    def __init__(self, name, label=None, assign=None, args=None):
        self.name, self.label, self.assign, self.args = name, label, assign, args


class Literal(Node):
    """String ("abc") or char ('a') literal; text includes quotes."""

    def __init__(self, text, label=None):
        self.text, self.label = text, label


class Range(Node):
    def __init__(self, lo, hi):
        self.lo, self.hi = lo, hi


class Not(Node):
    def __init__(self, elem):
        self.elem = elem


class Wildcard(Node):
    def __init__(self, label=None):
        self.label = label


class Action(Node):
    def __init__(self, text, line):
        self.text, self.line = text, line


class SemPred(Node):
    def __init__(self, text, line):
        self.text, self.line = text, line


class SubRule(Node):
    """suffix in {'', '?', '*', '+', '=>'}; '=>' marks a syntactic predicate."""

    def __init__(self, block, suffix):
        self.block, self.suffix = block, suffix


class Block(Node):
    def __init__(self, alts, options=None, init=None):
        self.alts, self.options, self.init = alts, options or {}, init


class Alt(Node):
    def __init__(self, elems):
        self.elems = elems


class Rule:
    def __init__(self):
        self.name = None
        self.protected = False
        self.args = None
        self.returns = None
        self.options = {}
        self.init = None
        self.block = None
        self.handlers = []   # list of (catch-arg, action-text)
        self.line = 0
        self.src_lines = 0
        self.sha = None


class GrammarClass:
    def __init__(self, name, kind, base):
        self.name, self.kind, self.base = name, kind, base
        self.options = {}
        self.tokens = []      # list of (name, literal-or-None)
        self.member_action = None
        self.preamble = None
        self.rules = []
        self.rule_map = {}


class Grammar:
    def __init__(self):
        self.header = None
        self.options = {}
        self.classes = []


# ---------------------------------------------------------------- parser


class Parser:
    def __init__(self, src):
        self.toks = tokenize(src)
        self.p = 0
        self.src = src

    def la(self, k=1):
        return self.toks[min(self.p + k - 1, len(self.toks) - 1)]

    def next(self):
        t = self.toks[self.p]
        self.p += 1
        return t

    def expect(self, kind, text=None):
        t = self.next()
        if t.kind != kind or (text is not None and t.text != text):
            raise SyntaxError("expected %s %s, got %r" % (kind, text or '', t))
        return t

    def parse_options(self):
        # 'options' already seen; next is ACTION containing k=v; pairs
        t = self.expect('ACTION')
        opts = {}
        for m in re.finditer(r'(\w+)\s*=\s*("([^"]*)"|\'[^\']*\'\s*\.\.\s*\'[^\']*\'|[^;]+);', t.text):
            v = m.group(2).strip()
            opts[m.group(1)] = v.strip('"')
        return opts

    def parse_grammar(self):
        g = Grammar()
        pending_action = None
        while self.la().kind != 'EOF':
            t = self.la()
            if t.kind == 'ID' and t.text == 'header':
                self.next()
                g.header = self.expect('ACTION').text
            elif t.kind == 'ID' and t.text == 'options' and not g.classes:
                self.next()
                g.options = self.parse_options()
            elif t.kind == 'ACTION':
                pending_action = self.next().text
            elif t.kind == 'ID' and t.text == 'class':
                cls = self.parse_class()
                cls.preamble = pending_action
                pending_action = None
                g.classes.append(cls)
            else:
                raise SyntaxError("unexpected %r at top level" % t)
        return g

    def parse_class(self):
        self.expect('ID', 'class')
        name = self.expect('ID').text
        self.expect('ID', 'extends')
        kind = self.expect('ID').text
        base = None
        if self.la().kind == '(':
            self.next()
            base = self.expect('STRING').text.strip('"')
            self.expect(')')
        self.expect(';')
        cls = GrammarClass(name, kind, base)
        while True:
            t = self.la()
            if t.kind == 'ID' and t.text == 'options':
                self.next()
                cls.options = self.parse_options()
            elif t.kind == 'ID' and t.text == 'tokens':
                self.next()
                body = self.expect('ACTION').text
                body = re.sub(r'//[^\n]*', '', body)
                for m in re.finditer(r'(\w+)\s*(=\s*"([^"]*)")?\s*;', body):
                    cls.tokens.append((m.group(1), m.group(3)))
            elif t.kind == 'ACTION':
                cls.member_action = self.next().text
            else:
                break
        # rules until next 'class' or a trailing ACTION followed by 'class' or EOF
        while True:
            t = self.la()
            if t.kind == 'EOF':
                break
            if t.kind == 'ACTION' and self.la(2).kind == 'ID' and self.la(2).text == 'class':
                break
            if t.kind == 'ID' and t.text == 'class':
                break
            r = self.parse_rule()
            cls.rules.append(r)
            cls.rule_map[r.name] = r
        return cls

    def parse_rule(self):
        r = Rule()
        first_tok = self.p
        t = self.la()
        if t.kind == 'ID' and t.text in ('protected', 'public', 'private'):
            r.protected = t.text == 'protected'
            self.next()
        nt = self.expect('ID')
        r.name, r.line = nt.text, nt.line
        if self.la().kind == '!':
            self.next()
        if self.la().kind == 'ARG':
            r.args = self.next().text
        if self.la().kind == 'ID' and self.la().text == 'returns':
            self.next()
            r.returns = self.expect('ARG').text
        if self.la().kind == 'ID' and self.la().text == 'throws':
            self.next()
            while self.la().kind in ('ID', ',', '.'):
                self.next()
        if self.la().kind == 'ID' and self.la().text == 'options':
            self.next()
            r.options = self.parse_options()
        if self.la().kind == 'ACTION':
            r.init = self.next().text
        self.expect(':')
        r.block = self.parse_alts(');')
        end = self.expect(';')
        if self.la().kind == 'ID' and self.la().text == 'exception':
            self.next()
            if self.la().kind == 'ARG':
                self.next()
            while self.la().kind == 'ID' and self.la().text == 'catch':
                self.next()
                arg = self.expect('ARG').text
                act = self.expect('ACTION').text
                r.handlers.append((arg, act))
        r.src_lines = self.la().line - r.line
        # identity of the rule's source (whitespace/comment-insensitive): overrides check it
        r.sha = hashlib.sha1(' '.join('%s:%s' % (x.kind, re.sub(r'\s+', ' ', x.text).strip())
                                      for x in self.toks[first_tok:self.p]).encode()).hexdigest()[:8]
        return r

    def parse_alts(self, enders):
        alts = [self.parse_alt(enders)]
        while self.la().kind == '|':
            self.next()
            alts.append(self.parse_alt(enders))
        return Block(alts)

    def parse_alt(self, enders):
        elems = []
        while True:
            t = self.la()
            if t.kind in ('|', ')', ';', 'EOF'):
                break
            if t.kind == 'ID' and t.text == 'exception':
                raise SyntaxError("alternative-level exception handlers unsupported (line %d)" % t.line)
            elems.append(self.parse_element())
        return Alt(elems)

    def parse_element(self):
        t = self.la()
        label = None
        # label:
        if t.kind == 'ID' and self.la(2).kind == ':':
            label = t.text
            self.next()
            self.next()
            t = self.la()
        assign = None
        if t.kind == 'ID' and self.la(2).kind == '=' and self.la(3).kind == 'ID':
            assign = t.text
            self.next()
            self.next()
            t = self.la()
        if t.kind == 'ACTION':
            self.next()
            return Action(t.text, t.line)
        if t.kind == 'SEMPRED':
            self.next()
            return SemPred(t.text, t.line)
        if t.kind == '~':
            self.next()
            inner = self.parse_element()
            return Not(inner)
        if t.kind == '.':
            self.next()
            return Wildcard(label)
        if t.kind in ('STRING', 'CHAR'):
            self.next()
            if self.la().kind == '..':
                self.next()
                hi = self.next()
                return Range(t.text, hi.text)
            self._skip_tree_ops()
            return Literal(t.text, label)
        if t.kind == 'ID':
            self.next()
            args = None
            if self.la().kind == 'ARG':
                args = self.next().text
            self._skip_tree_ops()
            if t.text[0].isupper() or t.text == 'EOF':
                return TokenRef(t.text, label, args)
            return RuleRef(t.text, label, assign, args)
        if t.kind == '(':
            self.next()
            opts, init = {}, None
            if self.la().kind == 'ID' and self.la().text == 'options':
                self.next()
                opts = self.parse_options()
                self.expect(':')
            elif self.la().kind == 'ACTION' and self.la(2).kind == ':':
                init = self.next().text
                self.next()
            blk = self.parse_alts(')')
            blk.options, blk.init = opts, init
            self.expect(')')
            suffix = ''
            if self.la().kind in ('?', '*', '+', '=>'):
                suffix = self.next().kind
            self._skip_tree_ops()
            return SubRule(blk, suffix)
        raise SyntaxError("unexpected %r in alternative" % t)

    def _skip_tree_ops(self):
        while self.la().kind in ('!', '^'):
            self.next()


def parse(src):
    return Parser(src).parse_grammar()


# ---------------------------------------------------------------- walking


def walk(node):
    """Yields every element node under a Block/Alt/element (pre-order)."""
    if isinstance(node, Block):
        for a in node.alts:
            yield from walk(a)
    elif isinstance(node, Alt):
        for e in node.elems:
            yield e
            yield from walk(e)
    elif isinstance(node, SubRule):
        yield from walk(node.block)
    elif isinstance(node, Not):
        yield node.elem
        yield from walk(node.elem)
