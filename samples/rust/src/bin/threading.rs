//! threading.rs — SuperServer's thread-per-attachment topology, from Rust.
//!
//! The rsfbclient twin of ../cpp/threading.cpp, and the same measurement:
//! MON$SERVER_PID names the engine process, /proc/<pid>/task counts its
//! threads, and the sample takes the census before, during and after
//! opening twelve concurrent attachments.  Here the client side really is
//! twelve std::thread workers, each owning its own connection — the
//! documented rule that one attachment must not be shared between threads
//! without a lock is enforced by Rust itself: SimpleConnection is not
//! Sync, so the borrow checker makes each thread build its own.
//! See ../../threading-and-synchronization.md.
//!
//! Run (see ../README.md):  cargo run --bin threading

use fb_handson_rust::{connect, print_table};
use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction};
use std::time::Duration;

fn count_threads(pid: i64) -> usize {
    std::fs::read_dir(format!("/proc/{}/task", pid))
        .map(|d| d.count())
        .unwrap_or(0)
}

fn main() -> Result<(), FbError> {
    let mut db = connect("threading")?;
    let cfg = TransactionConfiguration::default();

    // Scratch table (idempotent).
    {
        let mut t = SimpleTransaction::new(&mut db, cfg)?;
        let _ = t.execute("recreate table t (id int primary key, v int)", ());
        t.commit()?;
        let mut t = SimpleTransaction::new(&mut db, cfg)?;
        t.execute("update or insert into t values (1, 0) matching (id)", ())?;
        t.commit()?;
    }

    let mut t = SimpleTransaction::new(&mut db, cfg)?;
    let (pid,): (i64,) = t
        .query_first(
            "select MON$SERVER_PID from MON$ATTACHMENTS \
             where MON$ATTACHMENT_ID = current_connection",
            (),
        )?
        .unwrap();
    t.commit()?;
    println!(
        "engine process: pid {}, {} threads (1 attachment open)",
        pid,
        count_threads(pid)
    );

    // Twelve attachments from twelve threads of THIS process; each holds
    // its attachment open for 2 s.  Server side: one thread each (drawn
    // from the pool when idle threads exist, created otherwise).
    let workers: Vec<_> = (0..12)
        .map(|_| {
            std::thread::spawn(|| -> Result<(), FbError> {
                let mut w = connect("threading")?;
                let mut wt = SimpleTransaction::new(&mut w, TransactionConfiguration::default())?;
                let _: Option<(i64,)> = wt.query_first("select count(*) from t", ())?;
                wt.commit()?;
                std::thread::sleep(Duration::from_secs(2));
                Ok(())
            })
        })
        .collect();

    std::thread::sleep(Duration::from_secs(1));
    let mut t = SimpleTransaction::new(&mut db, cfg)?;
    let (users,): (i64,) = t
        .query_first(
            "select count(*) from MON$ATTACHMENTS where MON$SYSTEM_FLAG = 0",
            (),
        )?
        .unwrap();
    let (pids,): (i64,) = t
        .query_first(
            "select count(distinct MON$SERVER_PID) from MON$ATTACHMENTS",
            (),
        )?
        .unwrap();
    t.commit()?;
    println!(
        "with 12 extra attachments: {} threads | {} user attachments, {} distinct server pid",
        count_threads(pid),
        users,
        pids
    );

    for w in workers {
        w.join().map_err(|_| FbError::from("worker thread panicked"))??;
    }
    std::thread::sleep(Duration::from_secs(1));
    println!(
        "after they detach:        {} threads (pooled, not destroyed)",
        count_threads(pid)
    );

    // The engine's own workers hold real attachments, visible from SQL.
    let mut t = SimpleTransaction::new(&mut db, cfg)?;
    let sys: Vec<Row> = t.query(
        "select MON$ATTACHMENT_ID, MON$SYSTEM_FLAG, trim(MON$USER) as MON$USER, \
         coalesce(MON$REMOTE_PROCESS, '<internal>') as MON$REMOTE_PROCESS \
         from MON$ATTACHMENTS order by MON$ATTACHMENT_ID",
        (),
    )?;
    print_table(&sys);
    t.commit()?;
    Ok(())
}
