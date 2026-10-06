#!/usr/bin/env python3
"""Compare tsql_dump output against SqlScriptDOM (oracle) dumps.

    compare.py --oracle DIR --ours DIR --list FILE [--strict] [--show N] [--json OUT]

FILE lists corpus-relative .sql paths (one per line). Each `<path>.json` in
both dump directories holds {"errors": [...], "tree": NODE|null}. A file
passes when the oracle reports no errors and the trees are equal, or when
both report errors; with --strict the whole dump, errors included, must be
identical. The first difference per file is reported as a JSON path.
"""
import argparse
import collections
import json
import os
import sys


def first_diff(a, b, path="$"):
    """Returns (path, oracle value, our value) of the first difference, or None."""
    if type(a) is not type(b):
        return path, a, b
    if isinstance(a, dict):
        if a.get("$type") != b.get("$type"):
            return path + ".$type", a.get("$type"), b.get("$type")
        for key in a:
            if key not in b:
                return f"{path}.{key}", a[key], "<missing>"
            d = first_diff(a[key], b[key], f"{path}.{key}")
            if d:
                return d
        for key in b:
            if key not in a:
                return f"{path}.{key}", "<missing>", b[key]
        return None
    if isinstance(a, list):
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, f"{path}[{i}]")
            if d:
                return d
        if len(a) != len(b):
            return f"{path}.length", len(a), len(b)
        return None
    return None if a == b else (path, a, b)


def short(value, limit=120):
    text = json.dumps(value, ensure_ascii=False)
    return text if len(text) <= limit else text[:limit] + "..."


def category(diff):
    """Coarse bucket for a difference: the last property name on its path."""
    path = diff[0]
    tail = path.rsplit(".", 1)[-1]
    return tail.split("[", 1)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oracle", required=True)
    ap.add_argument("--ours", required=True)
    ap.add_argument("--list", required=True)
    ap.add_argument("--show", type=int, default=15)
    ap.add_argument("--json")
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    files = [l.strip() for l in open(args.list, encoding="utf-8") if l.strip()]
    results = collections.OrderedDict()
    buckets = collections.Counter()
    for rel in files:
        oracle_path = os.path.join(args.oracle, rel + ".json")
        ours_path = os.path.join(args.ours, rel + ".json")
        oracle = json.load(open(oracle_path, encoding="utf-8"))
        if not os.path.exists(ours_path):
            results[rel] = ("missing", None)
            buckets["<no output>"] += 1
            continue
        try:
            ours = json.load(open(ours_path, encoding="utf-8"))
        except json.JSONDecodeError as error:
            results[rel] = ("bad-json", str(error))
            buckets["<bad json>"] += 1
            continue
        if args.strict:
            # Errors (number, position, message) and the tree must both be identical.
            diff = first_diff(oracle, ours, "$")
            results[rel] = ("pass", None) if diff is None else ("fail", diff)
            if diff:
                buckets[category(diff)] += 1
            continue
        if oracle["errors"]:
            ok = bool(ours["errors"])
            results[rel] = ("pass" if ok else "fail", None if ok else ("$.errors", "errors", "none"))
            continue
        if ours["errors"]:
            results[rel] = ("fail", ("$.errors", [], ours["errors"][:1]))
            buckets["<parse error>"] += 1
            continue
        diff = first_diff(oracle["tree"], ours["tree"], "$.tree")
        results[rel] = ("pass", None) if diff is None else ("fail", diff)
        if diff:
            buckets[category(diff)] += 1

    counts = collections.Counter(status for status, _ in results.values())
    total = len(files)
    print(f"files: {total}  pass: {counts['pass']}  fail: {counts['fail']}  "
          f"missing: {counts['missing']}  bad-json: {counts['bad-json']}")
    if total:
        print(f"pass rate: {100.0 * counts['pass'] / total:.1f}%")
    print("first-difference buckets:")
    for name, n in buckets.most_common(20):
        print(f"  {n:4d}  {name}")
    shown = 0
    for rel, (status, diff) in results.items():
        if status == "pass" or shown >= args.show:
            continue
        shown += 1
        if status in ("missing", "bad-json"):
            print(f"- {rel}: {status} {diff or ''}")
        else:
            path, a, b = diff
            print(f"- {rel}: {path}\n    oracle: {short(a)}\n    ours:   {short(b)}")
    if args.json:
        json.dump({rel: {"status": s, "diff": d} for rel, (s, d) in results.items()},
                  open(args.json, "w", encoding="utf-8"), indent=1, default=str)
    return 0 if counts["pass"] == total else 1


if __name__ == "__main__":
    sys.exit(main())
