// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/CopyIdentifierOrValueOptionsHelper.cs
// Hand-written members of CopyIdentifierOrValueOptionsHelper (included into the generated class body).
// C# ArgumentNullException/ArgumentOutOfRangeException become std::invalid_argument (the parser
// reports them as internal error 46001, like ParseRuleWithStandardExceptionHandling).
    static constexpr int maxLength = 4000;
    static constexpr int MaxAsciiCharValue = 127;
    static constexpr int fieldQuoteMaxLength = 1;
    static constexpr int hexDecimalFieldQuoteMaxLen = 4;

    static TSqlParseErrorException Error(int number, std::string_view templ, const std::string& arg) {
        return TSqlParseErrorException(::tsql::ParseError{number, 1, 1, 1, FormatMessage(templ, arg)});
    }

    void AssignValueToCopyOption(::tsql::ast::CopyOption* copyOption, ::tsql::ast::SingleValueTypeCopyOption* singleValueTypeOption) const {
        using K = ::tsql::ast::CopyOptionKind;
        if (copyOption == nullptr) throw std::invalid_argument("copyOption");
        if (singleValueTypeOption == nullptr) throw std::invalid_argument("singleValueTypeOption");
        if (singleValueTypeOption->SingleValue == nullptr) throw std::invalid_argument("singleValueTypeOption.SingleValue");
        auto* single = singleValueTypeOption->SingleValue;
        std::string literalValue;
        switch (copyOption->Kind) {
            case K::File_Format: {
                auto* identifier = single->Identifier;
                if (identifier == nullptr || !identifier->Value)
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::FormatType);
                CheckForEmptyValues(CodeGenerationSupporter::FileFormat, Utf16Len(*identifier->Value));
                break;
            }
            case K::File_Type: {
                literalValue = ValidateAsStringLiteral(CodeGenerationSupporter::FileType, single->ValueExpression, true);
                CopyCommandFileFormatType fileType;
                if (!Enum_TryParse(literalValue, true, fileType))
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::FileType);
                break;
            }
            case K::ErrorFile:
                literalValue = ValidateAsStringLiteral(CodeGenerationSupporter::ErrorFile, single->ValueExpression, true);
                ExternalFileOption::CheckXMLValidity(literalValue, CodeGenerationSupporter::ErrorFile);
                break;
            case K::MaxErrors: {
                auto maxErrors = single->Value();
                int maxErrorsIntValue = 0;
                if (!maxErrors || !Int32_TryParse(*maxErrors, maxErrorsIntValue) || maxErrorsIntValue < 0)
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::MaxErrors);
                break;
            }
            case K::Compression: {
                literalValue = ValidateAsStringLiteral(CodeGenerationSupporter::Compression, single->ValueExpression, true);
                CopyCommandCompressionType compression;
                if (!Enum_TryParse(literalValue, true, compression))
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::Compression);
                break;
            }
            case K::FieldQuote:
                ValidateDelimiters(CodeGenerationSupporter::FieldQuote, single->ValueExpression);
                break;
            case K::FieldTerminator:
                ValidateDelimiters(CodeGenerationSupporter::FieldTerminator, single->ValueExpression);
                break;
            case K::RowTerminator:
                ValidateDelimiters(CodeGenerationSupporter::RowTerminator, single->ValueExpression);
                break;
            case K::FirstRow: {
                auto firstRow = single->Value();
                int firstRowIntValue = 0;
                if (!firstRow || !Int32_TryParse(*firstRow, firstRowIntValue) || firstRowIntValue <= 0)
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::FirstRow);
                break;
            }
            case K::DateFormat: {
                literalValue = ValidateAsStringLiteral(CodeGenerationSupporter::DateFormat, single->ValueExpression, true);
                CopyCommandDateFormat dateFormat;
                if (!Enum_TryParse(literalValue, true, dateFormat))
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::DateFormat);
                break;
            }
            case K::Encoding: {
                literalValue = ValidateAsStringLiteral(CodeGenerationSupporter::Encoding, single->ValueExpression, true);
                CopyCommandEncoding encoding;
                if (!Enum_TryParse(literalValue, true, encoding))
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::Encoding);
                break;
            }
            case K::Identity_Insert: {
                literalValue = ValidateAsStringLiteral(CodeGenerationSupporter::Identity_Insert, single->ValueExpression, true);
                CopyCommandIdentityInsert identityValue;
                if (!Enum_TryParse(literalValue, true, identityValue))
                    throw Error(46130, TSqlParserResource::SQL46130Message, CodeGenerationSupporter::Identity_Insert);
                break;
            }
            default:
                throw std::invalid_argument("Out of range argument");
        }
        copyOption->set_Value(singleValueTypeOption);
    }

    bool ValidateCopyCredential(::tsql::ast::CopyCredentialOption* credentialOption) const {
        CopyCommandCredentialType credType;
        if (GetCredentialType(CsStr(credentialOption->Identity->Value).get(), credType)) {
            switch (credType) {
                case CopyCommandCredentialType::Sas:
                case CopyCommandCredentialType::AccountKey:
                case CopyCommandCredentialType::ServicePrincipal:
                    if (credentialOption->Secret != nullptr && Utf16Len(Str_Trim(credentialOption->Secret->Value)) != 0) return true;
                    break;
                case CopyCommandCredentialType::ManagedIdentity:
                    if (credentialOption->Secret == nullptr) return true;
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    void ValidateSecret(::tsql::ast::ValueExpression* valueExpression) const {
        std::string value = ValidateAsStringLiteral(CodeGenerationSupporter::Secret, valueExpression);
        if (Utf16Len(value) > maxLength) throw Error(46131, TSqlParserResource::SQL46131Message, CodeGenerationSupporter::Secret);
        if (!value.empty()) {
            ExternalFileOption::CheckXMLValidity(value, CodeGenerationSupporter::Secret);
            CheckForInvalidChars(value);
        }
    }

    static void CheckForInvalidChars(const std::string& key) {
        for (unsigned char c : key)
            if (c > MaxAsciiCharValue)
                throw TSqlParseErrorException(::tsql::ParseError{46132, 1, 1, 1, FormatMessage(TSqlParserResource::SQL46132Message)});
    }

    static void CheckForEmptyValues(const std::string& optionName, int length) {
        if (length <= 0) throw Error(46131, TSqlParserResource::SQL46131Message, optionName);
    }

    void ValidateDelimiters(const std::string& optionName, ::tsql::ast::ValueExpression* valueExpression) const {
        int length = maxLength;
        const bool fieldQuote = optionName == CodeGenerationSupporter::FieldQuote;
        std::string value = ValidateAsStringLiteral(optionName, valueExpression, !fieldQuote);
        if (fieldQuote) {
            if (Str_StartsWith(value, "0x", StringComparison::OrdinalIgnoreCase))
                length = hexDecimalFieldQuoteMaxLen;
            else
                length = value.empty() ? 0 : fieldQuoteMaxLength;
        }
        if (Utf16Len(value) > length) throw Error(46131, TSqlParserResource::SQL46131Message, optionName);
        ExternalFileOption::CheckDelimiterValidity(value, optionName);
    }

    std::string ValidateAsStringLiteral(const std::string& optionName, ::tsql::ast::ValueExpression* valueExpression,
                                        bool checkForEmptyValues) const {
        std::string value = ValidateAsStringLiteral(optionName, valueExpression);
        if (checkForEmptyValues) CheckForEmptyValues(optionName, Utf16Len(value));
        return value;
    }

    std::string ValidateAsStringLiteral(const std::string& optionName, ::tsql::ast::ValueExpression* valueExpression) const {
        auto* literal = dynamic_cast<::tsql::ast::StringLiteral*>(valueExpression);
        if (literal == nullptr || !literal->Value) throw Error(46130, TSqlParserResource::SQL46130Message, optionName);
        return *literal->Value;
    }

    static bool GetCredentialType(std::string_view credentialIdentity, CopyCommandCredentialType& credentialType) {
        credentialType = CopyCommandCredentialType::None;
        static const std::pair<const char*, CopyCommandCredentialType> kMap[] = {
            {"Shared Access Signature", CopyCommandCredentialType::Sas},
            {"Storage Account Key", CopyCommandCredentialType::AccountKey},
            {"Managed Identity", CopyCommandCredentialType::ManagedIdentity},
            {"Service Principal", CopyCommandCredentialType::ServicePrincipal}};
        for (const auto& [k, v] : kMap)
            if (EqualsIgnoreCase(credentialIdentity, k)) {
                credentialType = v;
                return true;
            }
        return false;
    }
