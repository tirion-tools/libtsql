// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/OptionsHelper.cs
// (parsing half only; libtsql has no script generator, so the script-generation members are not ported).
#pragma once

#include <map>
#include <string>

#include "antlr4-runtime.h"
#include "CsCompat.h"
#include "ParseError.h"
#include "ParserErrors.h"
#include "generated/support/ParserEnums.h"

namespace tsql::parser {

template <class OptionType>
class OptionsHelper {
public:
    virtual ~OptionsHelper() = default;

    bool IsValidKeyword(antlr4::Token* token) const { return _stringToOptionInfo.count(AsciiUpper(token->getText())) != 0; }

    virtual OptionType ParseOption(antlr4::Token* token, SqlVersionFlags version) const {
        auto it = _stringToOptionInfo.find(AsciiUpper(token->getText()));
        if (it != _stringToOptionInfo.end() && IsValidIn(it->second, version)) return it->second.value;
        throw GetMatchingException(token);
    }
    OptionType ParseOption(antlr4::Token* token) const { return ParseOption(token, SqlVersionFlags::TSqlAll); }

    bool TryParseOption(antlr4::Token* token, SqlVersionFlags version, OptionType& returnValue) const {
        return TryParseOption(token->getText(), version, returnValue);
    }
    bool TryParseOption(const std::string& tokenString, SqlVersionFlags version, OptionType& returnValue) const {
        auto it = _stringToOptionInfo.find(AsciiUpper(tokenString));
        if (it != _stringToOptionInfo.end() && IsValidIn(it->second, version)) {
            returnValue = it->second.value;
            return true;
        }
        returnValue = OptionType{};
        return false;
    }
    bool TryParseOption(antlr4::Token* token, OptionType& returnValue) const {
        return TryParseOption(token, SqlVersionFlags::TSqlAll, returnValue);
    }

protected:
    struct OptionInfo {
        OptionType value;
        SqlVersionFlags validVersions;
    };

    // Identifier- and keyword-backed mappings both reduce to a case-insensitive string key
    // (C#: StringComparer.OrdinalIgnoreCase; keyword mappings use the token's lower-case text).
    void AddOptionMapping(OptionType option, const std::string& identifier, SqlVersionFlags validVersions) {
        _stringToOptionInfo.emplace(AsciiUpper(identifier), OptionInfo{option, validVersions});
    }
    void AddOptionMapping(OptionType option, const std::string& identifier) {
        AddOptionMapping(option, identifier, SqlVersionFlags::TSqlAll);
    }

    virtual TSqlParseErrorException GetMatchingException(antlr4::Token* token) const {
        return GetUnexpectedTokenErrorException(token);
    }

private:
    static bool IsValidIn(const OptionInfo& info, SqlVersionFlags version) {
        return (info.validVersions & version) != SqlVersionFlags{};
    }
    std::map<std::string, OptionInfo> _stringToOptionInfo;
};

}  // namespace tsql::parser
