// SPDX-License-Identifier: MIT
// libtsql parser pilot: TSqlFragmentVisitor / TSqlConcreteFragmentVisitor over a hand-built tree.
// Expected logs match Microsoft.SqlServer.TransactSql.ScriptDom 180.117.0 for the same tree.
#include "tsql/ast/ast.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

int g_failed = 0;

void expect_eq(std::string_view got, std::string_view want, const char* label) {
    if (got != want) {
        ++g_failed;
        std::cerr << "FAIL " << label << "\n"
                  << "  got:  [" << got << "]\n"
                  << "  want: [" << want << "]\n";
    }
}

using namespace tsql::ast;

// Logs Visit(TSqlFragment*) as "F:<dynamic type>" and a few typed Visit overloads by name;
// each override then calls the base implementation, like `base.Visit(node)` in C#.
template <class Base>
class LoggingVisitor : public Base {
public:
    std::string log;

    void Visit(TSqlFragment* node) override { Add(std::string("F:") + node->TypeName()); }
    void Visit(IntegerLiteral* node) override { Add("IntegerLiteral=" + node->Value.value_or("")); Base::Visit(node); }
    void Visit(Identifier* node) override { Add("Identifier=" + node->Value.value_or("")); Base::Visit(node); }

protected:
    void Add(const std::string& entry) {
        if (!log.empty()) log += ' ';
        log += entry;
    }
};

// TSqlConcreteFragmentVisitor seals the abstract classes' overloads, so only the plain visitor
// can log them.
class FullLoggingVisitor : public LoggingVisitor<TSqlFragmentVisitor> {
public:
    using LoggingVisitor<TSqlFragmentVisitor>::Visit;
    void Visit(ScalarExpression*) override { Add("ScalarExpression"); }
    void Visit(PrimaryExpression*) override { Add("PrimaryExpression"); }
    void Visit(ValueExpression*) override { Add("ValueExpression"); }
    void Visit(Literal*) override { Add("Literal"); }
};

// Only ExplicitVisit overridden, as a VersioningVisitor-style subclass does.
class IdentifierCounter : public TSqlConcreteFragmentVisitor {
public:
    int identifiers = 0;
    void ExplicitVisit(Identifier* node) override {
        ++identifiers;
        TSqlConcreteFragmentVisitor::ExplicitVisit(node);
    }
};

Identifier* MakeIdentifier(FragmentFactory& f, const char* value) {
    auto* id = f.CreateFragment<Identifier>();
    id->Value = value;
    return id;
}

}  // namespace

int main() {
    FragmentFactory f;

    // SELECT 1 COLLATE c AS x FROM dbo.t AS a
    auto* literal = f.CreateFragment<IntegerLiteral>();
    literal->Value = "1";
    literal->Collation = MakeIdentifier(f, "c");
    auto* columnName = f.CreateFragment<IdentifierOrValueExpression>();
    columnName->Identifier = MakeIdentifier(f, "x");
    auto* element = f.CreateFragment<SelectScalarExpression>();
    element->Expression = literal;
    element->ColumnName = columnName;

    auto* name = f.CreateFragment<SchemaObjectName>();
    name->Identifiers = {MakeIdentifier(f, "dbo"), MakeIdentifier(f, "t")};
    auto* table = f.CreateFragment<NamedTableReference>();
    table->SchemaObject = name;
    table->Alias = MakeIdentifier(f, "a");  // dumped before SchemaObject, visited after it
    auto* from = f.CreateFragment<FromClause>();
    from->TableReferences.push_back(table);

    auto* query = f.CreateFragment<QuerySpecification>();
    query->SelectElements.push_back(element);
    query->FromClause = from;
    auto* select = f.CreateFragment<SelectStatement>();
    select->QueryExpression = query;

    // TSqlFragmentVisitor: ExplicitVisit(T) visits the base types (nearest first, TSqlFragment
    // last), then Visit(T), then the children in tools/AstGen order.
    FullLoggingVisitor all;
    select->Accept(&all);
    expect_eq(all.log,
              "F:SelectStatement F:QuerySpecification F:SelectScalarExpression "
              "Literal ValueExpression PrimaryExpression ScalarExpression F:IntegerLiteral IntegerLiteral=1 "
              "F:Identifier Identifier=c "
              "F:IdentifierOrValueExpression F:Identifier Identifier=x "
              "F:FromClause F:NamedTableReference F:SchemaObjectName "
              "F:Identifier Identifier=dbo F:Identifier Identifier=t F:Identifier Identifier=a",
              "TSqlFragmentVisitor via Accept");

    // TSqlConcreteFragmentVisitor: no base-type visits; Visit(T) falls back to Visit(TSqlFragment).
    LoggingVisitor<TSqlConcreteFragmentVisitor> concrete;
    select->Accept(&concrete);
    expect_eq(concrete.log,
              "F:SelectStatement F:QuerySpecification F:SelectScalarExpression "
              "IntegerLiteral=1 F:IntegerLiteral Identifier=c F:Identifier "
              "F:IdentifierOrValueExpression Identifier=x F:Identifier "
              "F:FromClause F:NamedTableReference F:SchemaObjectName "
              "Identifier=dbo F:Identifier Identifier=t F:Identifier Identifier=a F:Identifier",
              "TSqlConcreteFragmentVisitor via Accept");

    // AcceptChildren skips the node itself; ExplicitVisit dispatches on the static type.
    FullLoggingVisitor children;
    literal->AcceptChildren(&children);
    children.ExplicitVisit(static_cast<ScalarExpression*>(literal));
    expect_eq(children.log,
              "F:Identifier Identifier=c "
              "F:IntegerLiteral ScalarExpression F:Identifier Identifier=c",
              "AcceptChildren and static ExplicitVisit");

    IdentifierCounter counter;
    select->Accept(&counter);
    select->Accept(nullptr);  // no-op, as in C#
    if (counter.identifiers != 5) {
        ++g_failed;
        std::cerr << "FAIL ExplicitVisit override: " << counter.identifiers << " identifiers\n";
    }

    if (g_failed == 0) std::cout << "tsql_ast_visitor: all passed\n";
    return g_failed == 0 ? 0 : 1;
}
