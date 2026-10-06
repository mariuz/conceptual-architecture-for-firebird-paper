//
// Pooling.cs - both directions of connection pooling (C# twin of
// ../../cpp/pooling.cpp; see ../../../connection-pooling.md).
//
// Outbound: the server-side external-connections (EDS) pool, exactly as in
// the C++ sample - tune it with ALTER EXTERNAL CONNECTIONS POOL, make three
// EXECUTE STATEMENT ... ON EXTERNAL calls, read the pool's context
// variables - plus a run with no FbTransaction, where the provider's
// implicit transaction is a hard commit and parks the external connection
// at once.
//
// Inbound: this is where the provider differs from every other twin.
// ADO.NET pooling is ON by default (FbSample turns it off so that one
// FbConnection is one attachment), and FirebirdClient implements it
// itself, in the client process: a pool per distinct connection string,
// Close() hands the attachment back instead of detaching, Open() pops it
// again, MaxPoolSize (100) caps the busy attachments, ConnectionLifetime
// (0 = forever) and a 2-second cleanup timer prune the idle ones, and
// FbConnection.ClearPool / ClearAllPools detach them.  On return it rolls
// back the open transaction and frees prepared statements, but runs no
// ALTER SESSION RESET, so session state outlives the borrower - the same
// lesson Jaybird's PooledConnection and the Go pool teach.
//
// Run:  cd samples/csharp && dotnet run -- Pooling [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Pooling
{
    const string App = "fbsamples-pooling-cs";

    static void PoolState(FbConnection con, FbTransaction? tx, string moment)
    {
        var r = Rows(con,
            "select rdb$get_context('SYSTEM', 'EXT_CONN_POOL_SIZE')," +
            "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_LIFETIME')," +
            "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT')," +
            "       rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT')" +
            " from rdb$database", tx)[0];
        Console.WriteLine($"{moment,-29}size={r[0]} lifetime={r[1]}s idle={r[2]} active={r[3]}");
    }

    static string Block(string external) =>
        "execute block returns (idle varchar(10), active varchar(10)) as\n" +
        "  declare i int = 0;\n" +
        "  declare v int;\n" +
        "begin\n" +
        "  while (i < 3) do\n" +
        "  begin\n" +
        $"    execute statement 'select 1 from rdb$database' on external '{external}'\n" +
        $"      as user '{User}' password '{Password}' into :v;\n" +
        "    i = i + 1;\n" +
        "  end\n" +
        "  idle   = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_IDLE_COUNT');\n" +
        "  active = rdb$get_context('SYSTEM', 'EXT_CONN_POOL_ACTIVE_COUNT');\n" +
        "  suspend;\n" +
        "end";

    static void Outbound(string database, string external)
    {
        Console.WriteLine("-- outbound: the server-side EDS pool --");
        using var con = Attach(database, "NONE");
        Execute(con, "alter external connections pool set size 5");
        Execute(con, "alter external connections pool set lifetime 30 second");
        PoolState(con, null, "before:");

        using (var tx = con.BeginTransaction())
        {
            var r = Rows(con, Block(external), tx)[0];
            Console.WriteLine($"{"inside the block:",-29}idle={r[0]} active={r[1]}   (3 calls, 1 outbound connection)");
            tx.CommitRetaining();
            PoolState(con, tx, "after CommitRetaining():");
            tx.Commit();
        }
        PoolState(con, null, "after Commit():");

        var auto = Rows(con, Block(external))[0];   // no FbTransaction: implicit, hard-committed
        Console.WriteLine($"{"inside (implicit tx):",-29}idle={auto[0]} active={auto[1]}");
        PoolState(con, null, "after the implicit commit:");

        Execute(con, "alter external connections pool clear all");
        PoolState(con, null, "after CLEAR ALL:");
    }

    static void Inbound(string database)
    {
        Console.WriteLine("\n-- inbound: FirebirdClient's own client-side pool --");
        var pooled = Builder(database, "NONE");
        pooled.Pooling = true;            // the ADO.NET default; FbSample switches it off
        pooled.MaxPoolSize = 2;
        pooled.ApplicationName = App;     // isc_dpb_process_name -> MON$REMOTE_PROCESS
        var cs = pooled.ToString();

        using var monitor = Attach(database, "NONE");   // unpooled, outside the pool
        int Attachments()
        {
            using var tx = monitor.BeginTransaction();  // fresh snapshot of MON$
            var n = Convert.ToInt32(Scalar(monitor,
                $"select count(*) from mon$attachments where mon$remote_process = '{App}'", tx));
            tx.Commit();
            return n;
        }

        long id1;
        using (var c1 = new FbConnection(cs))
        {
            c1.Open();
            id1 = Convert.ToInt64(Scalar(c1, "select current_connection from rdb$database"));
            Scalar(c1, "select rdb$set_context('USER_SESSION', 'BORROWER', 'first') from rdb$database");
            Console.WriteLine($"{"1st Open():",-29}CURRENT_CONNECTION = {id1}, sets USER_SESSION BORROWER = 'first'");
        }
        Console.WriteLine($"{"after Close():",-29}{Attachments()} attachment(s) of {App} still open");

        using (var c2 = new FbConnection(cs))
        {
            c2.Open();
            var id2 = Convert.ToInt64(Scalar(c2, "select current_connection from rdb$database"));
            var who = Text(Scalar(c2, "select rdb$get_context('USER_SESSION', 'BORROWER') from rdb$database"));
            Console.WriteLine($"{"2nd Open():",-29}CURRENT_CONNECTION = {id2}, BORROWER = {who}"
                              + (id2 == id1 ? "   <- same attachment, state leaked" : ""));
            Execute(c2, "alter session reset");
            who = Text(Scalar(c2, "select rdb$get_context('USER_SESSION', 'BORROWER') from rdb$database"));
            Console.WriteLine($"{"after ALTER SESSION RESET:",-29}BORROWER = {who}");

            using var c3 = new FbConnection(cs);
            c3.Open();
            Console.WriteLine($"{"two open at once:",-29}{Attachments()} attachment(s)");
            try
            {
                using var c4 = new FbConnection(cs);
                c4.Open();
                Console.WriteLine("unexpected: a third Open() succeeded");
            }
            catch (InvalidOperationException e)
            {
                Console.WriteLine($"{"a third (MaxPoolSize=2):",-29}{e.GetType().Name}: {e.Message}");
            }
        }
        Console.WriteLine($"{"both closed:",-29}{Attachments()} attachment(s) idle in the pool");

        var other = new FbConnectionStringBuilder(cs) { PacketSize = 16384 }.ToString();
        using (var c5 = new FbConnection(other))
            c5.Open();
        Console.WriteLine($"{"another connection string:",-29}{Attachments()} attachment(s) - a second pool");

        FbConnection.ClearAllPools();
        Console.WriteLine($"{"FbConnection.ClearAllPools:",-29}{Attachments()} attachment(s) left -- real detaches");
    }

    public static void Run(string[] args)
    {
        var database = args.Length > 0 ? args[0] : "employee";
        var external = args.Length > 1 ? args[1] : $"inet://{Host}/employee";   // the DSN the SERVER connects to
        try
        {
            Outbound(database, external);
            Inbound(database);
        }
        finally
        {
            FbConnection.ClearAllPools();   // never leave pooled attachments behind
        }
        Console.WriteLine("done.");
    }
}
