#!/bin/bash
# Parser gates: rebuild, compare against the SqlScriptDOM oracle dumps, benchmark.
#   tools/diff/check.sh [--version TSql160|...|all] [build-dir]
#       --version: one grammar (repeatable); default all grammars the build contains
#       build-dir: default $P/build (built through $P/bin/build when that helper exists, else by
#       ninja); dumps, comparisons and benchmarks run under $P/bin/capped when it exists
# Expects the pilot workspace P = $TSQL_PILOT_DIR, default ${XDG_CACHE_HOME:-~/.cache}/tsql-pilot
# (each corpus next to its oracle dumps), made by
# tools/diff/setup-workspace.sh.
# Per version V (corpora from tools/diff/mk_heldout_all.py, oracle = source-built SqlScriptDOM
# @ eaf3a6e with `--parser V`, dumps in oracle-<V>):
#   ok-V:  every unique test script V's oracle parses without errors (<V>-ok-list.txt)
#   err-V: the scripts it reports errors for (<V>-err-list.txt)
#   hoa-V: each top-level statement of the ok scripts as its own file (heldout-<V>, hoa-<V>-oracle)
#   crash-V: inputs that once crashed tsql_dump or exhausted its memory, and probes of decisions
#     ANTLR 4 compiles inline (crashes/, tools/diff/mk_crashes.sh; crash-o-<V>)
# TSql170 also runs the pilot's gates (dumped with the NuGet oracle): select corpus (oracle-dumps,
#   select-rel.txt), heldout (ho-oracle), probes (probes-oracle), mut: broken SELECTs (mut-o),
#   bigdir + bigerr: 427 KB scripts without/with an error.
# Gates are strict (errors and trees identical). `mut` has a floor: its oracle is the NuGet package
# 180.117.0, built from a newer internal grammar than eaf3a6e (one script, a FOR TIMESTAMP AS OF hint,
# differs; the source-built oracle reports what we report).
set -u
P=${TSQL_PILOT_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/tsql-pilot}
HERE=$(cd "$(dirname "$0")" && pwd)
ALL=(TSql130 TSql140 TSql150 TSql160 TSql170 TSql180 TSqlFabricDW)
versions=()
while [ $# -gt 0 ]; do
    case $1 in
        --version) [ "$2" = all ] && versions=("${ALL[@]}") || versions+=("$2"); shift 2 ;;
        *) break ;;
    esac
done
B=$(realpath -m "${1:-$P/build}")
C=$B/chk   # this build's dumps, so checks of different build dirs can run at the same time
mkdir -p "$C"
CAP=(); [ -x "$P/bin/capped" ] && CAP=("$P/bin/capped")
# Per tsql_dump: a runaway aborts on its own allocation (normal peak < 200 MB). TSQL_DUMP_LIMIT
# replaces the command prefix; empty for sanitizer builds (ASan reserves terabytes of address space:
# use ASAN_OPTIONS=hard_rss_limit_mb=2048 instead).
read -ra LIMIT <<<"${TSQL_DUMP_LIMIT-prlimit --as=$((2 << 30))}"
BUILD=(ninja -C "$B"); [ -x "$P/bin/build" ] && BUILD=("$P/bin/build" -C "$B")
if ! log=$("${BUILD[@]}" tsql_dump tsql_bench 2>&1); then
    printf '%s\n' "$log" | grep -E 'error|FAILED' | head -20
    echo "build failed: no gates run"
    exit 1
fi
DUMP=$B/src/parser/tsql_dump
BENCH=$B/src/parser/tsql_bench
[ ${#versions[@]} -eq 0 ] && mapfile -t versions < <("$DUMP" --list-versions)
fail=0
gate() {  # name floor version oracle-dir ours-dir list root files...   (floor: minimum passes; "all" = every file)
    local name=$1 floor=$2 version=$3 oracle=$4 ours=$5 list=$6 root=$7; shift 7
    rm -rf "$ours"
    local rc=0
    "${CAP[@]}" "${LIMIT[@]}" "$DUMP" --version "$version" --root "$root" "$ours" "$@" || rc=$?
    if [ $rc -ge 128 ]; then   # a crash fails the gate whatever its floor
        fail=1
        echo "$name: tsql_dump died with exit $rc (tools/diff/crash_sweep.py finds such inputs)"
    fi
    local report passed total
    report=$("${CAP[@]}" python3 "$HERE/compare.py" --oracle "$oracle" --ours "$ours" --list "$list" --strict --show 5)
    passed=$(sed -n '1s/.*pass: \([0-9]*\).*/\1/p' <<<"$report")
    total=$(sed -n '1s/files: \([0-9]*\).*/\1/p' <<<"$report")
    if [ -z "$passed" ] || [ -z "$total" ]; then   # no report: the gate failed to run
        fail=1
        echo "$name: compare.py gave no report"
        return
    fi
    [ "$floor" = all ] && floor=$total
    printf '%-17s %s (floor %s)\n' "$name" "$(head -1 <<<"$report")" "$floor"
    if [ "$passed" -lt "$floor" ]; then
        fail=1
        sed -n '4,30p' <<<"$report"
    fi
}
for v in "${versions[@]}"; do
    if [ "$v" = TSql170 ]; then
        gate select  all  $v "$P/oracle-dumps"  "$C/select"  "$P/select-rel.txt"   "$P/ssd/Test/SqlDom" $(cat "$P/oracle-dumps/select-corpus.txt")
        gate heldout all  $v "$P/ho-oracle"     "$C/heldout" "$P/ho-list.txt"      "$P/heldout" "$P"/heldout/*.sql
        gate big     all  $v "$P/big-o"         "$C/big"     "$P/big-list.txt"     "$P/bigdir" "$P"/bigdir/*.sql
        gate bigerr  all  $v "$P/bigerr-o"      "$C/bigerr"  "$P/bigerr-list.txt"  "$P/bigerr" "$P"/bigerr/*.sql
        gate probes  all  $v "$P/probes-oracle" "$C/probes"  "$P/probes-list.txt"  "$P/probes" "$P"/probes/*.sql
        gate mut     1913 $v "$P/mut-o"         "$C/mut"     "$P/mut-list.txt"     "$P/mut" "$P"/mut/*.sql
    fi
    gate "ok-$v"  all              $v "$P/oracle-$v" "$C/ok-$v"  "$P/$v-ok-list.txt"  "$P/ssd/Test/SqlDom" $(sed "s#^#$P/ssd/Test/SqlDom/#" "$P/$v-ok-list.txt")
    gate "err-$v" all              $v "$P/oracle-$v" "$C/err-$v" "$P/$v-err-list.txt" "$P/ssd/Test/SqlDom" $(sed "s#^#$P/ssd/Test/SqlDom/#" "$P/$v-err-list.txt")
    gate "hoa-$v" all              $v "$P/hoa-$v-oracle" "$C/hoa-$v" "$P/hoa-$v-list.txt" "$P/heldout-$v" "$P/heldout-$v"/*.sql
    gate "crash-$v" all            $v "$P/crash-o-$v" "$C/crash-$v" "$P/crashes-list.txt" "$P/crashes" "$P"/crashes/*.sql
done
# Benchmarks: every version on its own ok scripts (bench-big has ~1,500 errors before TSql170,
# which makes it an error-recovery benchmark there); TSql170 also on the pilot's inputs.
for v in "${versions[@]}"; do
    printf '%-17s ' "okfiles-$v"
    "${CAP[@]}" "$BENCH" --version "$v" 3 $(sed "s#^#$P/ssd/Test/SqlDom/#" "$P/$v-ok-list.txt") | tail -1
    if [ "$v" = TSql170 ]; then
        for input in bench-big.sql bench-big-err.sql; do
            printf '%-17s ' "${input%.sql}"
            "${CAP[@]}" "$BENCH" --version "$v" 3 "$P/$input" | tail -1
        done
        printf '%-17s ' select
        "${CAP[@]}" "$BENCH" --version "$v" 3 $(cat "$P/oracle-dumps/select-corpus.txt") | tail -1
    fi
done
exit $fail
