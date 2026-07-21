//! transactions.rs — the transactions-and-concurrency scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/transactions_demo.cpp: two attachments,
//! SNAPSHOT vs READ COMMITTED visibility of a concurrent commit, then a
//! NO WAIT update conflict.  The TPB decisions the OO-API sample spells
//! out byte by byte are a typed TransactionConfiguration here —
//! TrIsolationLevel::Concurrency / ReadCommited(RecordVersion),
//! TrLockResolution::NoWait — applied through explicit SimpleTransaction
//! objects.  (That explicitness matters: rsfbclient connections also carry
//! a hidden DEFAULT transaction whose commit()/rollback() are RETAINING —
//! it keeps its first configuration and, under SNAPSHOT, its first
//! snapshot, for the life of the connection.)
//! See ../../transactions-and-concurrency.md.
//!
//! Run (see ../README.md):  cargo run --bin transactions

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn main() -> Result<(), FbError> {
    let mut a = connect("transactions")?;
    let mut b = connect("transactions")?;

    let snapshot = TransactionConfiguration {
        isolation: TrIsolationLevel::Concurrency,
        lock_resolution: TrLockResolution::NoWait,
        ..TransactionConfiguration::default()
    };
    let read_committed = TransactionConfiguration {
        isolation: TrIsolationLevel::ReadCommited(TrRecordVersion::RecordVersion),
        lock_resolution: TrLockResolution::NoWait,
        ..TransactionConfiguration::default()
    };

    // Scratch table (idempotent).
    {
        let mut setup = SimpleTransaction::new(&mut a, read_committed)?;
        let _ = setup.execute("DROP TABLE ACCOUNTS", ());
        setup.execute(
            "CREATE TABLE ACCOUNTS (ID INT NOT NULL PRIMARY KEY, BALANCE INT)",
            (),
        )?;
        setup.commit()?;
    }
    {
        let mut setup = SimpleTransaction::new(&mut a, read_committed)?;
        setup.execute("INSERT INTO ACCOUNTS VALUES (1, 100)", ())?;
        setup.commit()?;
    }

    // -- 1. Visibility: a SNAPSHOT transaction cannot see a commit that
    //       happens after it starts; a READ COMMITTED one can.
    let mut tr_a = SimpleTransaction::new(&mut a, snapshot)?;
    let (before,): (i64,) = tr_a
        .query_first("SELECT BALANCE FROM ACCOUNTS WHERE ID = 1", ())?
        .unwrap();
    println!("snapshot sees balance {} at start", before);

    {
        let mut tr_b = SimpleTransaction::new(&mut b, read_committed)?;
        tr_b.execute("UPDATE ACCOUNTS SET BALANCE = 150 WHERE ID = 1", ())?;
        tr_b.commit()?;
        println!("B committed BALANCE = 150");
    }

    let (still,): (i64,) = tr_a
        .query_first("SELECT BALANCE FROM ACCOUNTS WHERE ID = 1", ())?
        .unwrap();
    println!("snapshot still sees {}   <- stable view", still);
    tr_a.commit()?;

    let mut tr_rc = SimpleTransaction::new(&mut a, read_committed)?;
    let (now,): (i64,) = tr_rc
        .query_first("SELECT BALANCE FROM ACCOUNTS WHERE ID = 1", ())?
        .unwrap();
    println!("read committed sees {}   <- follows commits", now);
    tr_rc.commit()?;

    // -- 2. Conflict: two NO WAIT updates of the same row.
    let mut tr_a = SimpleTransaction::new(&mut a, snapshot)?;
    let mut tr_b = SimpleTransaction::new(&mut b, snapshot)?;
    tr_a.execute("UPDATE ACCOUNTS SET BALANCE = BALANCE + 1 WHERE ID = 1", ())?;
    println!("A updated the row (uncommitted)");
    match tr_b.execute("UPDATE ACCOUNTS SET BALANCE = BALANCE + 2 WHERE ID = 1", ()) {
        Ok(_) => println!("BUG: conflicting update succeeded"),
        Err(e) => println!("B's conflicting update failed as designed:\n    {}", e),
    }
    tr_b.rollback()?;
    tr_a.commit()?;

    println!("done.");
    Ok(())
}
