#!/bin/bash
# Gate corpus `crash`: minimal repros of inputs that once crashed tsql_dump, plus their siblings
# (the same error in each statement shape whose result the parser drops after a syntax error):
#   tools/diff/mk_crashes.sh <out-dir>
# Compared strictly with the oracle of every version; tools/diff/crash_sweep.py searches for new ones.
# The bodies use errors whose position the port reports like SqlScriptDOM (`SELECT )` is reported at
# `)` instead of at SELECT, a separate error-position gap).
set -eu
[ $# -eq 1 ] || { echo "usage: $0 <out-dir>" >&2; exit 64; }
rm -rf "$1"
mkdir -p "$1"
cd "$1"

# CREATE OR ALTER <module> with a syntax error in its body: the module statement is dropped (null) and
# createOrAlterStatements then extends it to the CREATE token, a NullReferenceException in SqlScriptDOM
# (internal error 46001, no tree); tsql_dump used to die of SIGSEGV
printf 'CREATE OR ALTER PROCEDURE p AS SELECT o.' > coa_proc_eof.sql
printf 'CREATE OR ALTER PROCEDURE p AS SELECT a FROM t WHERE )' > coa_proc.sql
printf 'CREATE OR ALTER PROCEDURE p AS SELECT a FROM t WHERE )\nGO\nSELECT 1' > coa_proc_go.sql
printf 'CREATE OR ALTER PROCEDURE p AS BEGIN SELECT a FROM t WHERE ) END;\nGO\nCREATE OR ALTER PROCEDURE q AS SELECT 1' > coa_proc_block.sql
printf 'CREATE OR ALTER FUNCTION f() RETURNS INT AS BEGIN RETURN (SELECT a FROM t WHERE )); END' > coa_func.sql
printf 'CREATE OR ALTER TRIGGER t ON dbo.x AFTER INSERT AS SELECT a FROM t WHERE )' > coa_trigger.sql
printf 'CREATE OR ALTER VIEW v AS SELECT a FROM t WHERE )' > coa_view.sql
printf 'SELECT 1;\nCREATE OR ALTER PROCEDURE p AS SELECT o.\n' > coa_proc_not_first.sql

# The same errors where the dropped statement is not used afterwards
printf 'CREATE PROCEDURE p AS SELECT a FROM t WHERE )' > create_proc.sql
printf 'ALTER PROCEDURE p AS SELECT a FROM t WHERE )' > alter_proc.sql
printf 'CREATE FUNCTION f() RETURNS INT AS BEGIN RETURN (SELECT a FROM t WHERE )); END' > create_func.sql
printf 'ALTER FUNCTION f() RETURNS INT AS BEGIN RETURN (SELECT a FROM t WHERE )); END' > alter_func.sql
printf 'CREATE TRIGGER t ON dbo.x AFTER INSERT AS SELECT a FROM t WHERE )' > create_trigger.sql
printf 'ALTER TRIGGER t ON dbo.x AFTER INSERT AS SELECT a FROM t WHERE )' > alter_trigger.sql
printf 'IF 1 = 1 SELECT a FROM t WHERE ) ELSE SELECT 2;\nSELECT 3' > if_else.sql
printf 'WHILE 1 = 1 SELECT a FROM t WHERE );\nSELECT 3' > while.sql
printf 'BEGIN TRY SELECT a FROM t WHERE ) END TRY BEGIN CATCH SELECT 2 END CATCH;\nSELECT 3' > try_catch.sql
printf 'BEGIN SELECT a FROM t WHERE ) END;\nSELECT 3' > begin_end.sql
