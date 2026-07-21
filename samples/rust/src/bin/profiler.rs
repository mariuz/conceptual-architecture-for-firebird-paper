//! profiler.rs — the accumulation view, driven from Rust.
//!
//! The rsfbclient twin of ../cpp/profiler.cpp (see ../../profiler.md).
//! Nothing is lost from Rust: the profiler's control surface is a SQL
//! package (RDB$PROFILER) and its output is a SQL schema (PLG$PROFILER),
//! so any client that can run queries can profile.  One session brackets
//! two workloads, then the tables are queried like any other data:
//!
//!   - a join, read back from PLG$PROF_RECORD_SOURCE_STATS_VIEW as an
//!     indented plan tree with per-operator open/fetch counts and times;
//!   - a PSQL procedure with a hot loop, read back per line and column
//!     from PLG$PROF_PSQL_STATS_VIEW.
//!
//! Run (see ../README.md):  cargo run --bin profiler

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn main() -> Result<(), FbError> {
    let mut conn = connect("profiler")?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // --- workload fixtures: a table and a looping procedure --------------
    let _ = tr.execute("drop procedure hotspot", ());
    let _ = tr.execute("drop table nums", ());
    tr.execute("create table nums (id int primary key, val int)", ())?;
    tr.commit_retaining()?;
    tr.execute(
        "execute block as declare n int = 0; begin \
           while (n < 5000) do begin \
             insert into nums values (:n, mod(:n, 97)); n = n + 1; end end",
        (),
    )?;
    tr.execute(
        "create procedure hotspot returns (total bigint) as\n\
           declare i int = 0;\n\
           declare x int;\n\
         begin\n\
           total = 0;\n\
           while (i < 20000) do\n\
           begin\n\
             select val from nums where id = mod(:i, 5000) into :x;\n\
             total = total + coalesce(:x, 0);\n\
             i = i + 1;\n\
           end\n\
           suspend;\n\
         end",
        (),
    )?;
    tr.commit_retaining()?;

    // --- profile: START_SESSION ... workload ... FINISH_SESSION ----------
    let (profile_id,): (i64,) = tr
        .query_first(
            "select rdb$profiler.start_session('rust hands-on') from rdb$database",
            (),
        )?
        .unwrap();

    let _join_count: Option<(i64,)> = tr.query_first(
        "select count(*) from nums a join nums b on b.id = a.val",
        (),
    )?;
    let _total: Option<(i64,)> = tr.query_first("select total from hotspot", ())?;

    tr.execute("execute procedure rdb$profiler.finish_session(true)", ())?;
    // The plugin flushes through an autonomous transaction; a retained
    // snapshot would not see it, so start a genuinely new transaction.
    tr.commit()?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    println!("profile session {} finished and flushed\n", profile_id);

    // --- the plan tree, with per-operator counters and times -------------
    println!("record sources of the join (PLG$PROF_RECORD_SOURCE_STATS_VIEW):");
    let rows: Vec<(String, i64, i64, i64)> = tr.query(
        "select cast(lpad('', level * 2) || cast(access_path as varchar(120)) \
                    as varchar(140)) as access_path, \
                open_counter, fetch_counter, \
                open_fetch_total_elapsed_time \
         from plg$profiler.plg$prof_record_source_stats_view \
         where profile_id = ? and sql_text containing 'join nums' \
         order by cursor_id, record_source_id",
        (profile_id,),
    )?;
    println!("{:<60} {:>5} {:>7} {:>12}", "access_path", "opens", "fetches", "total_ns");
    for (path, opens, fetches, total_ns) in rows {
        println!("{:<60} {:>5} {:>7} {:>12}", path, opens, fetches, total_ns);
    }

    // --- the PSQL hotspot, per line and column ----------------------------
    println!("\nhotspot procedure, per PSQL line (PLG$PROF_PSQL_STATS_VIEW):");
    let rows: Vec<(i64, i64, i64, i64, i64)> = tr.query(
        "select line_num, column_num, counter, \
                total_elapsed_time, avg_elapsed_time \
         from plg$profiler.plg$prof_psql_stats_view \
         where profile_id = ? and routine_name = 'HOTSPOT' \
         order by total_elapsed_time desc",
        (profile_id,),
    )?;
    println!("{:>4} {:>4} {:>8} {:>12} {:>8}", "line", "col", "counter", "total_ns", "avg_ns");
    for (line, col, counter, total_ns, avg_ns) in rows {
        println!("{:>4} {:>4} {:>8} {:>12} {:>8}", line, col, counter, total_ns, avg_ns);
    }

    tr.commit()?;
    println!("\ndone.");
    Ok(())
}
