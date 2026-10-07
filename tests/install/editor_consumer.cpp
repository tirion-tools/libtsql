// Installed-package check of the parser and the editor support (tsql::editor): see CMakeLists.txt.
#include <tsql/editor.hpp>
#include <tsql/parser.hpp>

#include <cstdio>
#include <string>

int main() {
    const std::string script = "SELECT name FROM dbo.Customers;\nGO\nUPDATE dbo.Customers SET name = N'x'";
    const tsql::ParseResult r = tsql::parse(script, tsql::SqlVersion::Sql170);
    if (!r.script || !r.errors.empty() || r.script->Batches.size() != 2) {
        std::fprintf(stderr, "editor_consumer: unexpected parse result\n");
        return 1;
    }
    std::printf("parse: %zu batches, %zu errors, first statement %s\n", r.script->Batches.size(),
                r.errors.size(), r.script->Batches[0]->Statements[0]->TypeName());

    tsql::editor::Catalog catalog;
    catalog.currentDatabase = "Sales";
    catalog.databases = {"Sales"};
    tsql::editor::CatalogObject customers;
    customers.database = "Sales";
    customers.schema = "dbo";
    customers.name = "Customers";
    customers.type = tsql::editor::CatalogObject::Type::Table;
    customers.columns = {{"id", "int"}, {"name", "nvarchar(100)"}};
    catalog.objects.push_back(customers);

    const std::string sql = "SELECT * FROM dbo.Cu";
    const tsql::editor::CompletionResult c =
        tsql::editor::Complete(sql, sql.size(), tsql::SqlVersion::Sql170, catalog);
    std::printf("complete at %zu: replace [%zu,+%zu), %zu items\n", sql.size(), c.replaceStart,
                c.replaceLength, c.items.size());
    bool found = false;
    for (const auto& item : c.items) {
        std::printf("  %s\n", item.label.c_str());
        found = found || (item.kind == tsql::editor::CompletionKind::Table && item.label == "Customers");
    }
    if (!found || c.replaceStart != sql.size() - 2 || c.replaceLength != 2) {
        std::fprintf(stderr, "editor_consumer: unexpected completion result\n");
        return 1;
    }
    return 0;
}
