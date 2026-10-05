#!/bin/bash
# TSql170 pilot gate corpus `probes` (71 hand-written scripts), recreated byte for byte:
#   tools/diff/mk_probes.sh <out-dir>
# The scripts were first written by hand into the pilot workspace's smoke/{custom,lex} and copied into probes
# (lex/ files with a lex_ prefix), plus m*.sql (money literals) and g*.sql (GO placement). The printf
# lines are those original commands, unchanged; only lex/'s output names carry the prefix directly.
set -eu
[ $# -eq 1 ] || { echo "usage: $0 <out-dir>" >&2; exit 64; }
rm -rf "$1"
mkdir -p "$1"
cd "$1"

# smoke/custom: SELECT shapes, literals, errors
printf 'select 1;;\n  GO\n;select  2 -- c\n/* multi\n /* nested */ line */ GO\nselect 3\ngo 5\n' > go_batches.sql
printf 'WITH c (a) AS (SELECT 1 UNION ALL SELECT a+1 FROM c WHERE a < 10)\nSELECT TOP (5) WITH TIES a, COUNT(*) OVER (PARTITION BY a ORDER BY a ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) AS n\nFROM c LEFT OUTER JOIN [dbo].[t]] x] AS t2 ON c.a = t2.b CROSS APPLY (SELECT * FROM sys.objects o WHERE o.id = c.a) AS ca\nWHERE c.a IN (1,2,3) AND NOT EXISTS (SELECT 1) OR c.a BETWEEN 1 AND 2\nGROUP BY ROLLUP(a) HAVING COUNT(*) > 1 ORDER BY a DESC OFFSET 1 ROWS FETCH NEXT 2 ROWS ONLY OPTION (RECOMPILE, MAXDOP 2);\n' > complex.sql
printf 'SELECT N'"'"'héllo wörld 😀'"'"' AS [ünï], '"'"'it'"''"'s'"'"', 0x1F, $12.5, £3, 1e5, .5, 12345678901, $partition.pf(1), @@ROWCOUNT, @v\nFROM t WITH (NOLOCK) TABLESAMPLE (10 PERCENT)\n' > literals.sql
printf 'SELECT a FROM t WHERE (a = 1 AND (b = 2 OR c IN (SELECT d FROM e)))\nSELECT CASE WHEN a = 1 THEN '"'"'x'"'"' ELSE NULL END, CAST(a AS varchar(10)), CONVERT(int, b), TRY_CAST(c AS decimal(10,2)) COLLATE Latin1_General_CI_AS FROM t\nSELECT * FROM (VALUES (1,2),(3,4)) AS v(a,b) PIVOT (SUM(a) FOR b IN ([2],[4])) AS p\n' > exprs.sql
printf 'SELECT a FROM t WHERE\n' > err_eof.sql
printf 'SELECT a FROM t WHERE a = \nSELECT b FROM\nSELECT 1\n' > err_multi.sql
printf 'SELECT '"'"'unterminated\n' > err_string.sql
printf 'SELECT 1 /* open comment\n' > err_comment.sql
printf 'SELECT a ? b\n' > err_char.sql
printf 'SELECT a FROM t ORDER BY a\nINSERT INTO t VALUES (1)\nSELECT 2\n' > nonselect.sql
printf 'SELECT ALL DISTINCT a\n' > err_mid.sql
printf 'select a from t for xml path('"'"'r'"'"'), root('"'"'x'"'"'), type\nselect a from t for json auto, include_null_values\nselect {fn ucase(a)}, {d '"'"'2020-01-01'"'"'} from t\n' > forxml.sql
printf 'select a.b.c.d.e from t\nselect t.* , x = 1, y AS [z] from t\nlbl: select 1\n' > misc.sql
printf '\xef\xbb\xbfSELECT 1\r\nSELECT\t2\rSELECT 3\n' > bom_crlf.sql
printf 'SELECT "quoted" FROM "t" WHERE "" = '"'"''"'"'\n' > dquote.sql
printf 'SELECT a, FROM t\n' > e1.sql
printf 'SELECT a FROM t WHERE a = 1 GROUP a\nSELECT 2\n' > e2.sql
printf 'SELECT (1 + ) FROM t;\nSELECT 3;\n' > e3.sql
printf 'SELECT a FROM t1 JOIN t2\nSELECT 4\n' > e4.sql
printf 'SELECT TOP 5 a FROM t ORDER BY\n' > e5.sql
printf 'SELECT a FROM t;\nGO\nSELECT b FROM (SELECT 1\nGO\nSELECT c\n' > e6.sql
printf 'SELECT CAST(a AS) FROM t\n' > e7.sql
printf 'SELECT a FROM t ORDER BY a OFFSET\n' > e8.sql
printf 'SELECT * FROM t WHERE a IN ()\n' > e9.sql
printf 'SELECT a.b.c.d.e.f FROM t\n' > e10.sql
printf 'SELECT a FROM t ORDER BY a\n' > e11.sql
printf 'SELECT a INTO #t FROM t; SELECT * FROM #t\n' > v1.sql
printf 'SELECT a FROM t1 INNER HASH JOIN t2 ON t1.a=t2.a FOR BROWSE\n' > v2.sql
printf 'SELECT ROW_NUMBER() OVER (ORDER BY a), LAG(a,1) OVER (PARTITION BY b ORDER BY c), STRING_AGG(a, '"'"','"'"') WITHIN GROUP (ORDER BY a) FROM t\n' > v3.sql
printf 'SELECT IIF(a > 1, 1, 0), CHOOSE(1, a, b), a.b::c FROM t\nSELECT NEXT VALUE FOR s\n' > v4.sql
printf 'SELECT * FROM OPENJSON(@j) WITH (a int '"'"'$.a'"'"', b nvarchar(max) AS JSON)\nSELECT * FROM STRING_SPLIT(@s, '"'"','"'"')\nSELECT * FROM t FOR SYSTEM_TIME AS OF '"'"'2020-01-01'"'"'\n' > v5.sql
printf 'SELECT a FROM t UNION SELECT b FROM u EXCEPT (SELECT c FROM v) INTERSECT SELECT d FROM w ORDER BY 1\n' > v6.sql
printf 'select [a b], "c d", '"'"''"'"', N'"'"''"'"', 1.e5, 0x, 1.5e-3, -1, ~2, +3 from t where a !< 1 and b !> 2 and c <> 3 and d != 4 and e >= 5 and f <= 6\n' > v7.sql
printf 'SELECT a FROM t WHERE CONTAINS(a, '"'"'x'"'"') AND FREETEXT(*, '"'"'y'"'"') AND a LIKE '"'"'%%x'"'"' ESCAPE '"'"'\\'"'"' AND b IS NOT NULL\n' > v8.sql
printf 'SELECT a = b, @v = 1 FROM t TABLESAMPLE SYSTEM (10 PERCENT) REPEATABLE (5)\n' > v9.sql
printf 'select $IDENTITY, $ROWGUID, t.$NODE_ID from t\nselect * from sys.dm_exec_requests cross apply sys.dm_exec_sql_text(sql_handle)\n' > v10.sql
printf 'SELECT GREATEST(1,2), LEAST(3,4), DATE_BUCKET(week, 1, d), JSON_OBJECT('"'"'a'"'"':1), JSON_ARRAY(1,2), x.query('"'"'/a'"'"'), x.value('"'"'(/a)[1]'"'"', '"'"'int'"'"') FROM t\n' > v11.sql
printf 'SELECT a FROM t WHERE a = ALL (SELECT b FROM u) AND c > SOME (SELECT d FROM v) AND EXISTS (SELECT 1)\nSELECT COUNT(DISTINCT a), COUNT_BIG(*), SUM(ALL b) FROM t\n' > v12.sql
printf 'WITH XMLNAMESPACES ('"'"'uri'"'"' AS ns) SELECT a FROM t FOR XML RAW('"'"'r'"'"'), ELEMENTS XSINIL\n' > v13.sql
printf 'select a from t where a between 1 and 2 or not (b = 1)\n;;;\nselect\n1\n--end' > v14.sql
printf 'SELECT TOP 10 PERCENT a FROM t ORDER BY a\nSELECT TOP (@n) a FROM t\n' > v15.sql
printf 'SELECT {fn CONVERT(a, SQL_VARCHAR)}, {ts '"'"'2020-01-01 00:00:00'"'"'}, {guid '"'"'00000000-0000-0000-0000-000000000000'"'"'}\n' > v16.sql
printf 'SELECT * FROM t1 LEFT JOIN (t2 INNER JOIN t3 ON t2.a = t3.a) ON t1.a = t2.a\nSELECT * FROM ((t1))\nSELECT * FROM (t1 CROSS JOIN t2)\n' > v17.sql
printf 'select 1 as "a", 2 as '"'"'b'"'"', 3 [c], d = 4\nselect * from t with (index(ix1), forceseek)\nselect * from t (nolock)\n' > v18.sql

# smoke/lex: lexer edge cases
printf 'SELECT 𝒳a, [𝒳] FROM t\r\n\tWHERE\ra = 1 ?\n' > lex_l1.sql
printf '  go\nSELECT 1\n   GO  \nselect 2\n/* x */ go\nselect 3 -- go\ngo\n' > lex_l2.sql
printf 'SELECT 1 FROM t WHERE a = $ 12 AND b = £1abc\n' > lex_l3.sql
printf 'SELECT 0x, 0xAB\\\nCD, 12345678901234, 2147483648, 2147483647, 1.0e+10, 1e, 1.e\n' > lex_l4.sql
printf 'SELECT [abc\n' > lex_l5.sql
printf 'SELECT "abc\n' > lex_l6.sql
printf 'SELECT N'"'"'abc\n' > lex_l7.sql
printf 'SELECT $(foo\n' > lex_l8.sql
printf 'SELECT a FROM t\n--(* odbc\n' > lex_l9.sql
printf 'SELECT 1;  2\n' > lex_l10.sql
printf 'select a from t where b = 1 ; select 2 ;\n' > lex_l11.sql
printf 'select ´ from t\n' > lex_l12.sql
printf 'select a /* c1 /* c2 */ */ , b from t\n' > lex_l13.sql

# money literals before identifiers, GO placement
printf 'SELECT £abc' > m1.sql
printf 'SELECT £1abc' > m2.sql
printf 'SELECT £12' > m3.sql
printf 'SELECT £ 1' > m4.sql
printf 'SELECT £1e5abc' > m5.sql
printf 'SELECT £1.5abc' > m6.sql
printf 'SELECT $1x, €2, £+3, £-4.5e2' > m7.sql
printf 'SELECT 1\n go\nselect 2' > g1.sql
printf '/*c*/go\nselect 1' > g2.sql
printf 'select 1\ngo 5\nselect go2, gox' > g3.sql
printf 'select 1 go' > g4.sql
printf 'select 1\n-- c\ngo\nselect 2' > g5.sql
printf 'go:\nselect 1' > g6.sql
printf 'select 1\ngo:\nselect 2' > g7.sql
