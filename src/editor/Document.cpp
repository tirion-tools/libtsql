// tsql::editor::Document (a Buffer), the free functions (a Document used once) and Warm.
#include <array>
#include <string>

#include "Editor.h"

namespace tsql::editor {

using namespace detail;

struct Document::Impl {
    explicit Impl(const Grammar& g) : buffer(g) {}
    Buffer buffer;
};

Document::Document(SqlVersion version) : impl_(std::make_unique<Impl>(GrammarFor(version))) {}
Document::~Document() = default;
Document::Document(Document&&) noexcept = default;
Document& Document::operator=(Document&&) noexcept = default;

void Document::SetText(std::string_view sql) { impl_->buffer.SetText(sql); }

void Document::Edit(size_t start, size_t length, std::string_view replacement) {
    impl_->buffer.Edit(start, length, replacement);
}

const std::string& Document::Text() const { return impl_->buffer.Text(); }

CompletionResult Document::Complete(size_t caret, const Catalog& catalog) {
    return CompleteAt(impl_->buffer, caret, catalog);
}

std::vector<ColouredSpan> Document::Classify(size_t start, size_t end) { return ClassifyRange(impl_->buffer, start, end); }

bool Document::ParseAhead(std::chrono::steady_clock::duration budget) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();
    const auto deadline = budget >= Clock::time_point::max() - now ? Clock::time_point::max() : now + budget;
    return impl_->buffer.ParseAhead(deadline);
}

CompletionResult Complete(std::string_view sql, size_t caret, SqlVersion version, const Catalog& catalog) {
    Document d(version);
    d.SetText(sql);
    return d.Complete(caret, catalog);
}

std::vector<ColouredSpan> Classify(std::string_view sql, SqlVersion version) {
    Document d(version);
    d.SetText(sql);
    return d.Classify(0, sql.size());
}

namespace {

/// The statements of a performance-tuning session, for Warm: parsing them fills the prediction
/// caches the parser needs for scripts like these.
constexpr std::string_view kWarmScript = R"sql(SET NOCOUNT ON;
SET STATISTICS IO, TIME ON;
SET TRANSACTION ISOLATION LEVEL READ UNCOMMITTED;
USE Sales;
DECLARE @since datetime2 = DATEADD(day, -30, SYSDATETIME()), @region nvarchar(50) = N'West', @n int = 0;
DECLARE @t TABLE (Id int NOT NULL PRIMARY KEY, Amount money NULL, Note varchar(100));
SELECT TOP (100) c.CustomerName, SUM(o.TotalDue) AS Spend, COUNT(*) AS Orders, MAX(o.OrderDate) AS LastOrder
FROM dbo.Customers AS c WITH (NOLOCK)
INNER JOIN dbo.Orders AS o ON o.CustomerID = c.CustomerID
LEFT OUTER JOIN sales.Invoices i ON i.OrderID = o.OrderID AND i.Amount > 0
WHERE o.OrderDate >= @since AND c.Region = @region AND o.Status IN (1, 2, 3) AND c.Name LIKE N'A%'
  AND EXISTS (SELECT 1 FROM dbo.Payments p WHERE p.OrderID = o.OrderID) AND o.TotalDue BETWEEN 10 AND 100
GROUP BY c.CustomerName
HAVING SUM(o.TotalDue) > 1000
ORDER BY Spend DESC, c.CustomerName
OPTION (RECOMPILE, MAXDOP 1, OPTIMIZE FOR UNKNOWN);
WITH recent AS (SELECT OrderID, CustomerID, TotalDue,
    ROW_NUMBER() OVER (PARTITION BY CustomerID ORDER BY OrderDate DESC) AS rn FROM dbo.Orders WHERE Status = 1)
SELECT r.CustomerID, r.TotalDue, CASE WHEN r.TotalDue > 100 THEN 'big' ELSE 'small' END AS Size,
    CAST(r.TotalDue AS decimal(18, 2)) AS Due, CONVERT(varchar(10), GETDATE(), 120) AS Today, ISNULL(r.rn, 0) AS rn
FROM recent r CROSS APPLY dbo.fn_Lines(r.OrderID) AS l OUTER APPLY (SELECT TOP 1 x.Id FROM dbo.X x) AS y
WHERE r.rn = 1 UNION ALL SELECT 1, 2, 'a', 3, 'b', 4 EXCEPT SELECT 1, 2, 'a', 3, 'b', 4;
SELECT qs.execution_count, qs.total_worker_time / qs.execution_count AS avg_cpu, st.text, qp.query_plan,
    SUBSTRING(st.text, (qs.statement_start_offset / 2) + 1, ((CASE qs.statement_end_offset WHEN -1 THEN DATALENGTH(st.text)
    ELSE qs.statement_end_offset END - qs.statement_start_offset) / 2) + 1) AS statement_text
FROM sys.dm_exec_query_stats AS qs CROSS APPLY sys.dm_exec_sql_text(qs.sql_handle) AS st
CROSS APPLY sys.dm_exec_query_plan(qs.plan_handle) AS qp ORDER BY qs.total_worker_time DESC;
SELECT OBJECT_NAME(ips.object_id) AS tbl, ips.avg_fragmentation_in_percent
FROM sys.dm_db_index_physical_stats(DB_ID(), NULL, NULL, NULL, 'LIMITED') AS ips
JOIN sys.indexes AS ix ON ix.object_id = ips.object_id AND ix.index_id = ips.index_id WHERE ips.page_count > 1000;
CREATE TABLE #work (Id int IDENTITY(1, 1) NOT NULL, Amount money NULL DEFAULT 0, CONSTRAINT PK_w PRIMARY KEY CLUSTERED (Id));
INSERT INTO #work (Amount) SELECT TotalDue FROM dbo.Orders WHERE OrderDate < '2024-01-01';
INSERT INTO @t (Id, Amount) VALUES (1, 2.5), (2, NULL);
INSERT #work (Amount) EXEC dbo.usp_GetOrders @CustomerID = 1;
SELECT a.Id, b.Amount INTO #copy FROM #work a FULL JOIN @t b ON a.Id = b.Id;
UPDATE w SET w.Amount = w.Amount * 1.1, @n = @n + 1 FROM #work w JOIN dbo.Orders o ON o.OrderID = w.Id WHERE o.Status = 2;
UPDATE TOP (10) dbo.Orders SET Status = 3 OUTPUT inserted.OrderID, deleted.Status WHERE Status = 2;
DELETE FROM #work WHERE Amount IS NULL;
DELETE w FROM #work AS w WHERE NOT EXISTS (SELECT * FROM @t t WHERE t.Id = w.Id);
MERGE dbo.Targets AS t USING (SELECT Id, Amount FROM #work) AS s ON t.Id = s.Id
WHEN MATCHED AND s.Amount > 0 THEN UPDATE SET t.Amount = s.Amount
WHEN NOT MATCHED BY TARGET THEN INSERT (Id, Amount) VALUES (s.Id, s.Amount)
WHEN NOT MATCHED BY SOURCE THEN DELETE;
TRUNCATE TABLE #copy;
DROP TABLE IF EXISTS #work;
DROP TABLE #copy;
CREATE NONCLUSTERED INDEX IX_Orders_Date ON dbo.Orders (OrderDate DESC) INCLUDE (TotalDue) WHERE Status = 1
    WITH (ONLINE = ON, SORT_IN_TEMPDB = ON, DATA_COMPRESSION = PAGE);
CREATE STATISTICS st_Orders ON dbo.Orders (CustomerID, OrderDate) WITH FULLSCAN;
UPDATE STATISTICS dbo.Orders WITH SAMPLE 50 PERCENT;
ALTER INDEX ALL ON dbo.Orders REBUILD WITH (MAXDOP = 4);
ALTER TABLE dbo.Orders ADD Notes nvarchar(200) NULL;
EXEC sp_executesql N'SELECT * FROM dbo.Orders WHERE OrderID = @id', N'@id int', @id = 1;
EXEC dbo.usp_GetOrders 1, @Status = 2, @Count = @n OUTPUT;
EXECUTE ('SELECT 1');
DBCC FREEPROCCACHE;
DBCC SHOW_STATISTICS ('dbo.Orders', IX_Orders_Date);
CHECKPOINT;
IF EXISTS (SELECT 1 FROM sys.objects WHERE name = 'x') AND @n > 0
BEGIN
    PRINT 'yes';
    SET @n = @n + 1;
END
ELSE IF @n IS NULL SELECT 1 ELSE SELECT 2;
WHILE @n < 10
BEGIN
    SET @n += 1;
    IF @n = 5 CONTINUE;
    IF @n = 8 BREAK;
END;
BEGIN TRY
    BEGIN TRANSACTION;
    SELECT 1 / 0;
    COMMIT TRANSACTION;
END TRY
BEGIN CATCH
    IF @@TRANCOUNT > 0 ROLLBACK TRANSACTION;
    SELECT ERROR_NUMBER(), ERROR_MESSAGE();
    THROW;
END CATCH;
DECLARE c CURSOR LOCAL FAST_FORWARD FOR SELECT OrderID FROM dbo.Orders WHERE Status = 1;
OPEN c;
FETCH NEXT FROM c INTO @n;
CLOSE c;
DEALLOCATE c;
WAITFOR DELAY '00:00:01';
SELECT * FROM dbo.Orders ORDER BY OrderID OFFSET 10 ROWS FETCH NEXT 10 ROWS ONLY;
SELECT p.* FROM (SELECT CustomerID, Status, TotalDue FROM dbo.Orders) AS s
PIVOT (SUM(TotalDue) FOR Status IN ([1], [2])) AS p;
SELECT STRING_AGG(Name, ',') WITHIN GROUP (ORDER BY Name), COALESCE(NULL, 1), IIF(1 = 1, 'a', 'b'),
    LAG(TotalDue) OVER (ORDER BY OrderDate), DATEDIFF(ms, @since, SYSDATETIME()), TRY_CONVERT(int, '1')
FROM dbo.Customers FOR XML PATH('');
SELECT * FROM OPENJSON(@json) WITH (Id int '$.id');
GO
CREATE OR ALTER PROCEDURE dbo.usp_Report @from date, @to date = NULL, @rows int OUTPUT
WITH RECOMPILE
AS
BEGIN
    SET NOCOUNT ON;
    SELECT @rows = COUNT(*) FROM dbo.Orders WHERE OrderDate BETWEEN @from AND ISNULL(@to, GETDATE());
    RETURN 0;
END;
GO
CREATE OR ALTER VIEW dbo.vw_Spend WITH SCHEMABINDING AS SELECT CustomerID, COUNT_BIG(*) AS n FROM dbo.Orders GROUP BY CustomerID;
GO
CREATE OR ALTER FUNCTION dbo.fn_Lines (@order int) RETURNS TABLE AS RETURN (SELECT * FROM dbo.Lines WHERE OrderID = @order);
GO
)sql";

/// Carets for Warm's completions: after these texts of the script.
constexpr std::array<std::string_view, 12> kWarmCarets = {
    "SELECT TOP (100) c.", "FROM dbo.Customers AS c WITH (", "INNER JOIN ", "WHERE o.OrderDate >= @since AND ",
    "OPTION (", "UPDATE w SET ", "INSERT INTO #work (", "EXEC dbo.usp_GetOrders ", "CREATE NONCLUSTERED INDEX ",
    "DECLARE @t TABLE (Id ", "FROM sys.", "BEGIN\n    PRINT 'yes';\n"};

}  // namespace

void Warm(SqlVersion version) {
    Catalog catalog;
    catalog.currentDatabase = "Sales";
    catalog.databases = {"Sales"};
    catalog.objects.push_back({"Sales", "dbo", "Orders", CatalogObject::Type::Table, {{"OrderID", "int"}}, {}, {}});
    Document d(version);
    d.SetText(kWarmScript);
    d.Classify(0, kWarmScript.size());
    for (std::string_view after : kWarmCarets) {
        const size_t at = kWarmScript.find(after);
        if (at != std::string_view::npos) d.Complete(at + after.size(), catalog);
    }
}

}  // namespace tsql::editor
