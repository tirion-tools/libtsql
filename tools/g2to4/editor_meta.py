#!/usr/bin/env python3
"""Keyword metadata for the editor-support layer (src/editor), read from a generated parser.

SqlScriptDOM accepts most non-reserved keywords as Identifier tokens and checks their text in
actions (Match(tX, CodeGenerationSupporter::K), TryMatch, OptionsHelper ParseOption, parser-base
helpers such as ParseJoinOptimizerHint) or in predicates (NextTokenMatches(K)). An ATN-level
completion engine only sees "Identifier"; this script recovers which words each place accepts,
from the C++ the ANTLR tool generated for the grammar (setState(N) precedes every match and rule
call, so N is the ATN state of the element) and from the hand-written parser bases:

  - keyword states: ATN state of a token match (or rule call) -> words its action checks the
    matched token (or the returned identifier) against, and whether the action requires one of
    them (Match, ParseOption, a base function that throws otherwise) or only recognises them
    (TryMatch, TryParseOption, IsX, ParseDataType's user-defined-type fallback);
  - predicates: predicate index -> its condition as a small expression over NextTokenMatches,
    LA(k) == Token and opaque calls;
  - rule actions: per rule, how many actions it has and which of them can reject the input in a
    way the walk does not model (they throw a parse error or return from the rule early, other
    than the keyword check of the token right before them), read from the grammar (.g4) in text
    order, which is the order of their ATN states; and the actions that return from their rule
    when a condition the walk can evaluate holds (`if (NextTokenMatches(X)) return`), with it;
  - predicted decisions: the decisions the generated code predicts with adaptivePredict, where
    the runtime's emulation of ANTLR 2's decisions applies (the walk applies it there too).

Usage: editor_meta.py --grammar TSql170 --parser-dir <generated/TSql170> --support <generated/support>
                      --base-dir src/parser --out <file.inc>
"""
import argparse
import glob
import os
import re
import sys

ALL_FLAGS = 0xFFF

# ---------------------------------------------------------------------------------------- helpers


def balanced(text, start, open_ch='{', close_ch='}'):
    """Index just past the bracket group starting at text[start] == open_ch."""
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == '\\' else 1
        elif c == "'":
            i += 1
            while i < n and text[i] != "'":
                i += 2 if text[i] == '\\' else 1
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return n


def split_args(s):
    """Top-level comma split of an argument list (without the outer parentheses)."""
    out, depth, cur = [], 0, []
    for c in s:
        if c in '([{<' and c != '<':
            depth += 1
        elif c in ')]}':
            depth -= 1
        if c == ',' and depth == 0:
            out.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
    if ''.join(cur).strip():
        out.append(''.join(cur).strip())
    return out


def call_args(text, open_paren):
    end = balanced(text, open_paren, '(', ')')
    return split_args(text[open_paren + 1:end - 1]), end


def load_constants(support):
    consts = {}
    with open(os.path.join(support, 'CodeGenerationSupporter.h'), encoding='utf-8') as f:
        for m in re.finditer(r'static constexpr const char\* (\w+) = "((?:[^"\\]|\\.)*)";', f.read()):
            consts[m.group(1)] = m.group(2).encode().decode('unicode_escape')
    return consts


def load_flags(support):
    flags = {}
    with open(os.path.join(support, 'ParserEnums.h'), encoding='utf-8') as f:
        text = f.read()
    m = re.search(r'enum class SqlVersionFlags[^{]*\{(.*?)\};', text, re.S)
    for name, value in re.findall(r'(\w+) = (0x[0-9a-fA-F]+|\d+)', m.group(1)):
        flags[name] = int(value, 0)
    return flags


def word_of(arg, consts):
    arg = arg.strip()
    m = re.fullmatch(r'\(?CodeGenerationSupporter::(\w+)\)?', arg)
    if m:
        return consts.get(m.group(1))
    m = re.fullmatch(r'"([^"\\]*)"', arg)
    if m:
        return m.group(1)
    return None


def load_helpers(support, consts, flags):
    """OptionsHelper subclasses -> [(word, version mask)]."""
    helpers = {}
    with open(os.path.join(support, 'OptionHelpers.h'), encoding='utf-8') as f:
        text = f.read()
    for m in re.finditer(r'class (\w+) : public OptionsHelper<[^>]*> \{', text):
        body_start = text.index('{', m.end() - 1)
        body = text[body_start:balanced(text, body_start)]
        words = []
        for am in re.finditer(r'AddOptionMapping\(', body):
            args, _ = call_args(body, am.end() - 1)
            if len(args) < 2:
                continue
            w = word_of(args[1], consts)
            if w is None:
                continue
            mask = ALL_FLAGS
            if len(args) >= 3:
                fm = re.fullmatch(r'SqlVersionFlags::(\w+)', args[2].strip())
                mask = flags.get(fm.group(1), ALL_FLAGS) if fm else ALL_FLAGS
            words.append((w, mask))
        helpers[m.group(1)] = words
    return helpers


FUNC_HEAD = re.compile(r'(?:\b\w+::)?\b(\w+)\s*\(([^;{}()]*(?:\([^()]*\)[^;{}()]*)*)\)\s*(?:const\s*)?(?:override\s*)?\{')


def load_base_functions(base_dir, consts, helpers, flags):
    """Hand-written parser-base functions whose first parameter is a token or a token's text ->
    (words their bodies (and the base functions they call) compare it with: keyword constants,
    upper-case string literals and OptionsHelper mappings (only those of the version a
    TryParseOption(t, SqlVersionFlags::V) call names); binding). A function is binding when
    its own body throws and has no ::None fallback result."""
    direct, calls, binding = {}, {}, {}
    files = glob.glob(os.path.join(base_dir, '*.cpp')) + glob.glob(os.path.join(base_dir, '*.h'))
    files += glob.glob(os.path.join(base_dir, 'helpers', '*'))
    for path in sorted(files):
        with open(path, encoding='utf-8') as f:
            text = f.read()
        for m in FUNC_HEAD.finditer(text):
            name, params = m.group(1), m.group(2)
            if name in ('if', 'while', 'for', 'switch', 'catch', 'return'):
                continue
            first = params.split(',')[0]
            if not re.search(r'\bToken\*|\bCsStr\b|std::string&|\bstd::string_view\b', first):
                continue
            body = text[m.end() - 1:balanced(text, m.end() - 1)]
            words = set()
            for c in re.findall(r'CodeGenerationSupporter::(\w+)', body):
                if c in consts:
                    words.add((consts[c], ALL_FLAGS))
            for lit in re.findall(r'"([A-Z][A-Z0-9_]*)"', body):
                if not re.fullmatch(r'SQL\d+', lit):   # error numbers
                    words.add((lit, ALL_FLAGS))
            for hm in re.finditer(r'(\w+Helper)::Instance\(\)(\.(\w+)\()?', body):
                mappings = helpers.get(hm.group(1), [])
                args = call_args(body, hm.end() - 1)[0] if hm.group(2) else []
                vm = re.fullmatch(r'SqlVersionFlags::(\w+)', args[1].strip()) if len(args) >= 2 else None
                if vm and vm.group(1) in flags:
                    words.update((w, ALL_FLAGS) for w, f in mappings if f & flags[vm.group(1)])
                else:
                    words.update(mappings)
            direct.setdefault(name, set()).update(words)
            calls.setdefault(name, set()).update(re.findall(r'\b(\w+)\(', body))
            throws = re.search(r'\bthrow\b|\bThrow\w*Exception\(', body) is not None
            binding[name] = binding.get(name, False) or (throws and re.search(r'::None\b', body) is None)
    funcs = {}
    for name in direct:
        seen, todo, words = {name}, [name], set()
        while todo:
            n = todo.pop()
            words |= direct[n]
            for c in calls.get(n, ()):
                if c in direct and c not in seen:
                    seen.add(c)
                    todo.append(c)
        if words:
            funcs[name] = (words, binding.get(name, False))
    return funcs


def load_identifier_functions(base_dir, consts):
    """Hand-written parser-base functions taking ast::Identifier* parameters (ParseSecurityObjectKind(
    id1, id2, id3), ...) -> {(name, arity): ([(words, binding) per parameter], {word: enumerator})}:
    the words each identifier's text is compared with (Match / TryMatch(p, K), a switch on p->Value,
    Str_ToUpperInvariant(p->Value) == K, and the same in the functions it passes p to). A parameter
    with words is binding when its function (or one it passes p to) throws or calls Match and has
    no ::None fallback (the ParseSecurityObjectKind overloads: every path checks or throws). The
    second item maps each word of a one-identifier switch to the enumerator it returns."""
    direct, calls, binding, results = {}, {}, {}, {}
    files = glob.glob(os.path.join(base_dir, '*.cpp'))
    for path in sorted(files):
        with open(path, encoding='utf-8') as f:
            text = f.read()
        for m in FUNC_HEAD.finditer(text):
            name, params = m.group(1), m.group(2)
            if name in ('if', 'while', 'for', 'switch', 'catch', 'return') or 'ast::Identifier*' not in params:
                continue
            names = []
            for p in split_args(params):
                pm = re.fullmatch(r'\s*ast::Identifier\*\s*(\w+)\s*', p)
                names.append(pm.group(1) if pm else None)
            key = (name, len(names))
            body = text[m.end() - 1:balanced(text, m.end() - 1)]
            words = [set() for _ in names]
            for i, p in enumerate(names):
                if p is None:
                    continue
                for c in re.findall(r'\b(?:Try)?Match\(\s*%s\s*,\s*CodeGenerationSupporter::(\w+)\s*\)' % p, body):
                    if c in consts:
                        words[i].add(consts[c])
                for c in re.findall(r'Str_ToUpperInvariant\(\s*%s->Value\s*\)\s*==\s*CodeGenerationSupporter::(\w+)' % p, body):
                    if c in consts:
                        words[i].add(consts[c])
            # switches: const std::string sw_(...p->Value...); ... sw_ == CodeGenerationSupporter::K
            decls = list(re.finditer(r'\bsw_\(([^;]*)\);', body))
            for k, d in enumerate(decls):
                end = decls[k + 1].start() if k + 1 < len(decls) else len(body)
                for i, p in enumerate(names):
                    if p is not None and re.search(r'\b%s->Value\b' % p, d.group(1)):
                        for c in re.findall(r'\bsw_ == CodeGenerationSupporter::(\w+)', body[d.end():end]):
                            if c in consts:
                                words[i].add(consts[c])
            # one-identifier switches that return an enumerator per word: word -> enumerator
            if len(names) == 1:
                for c, e in re.findall(r'\bsw_ == CodeGenerationSupporter::(\w+)\)\s*\{\s*return ast::\w+::(\w+);', body):
                    if c in consts:
                        results.setdefault(key, {})[consts[c].upper()] = e
            links = []
            for cm in re.finditer(r'(?<![\w.>:])(\w+)\(', body):
                if cm.group(1) in ('if', 'while', 'for', 'switch', 'return', 'sw_'):
                    continue
                args, _ = call_args(body, cm.end() - 1)
                for j, arg in enumerate(args):
                    for i, p in enumerate(names):
                        if p is not None and arg.strip() == p:
                            links.append((i, (cm.group(1), len(args)), j))
            throws = re.search(r'\bthrow\b|\bThrow\w*Exception\(|\bMatch\(', body) is not None
            direct[key] = words
            calls[key] = links
            binding[key] = throws and re.search(r'::None\b', body) is None
    funcs = {}
    for key in direct:
        out = []
        for i in range(len(direct[key])):
            seen, todo, words, bind = {(key, i)}, [(key, i)], set(), binding[key]
            while todo:
                k, pi = todo.pop()
                words |= direct[k][pi]
                for src, callee, j in calls[k]:
                    if src == pi and callee in direct and (callee, j) not in seen:
                        seen.add((callee, j))
                        todo.append((callee, j))
                        bind = bind or binding[callee]
            out.append((words, bind and bool(words)))
        if any(w for w, _ in out):
            funcs[key] = (out, results.get(key, {}))
    return funcs


# ----------------------------------------------------------------------------------- predicates


class PredParser:
    """Predicate C++ expression -> s-expression string:
         m<k>:<WORD>;    NextTokenMatches(WORD, k)
         t<k>:<type>;    LA(k) == <token type name>
         b<k>:<WORD>;    the token k before the current one is WORD
         T               true
         !<e>  &<n><e>..  |<n><e>..
         $   opaque, reads the tokens only (a look-ahead scan, a syntactic predicate): the editor
             runs it on the tokens before the caret
         ?   opaque, reads the rule's locals: run only where the parser's own rule context is live"""

    def __init__(self, text, consts, rule=''):
        self.s = text
        self.i = 0
        self.consts = consts
        self.rule = rule

    def ws(self):
        while self.i < len(self.s) and self.s[self.i].isspace():
            self.i += 1

    def peek(self, tok):
        self.ws()
        return self.s.startswith(tok, self.i)

    def parse_or(self):
        items = [self.parse_and()]
        while self.peek('||'):
            self.i += 2
            items.append(self.parse_and())
        return items[0] if len(items) == 1 else '|%d%s' % (len(items), ''.join(items))

    def parse_and(self):
        items = [self.parse_unary()]
        while self.peek('&&'):
            self.i += 2
            items.append(self.parse_unary())
        return items[0] if len(items) == 1 else '&%d%s' % (len(items), ''.join(items))

    def parse_unary(self):
        self.ws()
        if self.peek('!') and not self.peek('!='):
            self.i += 1
            return '!' + self.parse_unary()
        if self.peek('('):
            # parenthesised boolean or a primary comparison like (LA(2) == X)
            save = self.i
            self.i += 1
            e = self.parse_or()
            if self.peek(')'):
                self.i += 1
                return e
            self.i = save
        return self.parse_primary()

    def parse_primary(self):
        """An operand up to the next top-level && / || / closing parenthesis."""
        self.ws()
        start = self.i
        depth = 0
        while self.i < len(self.s):
            if depth == 0 and (self.s.startswith('&&', self.i) or self.s.startswith('||', self.i)):
                break
            c = self.s[self.i]
            if c == '(':
                depth += 1
            elif c == ')':
                if depth == 0:
                    break
                depth -= 1
            self.i += 1
        return self.atom(self.s[start:self.i].strip())

    def atom(self, a):
        if self.rule == 'builtInFunctionCall':
            # builtInFunctionCall: nonQuotedIdentifier LeftParenthesis ( {FunctionName == X}? ... ): the
            # name is the token two before the alternatives
            if re.fullmatch(r'_localctx->vResult->FunctionName != nullptr', a):
                return 'T'
            m = re.fullmatch(r'Str_UpperEquals\(_localctx->vResult->FunctionName->Value, '
                             r'CodeGenerationSupporter::(\w+)\)', a)
            if m and m.group(1) in self.consts:
                return 'b2:%s;' % self.consts[m.group(1)].upper()
        m = re.fullmatch(r'NextTokenMatches\(\(?CodeGenerationSupporter::(\w+)\)?(?:,\s*(\d+))?\)', a)
        if m and m.group(1) in self.consts:
            return 'm%s:%s;' % (m.group(2) or '1', self.consts[m.group(1)].upper())
        m = re.fullmatch(r'\(?\s*Str_Equals\(LT\((\d+)\)->getText\(\), CodeGenerationSupporter::(\w+), '
                         r'StringComparison::OrdinalIgnoreCase\)\s*\)?', a)
        if m and m.group(2) in self.consts:
            return 'm%s:%s;' % (m.group(1), self.consts[m.group(2)].upper())
        m = re.fullmatch(r'\(?\s*LA\((\d+)\)\s*==\s*(?:\w+::)?(\w+)\s*\)?', a)
        if m:
            return 't%s:%s;' % (m.group(1), m.group(2))
        # the editor runs the whole predicate: it reads rule locals if any part does
        return '?' if '_localctx' in self.s else '$'


def load_predicates(sources, consts):
    preds = {}
    for text in sources:
        for m in re.finditer(r'bool \w+::(\w+)Sempred\(\w+ \*_localctx, size_t predicateIndex\) \{', text):
            body = text[m.end() - 1:balanced(text, m.end() - 1)]
            for cm in re.finditer(r'case (\d+): return ', body):
                end = body.index(';\n', cm.end())
                expr = ' '.join(body[cm.end():end].split())
                preds[int(cm.group(1))] = PredParser(expr, consts, m.group(1)).parse_or()
    return preds


def load_predicate_contexts(sources):
    """{rule index: context class} of the rules with predicates: the generated sempred() downcasts
    its context to the rule's class before it runs one of the rule's predicates."""
    contexts = {}
    for text in sources:
        for m in re.finditer(r'case (\d+): return \w+Sempred\(antlrcpp::downCast<(\w+) \*>\(context\), predicateIndex\);',
                             text):
            contexts[int(m.group(1))] = m.group(2)
    return contexts


# ------------------------------------------------------------------------------ keyword states

RULE_FN = re.compile(r'^\w+Parser::\w+Context\* \w+Parser::(\w+)\(', re.M)
ASSIGN_TOKEN = re.compile(r'setState\((\d+)\);\s*\n\s*antlrcpp::downCast<\w+ \*>\(_localctx\)->(\w+) = match\(\w+::(\w+)\);')
ASSIGN_RULE = re.compile(r'setState\((\d+)\);\s*\n\s*antlrcpp::downCast<\w+ \*>\(_localctx\)->(\w+) = \w+\(')
ASSIGN_VAR = re.compile(r'\b(\w+) = antlrcpp::downCast<\w+ \*>\(_localctx\)->(\w+)->vResult;')
CALL = re.compile(r'(?<![\w.>:])(?:(\w+Helper)::Instance\(\)\.)?(\w+)\(')
BINDING_CALLS = {'Match', 'ParseOption'}
WORD_CALLS = {'Match', 'TryMatch'}


ELEMENT = re.compile(r'setState\((\d+)\);\s*\n\s*(?:antlrcpp::downCast<\w+ \*>\(_localctx\)->\w+ = )?match\(\w+::\w+\);')


def keyword_states(text, consts, helpers, funcs, idfuncs):
    """-> (state -> [{word: mask}, binding], (state, guard state) -> {word: mask}).
    A guard: the action after the token matched at `state` requires the token matched earlier at
    `guard state` (in the same rule) to be one of the words (Match(tParameter, IMPORTANCE) after
    tImpValue=Identifier in workloadGroupParameter)."""
    states, guards = {}, {}
    starts = [m.start() for m in RULE_FN.finditer(text)] + [len(text)]
    for a, b in zip(starts, starts[1:]):
        body = text[a:b]
        # label -> [(position, state)] in text order: tokens matched into labels and
        # variables assigned from a rule call's vResult (the rule call's state)
        binds = {}
        for m in ASSIGN_TOKEN.finditer(body):
            binds.setdefault(m.group(2), []).append((m.end(), int(m.group(1))))
        rule_calls = {}
        for m in ASSIGN_RULE.finditer(body):
            rule_calls.setdefault(m.group(2), []).append((m.end(), int(m.group(1))))
        for m in ASSIGN_VAR.finditer(body):
            calls = [s for p, s in rule_calls.get(m.group(2), []) if p <= m.start()]
            if calls:
                binds.setdefault(m.group(1), []).append((m.end(), calls[-1]))
        if not binds:
            continue

        def state_of(label, pos):
            prior = [s for p, s in binds.get(label, []) if p <= pos]
            return prior[-1] if prior else None

        def add(st, words, binding):
            entry = states.setdefault(st, [{}, False])
            for w, f in words:
                entry[0][w.upper()] = entry[0].get(w.upper(), 0) | f
            entry[1] = entry[1] or binding

        # token matches in text order: (end of the statement, state)
        elements = [(m.end(), int(m.group(1))) for m in ELEMENT.finditer(body)]

        def element_before(pos):
            """The token match whose action runs the call at `pos` unconditionally (no other element,
            decision, if / else / switch / ?: or open brace between; outside a speculative parse, an
            action of a rule one runs is in `if (!Guessing()) {`, convert.py)."""
            prior = [(e, s) for e, s in elements if e <= pos]
            if not prior:
                return None
            between = re.sub(r'^\s*if \(!Guessing\(\)\) \{', '', body[prior[-1][0]:pos])
            if re.search(r'setState\(|\bif\b|\belse\b|\bswitch\b|\bcase\b|\?|\{|\}', between):
                return None
            return prior[-1][1]

        for m in CALL.finditer(body):
            helper, fn = m.group(1), m.group(2)
            args, _ = call_args(body, m.end() - 1)
            if not args:
                continue
            if helper is None and (fn, len(args)) in idfuncs:
                params, results = idfuncs[(fn, len(args))]
                # an action that rejects every result but one: vResult->set_X(fn(id)); if (!(vResult->X == E::K)) throw
                only = None
                sm = re.search(r'set_(\w+)\($', body[max(0, m.start() - 80):m.start()])
                if sm and results:
                    action_end = body.find('setState(', m.end())
                    om = re.search(r'if\s*\(\s*!\s*\(\s*\w+->%s\s*==\s*ast::\w+::(\w+)\s*\)\s*\)' % sm.group(1),
                                   body[m.end():action_end if action_end >= 0 else len(body)])
                    if om:
                        only = {w for w, e in results.items() if e == om.group(1)}
                # each identifier argument: the words its parameter is compared with
                for arg, (words, binding) in zip(args, params):
                    am = re.fullmatch(r'\$?(\w+)', arg.strip())
                    st = state_of(am.group(1), m.start()) if am else None
                    if only is not None:
                        words = {w for w in words if w.upper() in only}
                    if st is not None and words:
                        add(st, [(w, ALL_FLAGS) for w in words], binding)
                continue
            first = args[0].strip()
            lm = re.fullmatch(r'\$?(\w+)(?:->[\w()>-]*)?', first)
            if not lm or lm.group(1) not in binds:
                continue
            label = lm.group(1)
            st = state_of(label, m.start())
            if st is None:
                continue
            words = []
            binding = fn in BINDING_CALLS
            if helper is not None:
                if fn in ('ParseOption', 'TryParseOption'):
                    mask = ALL_FLAGS
                    if len(args) >= 2:
                        vm = re.fullmatch(r'SqlVersionFlags::(\w+)', args[1].strip())
                        if vm:
                            mask = None   # filtered by the mapping's own flags
                    words = [(w, f if mask is None else ALL_FLAGS) for w, f in helpers.get(helper, [])]
            elif fn in WORD_CALLS:
                for arg in args[1:]:
                    w = word_of(arg, consts)
                    if w is not None:
                        words.append((w, ALL_FLAGS))
            elif fn in funcs:
                words, binding = list(funcs[fn][0]), funcs[fn][1]
            if words:
                add(st, words, binding)
                owner = element_before(m.start())
                if binding and owner is not None and owner != st:
                    g = guards.setdefault((owner, st), {})
                    for w, f in words:
                        g[w.upper()] = g.get(w.upper(), 0) | f
    return states, guards


# --------------------------------------------------------------------------- rejecting actions

# a throw that rejects the input (not the argument checks ported from C#)
THROW = re.compile(r'\bthrow\b(?!\s+(?:NullReferenceException|std::invalid_argument)\b)')
# the keyword checks of a token the walk applies itself (keyword states)
OWN_CHECKS = {'Match', 'TryMatch', 'ParseOption', 'TryParseOption'}


def skip_code(text, i):
    """If text[i] starts a comment, string or character literal, the index just past it, else i."""
    if text.startswith('//', i):
        end = text.find('\n', i)
        return len(text) if end < 0 else end
    if text.startswith('/*', i):
        end = text.find('*/', i + 2)
        return len(text) if end < 0 else end + 2
    if text[i] in '"\'':
        q, j = text[i], i + 1
        while j < len(text) and text[j] != q:
            j += 2 if text[j] == '\\' else 1
        return j + 1
    return i


def code_group(text, start, open_ch, close_ch):
    """Index just past the bracket group starting at text[start] == open_ch, skipping comments and
    literals (an apostrophe in a comment is not a character literal)."""
    depth, i = 0, start
    while i < len(text):
        j = skip_code(text, i)
        if j != i:
            i = j
            continue
        if text[i] == open_ch:
            depth += 1
        elif text[i] == close_ch:
            depth -= 1
            if depth == 0:
                return i + 1
        i += 1
    return len(text)


def strip_code(text):
    """`text` without comments and literals."""
    out, i = [], 0
    while i < len(text):
        j = skip_code(text, i)
        if j != i:
            out.append(' ')
            i = j
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


def load_throwing_functions(base_dir):
    """Names of the hand-written parser-base functions that can reject the input: their body, or a
    function they call, throws a parse error."""
    bodies = {}
    files = glob.glob(os.path.join(base_dir, '*.cpp')) + glob.glob(os.path.join(base_dir, '*.h'))
    files += glob.glob(os.path.join(base_dir, 'helpers', '*'))
    for path in sorted(files):
        with open(path, encoding='utf-8') as f:
            text = f.read()
        for m in FUNC_HEAD.finditer(text):
            if m.group(1) in ('if', 'while', 'for', 'switch', 'catch', 'return'):
                continue
            bodies.setdefault(m.group(1), []).append(strip_code(text[m.end() - 1:balanced(text, m.end() - 1)]))
    throwing = {n for n, bs in bodies.items() if any(THROW.search(b) for b in bs)}
    changed = True
    while changed:
        changed = False
        for n, bs in bodies.items():
            if n not in throwing and any(set(re.findall(r'\b(\w+)\(', b)) & throwing for b in bs):
                throwing.add(n)
                changed = True
    return throwing


def grammar_actions(g4):
    """Rule name -> [(action code, label of the token matched right before it or None)], the
    rule's actions ({...} but not predicates {...}?) in text order, which is the order of their
    ATN states (the ATN builder numbers elements and actions as it meets them)."""
    rules = {}
    heads = [(m.start(), m.group(1)) for m in re.finditer(r'^([a-z]\w*)\b', g4, re.M)
             if m.group(1) not in ('locals', 'returns', 'options', 'throws', 'parser', 'tokens', 'import')]
    heads.append((len(g4), None))
    for (a, name), (b, _) in zip(heads, heads[1:]):
        body = g4[a:b]
        i, colon = 0, None
        while i < len(body) and colon is None:   # the header: [args] returns [...] locals [...] @init {...}
            c = body[i]
            if c == '[':
                i = code_group(body, i, '[', ']')
            elif c == '{':
                i = code_group(body, i, '{', '}')
            elif c == ':':
                colon = i
            else:
                i += 1
        if colon is None:
            continue
        actions, i = [], colon + 1
        while i < len(body):
            j = skip_code(body, i)
            if j != i:
                i = j
            elif body[i] == '[':
                i = code_group(body, i, '[', ']')
            elif body[i] == '{':
                end = code_group(body, i, '{', '}')
                if not body.startswith('?', end):
                    lm = re.search(r'\b(\w+)=([A-Z]\w*)\s*$', body[:i])
                    actions.append((body[i:end], lm.group(1) if lm else None))
                i = end
            else:
                i += 1
        rules[name] = actions
    return rules


# an action of a rule a syntactic predicate's speculative parse runs (convert.py): it runs outside
# the speculation only, where the walk always is
GUESSING_GUARD = re.compile(r'\s*\{\s*if\s*\(\s*!\s*Guessing\(\)\s*\)\s*(\{.*\})\s*\}\s*', re.S)


def return_condition(action, consts):
    """The condition of an action that only returns from its rule when it holds
    (`if (C) { return _localctx; }`), as a predicate expression (PredParser); else None."""
    code = strip_code(action)
    guarded = GUESSING_GUARD.fullmatch(code)
    if guarded:
        code = guarded.group(1)
    m = re.fullmatch(r'\s*\{\s*if\s*\((.*)\)\s*\{?\s*return\s+_localctx\s*;\s*\}?\s*\}\s*', code, re.S)
    return PredParser(' '.join(m.group(1).split()), consts).parse_or() if m else None


def rejecting(action, own, throwing, funcs):
    """Whether an action can reject the input in a way the walk does not model: it throws or
    returns from the rule, or calls a function that can throw, other than the keyword checks of
    `own` (the token matched right before it, which keyword states model)."""
    code = strip_code(action)
    # lambdas' bodies: their return statements do not leave the rule
    while True:
        m = re.search(r'\[[&=]?\]\s*\([^()]*\)\s*(?:->\s*[\w:<>*&\s]+?)?\s*\{', code)
        if not m:
            break
        code = code[:m.start()] + ' ' + code[balanced(code, m.end() - 1):]
    if re.search(r'\bthrow\b|\breturn\b', code):
        return True
    for cm in re.finditer(r'(?<![\w>:])(\w+)\(', code):
        fn = cm.group(1)
        if fn not in throwing:
            continue
        args, _ = call_args(code, cm.end() - 1)
        if own is not None and args and re.fullmatch(r'\$?%s' % own, args[0].strip()) and \
                (fn in OWN_CHECKS or fn in funcs):
            continue
        return True
    return False


# ------------------------------------------------------------------------------------- output


def cstr(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--grammar', required=True)
    ap.add_argument('--parser-dir', required=True)
    ap.add_argument('--support', required=True)
    ap.add_argument('--base-dir', required=True)
    ap.add_argument('--out', required=True)
    a = ap.parse_args()

    consts = load_constants(a.support)
    flags = load_flags(a.support)
    helpers = load_helpers(a.support, consts, flags)
    funcs = load_base_functions(a.base_dir, consts, helpers, flags)
    idfuncs = load_identifier_functions(a.base_dir, consts)
    sources = []
    for path in sorted(glob.glob(os.path.join(a.parser_dir, a.grammar + 'Parser*.cpp'))):
        with open(path, encoding='utf-8') as f:
            sources.append(f.read())
    preds = load_predicates(sources, consts)
    contexts = load_predicate_contexts(sources)
    states, guards = {}, {}
    for text in sources:
        found, found_guards = keyword_states(text, consts, helpers, funcs, idfuncs)
        for st, (words, binding) in found.items():
            entry = states.setdefault(st, [{}, False])
            for w, f in words.items():
                entry[0][w] = entry[0].get(w, 0) | f
            entry[1] = entry[1] or binding
        for key, words in found_guards.items():
            entry = guards.setdefault(key, {})
            for w, f in words.items():
                entry[w] = entry.get(w, 0) | f
    throwing = load_throwing_functions(a.base_dir)
    with open(os.path.join(a.parser_dir, 'grammar', a.grammar + 'Parser.g4'), encoding='utf-8') as f:
        actions = grammar_actions(f.read())
    rule_actions, returning = {}, []
    for rule, acts in actions.items():
        if not acts:
            continue
        rejects = []
        for k, (code, own) in enumerate(acts):
            cond = return_condition(code, consts)
            if cond is not None and '?' not in cond and '$' not in cond:
                returning.append((rule, k, cond))   # the walk takes both ways by the condition
            elif rejecting(code, own, throwing, funcs):
                rejects.append(k)
        rule_actions[rule] = (len(acts), rejects)

    # the decisions the generated code predicts with adaptivePredict (the others it decides with
    # LA(1) inline), where the runtime applies ANTLR 2's tests
    predicted = set()
    for text in sources:
        predicted.update(int(d) for d in re.findall(r'adaptivePredict\(_input, (\d+), _ctx\)', text))

    lines = ['// GENERATED by tools/g2to4/editor_meta.py from the %s parser. Do not edit.' % a.grammar,
             '// Keyword states: {ATN state, word, SqlVersionFlags mask, binding}; guards: {ATN state, guard',
             '// state, word, mask}; predicates: {index, expression}; rule actions: {rule, number of',
             '// actions, ordinals of those that can reject the input}; returning actions: {rule, ordinal,',
             '// condition under which it returns from the rule}; predicted decisions: the decisions the',
             '// generated code predicts with adaptivePredict; NewPredicateContext: a context of the class',
             '// the parser creates for a rule with predicates (its predicates run with it).',
             'static const ::tsql::editor::detail::KeywordStateEntry kKeywordStates[] = {']
    for st in sorted(states):
        words, binding = states[st]
        for w in sorted(words):
            lines.append('    {%d, %s, 0x%x, %s},' % (st, cstr(w), words[w], 'true' if binding else 'false'))
    lines.append('};')
    lines.append('static const ::tsql::editor::detail::KeywordGuardEntry kKeywordGuards[] = {')
    for st, guard in sorted(guards):
        words = guards[(st, guard)]
        for w in sorted(words):
            lines.append('    {%d, %d, %s, 0x%x},' % (st, guard, cstr(w), words[w]))
    lines.append('    {-1, -1, "", 0},   // end (keeps the array non-empty)')
    lines.append('};')
    lines.append('static const ::tsql::editor::detail::PredicateEntry kPredicates[] = {')
    for i in sorted(preds):
        lines.append('    {%d, %s},' % (i, cstr(preds[i])))
    lines.append('};')
    lines.append('static const ::tsql::editor::detail::RuleActionsEntry kRuleActions[] = {')
    for rule in sorted(rule_actions):
        count, rejects = rule_actions[rule]
        lines.append('    {%s, %d, %s},' % (cstr(rule), count, cstr(' '.join(str(k) for k in rejects))))
    lines.append('    {nullptr, 0, nullptr},   // end')
    lines.append('};')
    lines.append('static const ::tsql::editor::detail::ReturningActionEntry kReturningActions[] = {')
    for rule, k, cond in sorted(returning):
        lines.append('    {%s, %d, %s},' % (cstr(rule), k, cstr(cond)))
    lines.append('    {nullptr, 0, nullptr},   // end')
    lines.append('};')
    lines.append('static const int kPredictedDecisions[] = {')
    for d in sorted(predicted):
        lines.append('    %d,' % d)
    lines.append('    -1,   // end')
    lines.append('};')
    lines.append('std::unique_ptr<::antlr4::ParserRuleContext> NewPredicateContext(size_t rule) {')
    lines.append('    using P = ::tsql::parser::%sParser;' % a.grammar)
    lines.append('    switch (rule) {')
    for rule in sorted(contexts):
        lines.append('    case %d: return std::make_unique<P::%s>(nullptr, ::antlr4::atn::ATNState::INVALID_STATE_NUMBER);'
                     % (rule, contexts[rule]))
    lines.append('    default: return nullptr;')
    lines.append('    }')
    lines.append('}')
    out = '\n'.join(lines) + '\n'
    if not os.path.exists(a.out) or open(a.out, encoding='utf-8').read() != out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
            f.write(out)
    print('%s: %d keyword states, %d guards, %d predicates (%d opaque), %d of %d actions can reject' % (
        a.grammar, len(states), len(guards), len(preds), sum(1 for p in preds.values() if '?' in p or '$' in p),
        sum(len(r) for _, r in rule_actions.values()), sum(c for c, _ in rule_actions.values())),
        file=sys.stderr)


if __name__ == '__main__':
    main()
