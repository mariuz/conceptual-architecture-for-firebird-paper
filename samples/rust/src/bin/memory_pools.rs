//! memory_pools.rs — the memory-management scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/memory_pools.cpp: the pool hierarchy
//! seen through MON$MEMORY_USAGE — the per-level summary with the
//! parent-redirection signature (used > 0 while allocated = 0 for child
//! pools), one connection's own database -> attachment -> transaction
//! chain, and a transaction pool growing while an uncommitted 3000-row
//! UPDATE builds its undo log, watched from a second attachment because
//! MON$ snapshots are frozen per transaction (each observation below is
//! its own SimpleTransaction, hence its own fresh snapshot).
//! One driver note: SUM(bigint) is INT128 in Firebird 4+, a type
//! rsfbclient cannot fetch — the summary CASTs the sums back to BIGINT.
//! See ../../memory-management.md.
//!
//! Run (see ../README.md):  cargo run --bin memory_pools

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleConnection, SimpleTransaction};

// The six stat groups, summed — each call is a new transaction, i.e. a
// fresh MON$ snapshot.
fn level_summary(mon: &mut SimpleConnection) -> Result<(), FbError> {
    let mut t = SimpleTransaction::new(mon, Default::default())?;
    let rows: Vec<(i64, i64, i64, i64, i64)> = t.query(
        "select MON$STAT_GROUP, count(*), cast(sum(MON$MEMORY_USED) as bigint), \
                cast(sum(MON$MEMORY_ALLOCATED) as bigint), \
                count(nullif(MON$MEMORY_ALLOCATED, 0)) \
         from MON$MEMORY_USAGE group by 1 order by 1",
        (),
    )?;
    println!("stat_group (0=db 1=att 2=tra 3=stmt 5=cmp)  pools  used  allocated  with_own_extents");
    for (grp, pools, used, alloc, own) in rows {
        println!("  {:<10} {:>5}  {:>10}  {:>10}  {:>3}", grp, pools, used, alloc, own);
    }
    t.commit()
}

// One row of the worker's pool chain, freshly snapshotted.
fn pool_row(mon: &mut SimpleConnection, label: &str, join: &str) -> Result<(), FbError> {
    let mut t = SimpleTransaction::new(mon, Default::default())?;
    let row: Option<(i64, i64)> = t.query_first(
        &format!(
            "select MON$MEMORY_USED, MON$MEMORY_ALLOCATED from MON$MEMORY_USAGE {}",
            join
        ),
        (),
    )?;
    if let Some((used, alloc)) = row {
        println!("  {:<24} used={:<10} allocated={}", label, used, alloc);
    }
    t.commit()
}

fn main() -> Result<(), FbError> {
    let mut worker = connect("memory_pools")?;
    let mut monitor = connect("memory_pools")?;

    {
        let mut setup = SimpleTransaction::new(&mut worker, Default::default())?;
        setup.execute("recreate table t (id int, pad varchar(200))", ())?;
        setup.commit()?;
    }
    {
        let mut fill = SimpleTransaction::new(&mut worker, Default::default())?;
        fill.execute(
            "execute block as declare i int = 0; begin \
               while (i < 3000) do begin \
                 insert into t values (:i, rpad('x', 200, 'x')); i = i + 1; \
               end \
             end",
            (),
        )?;
        fill.commit()?;
    }

    println!("-- per-level summary (note used > 0 with allocated = 0: parent redirection)");
    level_summary(&mut monitor)?;

    // The worker's own chain: database -> attachment -> transaction.
    let mut tra = SimpleTransaction::new(&mut worker, Default::default())?;
    let (att, tr_id): (i64, i64) = tra
        .query_first(
            "select current_connection, current_transaction from rdb$database",
            (),
        )?
        .unwrap();

    println!("\n-- worker's pool chain (before the update)");
    pool_row(&mut monitor, "database pool:", "join MON$DATABASE using (MON$STAT_ID)")?;
    pool_row(
        &mut monitor,
        "worker attachment pool:",
        &format!("join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = {att}"),
    )?;
    pool_row(
        &mut monitor,
        "worker transaction pool:",
        &format!("join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = {tr_id}"),
    )?;

    // Grow the transaction pool: an uncommitted UPDATE of 3000 rows must
    // keep every old version in this transaction's undo log, and the undo
    // log lives in the transaction's pool.
    tra.execute("update t set pad = rpad('y', 200, 'y')", ())?;

    println!("\n-- after an uncommitted 3000-row UPDATE in that transaction");
    pool_row(
        &mut monitor,
        "worker attachment pool:",
        &format!("join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = {att}"),
    )?;
    pool_row(
        &mut monitor,
        "worker transaction pool:",
        &format!("join MON$TRANSACTIONS using (MON$STAT_ID) where MON$TRANSACTION_ID = {tr_id}"),
    )?;

    tra.rollback()?; // bulk-free: the whole pool goes at once
    println!("\n-- after rollback (transaction pool destroyed with its undo log)");
    pool_row(
        &mut monitor,
        "worker attachment pool:",
        &format!("join MON$ATTACHMENTS using (MON$STAT_ID) where MON$ATTACHMENT_ID = {att}"),
    )?;

    println!("\ndone.");
    Ok(())
}
