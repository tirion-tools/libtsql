// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/TSqlParseErrorException.cs
#pragma once

#include <cstddef>
#include <exception>
#include <string>

#include "tsql/parser.hpp"

namespace tsql::parser {

using ::tsql::ParseError;

class TSqlParseErrorException : public std::exception {
public:
    explicit TSqlParseErrorException(::tsql::ParseError error, bool doNotLog = false)
        : ParseError(std::move(error)), DoNotLog(doNotLog) {}
    /// new TSqlParseErrorException(null, doNotLog): no error to report
    TSqlParseErrorException(std::nullptr_t, bool doNotLog) : DoNotLog(doNotLog) {}
    const char* what() const noexcept override { return ParseError.Message.c_str(); }

    ::tsql::ParseError ParseError;
    bool DoNotLog;
};

}  // namespace tsql::parser
