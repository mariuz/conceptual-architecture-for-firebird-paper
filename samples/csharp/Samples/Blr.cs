//
// Blr.cs - stored BLR read raw from the catalog, and the BLR a wire client
// writes itself (C# twin of ../../cpp/blr.cpp; see
// ../../../blr-intermediate-language.md).
//
// Reads the computed column EMPLOYEE.FULL_NAME (RDB$FIELDS.RDB$COMPUTED_BLR)
// and the procedure GET_EMP_PROJ (RDB$PROCEDURES.RDB$PROCEDURE_BLR) from the
// stock employee database, hex-dumps them and decodes the opening bytes
// like the C++ sample: the whole expression tree of the computed column,
// the message declarations at the head of the procedure.  Fetching is
// FbDataReader.GetValue(): a sub_type 2 blob arrives as a byte[], segments
// read by the provider.  The opcode values are copied from
// firebird/impl/blr.h - the provider keeps its own copies internal.
//
// The third part is the other direction.  The managed (ServerType=Default)
// client, like Jaybird's and the Go driver's, has no libfbclient to describe
// rows for it, so it WRITES message BLR itself: for the input parameters on
// every op_execute, and for the output row on op_execute2 / op_fetch.  The
// provider has no public API for it, so the sample reaches the prepared
// statement's descriptors through reflection (FbCommand._statement ->
// Parameters / Fields -> Descriptor.ToBlr()), which is version-specific
// (FirebirdClient 10.3.4) and done here only to show the bytes.
//
// Read-only against employee.
//
// Run:  cd samples/csharp && dotnet run -- Blr
//
using System.Reflection;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Blr
{
    // From firebird/impl/blr.h.
    const byte blr_text = 14, blr_text2 = 15, blr_short = 7, blr_long = 8, blr_varying = 37,
               blr_varying2 = 38, blr_version5 = 5, blr_eoc = 76, blr_end = 255, blr_begin = 2,
               blr_message = 4, blr_literal = 21, blr_field = 23, blr_concatenate = 39;

    sealed class Reader(byte[] b)
    {
        public readonly byte[] B = b;
        public int P;
        public byte U8() => B[P++];
        public int U16() { var v = B[P] | (B[P + 1] << 8); P += 2; return v; }
        public bool AtEnd => P >= B.Length;

        /// <summary>Decode one expression - enough opcodes for a computed column.</summary>
        public void Expr(int depth)
        {
            Console.Write(new string(' ', depth * 3));
            var op = U8();
            switch (op)
            {
                case blr_concatenate:
                    Console.WriteLine("blr_concatenate");
                    Expr(depth + 1);
                    Expr(depth + 1);
                    break;
                case blr_field:
                {
                    int ctx = U8(), len = U8();
                    Console.WriteLine($"blr_field context {ctx}, '{System.Text.Encoding.ASCII.GetString(B, P, len)}'");
                    P += len;
                    break;
                }
                case blr_literal when B[P] == blr_text2:
                {
                    P++;
                    int cs = U16(), len = U16();
                    Console.WriteLine($"blr_literal blr_text2 charset {cs}, len {len}, \"{System.Text.Encoding.ASCII.GetString(B, P, len)}\"");
                    P += len;
                    break;
                }
                default:
                    Console.WriteLine($"opcode {op} (decoder stops here)");
                    P = B.Length;
                    break;
            }
        }

        /// <summary>Decode blr_message declarations, as found after blr_begin.</summary>
        public void Messages()
        {
            while (!AtEnd && B[P] == blr_message)
            {
                P++;
                int msg = U8(), count = U16();
                var sb = new System.Text.StringBuilder($"blr_message {msg}, {count} fields:");
                for (var i = 0; i < count; i++)
                {
                    var t = U8();
                    if (t == blr_short) sb.Append($" blr_short(scale {U8()})");
                    else if (t == blr_long) sb.Append($" blr_long(scale {U8()})");
                    else if (t is blr_text2 or blr_varying2) sb.Append($" {(t == blr_text2 ? "blr_text2" : "blr_varying2")}(cs {U16()}, len {U16()})");
                    else if (t is blr_text or blr_varying) sb.Append($" {(t == blr_text ? "blr_text" : "blr_varying")}(len {U16()})");
                    else { sb.Append($" dtype {t}?"); i = count; }
                }
                Console.WriteLine(sb);
            }
        }

        public void Head() =>
            Console.WriteLine($"{(U8() == blr_version5 ? "blr_version5" : "?")}, {(U8() == blr_begin ? "blr_begin" : "?")}");

        public void Tail() =>
            Console.WriteLine($"{(U8() == blr_end ? "blr_end" : "?")}, {(U8() == blr_eoc ? "blr_eoc" : "?")}");
    }

    static void HexDump(byte[] b, int limit)
    {
        for (var i = 0; i < b.Length && i < limit; i++)
            Console.Write($"{b[i]:x2}{(i % 16 == 15 ? "\n" : " ")}");
        Console.WriteLine(b.Length > limit ? $"... ({b.Length} bytes total)" : $"({b.Length} bytes total)");
    }

    static byte[] FetchBlob(FbConnection con, FbTransaction tx, string sql)
    {
        using var cmd = new FbCommand(sql, con, tx);
        using var r = cmd.ExecuteReader();
        if (!r.Read()) throw new InvalidOperationException("no row");
        return (byte[])r.GetValue(0);
    }

    /// <summary>The message BLR the managed client writes for a prepared command (reflection; 10.3.4 internals).</summary>
    static byte[] WrittenBlr(FbCommand cmd, string descriptor)
    {
        const BindingFlags Any = BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic;
        var statement = typeof(FbCommand).GetField("_statement", Any)!.GetValue(cmd)!;
        var desc = statement.GetType().GetProperty(descriptor, Any)!.GetValue(statement)!;
        var blr = desc.GetType().GetMethod("ToBlr", Any)!.Invoke(desc, null)!;
        return (byte[])blr.GetType().GetProperty("Data")!.GetValue(blr)!;   // (its Length is the message's byte size)
    }

    public static void Run(string[] args)
    {
        using var con = Employee();
        using var tx = con.BeginTransaction(new FbTransactionOptions
        {
            TransactionBehavior = FbTransactionBehavior.Read | FbTransactionBehavior.ReadCommitted | FbTransactionBehavior.RecVersion,
        });

        Console.WriteLine("== computed column EMPLOYEE.FULL_NAME - RDB$FIELDS.RDB$COMPUTED_BLR");
        var d = new Reader(FetchBlob(con, tx,
            "select f.rdb$computed_blr from rdb$fields f" +
            " join rdb$relation_fields rf on f.rdb$field_name = rf.rdb$field_source" +
            " where rf.rdb$relation_name = 'EMPLOYEE' and rf.rdb$field_name = 'FULL_NAME'"));
        Console.WriteLine($"(.NET type: {d.B.GetType().Name})");
        HexDump(d.B, 64);
        Console.WriteLine(d.U8() == blr_version5 ? "blr_version5" : "unexpected version!");
        d.Expr(1);
        Console.WriteLine(!d.AtEnd && d.B[d.P] == blr_eoc ? "blr_eoc" : "(no blr_eoc?)");

        Console.WriteLine("\n== procedure GET_EMP_PROJ - RDB$PROCEDURES.RDB$PROCEDURE_BLR");
        d = new Reader(FetchBlob(con, tx,
            "select rdb$procedure_blr from rdb$procedures where rdb$procedure_name = 'GET_EMP_PROJ'"));
        HexDump(d.B, 32);
        d.Head();
        d.Messages();
        Console.WriteLine($"... {d.B.Length - d.P} more bytes - see isql SET BLOB ALL for the full dump");

        // -- the other direction: the BLR this provider writes ------------------
        const string sql = "select proj_id from employee_project where emp_no = @emp and proj_id = @proj";
        using var cmd = new FbCommand(sql, con, tx);
        cmd.Parameters.Add("@emp", FbDbType.SmallInt).Value = 0;
        cmd.Parameters.Add("@proj", FbDbType.Char).Value = "";
        cmd.Prepare();
        foreach (var (what, desc) in new[] { ("parameters (op_execute)", "Parameters"), ("output row (op_fetch)", "Fields") })
        {
            Console.WriteLine($"\n== BLR FirebirdClient writes for the {what} of");
            Console.WriteLine($"   {sql}");
            d = new Reader(WrittenBlr(cmd, desc));
            HexDump(d.B, 64);
            d.Head();
            d.Messages();
            d.Tail();
        }
        tx.Commit();
    }
}
