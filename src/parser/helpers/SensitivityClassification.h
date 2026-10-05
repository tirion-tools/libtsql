// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/SensitivityClassification.cs
#pragma once

#include <string>

#include "CsCompat.h"
#include "generated/support/CodeGenerationSupporter.h"
#include "tsql/ast/ast.hpp"

namespace tsql::parser {

struct SensitivityClassification {
    using OptionType = ::tsql::ast::SensitivityClassification::OptionType;   // used by Ast.xml members
    enum class Rank { None = 0, Low = 10, Medium = 20, High = 30, Critical = 40 };

    static OptionType GetOptionTypeByName(CsStr option) {
        const std::string_view o = option.get();
        if (EqualsIgnoreCase(o, CodeGenerationSupporter::Label)) return OptionType::Label;
        if (EqualsIgnoreCase(o, CodeGenerationSupporter::LabelId)) return OptionType::LabelId;
        if (EqualsIgnoreCase(o, CodeGenerationSupporter::InformationType)) return OptionType::InformationType;
        if (EqualsIgnoreCase(o, CodeGenerationSupporter::InformationTypeId)) return OptionType::InformationTypeId;
        if (EqualsIgnoreCase(o, CodeGenerationSupporter::Rank)) return OptionType::Rank;
        return OptionType::Undefined;
    }
};

template <>
struct EnumNames<SensitivityClassification::Rank> {
    using R = SensitivityClassification::Rank;
    static constexpr EnumName<R> entries[] = {
        {"None", R::None}, {"Low", R::Low}, {"Medium", R::Medium}, {"High", R::High}, {"Critical", R::Critical}};
};

}  // namespace tsql::parser
