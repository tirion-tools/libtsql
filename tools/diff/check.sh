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
#   err-V: the scripts it reports errors for (<V>-err-list.txt); floor recorded below
#   hoa-V: each top-level statement of the ok scripts as its own file (heldout-<V>, hoa-<V>-oracle)
#   crash-V: inputs that once crashed tsql_dump (crashes/, tools/diff/mk_crashes.sh; crash-o-<V>)
# TSql170 also runs the pilot's gates (dumped with the NuGet oracle): select corpus (oracle-dumps,
#   select-rel.txt), heldout (ho-oracle), probes (probes-oracle), mut: broken SELECTs (mut-o),
#   bigdir + bigerr: 427 KB scripts without/with an error.
# Gates are strict (errors and trees identical); `mut`, `probes` and err-V report known gaps and
# only fail when they fall below their recorded floor.
set -u
P=${TSQL_PILOT_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/tsql-pilot}
HERE=$(cd "$(dirname "$0")" && pwd)
ALL=(TSql130 TSql140 TSql150 TSql160 TSql170 TSql180 TSqlFabricDW)
declare -A ERR_FLOOR=([TSql130]=246 [TSql140]=219 [TSql150]=177 [TSql160]=151 [TSql170]=119 [TSql180]=117 [TSqlFabricDW]=166)
versions=()
while [ $# -gt 0 ]; do
    case $1 in
        --version) [ "$2" = all ] && versions=("${ALL[@]}") || versions+=("$2"); shift 2 ;;
        *) break ;;
    esac
done
B=$(realpath -m "${1:-$P/build}")
CAP=(); [ -x "$P/bin/capped" ] && CAP=("$P/bin/capped")
if [ "$B" = "$P/build" ] && [ -x "$P/bin/build" ]; then
    "$P/bin/build" tsql_dump tsql_bench
else
    ninja -C "$B" tsql_dump tsql_bench
fi | grep -E 'error|FAILED' && exit 1
DUMP=$B/src/parser/tsql_dump
BENCH=$B/src/parser/tsql_bench
[ ${#versions[@]} -eq 0 ] && mapfile -t versions < <("$DUMP" --list-versions)
fail=0
gate() {  # name floor version oracle-dir ours-dir list root files...   (floor: minimum passes; "all" = every file)
    local name=$1 floor=$2 version=$3 oracle=$4 ours=$5 list=$6 root=$7; shift 7
    rm -rf "$ours"
    local rc=0
    "${CAP[@]}" "$DUMP" --version "$version" --root "$root" "$ours" "$@" || rc=$?
    if [ $rc -ge 128 ]; then   # a crash fails the gate whatever its floor
        fail=1
        echo "$name: tsql_dump died with exit $rc (tools/diff/crash_sweep.py finds such inputs)"
    fi
    local report passed total
    report=$("${CAP[@]}" python3 "$HERE/compare.py" --oracle "$oracle" --ours "$ours" --list "$list" --strict --show 5)
    passed=$(sed -n '1s/.*pass: \([0-9]*\).*/\1/p' <<<"$report")
    total=$(sed -n '1s/files: \([0-9]*\).*/\1/p' <<<"$report")
    [ "$floor" = all ] && floor=$total
    printf '%-17s %s (floor %s)\n' "$name" "$(head -1 <<<"$report")" "$floor"
    if [ "$passed" -lt "$floor" ]; then
        fail=1
        sed -n '4,30p' <<<"$report"
    fi
}
for v in "${versions[@]}"; do
    if [ "$v" = TSql170 ]; then
        gate select  all  $v "$P/oracle-dumps"  "$P/chk-select"  "$P/select-rel.txt"   "$P/ssd/Test/SqlDom" $(cat "$P/oracle-dumps/select-corpus.txt")
        gate heldout all  $v "$P/ho-oracle"     "$P/chk-heldout" "$P/ho-list.txt"      "$P/heldout" "$P"/heldout/*.sql
        gate big     all  $v "$P/big-o"         "$P/chk-big"     "$P/big-list.txt"     "$P/bigdir" "$P"/bigdir/*.sql
        gate bigerr  all  $v "$P/bigerr-o"      "$P/chk-bigerr"  "$P/bigerr-list.txt"  "$P/bigerr" "$P"/bigerr/*.sql
        gate probes  69   $v "$P/probes-oracle" "$P/chk-probes"  "$P/probes-list.txt"  "$P/probes" "$P"/probes/*.sql
        gate mut     1614 $v "$P/mut-o"         "$P/chk-mut"     "$P/mut-list.txt"     "$P/mut" "$P"/mut/*.sql
    fi
    gate "ok-$v"  all              $v "$P/oracle-$v" "$P/chk-ok-$v"  "$P/$v-ok-list.txt"  "$P/ssd/Test/SqlDom" $(sed "s#^#$P/ssd/Test/SqlDom/#" "$P/$v-ok-list.txt")
    gate "err-$v" "${ERR_FLOOR[$v]}" $v "$P/oracle-$v" "$P/chk-err-$v" "$P/$v-err-list.txt" "$P/ssd/Test/SqlDom" $(sed "s#^#$P/ssd/Test/SqlDom/#" "$P/$v-err-list.txt")
    gate "hoa-$v" all              $v "$P/hoa-$v-oracle" "$P/chk-hoa-$v" "$P/hoa-$v-list.txt" "$P/heldout-$v" "$P/heldout-$v"/*.sql
    gate "crash-$v" all            $v "$P/crash-o-$v" "$P/chk-crash-$v" "$P/crashes-list.txt" "$P/crashes" "$P"/crashes/*.sql
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
