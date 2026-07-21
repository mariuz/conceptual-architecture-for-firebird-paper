//! monitoring.rs — the monitoring-and-tuning scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/monitoring.cpp: the MON$ hierarchy
//! (database -> attachment -> transaction -> statement, counters joined
//! via MON$STAT_ID) and the defining architectural property of the
//! monitoring tables — the first MON$ select in a transaction takes a
//! STABLE SNAPSHOT.  Explicit SimpleTransaction objects matter doubly
//! here: only inside one explicit transaction can the "stale counters"
//! effect reproduce, and rsfbclient's hidden default transaction would
//! otherwise decide the snapshot boundaries for us.
//! See ../../monitoring-and-tuning.md.
//!
//! Run (see ../README.md):  cargo run --bin monitoring

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

// This attachment's record/page counters, via the MON$STAT_ID join.
const COUNTERS: &str = "SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, R.MON$RECORD_INSERTS, \
            I.MON$PAGE_FETCHES, I.MON$PAGE_READS \
     FROM MON$ATTACHMENTS A \
     JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID \
     JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID \
     WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION";

fn show_counters(tr: &mut SimpleTransaction) -> Result<(), FbError> {
    let (seq, idx, ins, fetches, reads): (i64, i64, i64, i64, i64) =
        tr.query_first(COUNTERS, ())?.unwrap();
    println!(
        "seq_reads={} idx_reads={} inserts={} page_fetches={} page_reads={}",
        seq, idx, ins, fetches, reads
    );
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("monitoring")?;

    // Workload table: 10000 rows to scan.
    {
        let mut t = SimpleTransaction::new(&mut conn, Default::default())?;
        let _ = t.execute("DROP TABLE MON_WORK", ());
        t.execute("CREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)", ())?;
        t.commit_retaining()?;
        t.execute(
            "EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN \
               WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); I = I + 1; END \
             END",
            (),
        )?;
        t.commit()?;
    }

    // -- 1. the hierarchy, one level per query, one consistent snapshot --
    let mut tra = SimpleTransaction::new(&mut conn, Default::default())?;
    println!("== MON$DATABASE: transaction markers ==");
    let (oit, oat, next, buffers): (i64, i64, i64, i64) = tra
        .query_first(
            "SELECT MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, MON$NEXT_TRANSACTION, \
                    MON$PAGE_BUFFERS FROM MON$DATABASE",
            (),
        )?
        .unwrap();
    println!("OIT={} OAT={} NEXT={} page_buffers={}", oit, oat, next, buffers);

    println!("\n== MON$ATTACHMENTS -> MON$TRANSACTIONS -> MON$STATEMENTS (me) ==");
    let (att, user, tr_id, state, sql_head): (i64, String, i64, i64, String) = tra
        .query_first(
            "SELECT A.MON$ATTACHMENT_ID, TRIM(A.MON$USER), T.MON$TRANSACTION_ID, S.MON$STATE, \
                    CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 40) AS VARCHAR(40)) \
             FROM MON$ATTACHMENTS A \
             JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID \
             JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID \
             WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION",
            (),
        )?
        .unwrap();
    println!(
        "attachment {} ({}), tx {}, statement state {} running: {}...",
        att, user, tr_id, state, sql_head
    );

    // -- 2. the snapshot property, measured on our own counters ---------
    print!("\n== my counters (snapshot 1) ==\n");
    show_counters(&mut tra)?;

    println!("\n... running workload: SELECT COUNT(*) full scan + indexed lookup ...");
    let (count,): (i64,) = tra.query_first("SELECT COUNT(*) FROM MON_WORK", ())?.unwrap();
    let (point,): (i64,) = tra
        .query_first("SELECT VAL FROM MON_WORK WHERE ID = 4242", ())?
        .unwrap();
    println!("count = {}, point = {}", count, point);

    println!("\n== same transaction, re-queried: STILL snapshot 1 ==");
    show_counters(&mut tra)?;
    tra.commit()?;

    let mut tra = SimpleTransaction::new(&mut conn, Default::default())?;
    println!("\n== new transaction: fresh snapshot, workload now visible ==");
    show_counters(&mut tra)?;
    tra.commit()?;

    println!("\ndone.");
    Ok(())
}
