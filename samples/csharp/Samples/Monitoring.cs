//
// Monitoring.cs - the MON$ hierarchy and its stable snapshot (C# twin of
// ../../cpp/monitoring.cpp; see ../../../monitoring-and-tuning.md).
//
// Walks MON$DATABASE -> MON$ATTACHMENTS -> MON$TRANSACTIONS ->
// MON$STATEMENTS down to this attachment, then runs a 10 000-row full scan
// inside the same transaction and shows its own MON$STAT_ID-joined counters
// frozen - the first MON$ select took a stable snapshot - until a new
// transaction refreshes them.  ADO.NET's trap is the JDBC one: a command
// without a transaction auto-commits, so every MON$ read would be a fresh
// snapshot; the sample runs the demonstration in one explicit
// FbTransaction (Concurrency = SNAPSHOT).  Beside MON$, the provider
// carries the live channel too: FbDatabaseInfo.GetFetches() and GetReads()
// are the info items isc_info_fetches and isc_info_reads over
// op_info_database, answered at the moment of the call - so they move
// while the MON$ row stands still.  (Its GetReadSeqCount() is no help: it
// decodes isc_info_read_seq_count's list of (relation id, count) pairs as
// one integer and returns noise, so the per-relation view Jaybird decodes
// is out of reach here.)
//
// Run:  cd samples/csharp && dotnet run -- Monitoring [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Monitoring
{
    const string Counters =
        "SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, R.MON$RECORD_INSERTS, "
      + "       I.MON$PAGE_FETCHES, I.MON$PAGE_READS "
      + "FROM MON$ATTACHMENTS A "
      + "JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID "
      + "JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID "
      + "WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION";

    static readonly FbTransactionOptions Snapshot = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency | FbTransactionBehavior.Write
                              | FbTransactionBehavior.Wait,
    };

    /// <summary>Every row as name=value pairs (names from the reader).</summary>
    static void Show(FbConnection con, FbTransaction tx, string label, string sql)
    {
        using var cmd = new FbCommand(sql, con, tx);
        using var r = cmd.ExecuteReader();
        while (r.Read())
        {
            var pairs = Enumerable.Range(0, r.FieldCount).Select(i =>
                r.GetName(i).Replace("MON$", "").Replace("RECORD_", "").ToLowerInvariant()
                + "=" + Text(r.GetValue(i)).Trim());
            Console.WriteLine($"{label,-39} {string.Join(" ", pairs)}");
        }
    }

    /// <summary>The live channel: database info items, no snapshot involved.</summary>
    static void LiveInfo(FbConnection con, string label)
    {
        var info = new FbDatabaseInfo(con);
        Console.WriteLine($"{label,-39} isc_info_fetches={info.GetFetches()}  isc_info_reads={info.GetReads()}");
    }

    public static void Run(string[] args)
    {
        using var con = AttachOrCreate(DbPath("monitoring", args));
        Execute(con, "RECREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)");
        Execute(con, "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN "
                   + "  WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END "
                   + "END");

        // One explicit SNAPSHOT transaction: auto-commit would refresh every read.
        using (var tx = con.BeginTransaction(Snapshot))
        {
            // -- 1. the hierarchy, one consistent snapshot ------------------
            Show(con, tx, "MON$DATABASE:", "SELECT MON$OLDEST_TRANSACTION AS OIT, MON$OLDEST_ACTIVE AS OAT, "
                                         + "MON$NEXT_TRANSACTION AS NEXT, MON$PAGE_BUFFERS FROM MON$DATABASE");
            Show(con, tx, "attachment -> transaction -> statement:",
                 "SELECT A.MON$ATTACHMENT_ID AS ATT, TRIM(A.MON$USER) AS USR, T.MON$TRANSACTION_ID AS TX, "
               + "       S.MON$STATE, CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 30) AS VARCHAR(30)) AS SQL_HEAD "
               + "FROM MON$ATTACHMENTS A "
               + "JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID "
               + "JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID "
               + "WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION");

            // -- 2. the snapshot property, against the live info channel ----
            Show(con, tx, "MON$ snapshot 1:", Counters);
            LiveInfo(con, "info items, before the workload:");
            Console.WriteLine("... running workload: SELECT COUNT(*) full scan + indexed lookup ...");
            Console.WriteLine($"count = {Scalar(con, "SELECT COUNT(*) FROM MON_WORK", tx)}, "
                              + $"point = {Scalar(con, "SELECT VAL FROM MON_WORK WHERE ID = 4242", tx)}");
            Show(con, tx, "same transaction: STILL snapshot 1:", Counters);
            LiveInfo(con, "info items, same moment: live:");
            tx.Commit();
        }
        using (var tx = con.BeginTransaction(Snapshot))
        {
            Show(con, tx, "new transaction: fresh snapshot:", Counters);
            tx.Commit();
        }
        Console.WriteLine("done.");
    }
}
