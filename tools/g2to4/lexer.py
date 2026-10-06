"""ANTLR 2 lexer (class <G>LexerInternal) -> ANTLR 4 lexer grammar (C++ target)."""
import os
import re

from antlr2 import Action, Literal, Not, Range, RuleRef, SemPred, SubRule, TokenRef, Wildcard

# C# lexer action idioms -> C++ (applied in order)
ACTION_REWRITES = [
    (r'\{\s*beginComplexToken\(\);\s*\}', ''),
    (r'\bbeginComplexToken\(\);', ''),
    # premature EOF inside complex tokens is reported by the EOF alternatives of the overridden
    # rules (see overrides/common/lexer_rules), offsets/lines are computed after lexing
    (r'\bcheckEOF\(TokenKind\.\w+\);', ''),
    (r'\bnewline\(\);', ''),
    (r'\$setType\((\w+)\)', r'setType(\1)'),
    (r'\btext\.ToString\(\)', 'getText()'),
    (r'\btext\.Length\b', 'Utf16Len(getText())'),
    (r'\bString\.Equals\(', 'String_Equals('),
    (r'\bStringComparison\.(\w+)', r'StringComparison::\1'),
    (r'\bCodeGenerationSupporter\.(\w+)', r'CodeGenerationSupporter::\1'),
    (r'\bCurrentOffset\b(?!\()', 'CurrentOffset()'),
    (r'\bLA\((\d)\)', r'_input->LA(\1)'),
]


class LexReport(dict):
    pass


def char4(lit):
    """ANTLR 2 char/string literal -> ANTLR 4 literal."""
    body = lit[1:-1]
    body = body.replace('\\"', '"')
    out, i = '', 0
    while i < len(body):
        c = body[i]
        if c == '\\':
            out += body[i:i + 2] if body[i + 1] != 'u' else body[i:i + 6]
            i += 2 if body[i + 1] != 'u' else 6
            continue
        out += "\\'" if c == "'" else c
        i += 1
    return "'" + out + "'"


class LexConverter:
    def __init__(self, conv, override_dirs):
        """override_dirs: most specific first (overrides/<G>/lexer_rules, overrides/common/lexer_rules)."""
        self.conv = conv
        self.L = conv.L
        self.vocab = conv.vocab_names
        self.overrides = {}
        for override_dir in reversed(override_dirs):
            if os.path.isdir(override_dir):
                for f in sorted(os.listdir(override_dir)):
                    if f.endswith('.g4'):
                        self.overrides[f[:-3]] = open(os.path.join(override_dir, f), encoding='utf-8').read().rstrip('\n')
        self.renames = {}
        for r in self.L.rules:
            if r.protected and r.name in self.vocab and not self.is_empty(r):
                self.renames[r.name] = 'F_' + r.name
        self.needs_hand = {}
        self.actions_total = 0
        self.actions_auto = 0

    @staticmethod
    def is_empty(r):
        return all(not a.elems for a in r.block.alts)

    def name(self, n):
        return self.renames.get(n, n)

    def action(self, text, rule):
        self.actions_total += 1
        t = text
        for pat, rep in ACTION_REWRITES:
            t = re.sub(pat, rep, t)
        self.actions_auto += 1
        return t.strip()

    def block(self, blk, rule):
        return ' | '.join(self.alt(a, rule) for a in blk.alts)

    def alt(self, a, rule):
        parts = []
        for e in a.elems:
            s = self.elem(e, rule)
            if s:
                parts.append(s)
        return ' '.join(parts)

    def elem(self, e, rule):
        if isinstance(e, Literal):
            return char4(e.text)
        if isinstance(e, Range):
            lo, hi = char4(e.lo), char4(e.hi)
            if hi in ("'\\ufffe'", "'\\uffff'") and lo <= "'\\ud800'":
                # .NET lexes UTF-16 code units: a supplementary character is two surrogates, both
                # inside this range. The C++ runtime sees code points, so add the planes 1-16.
                self.conv.stats['supplementary_ranges_added'] = self.conv.stats.get('supplementary_ranges_added', 0) + 1
                return "(%s..%s | '\\u{10000}'..'\\u{10FFFF}')" % (lo, hi)
            return '%s..%s' % (lo, hi)
        if isinstance(e, Not):
            return '~' + self.elem(e.elem, rule)
        if isinstance(e, (TokenRef, RuleRef)):
            return self.name(e.name)
        if isinstance(e, Wildcard):
            return '.'
        if isinstance(e, Action):
            t = self.action(e.text, rule)
            return '{%s}' % t if t else ''
        if isinstance(e, SemPred):
            self.actions_total += 1
            t = e.text
            if re.fullmatch(r'\s*LA\(1\)\s*!=\s*EOF_CHAR\s*', t):
                # ~(...) never matches EOF in ANTLR 4
                self.actions_auto += 1
                return ''
            for pat, rep in ACTION_REWRITES:
                t = re.sub(pat, rep, t)
            self.actions_auto += 1
            return '{%s}?' % t.strip()
        if isinstance(e, SubRule):
            if e.suffix == '=>':
                self.needs_hand.setdefault(rule.name, []).append('syntactic predicate')
                return ''
            if e.block.init:
                self.needs_hand.setdefault(rule.name, []).append('subrule init')
            suffix = e.suffix
            if e.block.options.get('greedy') == 'false':
                suffix += '?'
            return '(%s)%s' % (self.block(e.block, rule), suffix)
        raise SystemExit('lexer: unhandled element %r in %s' % (e, rule.name))

    def convert(self):
        c = self.conv
        out = ["// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: %s (class %s)\n" % (c.grammar_rel, self.L.name) +
               "// GENERATED by tools/g2to4/convert.py. Do not edit; edit the converter or tools/g2to4/overrides.\n",
               'lexer grammar %s;\n' % c.lexer_name,
               'options { caseInsensitive = true; tokenVocab = TSqlTokens; superClass = TSqlLexerBase; }\n',
               '@lexer::header {\n#include "TSqlLexerBase.h"\n}\n',
               '@lexer::postinclude {\nusing namespace tsql::parser;\n}\n']
        rules = {}
        dropped = []
        for r in self.L.rules:
            if r.protected and self.is_empty(r):
                dropped.append(r.name)   # token-type placeholder; the type comes from TSqlTokens
                continue
            if r.name in self.overrides:
                out.append(self.overrides[r.name] + '\n')
                rules[r.name] = 'override'
                continue
            if r.init:
                self.needs_hand.setdefault(r.name, []).append('rule init block')
            body = self.block(r.block, r)
            if r.options.get('testLiterals') == 'true':
                body = '(%s) {TestLiterals();}' % body
            head = ('fragment ' if r.protected else '') + self.name(r.name)
            out.append('%s\n    : %s\n    ;\n' % (head, body))
            rules[r.name] = 'mechanical'
        for n in self.overrides:
            if n not in rules:
                raise SystemExit('lexer override for unknown rule ' + n)
        missing = {n: why for n, why in self.needs_hand.items() if rules.get(n) != 'override'}
        if missing:
            raise SystemExit('lexer rules need hand overrides: %s' % missing)
        hand_lines = sum(len([ln for ln in self.overrides[n].splitlines() if ln.strip() and not ln.strip().startswith('//')])
                         for n in self.overrides)
        src_lines = sum(r.src_lines for r in self.L.rules if r.name in self.overrides)
        rep = LexReport(rules_total=len(self.L.rules), rules_dropped_placeholders=len(dropped),
                        rules_mechanical=sum(1 for v in rules.values() if v == 'mechanical'),
                        rules_overridden=sorted(n for n, v in rules.items() if v == 'override'),
                        override_hand_lines=hand_lines, overridden_source_lines=src_lines,
                        renamed_fragments=self.renames, actions_total=self.actions_total,
                        actions_auto=self.actions_auto)
        self.text = '\n'.join(out)
        self.report = rep
        return self


def convert_lexer(conv, override_dirs):
    return LexConverter(conv, override_dirs).convert()
