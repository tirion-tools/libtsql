// Parse benchmark: tsql_bench [--version TSql160] <rounds> <file>...
// Parses every file once per round in one process and prints per-round wall
// time, so the first (cold, ANTLR DFA cache empty) and later (warm) rounds can
// be compared. Files are read as UTF-8 (strip a BOM).
#include "tsql/parser.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    tsql::SqlVersion version = tsql::SqlVersion::Sql170;
    int first = 1;
    if (argc > 2 && std::string(argv[1]) == "--version") {
        if (!tsql::SqlVersionFromGrammarName(argv[2], version) || !tsql::IsParserAvailable(version)) {
            std::fprintf(stderr, "tsql_bench: no parser for --version %s in this build\n", argv[2]);
            return 2;
        }
        first = 3;
    }
    if (argc < first + 2) {
        std::fprintf(stderr, "usage: tsql_bench [--version TSql160] <rounds> <file>...\n");
        return 2;
    }
    const int rounds = std::atoi(argv[first]);
    std::vector<std::string> inputs;
    size_t bytes = 0;
    for (int i = first + 1; i < argc; ++i) {
        std::ifstream in(argv[i], std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        std::string s = text.str();
        if (s.size() >= 3 && s.compare(0, 3, "\xEF\xBB\xBF") == 0) s.erase(0, 3);
        bytes += s.size();
        inputs.push_back(std::move(s));
    }
    for (int r = 0; r < rounds; ++r) {
        size_t errors = 0;
        const auto start = std::chrono::steady_clock::now();
        for (const auto& s : inputs) errors += tsql::parse(s, version).errors.empty() ? 0 : 1;
        const double ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        std::printf("round %d: %.1f ms for %zu files, %.0f KB (%.2f MB/s), %zu with errors\n",
                    r + 1, ms, inputs.size(), bytes / 1024.0, bytes / 1048576.0 / (ms / 1000.0), errors);
    }
    return 0;
}
