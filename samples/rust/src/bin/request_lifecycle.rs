//! request_lifecycle.rs — one CREATE TABLE round trip, phase by phase.
//!
//! The rsfbclient twin of ../cpp/request_lifecycle.cpp: the document's exact
//! scenario, instrumented from the client.  rsfbclient's typed Transaction
//! exposes a real prepare step (Transaction::prepare -> op_prepare_statement),
//! so prepare / execute / commit are timed separately, just like the OO-API
//! sample.  Around them the engine's own counters (MON$IO_STATS /
//! MON$RECORD_STATS for the worker attachment) are sampled — from a SECOND
//! attachment, because Rust's borrow checker allows only one open transaction
//! per connection, whereas the C++ sample opens a sampling transaction beside
//! the DDL one.  The second attachment also shows the isolation side:
//!
//!   prepare  -> DSQL: parser picks DsqlDdlStatement (type DDL)
//!   execute  -> EXE/MET: STORE into RDB$RELATIONS etc. — record inserts
//!               jump, and the new row is visible to *this* transaction
//!               while the monitor attachment still sees nothing
//!   commit   -> TRA_commit -> DFW_perform_work -> CCH_flush -> PIO_write —
//!               the page-write counter jumps
//!
//! See ../../request-lifecycle-code-trace.md.
//!
//! Run (see ../README.md):  cargo run --bin request_lifecycle

use fb_handson_rust::{connect, db_path, password, user};
use rsfbclient::{charset, prelude::*, FbError, SimpleConnection, SimpleTransaction, Transaction};
use std::time::Instant;

struct Stats {
    fetches: i64,
    marks: i64,
    writes: i64,
    rec_ins: i64,
}

/// Sample the worker attachment's cumulative counters in a fresh transaction
/// (MON$ snapshots are frozen per transaction, so a new one sees fresh data).
fn sample(mon: &mut SimpleConnection, att_id: i64) -> Result<Stats, FbError> {
    let mut tr = SimpleTransaction::new(mon, TransactionConfiguration::default())?;
    let (fetches, marks, writes, rec_ins): (i64, i64, i64, i64) = tr
        .query_first(
            "SELECT i.MON$PAGE_FETCHES, i.MON$PAGE_MARKS, i.MON$PAGE_WRITES, \
                    r.MON$RECORD_INSERTS \
             FROM MON$ATTACHMENTS a \
             JOIN MON$IO_STATS i ON a.MON$STAT_ID = i.MON$STAT_ID \
             JOIN MON$RECORD_STATS r ON a.MON$STAT_ID = r.MON$STAT_ID \
             WHERE a.MON$ATTACHMENT_ID = ?",
            (att_id,),
        )?
        .ok_or_else(|| FbError::from("worker attachment not found in MON$".to_string()))?;
    tr.commit()?;
    Ok(Stats { fetches, marks, writes, rec_ins })
}

/// How many RDB$RELATIONS rows for TRACE_DEMO the monitor attachment sees.
fn monitor_sees(mon: &mut SimpleConnection) -> Result<i64, FbError> {
    let mut tr = SimpleTransaction::new(mon, TransactionConfiguration::default())?;
    let (n,): (i64,) = tr
        .query_first(
            "SELECT COUNT(*) FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = 'TRACE_DEMO'",
            (),
        )?
        .unwrap();
    tr.commit()?;
    Ok(n)
}

fn ms(t0: Instant) -> f64 {
    t0.elapsed().as_secs_f64() * 1000.0
}

fn main() -> Result<(), FbError> {
    // The worker keeps its typed Connection (not SimpleConnection) so that
    // Transaction::prepare is reachable — that is the visible prepare phase.
    let b = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(db_path("request_lifecycle"))
        .user(user())
        .pass(password())
        .charset(charset::UTF_8)
        .clone();
    let mut worker = match b.connect() {
        Ok(c) => c,
        Err(_) => b.create_database()?,
    };
    let mut monitor = connect("request_lifecycle")?;

    // Idempotency: drop a leftover table from a previous run, if any.
    {
        let mut t = Transaction::new(&mut worker, TransactionConfiguration::default())?;
        let _ = t.execute("DROP TABLE trace_demo", ());
        t.commit()?;
    }

    // The worker's attachment id, so the monitor can watch its counters.
    let att_id: i64 = {
        let mut t = Transaction::new(&mut worker, TransactionConfiguration::default())?;
        let (id,): (i64,) = t
            .query_first("SELECT CURRENT_CONNECTION FROM RDB$DATABASE", ())?
            .unwrap();
        t.commit()?;
        id
    };

    let s0 = sample(&mut monitor, att_id)?;
    let mut tra = Transaction::new(&mut worker, TransactionConfiguration::default())?;

    // -- prepare: Y-valve -> remote -> DSQL (Stages 1-5) -------------------
    let t0 = Instant::now();
    let mut stmt = tra.prepare(
        "CREATE TABLE trace_demo (id INT NOT NULL PRIMARY KEY, name VARCHAR(30))",
        false,
    )?;
    let t_prepare = ms(t0);

    // -- execute: EXE -> DdlNode -> MET catalog writes (Stages 6-8) --------
    let t0 = Instant::now();
    stmt.execute(())?;
    let t_execute = ms(t0);
    drop(stmt); // release the borrow on tra; the statement handle is freed

    let s1 = sample(&mut monitor, att_id)?;
    println!(
        "prepare  {:6.2} ms   (op_prepare_statement: parser picks DsqlDdlStatement)",
        t_prepare
    );
    println!(
        "execute  {:6.2} ms   catalog record inserts: +{}, page marks: +{}",
        t_execute,
        s1.rec_ins - s0.rec_ins,
        s1.marks - s0.marks
    );

    // Uncommitted, but the STORE into RDB$RELATIONS is visible to the
    // transaction that did it — and to nobody else yet:
    let (mine,): (i64,) = tra
        .query_first(
            "SELECT COUNT(*) FROM RDB$RELATIONS WHERE RDB$RELATION_NAME = 'TRACE_DEMO'",
            (),
        )?
        .unwrap();
    println!("         in this tx:          RDB$RELATIONS has TRACE_DEMO = {}", mine);
    println!(
        "         monitor attachment:  sees {}  (TRA_commit has not happened)",
        monitor_sees(&mut monitor)?
    );

    // -- commit: TRA_commit -> DFW -> CCH_flush -> PIO_write (Stage 9) -----
    let t0 = Instant::now();
    tra.commit()?;
    let t_commit = ms(t0);
    let s2 = sample(&mut monitor, att_id)?;
    println!(
        "commit   {:6.2} ms   page writes: +{}  (fetches: +{} over the whole trip)",
        t_commit,
        s2.writes - s1.writes,
        s2.fetches - s0.fetches
    );
    println!(
        "         monitor attachment:  sees {}  (the DDL is now durable and public)",
        monitor_sees(&mut monitor)?
    );

    println!("done.");
    Ok(())
}
