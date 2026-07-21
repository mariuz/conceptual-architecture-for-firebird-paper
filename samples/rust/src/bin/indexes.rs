//! indexes.rs — the indexing scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/indexes.cpp: one B-tree, many variants.
//! Builds a 3,000-row table, creates a descending, an expression
//! (COMPUTED BY), a partial (WHERE) and a plain index, then shows the
//! optimizer's access path for five queries.  rsfbclient exposes no
//! IStatement::getPlan, so instead of asking the prepared statement we ask
//! the engine itself: Firebird 6's RDB$SQL.EXPLAIN table function returns
//! the detailed access path as rows — reachable from any driver.
//! See ../../indexing-and-full-text-search.md.
//!
//! Run (see ../README.md):  cargo run --bin indexes

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

/// Print the detailed plan for `sql` via RDB$SQL.EXPLAIN.
fn plan(tr: &mut SimpleTransaction, sql: &str) -> Result<(), FbError> {
    println!("{}", sql);
    let rows: Vec<(i64, i64, Option<String>)> = tr.query(
        &format!(
            "SELECT PLAN_LINE, \"LEVEL\", CAST(ACCESS_PATH AS VARCHAR(512)) \
             FROM RDB$SQL.EXPLAIN('{}') ORDER BY PLAN_LINE",
            sql.replace('\'', "''")
        ),
        (),
    )?;
    for (_line, level, path) in &rows {
        println!("{}{}",
            "    ".repeat(*level as usize),
            path.as_deref().unwrap_or("(no access path)"));
    }
    println!();
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("indexes")?;

    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute(
            "recreate table doc ( \
               id integer, title varchar(60), status varchar(10), num integer)",
            (),
        )?;
        tr.commit()?;
    }
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute(
            "execute block as declare i integer = 0; begin \
               while (i < 3000) do begin \
                 insert into doc values (:i, 'Title ' || :i, \
                   iif(mod(:i, 3) = 0, 'active', 'done'), mod(:i, 100)); \
                 i = i + 1; \
               end \
             end",
            (),
        )?;
        tr.commit()?;
    }
    {
        let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        tr.execute("create descending index doc_id_desc on doc (id)", ())?;
        tr.execute("create index doc_upper_title on doc computed by (upper(title))", ())?;
        tr.execute("create index doc_active on doc (status) where status = 'active'", ())?;
        tr.execute("create index doc_num on doc (num)", ())?;
        tr.commit()?;
    }
    println!("3000 rows; indexes: descending, expression, partial, plain\n");

    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    plan(&mut tr, "select id from doc where upper(title) = 'TITLE 5'")?;
    plan(&mut tr, "select id from doc where status = 'active'")?;
    plan(&mut tr, "select first 1 id from doc order by id desc")?;
    plan(&mut tr, "select id from doc where num = 42 or id = 7")?;
    plan(&mut tr, "select id from doc where title containing 'itle 12'")?;

    let (matched,): (i64,) = tr
        .query_first("select count(*) from doc where title containing 'itle 12'", ())?
        .unwrap();
    println!("CONTAINING is correct but unindexed: matched {} rows by scanning all 3000", matched);
    tr.commit()?;

    println!("done.");
    Ok(())
}
