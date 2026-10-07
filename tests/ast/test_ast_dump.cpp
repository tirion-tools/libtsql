// SPDX-License-Identifier: MIT
// libtsql AST: hand-built AST -> DumpJson text (format shared with the .NET oracle).
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

// "SET ANSI_NULLS, QUOTED_IDENTIFIER ON;\nN'é𝄞'" — offsets are UTF-16 code units.
ScriptTokenStream MakeTokens() {
    return {
        {TSqlTokenType::Set, 0, 1, 1, "SET"},
        {TSqlTokenType::WhiteSpace, 3, 1, 4, " "},
        {TSqlTokenType::Identifier, 4, 1, 5, "ANSI_NULLS"},
        {TSqlTokenType::Comma, 14, 1, 15, ","},
        {TSqlTokenType::WhiteSpace, 15, 1, 16, " "},
        {TSqlTokenType::Identifier, 16, 1, 17, "QUOTED_IDENTIFIER"},
        {TSqlTokenType::WhiteSpace, 33, 1, 34, " "},
        {TSqlTokenType::On, 34, 1, 35, "ON"},
        {TSqlTokenType::Semicolon, 36, 1, 37, ";"},
        {TSqlTokenType::WhiteSpace, 37, 1, 38, "\n"},
        {TSqlTokenType::UnicodeStringLiteral, 38, 2, 1, "N'\xC3\xA9\xF0\x9D\x84\x9E'"},  // N'é𝄞' = 6 UTF-16 units
        {TSqlTokenType::EndOfFile, 44, 2, 7, ""},
    };
}

}  // namespace

int main() {
    ScriptTokenStream tokens = MakeTokens();
    FragmentFactory factory(&tokens);

    // SET ANSI_NULLS, QUOTED_IDENTIFIER ON  — a [Flags] enum combination and a bool.
    auto* set = factory.CreateFragment<PredicateSetStatement>();
    set->UpdateTokenInfo(0, 7);
    set->set_Options(SetOptions::AnsiNulls | SetOptions::QuotedIdentifier);
    set->set_IsOn(true);

    auto* batch = factory.CreateFragment<TSqlBatch>();
    batch->Statements.push_back(set);
    batch->UpdateTokenInfo(set);
    auto* emptyBatch = factory.CreateFragment<TSqlBatch>();  // empty collection, no position

    auto* script = factory.CreateFragment<TSqlScript>();
    script->Batches.push_back(batch);
    script->Batches.push_back(emptyBatch);
    script->UpdateTokenInfo(batch);

    // Null vs empty strings, string escaping, UTF-16 positions, setter via an Ast.xml interface.
    auto* collation = factory.CreateFragment<Identifier>();  // Value stays null
    collation->UpdateTokenInfo(11, 11);
    auto* literal = factory.CreateFragment<StringLiteral>();
    literal->UpdateTokenInfo(10, 10);
    literal->set_Value(std::string("\xC3\xA9\xF0\x9D\x84\x9E\"\\\t\x01"));
    literal->set_IsNational(true);
    ICollationSetter* collationSetter = literal;
    collationSetter->set_Collation(collation);  // UpdateTokenInfo extends the literal to token 11
    auto* emptyLiteral = factory.CreateFragment<StringLiteral>();
    emptyLiteral->set_Value(std::string());

    expect_eq(DumpJson(script, {{46010, 38, 2, 1, "Incorrect syntax near 'x'."}}),
              "{\"errors\":[{\"Number\":46010,\"Offset\":38,\"Line\":2,\"Column\":1,"
              "\"Message\":\"Incorrect syntax near 'x'.\"}],"
              "\"tree\":{\"$type\":\"TSqlScript\",\"$pos\":[0,7,0,36],\"Batches\":["
              "{\"$type\":\"TSqlBatch\",\"$pos\":[0,7,0,36],\"Statements\":["
              "{\"$type\":\"PredicateSetStatement\",\"$pos\":[0,7,0,36],\"IsOn\":true,"
              "\"Options\":\"QuotedIdentifier, AnsiNulls\"}]},"
              "{\"$type\":\"TSqlBatch\",\"$pos\":[-1,-1,-1,-1],\"Statements\":[]}],"
              "\"TrailingGoCount\":0}}",
              "script with error");

    expect_eq(DumpNodeJson(literal),
              "{\"$type\":\"StringLiteral\",\"$pos\":[10,11,38,6],"
              "\"Collation\":{\"$type\":\"Identifier\",\"$pos\":[11,11,44,0],\"Value\":null,\"QuoteType\":\"NotQuoted\"},"
              "\"Value\":\"\xC3\xA9\xF0\x9D\x84\x9E\\\"\\\\\\u0009\\u0001\",\"IsNational\":true,\"IsLargeObject\":false}",
              "literal with null-valued identifier");
    expect_eq(DumpNodeJson(emptyLiteral),
              "{\"$type\":\"StringLiteral\",\"$pos\":[-1,-1,-1,-1],\"Collation\":null,\"Value\":\"\","
              "\"IsNational\":false,\"IsLargeObject\":false}",
              "literal with empty value");
    expect_eq(DumpJson(nullptr), "{\"errors\":[],\"tree\":null}", "null tree");

    // .NET Enum.ToString edge cases.
    expect_eq(ToString(SetOptions::None), "None", "flags zero");
    expect_eq(ToString(static_cast<SetOptions>(0x40000001)), "1073741825", "flags undefined bit");
    expect_eq(ToString(PayloadOptionKinds::Authentication | PayloadOptionKinds::Encryption | PayloadOptionKinds::Role),
              "DatabaseMirroringOptions", "flags exact composite name");
    expect_eq(ToString(static_cast<QuoteType>(7)), "7", "plain undefined value");
    expect_eq(ToString(SensitivityClassification::OptionType::Undefined), "Undefined", "negative value");
    expect_eq(ToString(TSqlTokenType::EndOfFile), "EndOfFile", "token type");

    if (g_failed == 0) std::cout << "tsql_ast_dump: all passed\n";
    return g_failed == 0 ? 0 : 1;
}
