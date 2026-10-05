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
    LA(k) == Token and opaque calls.

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


def load_base_functions(base_dir, consts, helpers):
    """Hand-written parser-base functions whose first parameter is a token or a token's text ->
    (words their bodies (and the base functions they call) compare it with: keyword constants,
    upper-case string literals and OptionsHelper mappings; binding). A function is binding when
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
            for h in re.findall(r'(\w+Helper)::Instance\(\)', body):
                words.update(helpers.get(h, []))
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


# ----------------------------------------------------------------------------------- predicates


class PredParser:
    """Predicate C++ expression -> s-expression string:
         m<k>:<WORD>;    NextTokenMatches(WORD, k)
         t<k>:<type>;    LA(k) == <token type name>
         !<e>  &<n><e>..  |<n><e>..   ?   (opaque: unknown at completion time)"""

    def __init__(self, text, consts):
        self.s = text
        self.i = 0
        self.consts = consts

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
        return '?'


def load_predicates(sources, consts):
    preds = {}
    for text in sources:
        for m in re.finditer(r'bool \w+::\w+Sempred\(\w+ \*_localctx, size_t predicateIndex\) \{', text):
            body = text[m.end() - 1:balanced(text, m.end() - 1)]
            for cm in re.finditer(r'case (\d+): return ', body):
                end = body.index(';\n', cm.end())
                expr = ' '.join(body[cm.end():end].split())
                preds[int(cm.group(1))] = PredParser(expr, consts).parse_or()
    return preds


# ------------------------------------------------------------------------------ keyword states

RULE_FN = re.compile(r'^\w+Parser::\w+Context\* \w+Parser::(\w+)\(', re.M)
ASSIGN_TOKEN = re.compile(r'setState\((\d+)\);\s*\n\s*antlrcpp::downCast<\w+ \*>\(_localctx\)->(\w+) = match\(\w+::(\w+)\);')
ASSIGN_RULE = re.compile(r'setState\((\d+)\);\s*\n\s*antlrcpp::downCast<\w+ \*>\(_localctx\)->(\w+) = \w+\(')
ASSIGN_VAR = re.compile(r'\b(\w+) = antlrcpp::downCast<\w+ \*>\(_localctx\)->(\w+)->vResult;')
CALL = re.compile(r'(?<![\w.>:])(?:(\w+Helper)::Instance\(\)\.)?(\w+)\(')
BINDING_CALLS = {'Match', 'ParseOption'}
WORD_CALLS = {'Match', 'TryMatch'}


def keyword_states(text, consts, helpers, funcs):
    """state -> [{word: mask}, binding]"""
    states = {}
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

        for m in CALL.finditer(body):
            helper, fn = m.group(1), m.group(2)
            args, _ = call_args(body, m.end() - 1)
            if not args:
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
                entry = states.setdefault(st, [{}, False])
                for w, f in words:
                    entry[0][w.upper()] = entry[0].get(w.upper(), 0) | f
                entry[1] = entry[1] or binding
    return states


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
    funcs = load_base_functions(a.base_dir, consts, helpers)
    sources = []
    for path in sorted(glob.glob(os.path.join(a.parser_dir, a.grammar + 'Parser*.cpp'))):
        with open(path, encoding='utf-8') as f:
            sources.append(f.read())
    preds = load_predicates(sources, consts)
    states = {}
    for text in sources:
        for st, (words, binding) in keyword_states(text, consts, helpers, funcs).items():
            entry = states.setdefault(st, [{}, False])
            for w, f in words.items():
                entry[0][w] = entry[0].get(w, 0) | f
            entry[1] = entry[1] or binding

    lines = ['// GENERATED by tools/g2to4/editor_meta.py from the %s parser. Do not edit.' % a.grammar,
             '// Keyword states: {ATN state, word, SqlVersionFlags mask, binding}; predicates: {index, expression}.',
             'static const ::tsql::editor::detail::KeywordStateEntry kKeywordStates[] = {']
    for st in sorted(states):
        words, binding = states[st]
        for w in sorted(words):
            lines.append('    {%d, %s, 0x%x, %s},' % (st, cstr(w), words[w], 'true' if binding else 'false'))
    lines.append('};')
    lines.append('static const ::tsql::editor::detail::PredicateEntry kPredicates[] = {')
    for i in sorted(preds):
        lines.append('    {%d, %s},' % (i, cstr(preds[i])))
    lines.append('};')
    out = '\n'.join(lines) + '\n'
    if not os.path.exists(a.out) or open(a.out, encoding='utf-8').read() != out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, 'w', encoding='utf-8') as f:
            f.write(out)
    print('%s: %d keyword states, %d predicates (%d opaque)' % (
        a.grammar, len(states), len(preds), sum(1 for p in preds.values() if p == '?')), file=sys.stderr)


if __name__ == '__main__':
    main()
