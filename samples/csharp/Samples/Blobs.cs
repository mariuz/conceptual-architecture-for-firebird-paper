//
// Blobs.cs - segmented and stream blobs through ADO.NET (C# twin of
// ../../cpp/blobs.cpp; see ../../../blob-handling.md).
//
// FirebirdClient has no public segment API: a blob parameter is a byte[] or
// string that the provider writes itself (op_create_blob, then
// op_put_segment in chunks of PacketSize bytes - 8192 by default - into a
// SEGMENTED blob), and a blob column reads back whole (GetValue) or as a
// System.IO.Stream (GetStream -> FirebirdSql.Data.Common.BlobStream).  So
// the C++ scenario is played with what the provider and the engine offer:
//   1. three explicit segments, written in SQL: RDB$BLOB_UTIL.NEW_BLOB
//      (segmented) and one BLOB_APPEND per segment, stored in the row as an
//      8-byte blob id.  The PSQL variable is BINARY on purpose: declared as
//      a UTF8 text blob, the three appends came out as ONE 40-byte segment;
//   2. read back with BlobStream.Read(64 bytes): op_get_segment asks the
//      server to fill a buffer, the server packs every length-prefixed
//      segment that fits, the provider strips the prefixes - one 40-byte
//      chunk (as with Jaybird; libfbclient hands out one segment per call).
//      The boundaries are still stored: RDB$BLOB_UTIL.READ_DATA(h, NULL)
//      returns one segment per call, server side;
//   3. the provider's own write of a 20000-byte byte[]: three segments of
//      at most PacketSize bytes;
//   4. seeking: BlobStream.CanSeek says true, but op_seek_blob only works
//      on STREAM blobs - the provider's own (segmented) blob refuses it,
//      a stream blob made with RDB$BLOB_UTIL.NEW_BLOB(FALSE, ...) seeks.
//      And a provider bug shows up: BlobStream.Read(buffer, offset, count)
//      bounds the copy by buffer.Length - offset, not by count, so
//      Read(buf, 0, 6) on a 64-byte buffer returns 27 bytes (10.3.4, and
//      still so in NETProvider master); a buffer of exactly count bytes
//      sidesteps it;
//   5. subtype text vs binary from the catalog, and BLOB_APPEND.
//
// Run:  cd samples/csharp && dotnet run -- Blobs [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Blobs
{
    // One row per stored segment, read on the server.  (A first version
    // opened coalesce(note, data): COALESCE builds a new blob, and the copy
    // had merged the three segments into one.)
    static string Segments(string column) =>
        "execute block (id int = @id) returns (n int, len int) as\n" +
        "  declare h integer;\n" +
        "  declare seg varbinary(32765);\n" +
        "begin\n" +
        "  h = rdb$blob_util.open_blob((select " + column + " from docs where id = :id));\n" +
        "  n = 0;\n" +
        "  while (true) do begin\n" +
        "    seg = rdb$blob_util.read_data(h, null);\n" +
        "    if (seg is null) then leave;\n" +
        "    n = n + 1; len = octet_length(seg); suspend;\n" +
        "  end\n" +
        "end";

    static void StoredSegments(FbConnection con, FbTransaction tx, string column, int id)
    {
        using var cmd = new FbCommand(Segments(column), con, tx);
        cmd.Parameters.Add("@id", id);
        using var r = cmd.ExecuteReader();
        var lens = new List<int>();
        while (r.Read()) lens.Add(r.GetInt32(1));
        Console.WriteLine($"  stored (RDB$BLOB_UTIL.READ_DATA, server side): {lens.Count} segments [{string.Join(", ", lens)}]");
    }

    static Stream OpenBlob(FbConnection con, FbTransaction tx, string column, int id, out FbDataReader reader)
    {
        var cmd = new FbCommand($"select {column} from docs where id = {id}", con, tx);
        reader = cmd.ExecuteReader();
        reader.Read();
        return reader.GetStream(0);
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("blobs", args));
        Execute(con, "recreate table docs (id integer primary key," +
                     " note blob sub_type text character set utf8, data blob sub_type binary)");
        using var tx = con.BeginTransaction();

        // -- 1. three explicit segments, written in SQL -------------------------
        Execute(con,
            "execute block as\n" +
            "  declare b blob sub_type binary;\n" +
            "begin\n" +
            "  b = rdb$blob_util.new_blob(true, false);\n" +   // segmented, not a temp-storage blob
            "  b = blob_append(b, 'first segment');\n" +
            "  b = blob_append(b, 'second, longer segment');\n" +
            "  b = blob_append(b, 'third');\n" +
            "  insert into docs (id, note) values (1, :b);\n" +
            "end", tx);
        Console.WriteLine("id 1: 3 segments via RDB$BLOB_UTIL.NEW_BLOB + BLOB_APPEND (no putSegment in the provider)");

        // -- 2. read it back as a Stream ----------------------------------------
        var buf = new byte[64];
        using (var s = OpenBlob(con, tx, "note", 1, out var r1))
        using (r1)
        {
            Console.WriteLine($"  GetStream(): {s.GetType().FullName}, Length = {s.Length} (isc_info_blob_total_length)");
            int got, n = 0;
            while ((got = s.Read(buf, 0, buf.Length)) > 0)
                Console.WriteLine($"  Read(64) #{++n}: {got} bytes  \"{System.Text.Encoding.UTF8.GetString(buf, 0, got)}\"");
        }
        StoredSegments(con, tx, "note", 1);

        // -- 3. the provider's own write: a 20000-byte byte[] parameter ----------
        var big = new byte[20000];
        new Random(42).NextBytes(big);
        using (var ins = new FbCommand("insert into docs (id, data) values (3, @data)", con, tx))
        {
            ins.Parameters.Add("@data", FbDbType.Binary).Value = big;
            ins.ExecuteNonQuery();
        }
        Console.WriteLine($"\nid 3: a {big.Length}-byte byte[] parameter (PacketSize = {new FbConnectionStringBuilder(con.ConnectionString).PacketSize})");
        StoredSegments(con, tx, "data", 3);
        var back = (byte[])Scalar(con, "select data from docs where id = 3", tx)!;
        Console.WriteLine($"  GetValue(): byte[{back.Length}], identical = {back.AsSpan().SequenceEqual(big)}");

        // -- 4. seeking: only a stream blob can ---------------------------------
        Console.WriteLine();
        using (var s = OpenBlob(con, tx, "data", 3, out var r3))
        using (r3)
        {
            try
            {
                s.Seek(13, SeekOrigin.Begin);
                Console.WriteLine("id 3 (segmented): Seek(13) unexpectedly worked");
            }
            catch (Exception e) when (e is FbException || e.GetType().Name == "IscException")
            {
                // BlobStream throws the provider's INTERNAL IscException, not the public FbException
                Console.WriteLine($"id 3 (segmented): CanSeek = {s.CanSeek}, Seek(13) -> {e.GetType().Name}: {e.Message}");
            }
        }
        Execute(con,
            "execute block as\n" +
            "  declare b blob sub_type binary;\n" +
            "begin\n" +
            "  b = rdb$blob_util.new_blob(false, false);\n" +  // a STREAM blob
            "  b = blob_append(b, 'first segment');\n" +
            "  b = blob_append(b, 'second, longer segment');\n" +
            "  b = blob_append(b, 'third');\n" +
            "  insert into docs (id, note) values (4, :b);\n" +
            "end", tx);
        foreach (var size in new[] { 6, 64 })   // a fresh stream each time
        {
            using var s = OpenBlob(con, tx, "note", 4, out var r4);
            using (r4)
            {
                var b = new byte[size];
                s.Seek(13, SeekOrigin.Begin);
                var got = s.Read(b, 0, 6);
                Console.WriteLine($"id 4 (stream):    Seek(13), Read(byte[{size}], 0, 6) returned {got} bytes: " +
                                  $"\"{System.Text.Encoding.UTF8.GetString(b, 0, got)}\"" + (got > 6 ? "   <- count ignored" : ""));
            }
        }
        StoredSegments(con, tx, "note", 4);

        // -- 5. subtype text vs binary, and BLOB_APPEND -------------------------
        Console.WriteLine("\n-- column subtypes (RDB$FIELDS) --");
        foreach (var row in Rows(con,
                     "select trim(rf.rdb$field_name), f.rdb$field_sub_type, trim(cs.rdb$character_set_name) " +
                     "from rdb$relation_fields rf join rdb$fields f on rf.rdb$field_source = f.rdb$field_name " +
                     "left join rdb$character_sets cs on f.rdb$character_set_id = cs.rdb$character_set_id " +
                     "where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261 order by 1", tx))
            Console.WriteLine($"{Text(row[0]),-5} subtype {Text(row[1])}  charset {Text(row[2])}");

        Execute(con, "insert into docs (id, note) values (2, " +
                     "blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))", tx);
        var ba = Rows(con, "select octet_length(note), char_length(note), note from docs where id = 2", tx)[0];
        Console.WriteLine($"\n-- BLOB_APPEND result --\nid 2: {ba[0]} octets, {ba[1]} chars, GetValue() -> {ba[2]!.GetType().Name} \"{ba[2]}\"");

        tx.Commit();
        Console.WriteLine("done.");
    }
}
