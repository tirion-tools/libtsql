// Ported from Microsoft SqlScriptDOM (MIT) @ eaf3a6e: SqlScriptDom/Parser/TSql/PseudoColumnHelper.cs
// Hand-written member of PseudoColumnHelper (included into the generated class body).
    static bool IsGraphPseudoColumn(const ::tsql::ast::ColumnReferenceExpression* col) {
        using CT = ::tsql::ast::ColumnType;
        return col->ColumnType == CT::PseudoColumnGraphNodeId || col->ColumnType == CT::PseudoColumnGraphEdgeId ||
               col->ColumnType == CT::PseudoColumnGraphFromId || col->ColumnType == CT::PseudoColumnGraphToId;
    }
