//! sorting.rs — the TempCacheLimit threshold made visible, from Rust.
//!
//! The rsfbclient twin of ../cpp/sorting.cpp.  Fills a table with 200,000
//! rows (~82 MB of sort data with a 400-byte key), then runs two ORDER BY
//! queries that both get a Sort in their plan:
//!
//!   big sort   200,000 rows  ->  ~82 MB  >  TempCacheLimit (64 MB) -> spills
//!   small sort  20,000 rows  ->   ~8 MB  <  TempCacheLimit         -> in RAM
//!
//! While each query runs, a watcher thread (its own connection — Rust's
//! ownership rules keep it off the main attachment) samples two things:
//!   - the server's /proc/<pid>/fd table (via sudo) for fb_sort_* scratch
//!     files — they are unlinked on creation, so the fd table is the ONLY
//!     place they are visible;
//!   - MON$MEMORY_USAGE at database level (where TempSpace's cache is
//!     charged), each poll in a fresh transaction, because MON$ snapshots
//!     are per-transaction.
//!
//! rsfbclient has no getPlan API, so the Sort node (with its record and
//! key lengths — the very numbers that make up the 82 MB) is shown via
//! Firebird 6's RDB$SQL.EXPLAIN instead.  Needs to run on the server
//! machine with passwordless sudo (to read /proc/<serverpid>/fd of the
//! firebird-owned process).
//! See ../../sorting-and-temp-space.md.
//!
//! Run (see ../README.md):  cargo run --bin sorting

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};
use std::process::Command;
use std::sync::atomic::{AtomicBool, AtomicI64, Ordering};
use std::sync::Arc;

const MEMSQL: &str = "select m.mon$memory_allocated from mon$database d \
     join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id";

/// Sum the sizes of the server's open (already-unlinked) fb_sort_* files.
fn sample_scratch(pid: i64, peak_bytes: &AtomicI64, peak_files: &AtomicI64) {
    let cmd = format!(
        "sudo -n find /proc/{}/fd -lname '*fb_sort*' -print0 2>/dev/null \
         | xargs -0 -r sudo -n stat -L -c %s 2>/dev/null",
        pid
    );
    let out = match Command::new("sh").arg("-c").arg(&cmd).output() {
        Ok(o) => o,
        Err(_) => return,
    };
    let sizes: Vec<i64> = String::from_utf8_lossy(&out.stdout)
        .split_whitespace()
        .filter_map(|s| s.parse().ok())
        .collect();
    peak_bytes.fetch_max(sizes.iter().sum(), Ordering::Relaxed);
    peak_files.fetch_max(sizes.len() as i64, Ordering::Relaxed);
}

/// Watcher body: /proc fd sampling plus MON$ polling on its own attachment.
fn watch(
    pid: i64,
    running: &AtomicBool,
    peak_bytes: &AtomicI64,
    peak_files: &AtomicI64,
    peak_mem: &AtomicI64,
) -> Result<(), FbError> {
    let mut mon = connect("sorting")?; // second attachment: MON$ polling
    while running.load(Ordering::Relaxed) {
        sample_scratch(pid, peak_bytes, peak_files);
        let mut t = SimpleTransaction::new(&mut mon, TransactionConfiguration::default())?;
        let (mem,): (i64,) = t.query_first(MEMSQL, ())?.unwrap(); // new tx => new MON$ snapshot
        t.commit()?;
        peak_mem.fetch_max(mem, Ordering::Relaxed);
    }
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut db = connect("sorting")?;
    let cfg = TransactionConfiguration::default();

    // Build the bulk table once; reuse it on later runs.
    let have: Option<(i64,)> = {
        let mut t = SimpleTransaction::new(&mut db, cfg)?;
        let n = t.query_first("select count(*) from bulk", ()).ok().flatten();
        t.commit()?;
        n
    };
    if have != Some((200000,)) {
        println!("building bulk (200000 rows)...");
        let mut t = SimpleTransaction::new(&mut db, cfg)?;
        t.execute(
            "recreate table bulk (id integer, pad varchar(400) character set ascii)",
            (),
        )?;
        t.commit()?;
        let mut t = SimpleTransaction::new(&mut db, cfg)?;
        t.execute(
            "execute block as declare i integer = 0; begin \
               while (i < 200000) do begin \
                 insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x')); \
                 i = i + 1; \
               end \
             end",
            (),
        )?;
        t.commit()?;
    }
    println!("bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data");

    let mut t = SimpleTransaction::new(&mut db, cfg)?;
    let (pid,): (i64,) = t
        .query_first(
            "select mon$server_pid from mon$attachments \
             where mon$attachment_id = current_connection",
            (),
        )?
        .unwrap();
    let (mem_idle,): (i64,) = t.query_first(MEMSQL, ())?.unwrap();
    t.commit()?;
    println!(
        "server pid {}, database memory allocated while idle: {} bytes",
        pid, mem_idle
    );

    let cases = [
        (
            "big sort (200k rows, ~82 MB)",
            "select first 1 id from bulk order by pad desc",
        ),
        (
            "small sort (20k rows, ~8 MB)",
            "select first 1 id from bulk where mod(id, 10) = 0 order by pad desc",
        ),
    ];

    for (label, sql) in cases {
        let running = Arc::new(AtomicBool::new(true));
        let peak_bytes = Arc::new(AtomicI64::new(0));
        let peak_files = Arc::new(AtomicI64::new(0));
        let peak_mem = Arc::new(AtomicI64::new(0));

        let watcher = {
            let (running, peak_bytes, peak_files, peak_mem) = (
                Arc::clone(&running),
                Arc::clone(&peak_bytes),
                Arc::clone(&peak_files),
                Arc::clone(&peak_mem),
            );
            std::thread::spawn(move || watch(pid, &running, &peak_bytes, &peak_files, &peak_mem))
        };

        let mut t = SimpleTransaction::new(&mut db, cfg)?;
        let plan: Vec<(String,)> = t.query(
            "select ACCESS_PATH from RDB$SQL.EXPLAIN(?) order by PLAN_LINE",
            (sql,),
        )?;
        println!("\n{}", label);
        for (line,) in &plan {
            println!("  {}", line);
        }
        let (top,): (i64,) = t.query_first(sql, ())?.unwrap(); // the sort happens here
        t.commit()?;

        running.store(false, Ordering::Relaxed);
        watcher
            .join()
            .map_err(|_| FbError::from("watcher thread panicked"))??;

        println!("  top row id = {}", top);
        println!(
            "  peak fb_sort_* scratch: {} file(s), {} bytes",
            peak_files.load(Ordering::Relaxed),
            peak_bytes.load(Ordering::Relaxed)
        );
        let mem = peak_mem.load(Ordering::Relaxed);
        println!(
            "  peak database MON$MEMORY_ALLOCATED: {} bytes (+{} over idle)",
            mem,
            mem - mem_idle
        );
    }

    println!("\ndone.");
    Ok(())
}
