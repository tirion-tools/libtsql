#include "Builtins.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>

namespace tsql::editor::detail {

namespace {

using K = Builtin::Kind;
constexpr SqlVersion v130 = SqlVersion::Sql130, v140 = SqlVersion::Sql140, v150 = SqlVersion::Sql150,
                     v160 = SqlVersion::Sql160, v170 = SqlVersion::Sql170;

// Sorted by name (checked on first use).
const Builtin kBuiltins[] = {
    {"@@CONNECTIONS", K::GlobalVariable, v130}, {"@@CPU_BUSY", K::GlobalVariable, v130},
    {"@@CURSOR_ROWS", K::GlobalVariable, v130}, {"@@DATEFIRST", K::GlobalVariable, v130},
    {"@@DBTS", K::GlobalVariable, v130}, {"@@ERROR", K::GlobalVariable, v130},
    {"@@FETCH_STATUS", K::GlobalVariable, v130}, {"@@IDENTITY", K::GlobalVariable, v130},
    {"@@IDLE", K::GlobalVariable, v130}, {"@@IO_BUSY", K::GlobalVariable, v130},
    {"@@LANGID", K::GlobalVariable, v130}, {"@@LANGUAGE", K::GlobalVariable, v130},
    {"@@LOCK_TIMEOUT", K::GlobalVariable, v130}, {"@@MAX_CONNECTIONS", K::GlobalVariable, v130},
    {"@@MAX_PRECISION", K::GlobalVariable, v130}, {"@@MICROSOFTVERSION", K::GlobalVariable, v130},
    {"@@NESTLEVEL", K::GlobalVariable, v130}, {"@@OPTIONS", K::GlobalVariable, v130},
    {"@@PACKET_ERRORS", K::GlobalVariable, v130}, {"@@PACK_RECEIVED", K::GlobalVariable, v130},
    {"@@PACK_SENT", K::GlobalVariable, v130}, {"@@PROCID", K::GlobalVariable, v130},
    {"@@REMSERVER", K::GlobalVariable, v130}, {"@@ROWCOUNT", K::GlobalVariable, v130},
    {"@@SERVERNAME", K::GlobalVariable, v130}, {"@@SERVICENAME", K::GlobalVariable, v130},
    {"@@SPID", K::GlobalVariable, v130}, {"@@TEXTSIZE", K::GlobalVariable, v130},
    {"@@TIMETICKS", K::GlobalVariable, v130}, {"@@TOTAL_ERRORS", K::GlobalVariable, v130},
    {"@@TOTAL_READ", K::GlobalVariable, v130}, {"@@TOTAL_WRITE", K::GlobalVariable, v130},
    {"@@TRANCOUNT", K::GlobalVariable, v130}, {"@@VERSION", K::GlobalVariable, v130},
    {"ABS", K::Scalar, v130}, {"ACOS", K::Scalar, v130}, {"APPROX_COUNT_DISTINCT", K::Aggregate, v150},
    {"APPROX_PERCENTILE_CONT", K::Aggregate, v160}, {"APPROX_PERCENTILE_DISC", K::Aggregate, v160},
    {"APP_NAME", K::Scalar, v130}, {"ASCII", K::Scalar, v130}, {"ASIN", K::Scalar, v130},
    {"ATAN", K::Scalar, v130}, {"ATN2", K::Scalar, v130}, {"AVG", K::Aggregate, v130},
    {"BASE64_DECODE", K::Scalar, v170}, {"BASE64_ENCODE", K::Scalar, v170},
    {"BINARY_CHECKSUM", K::Scalar, v130}, {"BIT_COUNT", K::Scalar, v160}, {"CAST", K::Scalar, v130},
    {"CEILING", K::Scalar, v130}, {"CHAR", K::Scalar, v130}, {"CHARINDEX", K::Scalar, v130},
    {"CHECKSUM", K::Scalar, v130}, {"CHECKSUM_AGG", K::Aggregate, v130}, {"CHOOSE", K::Scalar, v130},
    {"COALESCE", K::Scalar, v130}, {"COLUMNPROPERTY", K::Scalar, v130}, {"COL_LENGTH", K::Scalar, v130},
    {"COL_NAME", K::Scalar, v130}, {"COMPRESS", K::Scalar, v130}, {"CONCAT", K::Scalar, v130},
    {"CONCAT_WS", K::Scalar, v140}, {"CONNECTIONPROPERTY", K::Scalar, v130}, {"CONTEXT_INFO", K::Scalar, v130},
    {"CONVERT", K::Scalar, v130}, {"COS", K::Scalar, v130}, {"COT", K::Scalar, v130},
    {"COUNT", K::Aggregate, v130}, {"COUNT_BIG", K::Aggregate, v130}, {"CUME_DIST", K::Window, v130},
    {"CURRENT_REQUEST_ID", K::Scalar, v130}, {"CURRENT_TIMESTAMP", K::Scalar, v130},
    {"CURRENT_TRANSACTION_ID", K::Scalar, v130}, {"CURRENT_USER", K::Scalar, v130},
    {"DATABASEPROPERTYEX", K::Scalar, v130}, {"DATALENGTH", K::Scalar, v130}, {"DATEADD", K::Scalar, v130},
    {"DATEDIFF", K::Scalar, v130}, {"DATEDIFF_BIG", K::Scalar, v130}, {"DATEFROMPARTS", K::Scalar, v130},
    {"DATENAME", K::Scalar, v130}, {"DATEPART", K::Scalar, v130}, {"DATETIME2FROMPARTS", K::Scalar, v130},
    {"DATETIMEFROMPARTS", K::Scalar, v130}, {"DATETIMEOFFSETFROMPARTS", K::Scalar, v130},
    {"DATETRUNC", K::Scalar, v160}, {"DATE_BUCKET", K::Scalar, v160}, {"DAY", K::Scalar, v130},
    {"DB_ID", K::Scalar, v130}, {"DB_NAME", K::Scalar, v130}, {"DECOMPRESS", K::Scalar, v130},
    {"DEGREES", K::Scalar, v130}, {"DENSE_RANK", K::Window, v130}, {"DIFFERENCE", K::Scalar, v130},
    {"EOMONTH", K::Scalar, v130}, {"ERROR_LINE", K::Scalar, v130}, {"ERROR_MESSAGE", K::Scalar, v130},
    {"ERROR_NUMBER", K::Scalar, v130}, {"ERROR_PROCEDURE", K::Scalar, v130}, {"ERROR_SEVERITY", K::Scalar, v130},
    {"ERROR_STATE", K::Scalar, v130}, {"EXP", K::Scalar, v130}, {"FILEGROUP_ID", K::Scalar, v130},
    {"FILEGROUP_NAME", K::Scalar, v130}, {"FILE_ID", K::Scalar, v130}, {"FILE_NAME", K::Scalar, v130},
    {"FIRST_VALUE", K::Window, v130}, {"FLOOR", K::Scalar, v130}, {"FORMAT", K::Scalar, v130},
    {"FORMATMESSAGE", K::Scalar, v130}, {"GENERATE_SERIES", K::TableValued, v160},
    {"GETDATE", K::Scalar, v130}, {"GETUTCDATE", K::Scalar, v130}, {"GET_BIT", K::Scalar, v160},
    {"GREATEST", K::Scalar, v160}, {"GROUPING", K::Aggregate, v130}, {"GROUPING_ID", K::Aggregate, v130},
    {"HASHBYTES", K::Scalar, v130}, {"HAS_PERMS_BY_NAME", K::Scalar, v130}, {"HOST_ID", K::Scalar, v130},
    {"HOST_NAME", K::Scalar, v130}, {"IDENT_CURRENT", K::Scalar, v130}, {"IDENT_INCR", K::Scalar, v130},
    {"IDENT_SEED", K::Scalar, v130}, {"IIF", K::Scalar, v130}, {"INDEXPROPERTY", K::Scalar, v130},
    {"INDEX_COL", K::Scalar, v130}, {"ISDATE", K::Scalar, v130}, {"ISJSON", K::Scalar, v130},
    {"ISNULL", K::Scalar, v130}, {"ISNUMERIC", K::Scalar, v130}, {"IS_MEMBER", K::Scalar, v130},
    {"IS_ROLEMEMBER", K::Scalar, v130}, {"IS_SRVROLEMEMBER", K::Scalar, v130},
    {"JSON_ARRAY", K::Scalar, v160}, {"JSON_ARRAYAGG", K::Aggregate, v170}, {"JSON_MODIFY", K::Scalar, v130},
    {"JSON_OBJECT", K::Scalar, v160}, {"JSON_OBJECTAGG", K::Aggregate, v170},
    {"JSON_PATH_EXISTS", K::Scalar, v160}, {"JSON_QUERY", K::Scalar, v130}, {"JSON_VALUE", K::Scalar, v130},
    {"LAG", K::Window, v130}, {"LAST_VALUE", K::Window, v130}, {"LEAD", K::Window, v130},
    {"LEAST", K::Scalar, v160}, {"LEFT", K::Scalar, v130}, {"LEFT_SHIFT", K::Scalar, v160},
    {"LEN", K::Scalar, v130}, {"LOG", K::Scalar, v130}, {"LOG10", K::Scalar, v130}, {"LOWER", K::Scalar, v130},
    {"LTRIM", K::Scalar, v130}, {"MAX", K::Aggregate, v130}, {"MIN", K::Aggregate, v130},
    {"MONTH", K::Scalar, v130}, {"NCHAR", K::Scalar, v130}, {"NEWID", K::Scalar, v130},
    {"NEWSEQUENTIALID", K::Scalar, v130}, {"NTILE", K::Window, v130}, {"NULLIF", K::Scalar, v130},
    {"OBJECTPROPERTY", K::Scalar, v130}, {"OBJECTPROPERTYEX", K::Scalar, v130}, {"OBJECT_DEFINITION", K::Scalar, v130},
    {"OBJECT_ID", K::Scalar, v130}, {"OBJECT_NAME", K::Scalar, v130}, {"OBJECT_SCHEMA_NAME", K::Scalar, v130},
    {"OPENJSON", K::TableValued, v130}, {"ORIGINAL_LOGIN", K::Scalar, v130}, {"PARSE", K::Scalar, v130},
    {"PARSENAME", K::Scalar, v130}, {"PATINDEX", K::Scalar, v130}, {"PERCENTILE_CONT", K::Window, v130},
    {"PERCENTILE_DISC", K::Window, v130}, {"PERCENT_RANK", K::Window, v130}, {"PI", K::Scalar, v130},
    {"POWER", K::Scalar, v130}, {"PRODUCT", K::Aggregate, v170}, {"QUOTENAME", K::Scalar, v130},
    {"RADIANS", K::Scalar, v130}, {"RAND", K::Scalar, v130}, {"RANK", K::Window, v130},
    {"REGEXP_COUNT", K::Scalar, v170}, {"REGEXP_INSTR", K::Scalar, v170}, {"REGEXP_LIKE", K::Scalar, v170},
    {"REGEXP_MATCHES", K::TableValued, v170}, {"REGEXP_REPLACE", K::Scalar, v170},
    {"REGEXP_SPLIT_TO_TABLE", K::TableValued, v170}, {"REGEXP_SUBSTR", K::Scalar, v170},
    {"REPLACE", K::Scalar, v130}, {"REPLICATE", K::Scalar, v130}, {"REVERSE", K::Scalar, v130},
    {"RIGHT", K::Scalar, v130}, {"RIGHT_SHIFT", K::Scalar, v160}, {"ROUND", K::Scalar, v130},
    {"ROWCOUNT_BIG", K::Scalar, v130}, {"ROW_NUMBER", K::Window, v130}, {"RTRIM", K::Scalar, v130},
    {"SCHEMA_ID", K::Scalar, v130}, {"SCHEMA_NAME", K::Scalar, v130}, {"SCOPE_IDENTITY", K::Scalar, v130},
    {"SERVERPROPERTY", K::Scalar, v130}, {"SESSIONPROPERTY", K::Scalar, v130}, {"SESSION_CONTEXT", K::Scalar, v130},
    {"SESSION_USER", K::Scalar, v130}, {"SET_BIT", K::Scalar, v160}, {"SIGN", K::Scalar, v130},
    {"SIN", K::Scalar, v130}, {"SMALLDATETIMEFROMPARTS", K::Scalar, v130}, {"SOUNDEX", K::Scalar, v130},
    {"SPACE", K::Scalar, v130}, {"SQRT", K::Scalar, v130}, {"SQUARE", K::Scalar, v130},
    {"STATS_DATE", K::Scalar, v130}, {"STDEV", K::Aggregate, v130}, {"STDEVP", K::Aggregate, v130},
    {"STR", K::Scalar, v130}, {"STRING_AGG", K::Aggregate, v140}, {"STRING_ESCAPE", K::Scalar, v130},
    {"STRING_SPLIT", K::TableValued, v130}, {"STUFF", K::Scalar, v130}, {"SUBSTRING", K::Scalar, v130},
    {"SUM", K::Aggregate, v130}, {"SUSER_ID", K::Scalar, v130}, {"SUSER_NAME", K::Scalar, v130},
    {"SUSER_SID", K::Scalar, v130}, {"SUSER_SNAME", K::Scalar, v130}, {"SWITCHOFFSET", K::Scalar, v130},
    {"SYSDATETIME", K::Scalar, v130}, {"SYSDATETIMEOFFSET", K::Scalar, v130}, {"SYSTEM_USER", K::Scalar, v130},
    {"SYSUTCDATETIME", K::Scalar, v130}, {"TAN", K::Scalar, v130}, {"TIMEFROMPARTS", K::Scalar, v130},
    {"TODATETIMEOFFSET", K::Scalar, v130}, {"TRANSLATE", K::Scalar, v140}, {"TRIM", K::Scalar, v140},
    {"TRY_CAST", K::Scalar, v130}, {"TRY_CONVERT", K::Scalar, v130}, {"TRY_PARSE", K::Scalar, v130},
    {"TYPE_ID", K::Scalar, v130}, {"TYPE_NAME", K::Scalar, v130}, {"UNICODE", K::Scalar, v130},
    {"UNISTR", K::Scalar, v170}, {"UPPER", K::Scalar, v130}, {"USER_ID", K::Scalar, v130},
    {"USER_NAME", K::Scalar, v130}, {"VAR", K::Aggregate, v130}, {"VARP", K::Aggregate, v130},
    {"VECTOR_DISTANCE", K::Scalar, v170}, {"VECTOR_NORM", K::Scalar, v170}, {"VECTOR_NORMALIZE", K::Scalar, v170},
    {"XACT_STATE", K::Scalar, v130}, {"YEAR", K::Scalar, v130},
};

int Rank(SqlVersion v) {
    switch (v) {
        case SqlVersion::Sql90: return 90;
        case SqlVersion::Sql80: return 80;
        case SqlVersion::Sql100: return 100;
        case SqlVersion::Sql110: return 110;
        case SqlVersion::Sql120: return 120;
        case SqlVersion::Sql130: return 130;
        case SqlVersion::Sql140: return 140;
        case SqlVersion::Sql150: return 150;
        case SqlVersion::Sql160: return 160;
        case SqlVersion::Sql170: return 170;
        case SqlVersion::SqlFabricDW: return 160;
        case SqlVersion::Sql180: return 180;
    }
    return 0;
}

}  // namespace

bool VersionAtLeast(SqlVersion a, SqlVersion b) { return Rank(a) >= Rank(b); }

const Builtin* FindBuiltin(std::string_view upperName) {
    const auto* end = std::end(kBuiltins);
    const auto* it = std::lower_bound(std::begin(kBuiltins), end, upperName,
                                      [](const Builtin& b, std::string_view n) { return std::string_view(b.name) < n; });
    return it != end && std::string_view(it->name) == upperName ? it : nullptr;
}

const std::vector<const Builtin*>& BuiltinsFor(SqlVersion version) {
    static std::array<std::vector<const Builtin*>, 12> byVersion;
    static std::array<std::once_flag, 12> once;
    const size_t i = static_cast<size_t>(version) % byVersion.size();
    std::call_once(once[i], [&] {
        for (const Builtin& b : kBuiltins)
            if (VersionAtLeast(version, b.since)) byVersion[i].push_back(&b);
    });
    return byVersion[i];
}

}  // namespace tsql::editor::detail
