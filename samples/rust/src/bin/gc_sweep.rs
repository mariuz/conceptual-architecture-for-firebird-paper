//! gc_sweep.rs — the garbage-collection-and-sweep scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/gc_sweep.cpp: creates record versions
//! under a pinned SNAPSHOT, then releases the snapshot and watches the
//! collectors work — entirely through the database-level MON$RECORD_STATS
//! counters, which count exactly the vio.cpp events the document describes
//! (MON$RECORD_IMGC = intermediate GC, MON$RECORD_PURGES = purge(),
//! MON$RECORD_EXPUNGES = expunge(), MON$BACKVERSION_READS = chain walks).
//! Honest delta at the end: the C++ twin's rolled-back OIT stump needs the
//! isc_tpb_no_auto_undo TPB flag, and rsfbclient's typed
//! TransactionConfiguration has no way to say it (nor to pass raw TPB
//! bytes, as the node-firebird twin does with [2, 20]).
//! See ../../garbage-collection-and-sweep.md.
//!
//! Run (see ../README.md):  cargo run --bin gc_sweep

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction};
use std::thread::sleep;
use std::time::Duration;

/// MON$ tables are a stable snapshot per transaction: use a fresh
/// transaction for every peek so the counters are current.
fn show_stats(conn: &mut SimpleConnection, label: &str) -> Result<(), FbError> {
    let mut t = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    let (upd, imgc, purges, expunges, backreads): (i64, i64, i64, i64, i64) = t
        .query_first(
            "select r.MON$RECORD_UPDATES, r.MON$RECORD_IMGC, \
                    r.MON$RECORD_PURGES, r.MON$RECORD_EXPUNGES, \
                    r.MON$BACKVERSION_READS \
             from MON$RECORD_STATS r join MON$DATABASE d using (MON$STAT_ID)",
            (),
        )?
        .unwrap();
    println!("{:<34} upd={:<4} imgc={:<3} purges={:<3} expunges={:<3} backreads={}",
        label, upd, imgc, purges, expunges, backreads);
    t.commit()
}

fn show_counters(conn: &mut SimpleConnection, label: &str) -> Result<(), FbError> {
    let mut t = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    let (oit, oat, ost, next, interval): (i64, i64, i64, i64, i64) = t
        .query_first(
            "select MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, \
                    MON$OLDEST_SNAPSHOT, MON$NEXT_TRANSACTION, \
                    MON$SWEEP_INTERVAL from MON$DATABASE",
            (),
        )?
        .unwrap();
    println!("{:<34} OIT={} OAT={} OST={} Next={} (sweep interval {})",
        label, oit, oat, ost, next, interval);
    t.commit()
}

fn one_shot(conn: &mut SimpleConnection, sql: &str) -> Result<(), FbError> {
    let mut t = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    t.execute(sql, ())?;
    t.commit()
}

fn read_val(conn: &mut SimpleConnection, sql: &str) -> Result<i64, FbError> {
    let mut t = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    let (v,): (i64,) = t.query_first(sql, ())?.unwrap();
    t.commit()?;
    Ok(v)
}

fn main() -> Result<(), FbError> {
    let mut writer = connect("gc_sweep")?;
    let mut pinner = connect("gc_sweep")?;

    let snapshot = TransactionConfiguration {
        isolation: TrIsolationLevel::Concurrency,
        ..TransactionConfiguration::default()
    };

    // Scratch table (idempotent).
    {
        let mut t = SimpleTransaction::new(&mut writer, TransactionConfiguration::default())?;
        let _ = t.execute("drop table gctest", ());
        t.commit()?;
    }
    one_shot(&mut writer, "create table gctest (id int primary key, val int)")?;
    one_shot(&mut writer, "insert into gctest values (1, 0)")?;

    // 1. Pin a snapshot: while this SNAPSHOT transaction lives, its
    //    tra_oldest_active holds the OST down and version 0 must survive.
    let mut snap = SimpleTransaction::new(&mut pinner, snapshot)?;
    let (v0,): (i64,) = snap
        .query_first("select val from gctest where id = 1", ())?
        .unwrap();
    println!("pinned SNAPSHOT reads val = {}", v0);
    show_stats(&mut writer, "before updates:")?;

    // 2. Twelve committed updates -> twelve back versions... in theory.
    for i in 1..=12 {
        one_shot(&mut writer, &format!("update gctest set val = {} where id = 1", i))?;
    }
    show_stats(&mut writer, "after 12 updates (snapshot open):")?;
    let (v1,): (i64,) = snap
        .query_first("select val from gctest where id = 1", ())?
        .unwrap();
    println!("pinned SNAPSHOT still reads val = {}", v1);

    // 3. Release the snapshot; a sequential scan now trips over the
    //    below-OST chain (cooperative GC) and/or notifies the GC thread.
    snap.commit()?;
    println!("snapshot released; new reader sees val = {}",
        read_val(&mut writer, "select val from gctest where id = 1")?);
    sleep(Duration::from_millis(1500));
    show_stats(&mut writer, "after release + scan + 1.5s:")?;

    // 4. A committed DELETE older than the OST is expunged, not purged.
    one_shot(&mut writer, "delete from gctest where id = 1")?;
    read_val(&mut writer, "select count(*) from gctest")?; // scan -> collect
    sleep(Duration::from_millis(1500));
    show_stats(&mut writer, "after DELETE + scan + 1.5s:")?;

    // 5. The C++ twin now rolls back an isc_tpb_no_auto_undo transaction to
    //    freeze the OIT on an "interesting" stump.  rsfbclient cannot ask
    //    for that: TransactionConfiguration only speaks isolation, lock
    //    resolution and access mode — no no_auto_undo, no raw TPB bytes.
    //    A default rollback keeps its undo log, so the engine undoes the
    //    work and marks the transaction COMMITTED in the TIP: no stump, and
    //    the OIT keeps moving.
    show_counters(&mut writer, "header counters before rollback:")?;
    {
        let mut t = SimpleTransaction::new(&mut writer, snapshot)?;
        t.execute("insert into gctest values (2, 0)", ())?;
        t.rollback()?;
    }
    show_counters(&mut writer, "after auto-undo rollback:")?;
    println!("no OIT stump: without isc_tpb_no_auto_undo (unreachable through");
    println!("rsfbclient's typed TPB) the rollback is undone in memory and the");
    println!("transaction leaves the TIP as committed — see the C++/JS twins.");
    Ok(())
}
