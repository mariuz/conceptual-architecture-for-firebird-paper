//! replication.rs — the client-visible half of replication: the publication.
//!
//! The rsfbclient twin of ../cpp/replication.cpp.  Everything here is plain
//! DDL plus system tables — no replication.conf, no restart:
//!
//!   ALTER DATABASE ENABLE PUBLICATION              -> RDB$PUBLICATIONS
//!   ALTER DATABASE INCLUDE TABLE ... / INCLUDE ALL -> RDB$PUBLICATION_TABLES
//!
//! The journal/segment transport behind it (Publisher -> ChangeLog ->
//! Applier) needs server-side replication.conf and stays as text in the
//! document's validated walk-through.  See ../../replication-architecture.md.
//!
//! Run (see ../README.md):  cargo run --bin replication

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn pub_state(tr: &mut SimpleTransaction, when: &str) -> Result<(), FbError> {
    println!("-- {}", when);
    let pubs: Vec<(String, i64, i64)> = tr.query(
        "SELECT TRIM(RDB$PUBLICATION_NAME), RDB$ACTIVE_FLAG, RDB$AUTO_ENABLE \
         FROM RDB$PUBLICATIONS",
        (),
    )?;
    for (name, active, auto_enable) in pubs {
        println!(
            "   publication {}: active={} auto_enable={}",
            name, active, auto_enable
        );
    }
    let tabs: Vec<(String, String)> = tr.query(
        "SELECT TRIM(RDB$TABLE_SCHEMA_NAME), TRIM(RDB$TABLE_NAME) \
         FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME",
        (),
    )?;
    let list: Vec<String> = tabs.iter().map(|(s, t)| format!("{}.{}", s, t)).collect();
    println!(
        "   published tables: {}\n",
        if list.is_empty() { "(none)".into() } else { list.join(", ") }
    );
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("replication")?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // Idempotent reset: back to a clean, unpublished state.  (A failed
    // statement does not doom a Firebird transaction — only that statement.)
    for sql in [
        "ALTER DATABASE EXCLUDE ALL FROM PUBLICATION",
        "ALTER DATABASE DISABLE PUBLICATION",
        "DROP TABLE REPL_ORDERS",
        "DROP TABLE REPL_SCRATCH",
    ] {
        let _ = tr.execute(sql, ());
    }
    tr.execute(
        "CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))",
        (),
    )?;
    tr.execute("CREATE TABLE REPL_SCRATCH (N INT)", ())?; // note: no key
    tr.commit_retaining()?;

    pub_state(&mut tr, "initial state (publication exists but is inactive)")?;

    tr.execute("ALTER DATABASE ENABLE PUBLICATION", ())?;
    tr.commit_retaining()?;
    pub_state(&mut tr, "after ENABLE PUBLICATION")?;

    tr.execute("ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION", ())?;
    tr.commit_retaining()?;
    pub_state(&mut tr, "after INCLUDE TABLE REPL_ORDERS")?;

    tr.execute("ALTER DATABASE INCLUDE ALL TO PUBLICATION", ())?;
    tr.commit_retaining()?;
    pub_state(
        &mut tr,
        "after INCLUDE ALL (auto-enable: future tables join automatically)",
    )?;

    // The monitoring view of the same facts, on this attachment.
    let (mode,): (i64,) = tr
        .query_first("SELECT MON$REPLICA_MODE FROM MON$DATABASE", ())?
        .unwrap();
    println!(
        "MON$REPLICA_MODE = {}   (0 = not a replica: this side publishes)",
        mode
    );

    tr.commit()?;
    println!("\ndone.");
    Ok(())
}
