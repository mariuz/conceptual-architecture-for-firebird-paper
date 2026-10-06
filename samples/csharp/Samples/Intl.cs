//
// Intl.cs - charset, collation and transliteration (C# twin of
// ../../cpp/intl.cpp; see ../../../internationalization.md).
//
// One table, three concepts: two UTF8 columns that differ only in collation
// (UNICODE_CI_AI vs UCS_BASIC) and a WIN1252 column beside them.  The
// Cafe/CAFE/cafe experiment shows the collation deciding equality and
// order; then the same stored WIN1252 'Cafe' (with e-acute) is fetched over
// connections with different connection charsets.
//
// FirebirdClient, like Jaybird, decodes each column with the charset the
// server describes it with, so on a Charset=NONE connection - where the
// server passes the stored E9 through and describes the column as WIN1252
// - the transliteration moves to the client.  The .NET-specific twist is
// that modern .NET ships without the legacy code pages: windows-1252 exists
// in the process only once System.Text.CodePagesEncodingProvider is
// registered, and it must be registered before the provider first builds
// its charset table (it does so once per process).  Run with
// --no-codepages to see the other outcome: the NONE connection decodes E9
// as U+FFFD and a Charset=WIN1252 connection is refused by the client
// before it reaches the server.  The provider never hands out the wire
// bytes of a text column (GetBytes needs a binary one), so the stored
// bytes are shown with HEX_ENCODE on the server and the received text as
// code points.
//
// Run:  cd samples/csharp && dotnet run -- Intl [--no-codepages] [database]
//
using System.Text;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Intl
{
    const string Where = " from t where name_bin starting with 'Caf' "
                         + "and name_bin <> 'CAFE' and name_bin <> 'cafe'";   // ASCII-only predicate

    static string Column(FbConnection con, string sql) =>
        string.Join("  ", Rows(con, sql).Select(r => Text(r[0])));

    static string CodePoints(string s) => string.Join(" ", s.Select(c => $"U+{(int)c:X4}"));

    static void Fetch(string label, string path, string charset)
    {
        try
        {
            using var con = Attach(path, charset);
            var s = (string)Scalar(con, "select name_win" + Where)!;
            Console.WriteLine($"  {label,-24} \"{s}\"  {CodePoints(s)}");
        }
        catch (ArgumentException e)                     // raised by the provider before any I/O
        {
            Console.WriteLine($"  {label,-24} refused by the client: {e.Message}");
        }
    }

    public static void Run(string[] args)
    {
        var codePages = !args.Contains("--no-codepages");
        if (codePages)                                  // before the provider's first use
            Encoding.RegisterProvider(CodePagesEncodingProvider.Instance);
        var path = DbPath("intl", args.Where(a => a != "--no-codepages").ToArray());
        using (var con = AttachOrCreate(path))         // Charset=UTF8
        {
            Execute(con, "recreate table t ("
                         + "  name_ci_ai varchar(30) character set utf8 collate unicode_ci_ai,"
                         + "  name_bin   varchar(30) character set utf8 collate ucs_basic,"
                         + "  name_win   varchar(30) character set win1252)");
            foreach (var v in new[] { "Café", "CAFE", "cafe" })
            {
                using var ins = new FbCommand("insert into t values (@v, @v, @v)", con);
                ins.Parameters.AddWithValue("@v", v);
                ins.ExecuteNonQuery();
            }

            // -- 1. The collation, not the data, decides what "equal" means. -------
            Console.WriteLine($"rows matching 'cafe' with UNICODE_CI_AI : {Scalar(con, "select count(*) from t where name_ci_ai = 'cafe'")}");
            Console.WriteLine($"rows matching 'cafe' with UCS_BASIC     : {Scalar(con, "select count(*) from t where name_bin = 'cafe'")}");
            Console.WriteLine($"UPPER('café èñ ß')                      : {Scalar(con, "select upper('café èñ ß') from rdb$database")}");
            Console.WriteLine($"ORDER BY name_ci_ai: {Column(con, "select name_ci_ai from t order by name_ci_ai")}");
            Console.WriteLine($"ORDER BY name_bin  : {Column(con, "select name_bin from t order by name_bin")}    (binary: uppercase codepoints first)");
            Console.WriteLine();

            // -- 2. What the column stores, independent of any connection charset. -
            Console.WriteLine($"stored bytes of name_win (HEX_ENCODE)  : {Scalar(con, "select hex_encode(name_win)" + Where)}");
        }

        // -- 3. Same stored WIN1252 value, three connection charsets. --------------
        Console.WriteLine("SELECT name_win ... 'Café' - same row, three connections:");
        Console.WriteLine(codePages ? " (CodePagesEncodingProvider registered: windows-1252 available)"
                                    : " (--no-codepages: .NET has no windows-1252)");
        Fetch("Charset=UTF8:", path, "UTF8");
        Fetch("Charset=NONE:", path, "NONE");
        Fetch("Charset=WIN1252:", path, "WIN1252");
        Console.WriteLine("  -> UTF8: the server transliterated E9 to C3 A9.  NONE: E9 crossed the wire and");
        Console.WriteLine(codePages ? "     the provider decoded it with the column's own charset (windows-1252)."
                                    : "     without windows-1252 the provider could only substitute U+FFFD.");
        Console.WriteLine("done.");
    }
}
