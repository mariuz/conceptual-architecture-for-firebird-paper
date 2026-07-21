//! plans.rs — watching the cost-based optimizer decide.
//!
//! The rsfbclient twin of ../cpp/plans.cpp (see
//! ../../query-optimizer-and-execution.md).  rsfbclient has no
//! IStatement::getPlan equivalent, so — like the node-firebird twin — this
//! sample takes the SQL-level route new in Firebird 6: the RDB$SQL.EXPLAIN
//! table function, which *prepares* the given statement server-side and
//! returns the detailed record-source tree as rows, one per operator,
//! never executing the query.  (The terse legacy PLAN string stays a C++
//! exclusive.)  Same experiments as the C++ twin: the plan flip from Full
//! Scan to Bitmap + Index Range Scan after CREATE INDEX, a unique-index PK
//! lookup, SORT over a nested loop, and the indexless equi-join that turns
//! into a Hash Join.
//!
//! Uses its own scratch database — safe to re-run.
//!
//! Run (see ../README.md):  cargo run --bin plans

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

// Ask RDB$SQL.EXPLAIN for the access path of one statement (prepared,
// never executed) and print it indented by operator depth.
fn explain(tr: &mut SimpleTransaction, sql: &str) -> Result<(), FbError> {
    println!("== {}", sql);
    let rows: Vec<(i64, i64, String)> = tr.query(
        "SELECT plan_line, \"LEVEL\", CAST(access_path AS VARCHAR(1024)) \
         FROM rdb$sql.explain(CAST(? AS VARCHAR(512))) ORDER BY plan_line",
        (sql,),
    )?;
    for (_line, level, path) in rows {
        // LEVEL gives the operator's depth; the text itself may span several
        // lines (an index retrieval prints its Bitmap/Index children inline).
        let pad = format!("   {}", "    ".repeat(level as usize));
        println!("{}{}", pad, path.trim_end().replace('\n', &format!("\n{}", pad)));
    }
    println!();
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("plans")?;

    // -- Build the schema: 20 departments, 2000 employees. ----------------
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    tr.execute(
        "RECREATE TABLE dept (id INT NOT NULL PRIMARY KEY, name VARCHAR(20))",
        (),
    )?;
    tr.execute(
        "RECREATE TABLE emp (id INT NOT NULL PRIMARY KEY,\
         dept_id INT, salary INT, name VARCHAR(20))",
        (),
    )?;
    tr.commit_retaining()?;
    tr.execute(
        "EXECUTE BLOCK AS DECLARE i INT = 1; BEGIN\n\
         WHILE (i <= 20) DO BEGIN\n\
           INSERT INTO dept VALUES (:i, 'dept ' || :i); i = i + 1;\n\
         END\n\
         i = 1;\n\
         WHILE (i <= 2000) DO BEGIN\n\
           INSERT INTO emp VALUES (:i, MOD(:i, 20) + 1,\n\
               1000 + MOD(:i * 37, 500), 'emp ' || :i); i = i + 1;\n\
         END\n\
         END",
        (),
    )?;
    tr.commit()?;

    // -- 1. No index on dept_id yet: the full scan is the only path. ------
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    explain(&mut tr, "SELECT name FROM emp WHERE dept_id = 5")?;

    // -- 2. Create the index; the same text now compiles differently. -----
    tr.execute("CREATE INDEX emp_dept ON emp (dept_id)", ())?;
    tr.commit()?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    println!("-- CREATE INDEX emp_dept ON emp (dept_id) --\n");
    explain(&mut tr, "SELECT name FROM emp WHERE dept_id = 5")?;

    // -- 3. PK equality: unique index, nothing cheaper than one row. ------
    explain(&mut tr, "SELECT name FROM emp WHERE id = 42")?;

    // -- 4. Join + ORDER BY: SORT over a nested loop with the index. ------
    explain(
        &mut tr,
        "SELECT e.name, d.name FROM emp e JOIN dept d ON e.dept_id = d.id ORDER BY e.salary",
    )?;

    // -- 5. Equi-join with no usable index on either side: hash join. -----
    explain(
        &mut tr,
        "SELECT COUNT(*) FROM emp a JOIN emp b ON a.salary = b.salary",
    )?;

    tr.commit()?;
    println!("done.");
    Ok(())
}
