// Contract tests for tsql::editor::Document (include/tsql/editor.hpp) against the free Complete/Classify, with the
// fixture catalog catalog.json in CASES_DIR.
//
//   test_document [--suite document|perf] [--filter TEXT] [--as-version NAME] [--verbose] CASES_DIR
//
// document (default): deterministic randomized edit sequences and the basics below; --filter runs only the runs
// whose id ("basics", "tuning:1", "large:7", ...) contains TEXT. perf: Document::Classify cost after an edit in a
// 200 KB script. --as-version runs under grammar NAME instead of TSql170. --verbose reports every run and prints
// every edit as it is made (to locate a crash).
#include "contract_support.h"

#include <chrono>
#include <cstdint>
#include <iterator>

namespace {

using namespace editor_contract;

// ---------------------------------------------------------------------------------------------- Document contract
//
// tsql::editor::Document must give, after any sequence of edits, the results the free functions give on Text().
// Deterministic randomized edit sequences (fixed seeds, own RNG arithmetic so every platform replays the same
// edits) over realistic scripts compare Document::Complete at several carets and Document::Classify over the whole
// text and over ranges with Complete/Classify on the edited text, after every edit.

const char* const kScriptTuning = R"SQL(/* Performance triage: top queries, waits, missing indexes.
   Run on the instance under test; Umsätze report at the end. */
SET NOCOUNT ON;
SET STATISTICS IO, TIME ON;
GO
-- Top CPU consumers
SELECT TOP (20)
    qs.execution_count,
    qs.total_worker_time / qs.execution_count AS avg_cpu,
    qs.total_logical_reads,
    SUBSTRING(st.text, (qs.statement_start_offset / 2) + 1,
        ((CASE qs.statement_end_offset WHEN -1 THEN DATALENGTH(st.text) ELSE qs.statement_end_offset END
          - qs.statement_start_offset) / 2) + 1) AS statement_text,
    qp.query_plan
FROM sys.dm_exec_query_stats AS qs
CROSS APPLY sys.dm_exec_sql_text(qs.sql_handle) AS st
OUTER APPLY sys.dm_exec_query_plan(qs.plan_handle) AS qp
WHERE st.text NOT LIKE N'%dm_exec%'
ORDER BY qs.total_worker_time DESC
OPTION (RECOMPILE);
GO
WITH waits AS (
    SELECT ws.wait_type, ws.wait_time_ms, ws.signal_wait_time_ms,
           100.0 * ws.wait_time_ms / SUM(ws.wait_time_ms) OVER () AS pct
    FROM sys.dm_os_wait_stats ws
    WHERE ws.wait_type NOT IN (N'SLEEP_TASK', N'LAZYWRITER_SLEEP', N'BROKER_TASK_STOP')
)
SELECT w.wait_type, CAST(w.pct AS decimal(5, 2)) AS pct
FROM waits w
WHERE w.pct > 1.0
ORDER BY w.pct DESC;
GO
SELECT d.statement, d.equality_columns, d.inequality_columns, d.included_columns,
       s.avg_user_impact, s.user_seeks
FROM sys.dm_db_missing_index_groups g
JOIN sys.dm_db_missing_index_group_stats s ON s.group_handle = g.index_group_handle
JOIN sys.dm_db_missing_index_details d ON d.index_handle = g.index_handle
WHERE d.database_id = DB_ID()
ORDER BY s.avg_user_impact * s.user_seeks DESC;
GO
DECLARE @since datetime2(3) = DATEADD(DAY, -7, SYSDATETIME()), @region nvarchar(50) = N'West';
SELECT c.CustomerName, COUNT(*) AS Orders, SUM(o.TotalDue) AS Revenue
FROM dbo.Customers AS c WITH (NOLOCK)
JOIN dbo.Orders AS o ON o.CustomerID = c.CustomerID
WHERE o.OrderDate >= @since AND c.Region = @region -- 'quoted' in a comment
GROUP BY c.CustomerName
HAVING SUM(o.TotalDue) > 1000
ORDER BY Revenue DESC;
SELECT [Jahr], SUM(Betrag) AS Summe FROM Reporting.dbo.Umsätze GROUP BY [Jahr];
SELECT * FROM dbo.[Order Details] AS od WHERE od.[Line Total] > 0 AND 'it''s' <> "x";
GO
SET STATISTICS IO, TIME OFF;
)SQL";

const char* const kScriptProcedure = R"SQL(CREATE OR ALTER PROCEDURE dbo.usp_ProcessOrders
    @CustomerID int,
    @Since date = NULL,
    @Processed int OUTPUT
AS
BEGIN
    SET NOCOUNT ON
    DECLARE @id int, @total money
    DECLARE order_cursor CURSOR LOCAL FAST_FORWARD FOR
        SELECT o.OrderID, o.TotalDue FROM dbo.Orders o WHERE o.CustomerID = @CustomerID
    OPEN order_cursor
    FETCH NEXT FROM order_cursor INTO @id, @total
    SET @Processed = 0
    WHILE @@FETCH_STATUS = 0
    BEGIN
        BEGIN TRY
            IF @total > 1000
                UPDATE dbo.Orders SET Status = 2 WHERE OrderID = @id
            ELSE
                UPDATE dbo.Orders SET Status = 1 WHERE OrderID = @id
            SET @Processed += 1
        END TRY
        BEGIN CATCH
            PRINT ERROR_MESSAGE()
        END CATCH
        FETCH NEXT FROM order_cursor INTO @id, @total
    END
    CLOSE order_cursor
    DEALLOCATE order_cursor
    MERGE dbo.Products AS t
    USING staging.ProductFeed AS s ON t.ProductID = s.ProductID
    WHEN MATCHED AND t.ListPrice <> s.ListPrice THEN UPDATE SET t.ListPrice = s.ListPrice
    WHEN NOT MATCHED BY TARGET THEN INSERT (ProductID, ProductName, ListPrice) VALUES (s.ProductID, s.ProductName, s.ListPrice)
    WHEN NOT MATCHED BY SOURCE THEN DELETE;
    SELECT p.ProductID,
           CASE WHEN p.Discontinued = 1 THEN N'retired'
                ELSE CASE WHEN p.ListPrice > 100 THEN N'premium' ELSE N'standard' END END AS tier
    FROM dbo.Products p
END
GO
DECLARE @n int;
EXEC dbo.usp_ProcessOrders @CustomerID = 42, @Processed = @n OUTPUT;
EXEC dbo.usp_GetOrders 42, @Status = 1;
SELECT @n AS processed;
GO
CREATE TABLE #work (Id int PRIMARY KEY, Amount money, Note nvarchar(100));
INSERT INTO #work (Id, Amount, Note) SELECT OrderID, TotalDue, N'€ batch' FROM dbo.Orders WHERE Status = 1;
UPDATE w SET w.Amount = w.Amount * 1.1 FROM #work w WHERE w.Note LIKE N'%batch%'
SET @x = 1 -- deliberately undeclared
SELECT w.Id, w.Amount FROM #work w
DROP TABLE #work;
GO
)SQL";

// Text an editor user inserts: batch separators, comment and string delimiters (opening and closing), brackets,
// keywords, partial statements, multi-byte characters, line ends.
const char* const kSnippets[] = {
    "\nGO\n", "GO", "\nGO", "GO\n", "/*", "*/", "--", "-- note\n", "'", "N'", "''", "\"", "[", "]", "(", ")", ";",
    "\n", "\r\n", " ", "\t", "SELECT ", "FROM ", "WHERE ", "o.", "dbo.", "sys.", "BEGIN ", "END ", "CASE WHEN 1 = 1 THEN ",
    "@x", "#t", "ä", "€", "😀", "0x1F", "1.5e3", "N'Umsätze'", "EXEC dbo.usp_GetOrders ", "SET NOCOUNT ON\n",
    "UPDATE dbo.Orders SET Status = 1\n", "DECLARE c CURSOR FOR SELECT 1\n", "/* unclosed ", "' unclosed",
};

// What a user types character by character (each keystroke is an edit, checked at the caret).
const char* const kTyped[] = {
    "\nGO\n", "SELECT o.", "SELECT * FROM sys.", "/* note */", "'it''s'", "-- todo\n", "WHERE ", "N'Umsätze'",
    "BEGIN\n", "END\n", "EXEC dbo.usp_GetOrders @", "JOIN dbo.Customers c ON c.",
};

// Deterministic on every platform (std::uniform_int_distribution is not).
class Rng {
public:
    explicit Rng(uint64_t seed) : state_(seed * 0x9E3779B97F4A7C15ull + 1) {}
    uint64_t next() {  // splitmix64
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
    bool chance(unsigned percent) { return below(100) < percent; }

private:
    uint64_t state_;
};

// Moves pos back to the start of the UTF-8 character containing it (editors edit whole characters).
size_t CharStart(const std::string& s, size_t pos) {
    pos = std::min(pos, s.size());
    while (pos > 0 && pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) --pos;
    return pos;
}

std::string SpanText(const std::string& sql, const ColouredSpan& s) {
    std::string text = s.start <= sql.size() ? sql.substr(s.start, std::min<size_t>(s.length, 40)) : std::string();
    return "[" + std::to_string(s.start) + "+" + std::to_string(s.length) + " " + NameOf(kClasses, s.cls) + " " + Quote(text) + "]";
}

// First difference between span lists (empty when equal).
std::string SpanDiff(const std::string& sql, const std::vector<ColouredSpan>& got, const std::vector<ColouredSpan>& want) {
    size_t n = std::min(got.size(), want.size());
    for (size_t i = 0; i < n; ++i)
        if (got[i].start != want[i].start || got[i].length != want[i].length || got[i].cls != want[i].cls)
            return "span " + std::to_string(i) + " is " + SpanText(sql, got[i]) + ", expected " + SpanText(sql, want[i]);
    if (got.size() == want.size()) return {};
    return std::to_string(got.size()) + " spans, expected " + std::to_string(want.size()) + "; first " +
           (got.size() > n ? "extra " + SpanText(sql, got[n]) : "missing " + SpanText(sql, want[n]));
}

std::string ItemText(const CompletionItem& item) {
    return DescribeItem(item) + (item.detail.empty() ? "" : " {" + item.detail + "}");
}

// First difference between completion results (empty when equal).
std::string CompletionDiff(const CompletionResult& got, const CompletionResult& want) {
    if (got.replaceStart != want.replaceStart || got.replaceLength != want.replaceLength)
        return "replace {" + std::to_string(got.replaceStart) + ", " + std::to_string(got.replaceLength) + "}, expected {" +
               std::to_string(want.replaceStart) + ", " + std::to_string(want.replaceLength) + "}";
    size_t n = std::min(got.items.size(), want.items.size());
    for (size_t i = 0; i < n; ++i) {
        const auto &a = got.items[i], &b = want.items[i];
        if (a.kind != b.kind || a.label != b.label || a.insertText != b.insertText || a.detail != b.detail)
            return "item " + std::to_string(i) + " is " + ItemText(a) + ", expected " + ItemText(b);
    }
    if (got.items.size() == want.items.size()) return {};
    return std::to_string(got.items.size()) + " items, expected " + std::to_string(want.items.size()) + "; first " +
           (got.items.size() > n ? "extra " + ItemText(got.items[n]) : "missing " + ItemText(want.items[n]));
}

// The spans of `all` (a whole-text Classify) overlapping [start, end).
std::vector<ColouredSpan> Overlapping(const std::vector<ColouredSpan>& all, size_t start, size_t end) {
    std::vector<ColouredSpan> out;
    for (const auto& s : all)
        if (s.start < end && s.start + s.length > start) out.push_back(s);
    return out;
}

// Document::Classify(start, end) with start < end <= size: the spans of the tokens overlapping the range,
// contiguous, the first containing `start` and the last containing end - 1.
void CheckRangeRules(const std::vector<ColouredSpan>& spans, size_t start, size_t end, std::vector<std::string>& issues) {
    std::string range = "Classify(" + std::to_string(start) + ", " + std::to_string(end) + ")";
    if (spans.empty()) {
        issues.push_back(range + " returned no spans");
        return;
    }
    for (size_t i = 0; i < spans.size(); ++i) {
        const auto& s = spans[i];
        if (s.length == 0) issues.push_back(range + ": empty span at " + std::to_string(s.start));
        if (i > 0 && s.start != spans[i - 1].start + spans[i - 1].length) {
            issues.push_back(range + ": span at " + std::to_string(s.start) + " does not follow the previous one (gap or overlap)");
            return;
        }
        if (s.start >= end || s.start + s.length <= start)
            issues.push_back(range + ": span at " + std::to_string(s.start) + "+" + std::to_string(s.length) + " lies outside the range");
    }
    if (spans.front().start > start) issues.push_back(range + ": first span starts at " + std::to_string(spans.front().start));
    if (spans.back().start + spans.back().length < end)
        issues.push_back(range + ": last span ends at " + std::to_string(spans.back().start + spans.back().length));
}

class DocumentRun {
public:
    DocumentRun(std::string name, SqlVersion version, const Catalog& catalog, uint64_t seed, std::vector<std::string> bases)
        : name_(std::move(name)), version_(version), catalog_(catalog), seed_(seed), rng_(seed), bases_(std::move(bases)),
          doc_(version) {}

    // Runs `macros` user actions (each one or more edits, every edit checked); true when every check passed.
    bool Run(size_t macros, bool verbose) {
        verbose_ = verbose;
        SetText(bases_[0], "SetText(base 0)");
        for (step_ = 0; step_ < macros && !failed_; ++step_) {
            if (step_ == macros / 2) {
                // Moving keeps the buffer and its state.
                Document moved(std::move(doc_));
                doc_ = std::move(moved);
                Check(rng_.below(text_.size() + 1), true);
            }
            Macro();
        }
        if (!failed_) Check(text_.size(), true);
        if (verbose_ || failed_)
            std::printf("%s document %s seed %llu: %zu checks over %zu actions\n", failed_ ? "FAIL" : "ok  ", name_.c_str(),
                        static_cast<unsigned long long>(seed_), checks_, step_);
        return !failed_;
    }

private:
    size_t Pos() { return CharStart(text_, rng_.below(text_.size() + 1)); }
    size_t LineStart(size_t pos) {
        while (pos > 0 && text_[pos - 1] != '\n') --pos;
        return pos;
    }
    std::string Snippet() { return kSnippets[rng_.below(std::size(kSnippets))]; }
    // [start, start+length) within the text on character boundaries, length in 1..maxLength when the text allows.
    std::pair<size_t, size_t> Range(size_t maxLength) {
        size_t start = Pos();
        size_t end = CharStart(text_, start + 1 + rng_.below(maxLength));
        if (end <= start) end = std::min(text_.size(), start + 1);
        while (end < text_.size() && (static_cast<unsigned char>(text_[end]) & 0xC0) == 0x80) ++end;
        return {start, end - start};
    }

    void SetText(const std::string& text, std::string what) {
        Record(std::move(what));
        text_ = text;
        doc_.SetText(text);
        Check(rng_.below(text_.size() + 1), true);
    }

    void Apply(size_t start, size_t length, const std::string& replacement, std::string what, bool full = false) {
        if (failed_) return;
        Record(what + " at " + std::to_string(start) + " len " + std::to_string(length) + " -> " + Quote(replacement));
        text_.replace(start, length, replacement);
        doc_.Edit(start, length, replacement);
        if (!deferCheck_) Check(start + replacement.size(), full);
    }

    // Keeps the last actions for a failure report; --verbose also prints each one as it happens (for crashes).
    void Record(std::string what) {
        history_.push_back("action " + std::to_string(step_) + ": " + std::move(what));
        if (verbose_) std::printf("  %s %s\n", name_.c_str(), history_.back().c_str());
        if (history_.size() > 12) history_.erase(history_.begin());
    }

    void Macro() {
        size_t base = bases_[0].size();
        if (text_.size() > base * 2 + 256) {
            auto [start, length] = Range(text_.size() / 3);
            Apply(start, length, "", "trim");
            return;
        }
        if (text_.size() < base / 4) {
            SetText(bases_[rng_.below(bases_.size())], "SetText(reset)");
            return;
        }
        unsigned roll = static_cast<unsigned>(rng_.below(100));
        if (roll < 22) {
            Apply(Pos(), 0, Snippet(), "insert");
        } else if (roll < 36) {
            auto [start, length] = Range(60);
            Apply(start, length, "", "delete");
        } else if (roll < 46) {
            auto [start, length] = Range(30);
            Apply(start, length, Snippet(), "replace");
        } else if (roll < 60) {
            std::string typed = kTyped[rng_.below(std::size(kTyped))];
            size_t pos = rng_.chance(50) ? LineStart(Pos()) : Pos();
            Record("typing " + Quote(typed) + " at " + std::to_string(pos));
            for (size_t i = 0; i < typed.size() && !failed_;) {
                size_t n = 1;
                while (i + n < typed.size() && (static_cast<unsigned char>(typed[i + n]) & 0xC0) == 0x80) ++n;
                Apply(pos, 0, typed.substr(i, n), "type");
                pos += n;
                i += n;
            }
        } else if (roll < 68) {
            // Delete a batch separator line (merging two batches), or add one when there is none.
            std::vector<std::pair<size_t, size_t>> separators;
            for (size_t at = 0; at < text_.size();) {
                size_t eol = text_.find('\n', at);
                size_t end = eol == std::string::npos ? text_.size() : eol + 1;
                std::string line = text_.substr(at, end - at);
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
                if (Lower(line) == "go") separators.emplace_back(at, end - at);
                at = end;
            }
            if (separators.empty()) Apply(LineStart(Pos()), 0, "GO\n", "add separator");
            else {
                auto [start, length] = separators[rng_.below(separators.size())];
                Apply(start, length, "", "delete separator");
            }
        } else if (roll < 76) {
            // Comment out a region: open, then (usually) close further on.
            size_t open = Pos();
            Apply(open, 0, "/*", "open comment");
            if (rng_.chance(75)) Apply(CharStart(text_, std::min(text_.size(), open + 2 + rng_.below(400))), 0, "*/", "close comment");
        } else if (roll < 82) {
            size_t open = Pos();
            Apply(open, 0, rng_.chance(50) ? "'" : "N'", "open string");
            if (rng_.chance(60)) Apply(CharStart(text_, std::min(text_.size(), open + 2 + rng_.below(200))), 0, "'", "close string");
        } else if (roll < 88) {
            // Delete and restore (undo): the buffer returns to the same text.
            auto [start, length] = Range(120);
            std::string removed = text_.substr(start, length);
            Apply(start, length, "", "cut");
            Apply(start, 0, removed, "undo cut", true);
        } else if (roll < 91) {
            Apply(rng_.chance(50) ? 0 : text_.size(), 0, Snippet(), "insert at edge");
        } else if (roll < 94) {
            // Paste a batch of another script at a line start.
            const std::string& other = bases_[rng_.below(bases_.size())];
            size_t from = CharStart(other, rng_.below(other.size()));
            size_t to = CharStart(other, std::min(other.size(), from + 200 + rng_.below(600)));
            Apply(LineStart(Pos()), 0, other.substr(from, to - from), "paste");
        } else if (roll < 98) {
            // Several edits before the next query (fast typing, multi-cursor, replace-all).
            deferCheck_ = true;
            size_t last = 0, n = 2 + rng_.below(5);
            for (size_t k = 0; k < n; ++k) {
                if (rng_.chance(60)) {
                    last = Pos();
                    std::string s = Snippet();
                    Apply(last, 0, s, "burst insert");
                    last += s.size();
                } else {
                    auto [start, length] = Range(40);
                    Apply(start, length, "", "burst delete");
                    last = start;
                }
            }
            deferCheck_ = false;
            Check(last, rng_.chance(50));
        } else {
            SetText(bases_[rng_.below(bases_.size())], "SetText(other)");
        }
    }

    // Compares the Document with the free functions on the current text. Ranges are classified before any
    // whole-text Classify so that lazily updated state is exercised; `full` forces the whole-text comparison.
    void Check(size_t focus, bool full) {
        if (failed_) return;
        ++checks_;
        std::vector<std::string> issues;
        if (doc_.Text() != text_) {
            size_t at = 0;
            while (at < text_.size() && at < doc_.Text().size() && text_[at] == doc_.Text()[at]) ++at;
            issues.push_back("Text() differs from the edited text at byte " + std::to_string(at));
        }
        focus = CharStart(text_, focus);
        const size_t size = text_.size();
        try {
            std::vector<ColouredSpan> all = Classify(text_, version_);
            std::vector<std::pair<size_t, size_t>> ranges;
            if (size > 0) {
                ranges.emplace_back(focus > 40 ? focus - 40 : 0, std::min(size, focus + 40));
                for (int k = 0; k < 2; ++k) {
                    size_t a = rng_.below(size);
                    ranges.emplace_back(a, a + 1 + rng_.below(std::min<size_t>(size - a, 400)));
                }
            }
            for (auto [a, b] : ranges) {
                if (a >= b) continue;
                std::vector<ColouredSpan> got = doc_.Classify(a, b);
                CheckRangeRules(got, a, b, issues);
                std::string diff = SpanDiff(text_, got, Overlapping(all, a, b));
                if (!diff.empty()) issues.push_back("Classify(" + std::to_string(a) + ", " + std::to_string(b) + "): " + diff);
            }
            if (full || rng_.chance(35)) {
                std::string diff = SpanDiff(text_, doc_.Classify(0, size), all);
                if (!diff.empty()) issues.push_back("Classify(0, " + std::to_string(size) + "): " + diff);
            }
            for (size_t caret : {focus, CharStart(text_, rng_.below(size + 1)), size}) {
                CompletionResult got = doc_.Complete(caret, catalog_);
                CompletionResult want = Complete(text_, caret, version_, catalog_);
                std::string diff = CompletionDiff(got, want);
                if (!diff.empty()) issues.push_back("Complete(" + std::to_string(caret) + "): " + diff);
                CheckCompletionInvariants(text_, caret, got, issues);
            }
        } catch (const std::exception& e) {
            issues.push_back(std::string("threw: ") + e.what());
        }
        if (issues.empty()) return;
        failed_ = true;
        std::printf("FAIL document %s seed %llu, check %zu after:\n", name_.c_str(), static_cast<unsigned long long>(seed_), checks_);
        for (const auto& h : history_) std::printf("    %s\n", h.c_str());
        for (const auto& i : issues) std::printf("  - %s\n", i.c_str());
        size_t from = focus > 120 ? CharStart(text_, focus - 120) : 0;
        std::printf("  text (%zu bytes) around %zu: %s\n", size, focus, Quote(text_.substr(from, 240)).c_str());
    }

    std::string name_;
    SqlVersion version_;
    const Catalog& catalog_;
    uint64_t seed_;
    Rng rng_;
    std::vector<std::string> bases_;
    Document doc_;
    std::string text_;
    std::vector<std::string> history_;
    size_t step_ = 0, checks_ = 0;
    bool failed_ = false, verbose_ = false, deferCheck_ = false;
};

// Contract points that are not edit sequences. Returns the number of failures.
size_t RunDocumentBasics(SqlVersion version, const Catalog& catalog) {
    std::vector<std::string> issues;
    // A Document of a version without a parser throws like the free functions.
    for (int v = 0; v <= static_cast<int>(SqlVersion::Sql180); ++v) {
        auto sv = static_cast<SqlVersion>(v);
        if (tsql::IsParserAvailable(sv)) continue;
        try {
            Document d(sv);
            issues.push_back(std::string("Document(") + std::to_string(v) + ") did not throw for a version without a parser");
        } catch (const std::invalid_argument&) {
        } catch (const std::exception& e) {
            issues.push_back(std::string("Document(") + std::to_string(v) + ") threw something other than invalid_argument: " + e.what());
        }
    }
    Document doc(version);
    // A new Document is empty.
    if (!doc.Text().empty()) issues.push_back("a new Document's Text() is not empty");
    if (!doc.Classify(0, 0).empty()) issues.push_back("Classify(0, 0) of an empty Document returned spans");
    {
        std::string diff = CompletionDiff(doc.Complete(0, catalog), Complete("", 0, version, catalog));
        if (!diff.empty()) issues.push_back("Complete(0) of an empty Document: " + diff);
    }
    // Building the text by edits gives the same results as SetText of the whole text.
    std::string text = kScriptTuning;
    for (size_t at = 0; at < text.size();) {
        size_t eol = text.find('\n', at);
        size_t end = eol == std::string::npos ? text.size() : eol + 1;
        doc.Edit(at, 0, text.substr(at, end - at));
        at = end;
    }
    if (doc.Text() != text) issues.push_back("Text() after line-by-line Edits differs from the text");
    else {
        std::string diff = SpanDiff(text, doc.Classify(0, text.size()), Classify(text, version));
        if (!diff.empty()) issues.push_back("Classify after line-by-line Edits: " + diff);
    }
    // SetText replaces everything; a following Edit applies to the new text.
    doc.SetText("SELECT 1;");
    doc.Edit(7, 1, "o.| FROM dbo.Orders o");
    std::string expected = "SELECT o.| FROM dbo.Orders o;";
    if (doc.Text() != expected) issues.push_back("Text() after SetText+Edit is " + Quote(doc.Text()));
    else {
        size_t caret = expected.find('|');
        doc.Edit(caret, 1, "");
        expected.erase(caret, 1);
        std::string diff = CompletionDiff(doc.Complete(caret, catalog), Complete(expected, caret, version, catalog));
        if (!diff.empty()) issues.push_back("Complete after SetText+Edit: " + diff);
    }
    for (const auto& i : issues) std::printf("FAIL document basics: %s\n", i.c_str());
    return issues.size();
}

// Returns the number of failed runs.
size_t RunDocumentSuite(SqlVersion version, const Catalog& catalog, const std::string& filter, bool verbose) {
    std::string big;
    for (int k = 0; k < 6; ++k) big += std::string(kScriptTuning) + kScriptProcedure;
    struct Plan {
        std::string name;
        std::vector<std::string> bases;
        std::vector<uint64_t> seeds;
        size_t macros;
    };
    const Plan plans[] = {
        {"tuning", {kScriptTuning, kScriptProcedure}, {1, 2, 3}, 60},
        {"procedure", {kScriptProcedure, kScriptTuning}, {4, 5, 6}, 60},
        {"large", {big}, {7, 8}, 30},
    };
    size_t failures = 0, runs = 0;
    if (filter.empty() || std::string("basics").find(filter) != std::string::npos) {
        ++runs;
        failures += RunDocumentBasics(version, catalog) ? 1 : 0;
    }
    for (const auto& plan : plans) {
        for (uint64_t seed : plan.seeds) {
            std::string id = plan.name + ":" + std::to_string(seed);
            if (!filter.empty() && id.find(filter) == std::string::npos) continue;
            ++runs;
            DocumentRun run(id, version, catalog, seed, plan.bases);
            if (!run.Run(plan.macros, verbose)) ++failures;
        }
    }
    std::printf("document %s: %zu runs, %zu failed\n", tsql::GrammarName(version), runs, failures);
    return failures;
}

template <typename F>
double MedianMs(int reps, F&& f) {
    std::vector<double> times;
    for (int i = 0; i < reps; ++i) {
        auto t0 = std::chrono::steady_clock::now();
        f();
        times.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

// Document::Classify(start, end) costs in proportion to the range, not the document: after a one-character edit
// in a ~200 KB script, classifying a 4 KB window must be much cheaper than classifying the whole text with the free
// function (a generous factor, so that machine load does not make it flaky). Document::Complete timings are
// printed for information. Returns the number of failures.
size_t RunDocumentPerf(SqlVersion version, const Catalog& catalog) {
#ifndef NDEBUG
    std::printf("document perf skipped: assertions enabled (not an optimised build)\n");
    (void)version;
    (void)catalog;
    return 0;
#endif
    std::string text;
    while (text.size() < 200 * 1024) text += std::string(kScriptTuning) + kScriptProcedure;
    Document doc(version);
    doc.SetText(text);
    size_t mid = text.find("WHERE o.OrderDate", text.size() / 2);
    size_t edit = mid + 6;  // inside "o.OrderDate"
    doc.Classify(0, text.size());
    doc.Complete(edit, catalog);
    Complete(text, edit, version, catalog);
    double freeClassify = MedianMs(3, [&] { Classify(text, version); });
    double freeComplete = MedianMs(3, [&] { Complete(text, edit + 2, version, catalog); });
    bool toggle = false;
    auto editOnce = [&] {
        // Alternately insert and remove one character in the identifier.
        if ((toggle = !toggle)) doc.Edit(edit, 0, "x");
        else doc.Edit(edit, 1, "");
    };
    double rangeClassify = MedianMs(15, [&] {
        editOnce();
        doc.Classify(edit - 2048, edit + 2048);
    });
    double docComplete = MedianMs(15, [&] {
        editOnce();
        doc.Complete(edit + 2, catalog);
    });
    std::printf("document perf %s, %zu bytes: free Classify %.1f ms; edit + Classify(4 KB window) %.2f ms; "
                "free Complete %.1f ms; edit + Document::Complete %.2f ms\n",
                tsql::GrammarName(version), text.size(), freeClassify, rangeClassify, freeComplete, docComplete);
    if (rangeClassify * 5 > freeClassify) {
        std::printf("FAIL document perf: Classify of a 4 KB window after an edit takes %.2f ms, more than a fifth of "
                    "classifying all %zu bytes (%.1f ms)\n", rangeClassify, text.size(), freeClassify);
        return 1;
    }
    return 0;
}

int Usage() {
    std::fprintf(stderr, "usage: test_document [--suite document|perf] [--filter TEXT] [--as-version NAME] [--verbose] CASES_DIR\n");
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);  // keep the edit history printed before a crash
    bool verbose = false;
    std::string suite = "document", filter, dir, versionName = "TSql170";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--verbose" || a == "-v") verbose = true;
        else if (a == "--suite" && i + 1 < argc) suite = argv[++i];
        else if (a == "--filter" && i + 1 < argc) filter = argv[++i];
        else if (a == "--as-version" && i + 1 < argc) versionName = argv[++i];
        else if (!a.empty() && a[0] != '-' && dir.empty()) dir = a;
        else return Usage();
    }
    if (dir.empty() || (suite != "document" && suite != "perf")) return Usage();
    Errors errors;
    auto catalog = LoadCatalog(dir + "/catalog.json", errors);
    Json spec;
    spec.type = Json::Type::Object;
    Json name;
    name.type = Json::Type::String;
    name.str = versionName;
    spec.obj.emplace_back("version", std::move(name));
    SqlVersion version{};
    std::string ignored;
    ParseVersion(spec, "--as-version", ignored, version, errors);
    if (!errors.empty() || !catalog) {
        errors.print();
        return 1;
    }
    if (!tsql::IsParserAvailable(version)) {
        std::printf("skipped: no %s parser in this build\n", versionName.c_str());
        return 0;
    }
    size_t failures = suite == "perf" ? RunDocumentPerf(version, *catalog) : RunDocumentSuite(version, *catalog, filter, verbose);
    return failures == 0 ? 0 : 1;
}
