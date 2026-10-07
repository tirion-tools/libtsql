// tsql_dump [--root DIR] <out-dir> <file>...
// Parses each file with tsql::parse (TSql170, QUOTED_IDENTIFIER on) and writes the shared dump
// format to <out-dir>/<path relative to --root>.json (file name only without --root), the same
// naming the Oracle uses. Files are decoded like .NET's StreamReader (BOM detection, UTF-8 default).
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "ScriptFile.h"
#include "tsql/parser.hpp"

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() == 1 && args[0] == "--list-versions") {   // the grammars this build contains
        for (int v = 0; v <= static_cast<int>(tsql::SqlVersion::Sql180); ++v)
            if (tsql::IsParserAvailable(static_cast<tsql::SqlVersion>(v)))
                std::cout << tsql::GrammarName(static_cast<tsql::SqlVersion>(v)) << "\n";
        return 0;
    }
    std::string root;
    tsql::SqlVersion version = tsql::SqlVersion::Sql170;
    while (args.size() >= 2 && (args[0] == "--root" || args[0] == "--version")) {
        if (args[0] == "--root") {
            root = args[1];
        } else if (!tsql::SqlVersionFromGrammarName(args[1], version) || !tsql::IsParserAvailable(version)) {
            std::cerr << "tsql_dump: no parser for --version " << args[1] << " in this build\n";
            return 2;
        }
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.size() < 2) {
        std::cerr << "usage: tsql_dump [--root DIR] [--version TSql130|...|TSql180|TSqlFabricDW (default TSql170)] "
                     "<out-dir> <file>...\n       tsql_dump --list-versions\n";
        return 2;
    }
    fs::path outDir = args[0];
    int failures = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        fs::path src = args[i];
        std::ifstream in(src, std::ios::binary);
        if (!in) {
            std::cerr << "tsql_dump: cannot read " << src << "\n";
            ++failures;
            continue;
        }
        std::stringstream ss;
        ss << in.rdbuf();
        std::string text = tsql::tools::DecodeScriptFile(ss.str());

        tsql::ParseResult r = tsql::parse(text, version, true);
        std::vector<tsql::ast::DumpError> errors;
        for (const auto& e : r.errors) errors.push_back({e.Number, e.Offset, e.Line, e.Column, e.Message});
        std::string json = tsql::ast::DumpJson(r.script, errors);

        fs::path rel = root.empty() ? src.filename() : fs::relative(fs::absolute(src), fs::absolute(root));
        fs::path out = outDir / (rel.string() + ".json");
        fs::create_directories(out.parent_path());
        std::ofstream o(out, std::ios::binary);
        o << json;
        if (!o) {
            std::cerr << "tsql_dump: cannot write " << out << "\n";
            ++failures;
        }
    }
    return failures ? 1 : 0;
}
