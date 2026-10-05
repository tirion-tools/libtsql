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

#include "tsql/parser.hpp"

namespace fs = std::filesystem;

namespace {

void AppendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string Utf16ToUtf8(const std::string& bytes, size_t start, bool bigEndian) {
    std::string out;
    auto unit = [&](size_t i) -> char32_t {
        unsigned char a = static_cast<unsigned char>(bytes[i]), b = static_cast<unsigned char>(bytes[i + 1]);
        return bigEndian ? (a << 8 | b) : (b << 8 | a);
    };
    for (size_t i = start; i + 1 < bytes.size(); i += 2) {
        char32_t u = unit(i);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < bytes.size()) {
            char32_t lo = unit(i + 2);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                AppendUtf8(out, 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00));
                i += 2;
                continue;
            }
        }
        if (u >= 0xD800 && u <= 0xDFFF) u = 0xFFFD;
        AppendUtf8(out, u);
    }
    if (bytes.size() > start && (bytes.size() - start) % 2) AppendUtf8(out, 0xFFFD);
    return out;
}

std::string DecodeFile(const std::string& bytes) {
    auto has = [&](std::initializer_list<unsigned char> bom) {
        if (bytes.size() < bom.size()) return false;
        size_t i = 0;
        for (unsigned char c : bom)
            if (static_cast<unsigned char>(bytes[i++]) != c) return false;
        return true;
    };
    if (has({0xEF, 0xBB, 0xBF})) return bytes.substr(3);
    if (has({0xFF, 0xFE}) && !has({0xFF, 0xFE, 0x00, 0x00})) return Utf16ToUtf8(bytes, 2, false);
    if (has({0xFE, 0xFF})) return Utf16ToUtf8(bytes, 2, true);
    return bytes;   // UTF-8; invalid sequences become U+FFFD inside tsql::parse
}

}  // namespace

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
        std::string text = DecodeFile(ss.str());

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
