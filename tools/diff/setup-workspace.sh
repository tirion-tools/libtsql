#!/bin/bash
# Rebuilds the parser-gate workspace P that tools/diff/check.sh reads, from nothing (P =
# $TSQL_PILOT_DIR, default ${XDG_CACHE_HOME:-~/.cache}/tsql-pilot; not /tmp, which a reboot wipes):
#   tools/diff/setup-workspace.sh [--force STEP]...
# Idempotent: each step leaves a stamp in $P/.setup and is skipped while its stamp is
# newer than the stamps and files it depends on; --force STEP (repeatable) redoes STEP and what
# depends on it. Needs dotnet (SDK 10), java, PowerShell 7 (pwsh or pwsh-preview), python3, curl.
#
# Steps:
#   ssd           SqlScriptDOM @ the commit pinned in cmake/TsqlParserPilot.cmake, copied from the
#                 build's FetchContent source when present, else fetched and checked by SHA-256
#   antlr2        antlr-2.7.5.jar (SHA-256 pinned), which ScriptDom's own build would download unchecked
#   ssd-build     ScriptDom built from that source (net8.0, run on the newer runtime: DOTNET_ROLL_FORWARD
#                 =Major; its build calls `pwsh`, provided by the shim dir $P/shim)
#   oracle-nuget  tools/oracle on NuGet ScriptDom 180.117.0 -> build-Oracle (TSql170 pilot gates)
#   oracle-src    tools/oracle on the source-built ScriptDom -> build-OracleSrc (per-version gates)
#   oracle-dumps  NuGet oracle over ssd/Test/SqlDom + classify (index.json, select-corpus.txt)
#   pilot-corpora tools/diff/mk_pilot_corpora.py (select-rel, heldout, bench-big*, bigdir, bigerr, mut)
#   probes        tools/diff/mk_probes.sh
#   pilot-dumps   NuGet oracle over heldout, bigdir, bigerr, probes, mut
#   oracle-<V>    source oracle --parser V over ssd/Test/SqlDom + classify, for each version V
#   heldout-<V>   tools/diff/mk_heldout_all.py --version V (lists, heldout-V, hoa-V-oracle)
#   crashes       tools/diff/mk_crashes.sh (former tsql_dump crashes) -> crashes/, crashes-list.txt
#   crash-<V>     source oracle --parser V over crashes -> crash-o-<V>
# Builds hold $P/build.lock (shared with tools/diff/check.sh and the CMake build helper)
# and every heavy step runs in the memory-capped slice when $P/bin/capped exists.
set -euo pipefail
P=${TSQL_PILOT_DIR:-${XDG_CACHE_HOME:-$HOME/.cache}/tsql-pilot}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
STAMPS=$P/.setup
VERSIONS=(TSql130 TSql140 TSql150 TSql160 TSql170 TSql180 TSqlFabricDW)
ANTLR2_URL=https://www.antlr2.org/download/antlr-2.7.5.jar
ANTLR2_SHA256=2433e7e36ebbebe72390036ec555f4c6771eaed33d507b3d5d65497804093a0d   # = Maven Central antlr:antlr:2.7.5
JOBS=6   # oracle dump threads

pin() { sed -n "s/^ *$1[ =]\([0-9a-f]\{40,64\}\).*/\1/p" "$ROOT/cmake/TsqlParserPilot.cmake" | head -1; }
SSD_COMMIT=$(pin 'set(TSQL_SSD_COMMIT')
SSD_SHA256=$(sed -n '/FetchContent_Declare(tsql_sqlscriptdom/,/)/s/.*URL_HASH SHA256=\([0-9a-f]*\).*/\1/p' "$ROOT/cmake/TsqlParserPilot.cmake")
[ -n "$SSD_COMMIT" ] && [ -n "$SSD_SHA256" ] || { echo "cannot read the SqlScriptDOM pin from cmake/TsqlParserPilot.cmake" >&2; exit 1; }

force=()
while [ $# -gt 0 ]; do
    case $1 in
        --force) force+=("$2"); shift 2 ;;
        *) echo "usage: $0 [--force STEP]..." >&2; exit 64 ;;
    esac
done

mkdir -p "$P" "$STAMPS"
CAP=(); [ -x "$P/bin/capped" ] && CAP=("$P/bin/capped")
for f in "${force[@]}"; do rm -f "$STAMPS/$f"; done

# step NAME DEP... -- COMMAND...: DEP is another step's name or a file path (contains '/').
step() {
    local name=$1 dep ref stale=0; shift
    local deps=()
    while [ "$1" != -- ]; do deps+=("$1"); shift; done
    shift
    if [ -e "$STAMPS/$name" ]; then
        for dep in "${deps[@]}"; do
            case $dep in */*) ref=$dep ;; *) ref=$STAMPS/$dep ;; esac
            if [ ! -e "$ref" ] || [ "$ref" -nt "$STAMPS/$name" ]; then stale=1; fi
        done
        if [ $stale = 0 ]; then echo "-- $name: up to date"; return; fi
    fi
    rm -f "$STAMPS/$name"
    echo "== $name"
    local t0=$SECONDS
    "$@"
    touch "$STAMPS/$name"
    echo "   $name: $((SECONDS - t0)) s"
}

# dotnet DIR ARGS...: one MSBuild run, holding the build lock, capped, leaving no server behind.
dotnet_build() {
    local dir=$1; shift
    (cd "$dir" && env DOTNET_CLI_USE_MSBUILD_SERVER=0 DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1 \
        DOTNET_SKIP_FIRST_TIME_EXPERIENCE=1 MSBUILDDISABLENODEREUSE=1 \
        flock "$P/build.lock" "${CAP[@]}" dotnet build "$@" \
        -nodeReuse:false -p:UseSharedCompilation=false -maxcpucount:4 -v:minimal -clp:ErrorsOnly)
}

ORACLE_NUGET=$P/build-Oracle/bin/Release/net10.0/oracle
ORACLE_SRC=$P/build-OracleSrc/bin/Release/net10.0/oracle
SSD_DLL=$P/ssd-build/out/Release/net8.0/Microsoft.SqlServer.TransactSql.ScriptDom.dll

do_ssd() {
    rm -rf "$P/ssd" "$P/ssd.tmp"
    local src=$P/build/_deps/tsql_sqlscriptdom-src sub=$P/build/_deps/tsql_sqlscriptdom-subbuild/CMakeLists.txt
    if [ -f "$src/SqlScriptDom/Parser/TSql/Ast.xml" ] && grep -q "$SSD_COMMIT" "$sub" 2>/dev/null &&
       grep -q "$SSD_SHA256" "$sub"; then
        echo "   copying $src"
        cp -a "$src" "$P/ssd.tmp"
    else
        echo "   fetching SqlScriptDOM $SSD_COMMIT"
        mkdir "$P/ssd.tmp"
        curl -fsSL -o "$P/ssd.tar.gz" "https://github.com/microsoft/SqlScriptDOM/archive/$SSD_COMMIT.tar.gz"
        echo "$SSD_SHA256  $P/ssd.tar.gz" | sha256sum -c --quiet
        tar -xzf "$P/ssd.tar.gz" -C "$P/ssd.tmp" --strip-components=1
        rm -f "$P/ssd.tar.gz"
    fi
    mv "$P/ssd.tmp" "$P/ssd"
}

do_antlr2() {
    curl -fsSL -o "$P/antlr-2.7.5.jar.tmp" "$ANTLR2_URL"
    echo "$ANTLR2_SHA256  $P/antlr-2.7.5.jar.tmp" | sha256sum -c --quiet
    mv "$P/antlr-2.7.5.jar.tmp" "$P/antlr-2.7.5.jar"
}

do_shim() {   # ScriptDom's build runs `pwsh`; PowerShell 7 may be installed only as pwsh-preview
    local exe=""
    for c in pwsh pwsh-preview; do
        if command -v "$c" >/dev/null && [ "$("$c" -NoProfile -Command '$PSVersionTable.PSVersion.Major')" -ge 7 ] 2>/dev/null; then
            exe=$(command -v "$c"); break
        fi
    done
    [ -n "$exe" ] || { echo "PowerShell 7 (pwsh or pwsh-preview) not found: ScriptDom's build needs it" >&2; exit 1; }
    mkdir -p "$P/shim"
    ln -sfn "$exe" "$P/shim/pwsh"
}

do_ssd_build() {
    rm -rf "$P/ssd-build"
    cp -a "$P/ssd" "$P/ssd-build"
    PATH=$P/shim:$PATH DOTNET_ROLL_FORWARD=Major dotnet_build "$P/ssd-build" \
        SqlScriptDom/Microsoft.SqlServer.TransactSql.ScriptDom.csproj -c Release -f net8.0 \
        "-p:AntlrLocation=$P/antlr-2.7.5.jar"
    [ -f "$SSD_DLL" ] || { echo "ScriptDom build produced no $SSD_DLL" >&2; exit 1; }
}

do_oracle_nuget() {
    rm -rf "$P/build-Oracle"
    dotnet_build "$ROOT/tools/oracle" -c Release "-p:OracleBuildRoot=$P/build-Oracle"
}

do_oracle_src() {
    rm -rf "$P/build-OracleSrc"
    dotnet_build "$ROOT/tools/oracle" -c Release "-p:OracleBuildRoot=$P/build-OracleSrc" "-p:ScriptDomDll=$SSD_DLL"
}

# dump ORACLE OUT ARGS...: removes OUT, then runs ORACLE ARGS... (which write OUT) capped
dump() {
    local oracle=$1 out=$2; shift 2
    rm -rf "$out"
    "${CAP[@]}" "$oracle" "$@" >/dev/null
}

do_oracle_dumps() {
    dump "$ORACLE_NUGET" "$P/oracle-dumps" dump --jobs $JOBS "$P/oracle-dumps" "$P/ssd/Test/SqlDom"
    "${CAP[@]}" "$ORACLE_NUGET" classify "$P/oracle-dumps"
    "$ORACLE_NUGET" tokentypes "$P/oracle-dumps/TSqlTokenType.package.txt"
}

do_pilot_corpora() { "${CAP[@]}" python3 "$HERE/mk_pilot_corpora.py"; }

do_probes() {
    "$HERE/mk_probes.sh" "$P/probes"
    (cd "$P/probes" && LC_ALL=C ls) > "$P/probes-list.txt"
}

do_pilot_dumps() {
    local pair
    for pair in heldout:ho-oracle bigdir:big-o bigerr:bigerr-o probes:probes-oracle mut:mut-o; do
        dump "$ORACLE_NUGET" "$P/${pair#*:}" dump --jobs $JOBS "$P/${pair#*:}" "$P/${pair%%:*}"
    done
}

do_oracle_version() {
    local v=$1
    dump "$ORACLE_SRC" "$P/oracle-$v" --parser "$v" dump --jobs $JOBS "$P/oracle-$v" "$P/ssd/Test/SqlDom"
    "${CAP[@]}" "$ORACLE_SRC" --parser "$v" classify "$P/oracle-$v"
}

do_heldout_version() { "${CAP[@]}" python3 "$HERE/mk_heldout_all.py" --version "$1"; }

do_crashes() {
    "$HERE/mk_crashes.sh" "$P/crashes"
    (cd "$P/crashes" && LC_ALL=C ls) > "$P/crashes-list.txt"
}

do_crash_version() {
    dump "$ORACLE_SRC" "$P/crash-o-$1" --parser "$1" dump --jobs $JOBS "$P/crash-o-$1" "$P/crashes"
}

T0=$SECONDS
do_shim
[ "$(cat "$STAMPS/ssd" 2>/dev/null)" = "$SSD_COMMIT" ] || rm -f "$STAMPS/ssd"   # the stamp names the commit
step ssd                                                            -- do_ssd
[ -s "$STAMPS/ssd" ] || echo "$SSD_COMMIT" > "$STAMPS/ssd"
step antlr2                                                         -- do_antlr2
step ssd-build     ssd antlr2                                       -- do_ssd_build
step oracle-nuget  "$ROOT/tools/oracle"/{Program.cs,oracle.csproj,Directory.Build.props} -- do_oracle_nuget
step oracle-src    ssd-build "$ROOT/tools/oracle"/{Program.cs,oracle.csproj,Directory.Build.props} -- do_oracle_src
step oracle-dumps  ssd oracle-nuget                                 -- do_oracle_dumps
step pilot-corpora oracle-dumps "$HERE/mk_pilot_corpora.py"         -- do_pilot_corpora
step probes        "$HERE/mk_probes.sh"                             -- do_probes
step pilot-dumps   pilot-corpora probes oracle-nuget                -- do_pilot_dumps
step crashes       "$HERE/mk_crashes.sh"                            -- do_crashes
for v in "${VERSIONS[@]}"; do
    step "oracle-$v"  ssd oracle-src                                -- do_oracle_version "$v"
    step "heldout-$v" "oracle-$v" "$HERE/mk_heldout_all.py"         -- do_heldout_version "$v"
    step "crash-$v"   crashes oracle-src                            -- do_crash_version "$v"
done
echo "workspace ready in $((SECONDS - T0)) s"
