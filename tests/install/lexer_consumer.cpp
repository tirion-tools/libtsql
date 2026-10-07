// Installed-package check of the lexer (tsql::tsql): see CMakeLists.txt.
#include <tsql/tsql.hpp>

#include <cstdio>
#include <string>
#include <unordered_map>

int main() {
    const std::string sql = "SELECT [Customer].name FROM Customer";
    const auto tokens = tsql::tokenize(sql);
    const std::string out = tsql::anonymize_identifiers(sql, {{"customer", "Tbl_1"}, {"name", "Col_1"}});
    std::printf("tokens: %zu\nanonymized: %s\n", tokens.size(), out.c_str());
    if (tokens.empty() || !tsql::is_keyword("select") || out != "SELECT [Tbl_1].Col_1 FROM Tbl_1") {
        std::fprintf(stderr, "lexer_consumer: unexpected result\n");
        return 1;
    }
    return 0;
}
