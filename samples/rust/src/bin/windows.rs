//! windows.rs — aggregate and window functions in Rust.
//!
//! The rsfbclient twin of ../cpp/windows.cpp: the same six-row sales table
//! and the same flagship analytics.  Window functions are plain SQL, so a
//! driver needs nothing special to run them — the results come back as
//! ordinary INT64/DOUBLE/VARCHAR messages, printed here from untyped
//! `Row`s.  The one API delta: rsfbclient has no `getPlan`, so the SORT
//! the document attributes to SortedStream -> WindowedStream is shown via
//! Firebird 6's `RDB$SQL.EXPLAIN` table function instead — a *more*
//! detailed plan, fetched with plain SQL.
//! See ../../aggregate-and-window-functions.md.
//!
//! Run (see ../README.md):  cargo run --bin windows

use fb_handson_rust::{connect, print_table};
use rsfbclient::{prelude::*, FbError, Row, SimpleTransaction};

fn main() -> Result<(), FbError> {
    let mut conn = connect("windows")?;

    // Scratch table (idempotent).
    {
        let mut ddl = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let _ = ddl.execute("DROP TABLE sales", ());
        ddl.execute(
            "CREATE TABLE sales (id INT PRIMARY KEY, region VARCHAR(10), amount NUMERIC(10,2))",
            (),
        )?;
        ddl.commit()?;
    }

    let mut tra = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    for row in [
        "(1,'East',100)",
        "(2,'East',200)",
        "(3,'East',150)",
        "(4,'West',300)",
        "(5,'West',250)",
        "(6,'West',400)",
    ] {
        tra.execute(&format!("INSERT INTO sales VALUES {}", row), ())?;
    }

    // -- 1. The flagship window query: partitioned ranking, a framed
    //       running total, and LAG navigation — every row kept.
    //       One driver delta hides here: SUM over NUMERIC(10,2) widens to
    //       NUMERIC(20,2), which travels as INT128 — a wire type rsfbclient
    //       cannot fetch ("Unsupported column type (32752 1)").  The CAST
    //       back to NUMERIC(10,2) keeps the running total fetchable.
    let win_sql = "SELECT region, amount, \
         ROW_NUMBER() OVER (PARTITION BY region ORDER BY amount) AS rn, \
         RANK() OVER (ORDER BY amount DESC) AS overall_rank, \
         CAST(SUM(amount) OVER (PARTITION BY region ORDER BY id \
           ROWS BETWEEN UNBOUNDED PRECEDING AND CURRENT ROW) \
           AS NUMERIC(10,2)) AS running_total, \
         LAG(amount) OVER (PARTITION BY region ORDER BY id) AS prev_amount \
         FROM sales";
    println!("== window functions ==");
    let rows: Vec<Row> = tra.query(win_sql, ())?;
    print_table(&rows);

    // rsfbclient has no plan-retrieval API; RDB$SQL.EXPLAIN (Firebird 6)
    // returns the detailed access path as rows instead — same engine
    // information, reached with plain SQL.
    println!("\nplan (RDB$SQL.EXPLAIN — rsfbclient has no getPlan API):");
    let plan: Vec<(i64, String)> = tra.query(
        "SELECT \"LEVEL\", ACCESS_PATH FROM RDB$SQL.EXPLAIN(?) ORDER BY PLAN_LINE",
        (win_sql,),
    )?;
    for (level, line) in &plan {
        println!("  {}{}", "  ".repeat(*level as usize), line);
    }

    // -- 2. Aggregates: FILTER (FB5), ordered LISTAGG, statistical.
    println!("\n== aggregates: FILTER / LISTAGG / STDDEV_POP ==");
    let rows: Vec<Row> = tra.query(
        "SELECT region, COUNT(*) AS n, \
         COUNT(*) FILTER (WHERE amount > 150) AS big_sales, \
         CAST(LISTAGG(amount, ',') WITHIN GROUP (ORDER BY amount) \
           AS VARCHAR(60)) AS amounts, \
         CAST(STDDEV_POP(amount) AS NUMERIC(10,2)) AS stddev \
         FROM sales GROUP BY region",
        (),
    )?;
    print_table(&rows);

    // -- 3. Ordered-set and hypothetical-set aggregates: the median,
    //       and "what rank would a 175 sale have in each region?"
    println!("\n== PERCENTILE_CONT median / hypothetical RANK(175) ==");
    let rows: Vec<Row> = tra.query(
        "SELECT region, \
         PERCENTILE_CONT(0.5) WITHIN GROUP (ORDER BY amount) AS median, \
         RANK(175) WITHIN GROUP (ORDER BY amount) AS rank_of_175 \
         FROM sales GROUP BY region",
        (),
    )?;
    print_table(&rows);

    // -- 4. FB6 frame exclusion: each row's neighbours' average,
    //       the row itself EXCLUDEd from its own frame.
    println!("\n== FB6 frame EXCLUDE CURRENT ROW (neighbours' average) ==");
    let rows: Vec<Row> = tra.query(
        "SELECT id, amount, \
         CAST(AVG(amount) OVER (ORDER BY id \
           ROWS BETWEEN 1 PRECEDING AND 1 FOLLOWING \
           EXCLUDE CURRENT ROW) AS NUMERIC(10,2)) AS neighbour_avg \
         FROM sales",
        (),
    )?;
    print_table(&rows);

    tra.commit()?;
    println!("\ndone.");
    Ok(())
}
