//
// OdsHeader.cs - the header page read three ways (C# twin of
// ../../cpp/ods_header.cpp; see ../../../on-disk-structure.md).
//
// The same acts: create a scratch database through the server, ask
// MON$DATABASE for the server's view, detach, then read page 0 straight
// from the file at the byte offsets src/jrd/ods.h pins with static_asserts,
// and finish with a page-type census (byte 0 of every page).  Between the
// two sits the database-info API, and here FirebirdClient sits at the
// opposite end from Jaybird's raw buffer: FbDatabaseInfo has one typed
// method per isc_info_* item (GetOdsVersion, GetPageSize,
// GetAllocationPages, GetOldestTransaction ... GetNextTransaction as Int64,
// GetDbGuid as a System.Guid), each its own isc_database_info round trip,
// with the clumplets decoded inside the provider.  The file act is a
// FileStream + BinaryPrimitives (little-endian), and hdr_guid needs no
// hand formatting: its on-disk Win32 GUID layout is exactly what
// new Guid(ReadOnlySpan<byte>) expects.  It reads the server-owned file, so
// run it on the server machine with read access to it (a member of the
// firebird group).
//
// Run:  cd samples/csharp && dotnet run -- OdsHeader [database]
//
using System.Buffers.Binary;
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class OdsHeader
{
    static readonly string[] PageTypes = { "undefined", "pag_header", "pag_pages (PIP)",
        "pag_transactions (TIP)", "pag_pointer", "pag_data", "pag_root",
        "pag_index (b-tree)", "pag_blob", "pag_ids (generators)", "pag_scns" };

    static string G(Guid g) => g.ToString("B").ToUpperInvariant();

    static Guid InfoView(FbConnection con)
    {
        var info = new FbDatabaseInfo(con);
        Console.WriteLine($"  GetOdsVersion()={info.GetOdsVersion()}  GetOdsMinorVersion()={info.GetOdsMinorVersion()}"
                          + $"  GetPageSize()={info.GetPageSize()}  GetAllocationPages()={info.GetAllocationPages()}");
        Console.WriteLine($"  GetOldestTransaction()={info.GetOldestTransaction()}  GetOldestActiveTransaction()="
                          + $"{info.GetOldestActiveTransaction()}  GetOldestActiveSnapshot()={info.GetOldestActiveSnapshot()}"
                          + $"  GetNextTransaction()={info.GetNextTransaction()}");
        var guid = info.GetDbGuid();
        Console.WriteLine($"  GetDbGuid()={G(guid)}  GetForcedWrites()={info.GetForcedWrites()}");
        return guid;
    }

    static Guid FileView(string file)
    {
        using var fs = new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        var h = new byte[152];                           // sizeof(Ods::header_page)
        fs.ReadExactly(h);
        int u16(int o) => BinaryPrimitives.ReadUInt16LittleEndian(h.AsSpan(o));
        long i64(int o) => BinaryPrimitives.ReadInt64LittleEndian(h.AsSpan(o));

        int type = h[0], pageSize = u16(16), ods = u16(18), flags = u16(22);
        Console.WriteLine($"pag_type      @0   = {type} ({PageTypes[type]})");
        Console.WriteLine($"pag_flags     @1   = {h[1]}");
        Console.WriteLine($"hdr_page_size @16  = {pageSize}");
        Console.WriteLine($"hdr_ods_version @18 = 0x{ods:x4} -> ODS {ods & 0x7fff} (FIREBIRD flag 0x8000 "
                          + $"{((ods & 0x8000) != 0 ? "set" : "clear")}), minor @20 = {u16(20)}");
        Console.WriteLine($"hdr_flags     @22  = 0x{flags:x2} ({((flags & 0x2) != 0 ? "force_write " : "")}"
                          + $"{((flags & 0x8) != 0 ? "no_reserve " : "")}{((flags & 0x10) != 0 ? "SQL_dialect_3" : "")})");
        Console.WriteLine($"hdr_PAGES     @28  = {BinaryPrimitives.ReadUInt32LittleEndian(h.AsSpan(28))}"
                          + "   <- pointer page of RDB$PAGES (catalog bootstrap anchor)");
        Console.WriteLine($"hdr_next_transaction   @40 = {i64(40)}");
        Console.WriteLine($"hdr_oldest_transaction @48 = {i64(48)} (OIT)");
        Console.WriteLine($"hdr_oldest_active      @56 = {i64(56)} (OAT)");
        Console.WriteLine($"hdr_oldest_snapshot    @64 = {i64(64)} (OST)");
        var guid = new Guid(h.AsSpan(84, 16));           // Win32 GUID layout = System.Guid's byte layout
        Console.WriteLine($"hdr_guid      @84  = {G(guid)}");

        // Page-type census: byte 0 of every page.
        var pages = fs.Length / pageSize;
        var counts = new int[PageTypes.Length];
        var one = new byte[1];
        for (long p = 0; p < pages; p++)
        {
            fs.Position = p * pageSize;
            fs.ReadExactly(one);
            counts[one[0] < counts.Length ? one[0] : 0]++;
        }
        Console.WriteLine();
        Console.WriteLine($"-- page-type census: {pages} pages of {pageSize} bytes --");
        for (var t = 0; t < counts.Length; t++)
            if (counts[t] > 0)
                Console.WriteLine($"  type {t,2}  {PageTypes[t],-22} {counts[t],5}");
        return guid;
    }

    public static void Run(string[] args)
    {
        var path = DbPath("ods", args);
        Guid fromInfo;
        using (var con = AttachOrCreate(path))
        {
            Console.WriteLine("-- server's view (MON$DATABASE) --");
            var r = Rows(con, "select mon$page_size, mon$ods_major, mon$ods_minor, mon$oldest_transaction,"
                              + " mon$oldest_active, mon$oldest_snapshot, mon$next_transaction from mon$database")[0];
            Console.WriteLine($"page_size ods_major ods_minor oit oat ost next = {string.Join(" ", r.Select(Text))}");

            Console.WriteLine();
            Console.WriteLine("-- the same through the info API (FbDatabaseInfo, typed) --");
            fromInfo = InfoView(con);
        }

        Console.WriteLine();
        Console.WriteLine($"-- header page, parsed from {path} (offsets per ods.h) --");
        var fromFile = FileView(path);
        Console.WriteLine($"GetDbGuid() == hdr_guid ? {fromInfo == fromFile}");
        Console.WriteLine("done.");
    }
}
