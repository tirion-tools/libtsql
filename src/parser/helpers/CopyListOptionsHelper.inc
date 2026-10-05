// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/CopyListOptionsHelper.cs
// Hand-written member of CopyListOptionsHelper (included into the generated class body). C#
// System.Exception/Argument*Exception are not parse errors: std::runtime_error/invalid_argument
// (internal error 46001).
    void AssignCopyColumnOptions(::tsql::ast::CopyColumnOption* columnOption, ::tsql::ast::Identifier* columnName,
                                 ::tsql::ast::ScalarExpression* defaultColumnValue, bool defaultSpecified,
                                 ::tsql::ast::IntegerLiteral* fieldNumber, int columnCount) const {
        if (columnOption == nullptr) throw std::invalid_argument("Missing columnOption");
        if (columnName == nullptr) throw std::invalid_argument("Missing columnName");
        if (columnCount <= 0) throw std::invalid_argument("Column count should be greater than/equal to 1.");
        auto fail = [&](const char* what) {
            throw std::runtime_error(FormatMessage("{0} of column {1} in the list", what, columnCount));
        };
        columnOption->set_ColumnName(columnName);
        if (columnOption->ColumnName == nullptr || !columnOption->ColumnName->Value || columnOption->ColumnName->Value->empty())
            fail("Invalid column name");
        columnOption->set_DefaultValue(defaultSpecified ? defaultColumnValue : nullptr);
        if (defaultSpecified && columnOption->DefaultValue == nullptr) fail("Null default value");
        columnOption->set_FieldNumber(fieldNumber);
        int result = 0;
        if (columnOption->FieldNumber != nullptr && (!Int32_TryParse(columnOption->FieldNumber->Value, result) || result <= 0))
            fail("Invalid field number");
    }
