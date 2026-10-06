#!/bin/bash
# Gate corpus `crash`: minimal repros of inputs that once crashed tsql_dump or exhausted its memory,
# plus their siblings (the same error in each statement shape whose result the parser drops after a
# syntax error), and probes of the ANTLR 2 decisions ANTLR 4 compiles to inline code:
#   tools/diff/mk_crashes.sh <out-dir>
# Compared strictly with the oracle of every version; tools/diff/crash_sweep.py searches for new ones.
# check.sh runs tsql_dump with 2 GB of address space: a runaway ends in internal error 46001 (or
# dies), which fails the comparison.
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

# Exponential speculation: checking ANTLR 4's own alternative's syntactic predicate inside a
# speculative parse started the same speculation again at every level of nested FROM parentheses,
# so time and memory almost doubled per level (TSql130 and TSql170 alike); 21 levels took over
# 2 GB. From the crash sweep's TestScripts/FromClauseTests.sql variants (its line 108, valid
# input) and siblings.
open=$(printf '(%.0s' {1..21})
close=$(printf ')%.0s' {1..20})
printf 'select * from %s(select * from t1) as t10 join (select * from t2) as t20 on t10.c1 = t20.c2)%s\n' "$open" "$close" > runaway_join_derived.sql
printf 'select * from %st1 join t2 on 1=1)%s\n' "$open" "$close" > runaway_join.sql
printf 'select * from %s(select * from t1) as t10 cross join t2)%s\n' "$open" "$close" > runaway_cross_join.sql
printf 'select * from t0 join %s(select * from t1) as t10 join t2 on 1=1)%s on 1=1\n' "$open" "$close" > runaway_join_inner.sql
printf 'select * from %st1 cross apply (select 1 a) as t10)%s\n' "$open" "$close" > runaway_cross_apply.sql

# Syntactic predicates at decisions ANTLR 4 compiles to inline LL(1) code, which never calls
# adaptivePredict (so the runtime's ANTLR 2 emulation does not run there): ANTLR 2 found these
# decisions deterministic with LA(1) too and generated no speculative parse for them, so the error
# is past the predicate's tokens, where the alternative itself fails. foreignConstraintColumnsOpt
# ((LeftParenthesis identifier)=>, every grammar), overClauseBeginningNoWindowName
# ((Identifier By)=>, TSql160+), predictWithClauseOpt ((With)=> on its rule's only alternative).
printf 'CREATE TABLE t (a INT, FOREIGN KEY (1) REFERENCES r (a))' > inline_fk_table_literal.sql
printf 'CREATE TABLE t (a INT FOREIGN KEY (1) REFERENCES r (a))' > inline_fk_column_literal.sql
printf 'CREATE TABLE t (a INT, FOREIGN KEY () REFERENCES r (a))' > inline_fk_table_empty.sql
printf 'CREATE TABLE t (a INT FOREIGN KEY (a, 1) REFERENCES r (a))' > inline_fk_column_list.sql
printf 'CREATE TABLE t (a INT FOREIGN KEY REFERENCES r (a), b INT, FOREIGN KEY (b) REFERENCES r (b))' > inline_fk_ok.sql
printf 'CREATE TABLE t (a INT FOREIGN KEY x REFERENCES r (a))' > inline_fk_column_exit.sql
printf 'CREATE TABLE t (a INT, FOREIGN KEY x REFERENCES r (a))' > inline_fk_table_exit.sql
printf 'SELECT JSON_ARRAYAGG(name) OVER (PARTITION dept) FROM t' > inline_over_partition_no_by.sql
printf 'SELECT JSON_ARRAYAGG(name) OVER (x BY dept) FROM t' > inline_over_not_partition.sql
printf 'SELECT JSON_ARRAYAGG(name) OVER (dept) FROM t' > inline_over_identifier.sql
printf 'SELECT JSON_ARRAYAGG(name) OVER (1) FROM t' > inline_over_literal.sql
printf 'SELECT JSON_OBJECTAGG(a:b) OVER (ORDER BY a) FROM t' > inline_over_order.sql
printf 'SELECT JSON_OBJECTAGG(a:b) OVER (PARTITION BY a), JSON_ARRAYAGG(b) OVER () FROM t' > inline_over_ok.sql
printf 'SELECT * FROM PREDICT(MODEL = @m, DATA = dbo.t AS d) AS p' > inline_predict_no_with.sql
printf 'SELECT * FROM PREDICT(MODEL = @m, DATA = dbo.t AS d) WITH score FLOAT) AS p' > inline_predict_with_no_paren.sql
printf 'SELECT * FROM PREDICT(MODEL = @m, DATA = dbo.t AS d) WITH (1) AS p' > inline_predict_with_literal.sql
printf 'SELECT * FROM PREDICT(MODEL = @m, DATA = dbo.t AS d) WITH (score FLOAT) AS p' > inline_predict_ok.sql
