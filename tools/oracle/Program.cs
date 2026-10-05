// Reference ("oracle") dumper for the libtsql ANTLR4 pilot.
//
// Parses T-SQL with Microsoft.SqlServer.TransactSql.ScriptDom's TSql170Parser(initialQuotedIdentifiers: true)
// and writes the shared JSON dump format that the C++ port also emits, so the two can be diffed structurally.
//
// Package: Microsoft.SqlServer.TransactSql.ScriptDom 180.117.0 (exact pin in oracle.csproj). Its nuspec names
// internal commit e3e282510b9d, which is not on public GitHub; the reference source is public master eaf3a6e.
// Run `oracle audit` to list Ast.xml-vs-assembly differences.
//
// Commands:
//   oracle dump [--ast <Ast.xml>] [--jobs N] <out-dir> <file-or-dir>...
//       <out-dir>/<path relative to the argument>.json  (e.g. Baselines170/Foo.sql -> Baselines170/Foo.sql.json;
//       a file argument maps to its file name). Also maintains <out-dir>/manifest.json (source path, sha256, encoding).
//   oracle classify <out-dir>
//       <out-dir>/index.json and <out-dir>/select-corpus.txt from manifest.json + the dumps.
//   oracle audit [--ast <Ast.xml>]
//       Compare Ast.xml classes/members against the runtime assembly.
//   oracle tokentypes <out-file>
//       Write the package's TSqlTokenType enum as Name=Value lines.

using System.Collections.Concurrent;
using System.Diagnostics;
using System.Globalization;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Xml.Linq;
using Microsoft.SqlServer.TransactSql.ScriptDom;

namespace Oracle;

internal sealed record AstMember(string Name, string Type, bool Collection);

internal sealed class AstClass
{
    public required string Name { get; init; }
    public string? Base { get; init; }
    public List<AstMember> Members { get; } = new();
}

internal sealed class OracleFailure(string message) : Exception(message);

/// <summary>Ast.xml model + reflection bindings to the runtime ScriptDom assembly.</summary>
internal sealed class AstSchema
{
    private const string Ns = "Microsoft.SqlServer.TransactSql.ScriptDom";

    public Dictionary<string, AstClass> Classes { get; } = new(StringComparer.Ordinal);
    private readonly Dictionary<string, List<AstMember>> _interfaces = new(StringComparer.Ordinal);
    private readonly ConcurrentDictionary<Type, Accessor[]> _accessors = new();
    private readonly Assembly _asm = typeof(TSqlFragment).Assembly;

    internal sealed record Accessor(string Name, bool Collection, Func<object, object?> Get);
    public List<string> SkippedStreamAnalytics { get; } = new();

    public AstSchema(string astXmlPath)
    {
        var doc = XDocument.Load(astXmlPath);
        var root = doc.Root ?? throw new OracleFailure($"{astXmlPath}: empty document");
        foreach (var iface in root.Elements("Interface"))
            _interfaces[(string)iface.Attribute("Name")!] = iface.Elements("Member").Select(ParseMember).ToList();
        foreach (var cls in root.Elements("Class"))
        {
            var c = new AstClass { Name = (string)cls.Attribute("Name")!, Base = (string?)cls.Attribute("Base") };
            // Document order: <Member> as declared; <Implements Interface="X"/> expands to X's members in place.
            // <InheritedMember>/<InheritedClass> are code-generation hints only (members come via the Base chain).
            foreach (var el in cls.Elements())
            {
                switch (el.Name.LocalName)
                {
                    case "Member":
                        // Same as SqlScriptDOM's tools/AstGen default build: IsStreamAnalyticsExtension="true"
                        // members (NamedTableReference.PartitionBy/TimestampBy/Over) are not generated.
                        if (XmlBool(el, "IsStreamAnalyticsExtension"))
                        {
                            SkippedStreamAnalytics.Add($"{c.Name}.{(string)el.Attribute("Name")!}");
                            break;
                        }
                        c.Members.Add(ParseMember(el));
                        break;
                    case "Implements":
                        var iname = (string)el.Attribute("Interface")!;
                        if (!_interfaces.TryGetValue(iname, out var im))
                            throw new OracleFailure($"Ast.xml: class {c.Name} implements unknown interface {iname}");
                        c.Members.AddRange(im);
                        break;
                }
            }
            if (!Classes.TryAdd(c.Name, c))
                throw new OracleFailure($"Ast.xml: duplicate class {c.Name}");
        }
    }

    private static AstMember ParseMember(XElement m) =>
        new((string)m.Attribute("Name")!, (string)m.Attribute("Type")!, XmlBool(m, "Collection"));

    // Ast.xml spells booleans loosely (Collection="True", Collection=" true"); AstGen reads them with
    // Convert.ToBoolean, which trims and ignores case. Anything else unparseable is an error.
    private static bool XmlBool(XElement e, string attr)
    {
        var v = (string?)e.Attribute(attr);
        if (v == null) return false;
        return bool.TryParse(v.Trim(), out var b) ? b
            : throw new OracleFailure($"Ast.xml: {attr}=\"{v}\" on {(string?)e.Attribute("Name")} is not a boolean");
    }

    /// <summary>Ast.xml members of a class, top-most ancestor first.</summary>
    public List<AstMember> OrderedMembers(string className)
    {
        var chain = new List<AstClass>();
        for (string? n = className; n != null;)
        {
            if (!Classes.TryGetValue(n, out var c))
            {
                if (n == "TSqlFragment") break; // implicit root, not declared in Ast.xml
                throw new OracleFailure($"Ast.xml: class {n} (in base chain of {className}) is not declared");
            }
            chain.Add(c);
            n = c.Base;
        }
        chain.Reverse();
        return chain.SelectMany(c => c.Members).ToList();
    }

    public Type? RuntimeType(string className) => _asm.GetType($"{Ns}.{className}", throwOnError: false);

    /// <summary>Most-derived public instance property with this name (handles `new`-hidden properties).</summary>
    private static PropertyInfo? FindProperty(Type t, string name)
    {
        for (var cur = t; cur != null; cur = cur.BaseType)
        {
            var p = cur.GetProperty(name, BindingFlags.Public | BindingFlags.Instance | BindingFlags.DeclaredOnly);
            if (p != null && p.GetIndexParameters().Length == 0) return p;
        }
        return null;
    }

    public Accessor[] AccessorsFor(Type runtimeType) => _accessors.GetOrAdd(runtimeType, t =>
    {
        if (t.Namespace != Ns || !Classes.ContainsKey(t.Name))
            throw new OracleFailure($"runtime fragment type {t.FullName} is not declared in Ast.xml");
        var result = new List<Accessor>();
        foreach (var m in OrderedMembers(t.Name))
        {
            var p = FindProperty(t, m.Name)
                ?? throw new OracleFailure($"Ast.xml member {t.Name}.{m.Name} ({m.Type}) has no public property on runtime type {t.FullName}");
            if (p.GetMethod == null) throw new OracleFailure($"{t.Name}.{m.Name} has no getter");
            var obj = System.Linq.Expressions.Expression.Parameter(typeof(object));
            var body = System.Linq.Expressions.Expression.Convert(
                System.Linq.Expressions.Expression.Property(System.Linq.Expressions.Expression.Convert(obj, t), p), typeof(object));
            var get = System.Linq.Expressions.Expression.Lambda<Func<object, object?>>(body, obj).Compile();
            result.Add(new Accessor(m.Name, m.Collection, get));
        }
        return result.ToArray();
    });

    /// <summary>Lists every Ast.xml class/member that cannot be bound, plus assembly fragment types missing from Ast.xml.</summary>
    public List<string> Audit()
    {
        var problems = new List<string>();
        foreach (var c in Classes.Values)
        {
            var t = RuntimeType(c.Name);
            if (t == null) { problems.Add($"class-missing-at-runtime {c.Name}"); continue; }
            if (!typeof(TSqlFragment).IsAssignableFrom(t)) problems.Add($"class-not-a-fragment {c.Name}");
            var rtBase = t.BaseType?.Name;
            var xmlBase = c.Base ?? "TSqlFragment";
            if (rtBase != xmlBase) problems.Add($"base-mismatch {c.Name}: Ast.xml {xmlBase}, runtime {rtBase}");
            foreach (var m in OrderedMembers(c.Name))
            {
                var p = FindProperty(t, m.Name);
                if (p == null) { problems.Add($"member-missing-at-runtime {c.Name}.{m.Name} ({m.Type})"); continue; }
                bool rtCollection = p.PropertyType != typeof(string) && typeof(System.Collections.IEnumerable).IsAssignableFrom(p.PropertyType);
                if (rtCollection != m.Collection)
                    problems.Add($"collection-mismatch {c.Name}.{m.Name}: Ast.xml {m.Collection}, runtime {p.PropertyType.Name}");
            }
        }
        foreach (var t in _asm.GetExportedTypes())
            if (typeof(TSqlFragment).IsAssignableFrom(t) && t != typeof(TSqlFragment) && t.Namespace == Ns && !Classes.ContainsKey(t.Name))
                problems.Add($"runtime-class-not-in-Ast.xml {t.Name}");
        return problems;
    }
}

internal static class Json
{
    public static void String(StringBuilder sb, string s)
    {
        sb.Append('"');
        foreach (var ch in s)
        {
            switch (ch)
            {
                case '"': sb.Append("\\\""); break;
                case '\\': sb.Append("\\\\"); break;
                default:
                    if (ch < 0x20) sb.Append("\\u").Append(((int)ch).ToString("x4", CultureInfo.InvariantCulture));
                    else sb.Append(ch); // non-ASCII raw; written as UTF-8 (lone surrogates become U+FFFD)
                    break;
            }
        }
        sb.Append('"');
    }
}

internal sealed class Dumper(AstSchema schema)
{
    public string Dump(TSqlFragment? tree, IList<ParseError> errors)
    {
        var sb = new StringBuilder(4096);
        sb.Append("{\"errors\":[");
        for (int i = 0; i < errors.Count; i++)
        {
            var e = errors[i];
            if (i > 0) sb.Append(',');
            sb.Append("{\"Number\":").Append(e.Number.ToString(CultureInfo.InvariantCulture))
              .Append(",\"Offset\":").Append(e.Offset.ToString(CultureInfo.InvariantCulture))
              .Append(",\"Line\":").Append(e.Line.ToString(CultureInfo.InvariantCulture))
              .Append(",\"Column\":").Append(e.Column.ToString(CultureInfo.InvariantCulture))
              .Append(",\"Message\":");
            Json.String(sb, e.Message);
            sb.Append('}');
        }
        sb.Append("],\"tree\":");
        if (tree == null) sb.Append("null"); else Node(sb, tree);
        sb.Append('}');
        return sb.ToString();
    }

    private void Node(StringBuilder sb, TSqlFragment f)
    {
        var t = f.GetType();
        sb.Append("{\"$type\":");
        Json.String(sb, t.Name);
        sb.Append(",\"$pos\":[")
          .Append(f.FirstTokenIndex.ToString(CultureInfo.InvariantCulture)).Append(',')
          .Append(f.LastTokenIndex.ToString(CultureInfo.InvariantCulture)).Append(',')
          .Append(f.StartOffset.ToString(CultureInfo.InvariantCulture)).Append(',')
          .Append(f.FragmentLength.ToString(CultureInfo.InvariantCulture)).Append(']');
        foreach (var a in schema.AccessorsFor(t))
        {
            sb.Append(',');
            Json.String(sb, a.Name);
            sb.Append(':');
            object? v;
            try { v = a.Get(f); }
            catch (Exception ex) { throw new OracleFailure($"{t.Name}.{a.Name} getter threw: {ex.Message}"); }
            if (a.Collection)
            {
                if (v == null) { sb.Append("[]"); continue; } // C# generated collections are never null
                if (v is not System.Collections.IEnumerable items)
                    throw new OracleFailure($"{t.Name}.{a.Name} is Collection=\"true\" but runtime value is {v.GetType()}");
                sb.Append('[');
                bool first = true;
                foreach (var item in items)
                {
                    if (!first) sb.Append(',');
                    first = false;
                    Value(sb, item, t.Name, a.Name);
                }
                sb.Append(']');
            }
            else
            {
                Value(sb, v, t.Name, a.Name);
            }
        }
        sb.Append('}');
    }

    private void Value(StringBuilder sb, object? v, string owner, string member)
    {
        switch (v)
        {
            case null: sb.Append("null"); break;
            case TSqlFragment frag: Node(sb, frag); break;
            case string s: Json.String(sb, s); break;
            case bool b: sb.Append(b ? "true" : "false"); break;
            case Enum e: Json.String(sb, e.ToString()); break;
            case int or long or short or sbyte or byte or ushort or uint or ulong:
                sb.Append(Convert.ToString(v, CultureInfo.InvariantCulture)); break;
            default:
                throw new OracleFailure($"{owner}.{member}: unsupported member value type {v.GetType().FullName}");
        }
    }
}

internal sealed record SourceInfo(string Path, string Source, string Sha256, string Encoding, int Bytes);

internal static class Program
{
    // The pilot workspace, as tools/diff/workspace.py: $TSQL_PILOT_DIR, default ${XDG_CACHE_HOME:-~/.cache}/tsql-pilot.
    private static readonly string DefaultAst = Path.Combine(
        Environment.GetEnvironmentVariable("TSQL_PILOT_DIR") is { Length: > 0 } dir ? dir
            : Path.Combine(Environment.GetEnvironmentVariable("XDG_CACHE_HOME") is { Length: > 0 } cache ? cache
                : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".cache"), "tsql-pilot"),
        "ssd/SqlScriptDom/Parser/TSql/Ast.xml");

    private static int Main(string[] args)
    {
        try
        {
            var rest = new List<string>(args);
            string ast = DefaultAst;
            int ai = rest.IndexOf("--ast");
            if (ai >= 0)
            {
                if (ai + 1 >= rest.Count) return Usage();
                ast = rest[ai + 1];
                rest.RemoveRange(ai, 2);
            }
            int pi = rest.IndexOf("--parser");
            if (pi >= 0)
            {
                if (pi + 1 >= rest.Count) return Usage();
                SetParser(rest[pi + 1]);
                rest.RemoveRange(pi, 2);
            }
            int jobs = Environment.ProcessorCount;
            int ji = rest.IndexOf("--jobs");
            if (ji >= 0)
            {
                if (ji + 1 >= rest.Count || !int.TryParse(rest[ji + 1], out jobs) || jobs < 1) return Usage();
                rest.RemoveRange(ji, 2);
            }
            if (rest.Count == 0) return Usage();
            switch (rest[0])
            {
                case "dump" when rest.Count >= 3: return Dump(ast, rest[1], rest.Skip(2).ToList(), jobs);
                case "classify" when rest.Count == 2: return Classify(rest[1]);
                case "audit" when rest.Count == 1: return Audit(ast);
                case "tokentypes" when rest.Count == 2: return TokenTypes(rest[1]);
                case "bench" when rest.Count >= 3: return Bench(int.Parse(rest[1]), rest.Skip(2).ToList());
                default: return Usage();
            }
        }
        catch (OracleFailure ex)
        {
            Console.Error.WriteLine($"oracle: FATAL: {ex.Message}");
            return 2;
        }
    }

    // --parser TSql130 | TSql140 | ... | TSql180 | TSqlFabricDW (default TSql170): the ScriptDom parser class
    // (<name>Parser) every command uses, always with initialQuotedIdentifiers: true.
    private static string ParserName = "TSql170";
    private static Type ParserType = typeof(TSql170Parser);

    private static void SetParser(string name)
    {
        ParserType = typeof(TSqlParser).Assembly.GetType($"Microsoft.SqlServer.TransactSql.ScriptDom.{name}Parser")
            ?? throw new OracleFailure($"no ScriptDom parser class {name}Parser");
        ParserName = name;
    }

    private static TSqlParser NewParser() => (TSqlParser)Activator.CreateInstance(ParserType, new object[] { true })!;

    // bench <rounds> <file>...: parses every file once per round, single-threaded,
    // and prints per-round wall time (round 1 includes JIT).
    private static int Bench(int rounds, List<string> files)
    {
        var texts = files.Select(f => File.ReadAllText(f)).ToList();
        long bytes = files.Sum(f => new FileInfo(f).Length);
        for (int r = 0; r < rounds; ++r)
        {
            int withErrors = 0;
            var watch = System.Diagnostics.Stopwatch.StartNew();
            foreach (var text in texts)
            {
                var parser = NewParser();
                parser.Parse(new StringReader(text), out IList<ParseError> errors);
                if (errors.Count > 0) ++withErrors;
            }
            double ms = watch.Elapsed.TotalMilliseconds;
            Console.WriteLine($"round {r + 1}: {ms:F1} ms for {texts.Count} files, {bytes / 1024.0:F0} KB ({bytes / 1048576.0 / (ms / 1000.0):F2} MB/s), {withErrors} with errors");
        }
        return 0;
    }

    private static int Usage()
    {
        Console.Error.WriteLine("usage: oracle dump [--ast Ast.xml] [--jobs N] <out-dir> <file-or-dir>...\n" +
                                "       oracle classify <out-dir>\n" +
                                "       oracle audit [--ast Ast.xml]\n" +
                                "       oracle tokentypes <out-file>");
        return 64;
    }

    private static int Audit(string ast)
    {
        var schema = new AstSchema(ast);
        foreach (var s in schema.SkippedStreamAnalytics) Console.WriteLine($"skipped-IsStreamAnalyticsExtension {s}");
        var problems = schema.Audit();
        foreach (var p in problems) Console.WriteLine(p);
        Console.Error.WriteLine($"{problems.Count} problem(s); package {PackageVersion()}");
        return problems.Count == 0 ? 0 : 1;
    }

    private static string PackageVersion() =>
        typeof(TSqlFragment).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion
        ?? typeof(TSqlFragment).Assembly.GetName().Version?.ToString() ?? "?";

    private static int TokenTypes(string outFile)
    {
        var sb = new StringBuilder();
        foreach (var name in Enum.GetNames<TSqlTokenType>())
            sb.Append(name).Append('=').Append(((int)Enum.Parse<TSqlTokenType>(name)).ToString(CultureInfo.InvariantCulture)).Append('\n');
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(outFile))!);
        File.WriteAllText(outFile, sb.ToString());
        return 0;
    }

    private static int Dump(string ast, string outDir, List<string> inputs, int jobs)
    {
        var sw = Stopwatch.StartNew();
        var schema = new AstSchema(ast);
        // Fail loudly up front: every Ast.xml class/member must bind to the runtime type with the same shape.
        // (Runtime-only fragment classes fail only if a parse actually produces one.)
        var missing = schema.Audit().Where(p => !p.StartsWith("runtime-class-not-in-Ast.xml", StringComparison.Ordinal)).ToList();
        if (missing.Count > 0)
            throw new OracleFailure("Ast.xml does not match the runtime assembly:\n  " + string.Join("\n  ", missing));
        var dumper = new Dumper(schema);

        var work = new List<(string Rel, string Src)>();
        foreach (var input in inputs)
        {
            var full = Path.GetFullPath(input);
            if (Directory.Exists(full))
            {
                foreach (var f in Directory.EnumerateFiles(full, "*.sql", SearchOption.AllDirectories))
                    work.Add((Path.GetRelativePath(full, f).Replace('\\', '/'), f));
            }
            else if (File.Exists(full)) work.Add((Path.GetFileName(full), full));
            else throw new OracleFailure($"no such file or directory: {input}");
        }
        work.Sort((a, b) => string.CompareOrdinal(a.Rel, b.Rel));
        var dupRel = work.GroupBy(w => w.Rel).FirstOrDefault(g => g.Count() > 1);
        if (dupRel != null) throw new OracleFailure($"two inputs map to the same output path {dupRel.Key}");

        Directory.CreateDirectory(outDir);
        var infos = new SourceInfo[work.Count];
        // Warm-up on one thread: concurrent first use of TSql170ParserInternal (static initialization racing in
        // GetGCStaticBaseSlow) intermittently kills .NET 10.0.12 with "Internal CLR error (0x80131506)".
        NewParser().Parse(new StringReader("SELECT 1;"), out _);
        var failures = new ConcurrentBag<string>();
        Parallel.For(0, work.Count, new ParallelOptions { MaxDegreeOfParallelism = jobs }, i =>
        {
            var (rel, src) = work[i];
            try
            {
                var bytes = File.ReadAllBytes(src);
                // Decode exactly as StreamReader(path) does: BOM detection, default UTF-8 with U+FFFD replacement.
                string encodingName;
                TSqlFragment? tree;
                IList<ParseError> errors;
                using (var reader = new StreamReader(new MemoryStream(bytes), Encoding.UTF8, detectEncodingFromByteOrderMarks: true))
                {
                    var text = reader.ReadToEnd();
                    encodingName = DescribeEncoding(bytes, reader.CurrentEncoding);
                    var parser = NewParser();
                    using var tr = new StringReader(text);
                    tree = parser.Parse(tr, out errors);
                }
                var json = dumper.Dump(tree, errors);
                var outPath = Path.Combine(outDir, rel + ".json");
                Directory.CreateDirectory(Path.GetDirectoryName(outPath)!);
                File.WriteAllText(outPath, json, new UTF8Encoding(false));
                infos[i] = new SourceInfo(rel, src, Convert.ToHexStringLower(SHA256.HashData(bytes)), encodingName, bytes.Length);
            }
            catch (OracleFailure ex) { failures.Add($"{src}: {ex.Message}"); }
        });
        if (!failures.IsEmpty)
            throw new OracleFailure(string.Join("\n", failures.OrderBy(s => s, StringComparer.Ordinal)));

        // Merge into manifest.json so several dump invocations can share one out-dir.
        var manifestPath = Path.Combine(outDir, "manifest.json");
        var manifest = new SortedDictionary<string, SourceInfo>(StringComparer.Ordinal);
        if (File.Exists(manifestPath))
            foreach (var e in JsonSerializer.Deserialize<List<SourceInfo>>(File.ReadAllText(manifestPath))!)
                manifest[e.Path] = e;
        foreach (var info in infos) manifest[info.Path] = info;
        File.WriteAllText(manifestPath, JsonSerializer.Serialize(manifest.Values.ToList(), new JsonSerializerOptions { WriteIndented = true }));
        Console.Error.WriteLine($"oracle: dumped {work.Count} file(s) in {sw.Elapsed.TotalSeconds:F2}s with {PackageVersion()}");
        return 0;
    }

    private static string DescribeEncoding(byte[] bytes, Encoding detected)
    {
        if (detected is UTF8Encoding)
        {
            bool bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
            bool valid;
            try { new UTF8Encoding(false, throwOnInvalidBytes: true).GetCharCount(bytes, bom ? 3 : 0, bytes.Length - (bom ? 3 : 0)); valid = true; }
            catch (DecoderFallbackException) { valid = false; }
            return (bom ? "utf-8-bom" : "utf-8") + (valid ? "" : "+invalid(U+FFFD)");
        }
        return detected.WebName + "-bom"; // utf-16 / utf-16BE / utf-32 are only selected via BOM
    }

    private static int Classify(string outDir)
    {
        var manifestPath = Path.Combine(outDir, "manifest.json");
        if (!File.Exists(manifestPath)) throw new OracleFailure($"{manifestPath} not found; run `oracle dump` first");
        var manifest = JsonSerializer.Deserialize<List<SourceInfo>>(File.ReadAllText(manifestPath))!;
        manifest.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));

        var entries = new List<Dictionary<string, object?>>();
        var bySha = new Dictionary<string, Dictionary<string, object?>>(StringComparer.Ordinal);
        var select = new List<string>();
        int ok = 0;
        foreach (var info in manifest)
        {
            if (bySha.TryGetValue(info.Sha256, out var firstEntry))
            {
                ((List<string>)firstEntry["duplicates"]!).Add(info.Path);
                continue;
            }
            using var doc = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(outDir, info.Path + ".json")));
            var root = doc.RootElement;
            int errorCount = root.GetProperty("errors").GetArrayLength();
            var tree = root.GetProperty("tree");
            int batches = 0, statements = 0;
            var types = new List<string>();
            if (tree.ValueKind == JsonValueKind.Object)
            {
                foreach (var batch in tree.GetProperty("Batches").EnumerateArray())
                {
                    batches++;
                    foreach (var st in batch.GetProperty("Statements").EnumerateArray())
                    {
                        statements++;
                        var ty = st.GetProperty("$type").GetString()!;
                        if (!types.Contains(ty)) types.Add(ty);
                    }
                }
            }
            bool isOk = errorCount == 0;
            if (isOk) ok++;
            if (isOk && statements > 0 && types.Count == 1 && types[0] == "SelectStatement") select.Add(info.Source);
            var entry = new Dictionary<string, object?>
            {
                ["path"] = info.Path,
                ["source"] = info.Source,
                ["sha256"] = info.Sha256,
                ["encoding"] = info.Encoding,
                ["bytes"] = info.Bytes,
                ["ok"] = isOk,
                ["errors"] = errorCount,
                ["batches"] = batches,
                ["statements"] = statements,
                ["statementTypes"] = types,
                ["duplicates"] = new List<string>(),
            };
            bySha[info.Sha256] = entry;
            entries.Add(entry);
        }
        var index = new Dictionary<string, object?>
        {
            ["package"] = $"Microsoft.SqlServer.TransactSql.ScriptDom 180.117.0 (assembly {PackageVersion()})",
            ["parser"] = $"{ParserName}Parser(initialQuotedIdentifiers: true)",
            ["files"] = manifest.Count,
            ["unique"] = entries.Count,
            ["ok"] = ok,
            ["okFiles"] = entries.Where(e => (bool)e["ok"]!).Sum(e => 1 + ((List<string>)e["duplicates"]!).Count),
            ["selectCorpus"] = select.Count,
            ["entries"] = entries,
        };
        File.WriteAllText(Path.Combine(outDir, "index.json"), JsonSerializer.Serialize(index, new JsonSerializerOptions
            { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }));
        File.WriteAllText(Path.Combine(outDir, "select-corpus.txt"), string.Concat(select.Select(s => s + "\n")));
        Console.Error.WriteLine($"oracle: files={manifest.Count} unique={entries.Count} ok={ok} select-corpus={select.Count}");
        return 0;
    }
}
