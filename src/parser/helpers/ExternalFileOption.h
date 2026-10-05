// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/ExternalFileOption.cs
#pragma once

#include <regex>
#include <stdexcept>
#include <string>

#include "CsCompat.h"
#include "generated/support/CodeGenerationSupporter.h"
#include "tsql/ast/ast.hpp"

namespace tsql::parser {

struct ExternalFileOption {
    static void CheckXMLValidity(const ::tsql::ast::StringLiteral* sequence, const std::string& option) {
        if (sequence == nullptr) throw std::invalid_argument("ArgumentNullException: sequence");
        CheckXMLValidity(CsStr(sequence->Value).get(), option);
    }

    /// XmlConvert.IsXmlChar for every UTF-16 code unit (supplementary characters are surrogate
    /// pairs, which are not XML characters on their own).
    static void CheckXMLValidity(std::string_view sequence, const std::string& option) {
        for (size_t i = 0; i < sequence.size();) {
            unsigned char c = static_cast<unsigned char>(sequence[i]);
            char32_t cp;
            size_t n = c < 0x80 ? 0 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            cp = n == 0 ? c : (c & (0x3F >> n));
            for (size_t k = 1; k <= n && i + k < sequence.size(); ++k)
                cp = (cp << 6) | (static_cast<unsigned char>(sequence[i + k]) & 0x3F);
            i += n + 1;
            bool xml = cp == 0x9 || cp == 0xA || cp == 0xD || (cp >= 0x20 && cp <= 0xD7FF) || (cp >= 0xE000 && cp <= 0xFFFD);
            if (!xml)
                // System.Exception: not caught by the parser (an internal error, like C# would crash)
                throw std::runtime_error(FormatMessage(
                    "COPY statement failed because the value provided for option {0} has character(s) outside of XML 1.0 character range.",
                    option));
        }
    }

    static void CheckDelimiterValidity(const std::string& sequence, const std::string& option) {
        CheckXMLValidity(sequence, option);
        static const std::regex hexPrefix("^0[xX]");
        static const std::regex hexFull("^0[xX]([0-9a-fA-F]{1,4})+$");
        if (std::regex_search(sequence, hexPrefix) && !std::regex_search(sequence, hexFull))
            throw std::runtime_error(FormatMessage(
                "COPY statement failed because the value provided for option '{0}' is not a valid hexadecimal. Please use the correct hexadecimal format: 0x[*]+, where each * is in the range 0-9, a-f, A-F, and the length of * is between 1 and 4.",
                option));
        if (option == CodeGenerationSupporter::NullValuesOption && sequence.find("','") != std::string::npos)
            throw std::runtime_error(
                "COPY statement failed because NULL_VALUES cannot contain STRING_DELIMITER, FIELD_TERMINATOR, ROW_DELIMITER (\\n, \\r, \\r\\n) and / or ','.");
    }
};

}  // namespace tsql::parser
