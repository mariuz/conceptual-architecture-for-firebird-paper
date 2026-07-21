//! metadata_cache.rs — the metadata-cache scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/metadata_cache.cpp: the cache's
//! visibility rule observed through two attachments.  An uncommitted
//! ALTER is visible only to its own transaction; a committed one is
//! visible immediately even to a statement prepared inside an older,
//! still-open SNAPSHOT transaction (metadata is read-committed, records
//! are not); two uncommitted DDLs collide with the engine's "object in
//! use" newVersion error; and RDB$FORMATS keeps one row per shape the
//! table has lived through.  Explicit SimpleTransaction objects keep each
//! prepare inside the intended transaction (each connection's hidden
//! default transaction would blur exactly the boundaries this sample is
//! about).  See ../../metadata-cache.md.
//!
//! Run (see ../README.md):  cargo run --bin metadata_cache

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn flat(e: &FbError) -> String {
    e.to_string().replace('\n', " ")
}

// Run a single-value query in the given transaction, printing the value
// or the error (both outcomes are data here).
fn try_query(who: &str, tr: &mut SimpleTransaction, sql: &str) {
    match tr.query_first::<(), (Option<i64>,)>(sql, ()) {
        Ok(Some((Some(v),))) => println!("{}: {} -> {}", who, sql, v),
        Ok(Some((None,))) => println!("{}: {} -> <null>", who, sql),
        Ok(None) => println!("{}: {} -> (no rows)", who, sql),
        Err(e) => println!("{}: {} -> ERROR: {}", who, sql, flat(&e)),
    }
}

fn main() -> Result<(), FbError> {
    let mut a = connect("metadata_cache")?;
    let mut b = connect("metadata_cache")?;

    {
        let mut t = SimpleTransaction::new(&mut a, Default::default())?;
        t.execute("recreate table t (a integer)", ())?;
        t.commit()?;
    }
    {
        let mut t = SimpleTransaction::new(&mut a, Default::default())?;
        t.execute("insert into t values (1)", ())?;
        t.commit()?;
    }

    // -- 1. uncommitted DDL: mine, and mine alone -----------------------
    println!("== 1. uncommitted ALTER: visible to creator only ==");
    let mut a_ddl = SimpleTransaction::new(&mut a, Default::default())?; // stays open
    a_ddl.execute("alter table t add e integer", ())?;
    try_query("A (same tx)  ", &mut a_ddl, "select e from t");
    {
        let mut b_tra = SimpleTransaction::new(&mut b, Default::default())?;
        try_query("B            ", &mut b_tra, "select e from t");
        b_tra.commit()?;
    }

    // -- 2. committed DDL ignores open snapshots ------------------------
    println!("\n== 2. committed ALTER: seen even inside B's open SNAPSHOT tx ==");
    let snapshot = TransactionConfiguration {
        isolation: TrIsolationLevel::Concurrency,
        ..TransactionConfiguration::default()
    };
    let mut b_snap = SimpleTransaction::new(&mut b, snapshot)?;
    try_query("B (snapshot) ", &mut b_snap, "select count(*) from t");
    a_ddl.commit()?; // E becomes committed
    {
        let mut t = SimpleTransaction::new(&mut a, Default::default())?;
        t.execute("alter table t add d integer", ())?;
        t.commit()?; // D committed after B's snapshot started
    }
    try_query("B (same  tx) ", &mut b_snap, "select d from t");
    println!(
        "   (records are snapshot-isolated; metadata is read-committed —\n\
         \x20   the new statement was prepared against the chain's current head)"
    );
    b_snap.commit()?;

    // -- 3. concurrent DDL: the newVersion collision --------------------
    println!("\n== 3. two uncommitted DDLs on one object ==");
    let mut a_ddl = SimpleTransaction::new(&mut a, Default::default())?;
    a_ddl.execute("alter table t add f integer", ())?;
    let mut b_ddl = SimpleTransaction::new(&mut b, Default::default())?;
    match b_ddl.execute("alter table t add g integer", ()) {
        Ok(_) => println!("B: ALTER unexpectedly succeeded"),
        Err(e) => println!("B: ALTER failed:\n{}", e),
    }
    b_ddl.rollback()?;
    a_ddl.rollback()?; // F vanishes with the rollback

    // -- 4. the on-disk half: one format per committed shape ------------
    println!("\n== 4. RDB$FORMATS after the committed DDL ==");
    let mut t = SimpleTransaction::new(&mut a, Default::default())?;
    let (formats,): (i64,) = t
        .query_first(
            "select count(*) from rdb$formats f \
             join rdb$relations r on f.rdb$relation_id = r.rdb$relation_id \
             where r.rdb$relation_name = 'T'",
            (),
        )?
        .unwrap();
    println!("formats stored for T: {} (T has lived through that many shapes)", formats);
    let (ca, ce, cd): (Option<i64>, Option<i64>, Option<i64>) = t
        .query_first("select a, e, d from t", ())?
        .unwrap();
    let show = |v: Option<i64>| v.map_or("<null>".to_string(), |x| x.to_string());
    println!(
        "row written under shape 1 decodes as: a={}, e={}, d={}",
        show(ca), show(ce), show(cd)
    );
    t.commit()?;

    println!("done.");
    Ok(())
}
