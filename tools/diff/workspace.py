"""The parser-gate workspace: $TSQL_PILOT_DIR, default ${XDG_CACHE_HOME:-~/.cache}/tsql-pilot (kept out
of /tmp so a reboot does not wipe it). The shell scripts, tools/oracle and its Directory.Build.props
resolve the same default."""
import os

P = os.environ.get('TSQL_PILOT_DIR') or os.path.join(
    os.environ.get('XDG_CACHE_HOME') or os.path.expanduser('~/.cache'), 'tsql-pilot')
