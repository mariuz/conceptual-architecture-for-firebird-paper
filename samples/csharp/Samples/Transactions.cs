//
// Transactions.cs - MVCC isolation seen from the client (C# twin of
// ../../cpp/transactions_demo.cpp; see ../../../transactions-and-concurrency.md).
//
// Two attachments play the same scenario: SNAPSHOT stability, READ
// COMMITTED freshness, then a NO WAIT write conflict.  ADO.NET's
// IsolationLevel maps onto fixed TPBs, but the provider also takes the TPB
// item by item: FbTransactionOptions.TransactionBehavior is a [Flags] enum
// whose members are the isc_tpb_* items themselves (Concurrency,
// ReadCommitted | RecVersion, Write, NoWait), so the TPB the C++ sample
// packs by hand is spelled here as a flags expression.
//
// Run:  cd samples/csharp && dotnet run -- Transactions [database]
//
using FirebirdSql.Data.FirebirdClient;
using static FbSamples.FbSample;

namespace FbSamples.Samples;

public static class Transactions
{
    const string Select = "select amount from balance where id = 1";

    static readonly FbTransactionOptions Snapshot = new()
    {
        TransactionBehavior = FbTransactionBehavior.Concurrency
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.NoWait,
    };

    static readonly FbTransactionOptions ReadCommitted = new()
    {
        TransactionBehavior = FbTransactionBehavior.ReadCommitted
                              | FbTransactionBehavior.RecVersion
                              | FbTransactionBehavior.Write
                              | FbTransactionBehavior.NoWait,
    };

    public static void Run(string[] args)
    {
        var path = DbPath("tx", args);
        using var a = AttachOrCreate(path);
        using var b = Attach(path);

        Execute(a, "recreate table balance (id integer primary key, amount integer)");
        Execute(a, "insert into balance values (1, 100)");

        // --- 1. SNAPSHOT stability ---------------------------------------------
        using (var snapA = a.BeginTransaction(Snapshot))
        {
            Console.WriteLine($"A (SNAPSHOT)       sees amount = {Scalar(a, Select, snapA)}");

            Execute(b, "update balance set amount = 999 where id = 1");
            Console.WriteLine("B                  committed amount = 999");

            Console.WriteLine($"A (same SNAPSHOT)  sees amount = {Scalar(a, Select, snapA)}   <- still the start-of-tx version");
            snapA.Commit();
        }

        // --- 2. READ COMMITTED sees the new version -----------------------------
        using (var rcA = a.BeginTransaction(ReadCommitted))
        {
            Console.WriteLine($"A (READ COMMITTED) sees amount = {Scalar(a, Select, rcA)}   <- the committed version");
            rcA.Commit();
        }

        // --- 3. Write conflict under NO WAIT ------------------------------------
        using var holdB = b.BeginTransaction(Snapshot);
        Execute(b, "update balance set amount = amount + 1 where id = 1", holdB);

        using var loserA = a.BeginTransaction(Snapshot);
        try
        {
            Execute(a, "update balance set amount = amount + 10 where id = 1", loserA);
            Console.WriteLine("unexpected: conflicting update succeeded");
        }
        catch (FbException e)
        {
            Console.WriteLine("A conflicting update failed as designed:");
            Console.WriteLine("    " + ErrorText(e).Replace("\n", "\n    "));
            Console.WriteLine($"    SQLSTATE {e.SQLSTATE} / gds {e.ErrorCode} / {e.Errors.Count} status entries");
        }
        loserA.Rollback();
        holdB.Commit();
        Console.WriteLine("done.");
    }
}
