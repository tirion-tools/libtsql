// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/OnOffSimpleDbOptionsHelper.cs
// Hand-written member of OnOffSimpleDbOptionsHelper (included into the generated class body).
    bool RequiresEqualsSign(::tsql::ast::DatabaseOptionKind optionKind) const {
        using K = ::tsql::ast::DatabaseOptionKind;
        switch (optionKind) {
            case K::MemoryOptimizedElevateToSnapshot:
            case K::NestedTriggers:
            case K::TransformNoiseWords:
            case K::Ledger:
            case K::AutomaticIndexCompaction:
                return true;
            default:
                return false;
        }
    }
