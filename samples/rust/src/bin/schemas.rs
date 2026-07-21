//! schemas.rs — schema search paths and name resolution, from Rust.
//!
//! The rsfbclient twin of ../cpp/schemas.cpp:
//!
//!  1. RDB$SCHEMAS and the default search path ("PUBLIC", "SYSTEM").
//!  2. Two same-named tables (PUBLIC.CUSTOMERS / APP.CUSTOMERS); the same
//!     unqualified SELECT resolves differently as SET SEARCH_PATH changes —
//!     and the path is ATTACHMENT state, so it survives across transactions.
//!     A driver twist makes the timing visible: names bind at PREPARE time,
//!     and rsfbclient caches prepared statements per connection, so the
//!     identical SQL text re-executes its OLD resolution until the text (and
//!     therefore the cache key) changes.
//!  3. SYSTEM is auto-appended when omitted from SET SEARCH_PATH.
//!  4. A stored procedure created while APP leads the path binds
//!     APP.CUSTOMERS — and keeps meaning that after the session's path flips
//!     to PUBLIC (stored code resolves in its OWN schema, never the caller's
//!     path).  RDB$DEPENDENCIES records the resolution.
//!  5. The C++ sample reads the schema-qualified plan via getPlan();
//!     rsfbclient has no plan API, so Firebird 6's RDB$SQL.EXPLAIN table
//!     function shows the same access path — from plain SQL.
//!
//! See ../../schemas-and-name-resolution.md.
//!
//! Run (see ../README.md):  cargo run --bin schemas

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn ctx_path(tr: &mut SimpleTransaction) -> Result<String, FbError> {
    let (p,): (String,) = tr
        .query_first(
            "SELECT RDB$GET_CONTEXT('SYSTEM','SEARCH_PATH') FROM RDB$DATABASE",
            (),
        )?
        .unwrap();
    Ok(p)
}

fn one(tr: &mut SimpleTransaction, sql: &str) -> Result<String, FbError> {
    let row: Option<(String,)> = tr.query_first(sql, ())?;
    Ok(row.map(|(v,)| v).unwrap_or_else(|| "<none>".into()))
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("schemas")?;

    // -- Idempotent cleanup + setup.
    let mut ddl = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    for s in [
        "DROP PROCEDURE APP.WHICH_ONE",
        "DROP TABLE PUBLIC.CUSTOMERS",
        "DROP TABLE APP.CUSTOMERS",
        "DROP SCHEMA APP",
    ] {
        let _ = ddl.execute(s, ());
    }
    ddl.execute("CREATE SCHEMA APP", ())?;
    ddl.execute("CREATE TABLE PUBLIC.CUSTOMERS (ID INT, ORIGIN VARCHAR(20))", ())?;
    ddl.execute("CREATE TABLE APP.CUSTOMERS    (ID INT, ORIGIN VARCHAR(20))", ())?;
    ddl.commit()?;

    // -- 1. The catalog and the default path.
    let mut tra = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    tra.execute("INSERT INTO PUBLIC.CUSTOMERS VALUES (1, 'from PUBLIC')", ())?;
    tra.execute("INSERT INTO APP.CUSTOMERS    VALUES (2, 'from APP')", ())?;
    let schemas: Vec<(String,)> = tra.query(
        "SELECT TRIM(RDB$SCHEMA_NAME) FROM RDB$SCHEMAS ORDER BY 1",
        (),
    )?;
    print!("schemas in RDB$SCHEMAS      : ");
    for (s,) in &schemas {
        print!("{}  ", s);
    }
    println!("\ndefault search path         : {}", ctx_path(&mut tra)?);

    // -- 2. Same statement, two resolutions — with a driver twist: names
    //       bind at PREPARE time, and rsfbclient keeps a client-side cache
    //       of prepared statements, so re-running the identical SQL text
    //       re-executes the statement compiled under the OLD path.  Any
    //       textual difference forces a fresh prepare under the new path.
    println!("\nSELECT ORIGIN FROM CUSTOMERS, as the path changes:");
    println!(
        "  path PUBLIC,SYSTEM        -> {}",
        one(&mut tra, "SELECT ORIGIN FROM CUSTOMERS")?
    );
    tra.execute("SET SEARCH_PATH TO APP, PUBLIC", ())?;
    println!(
        "  path APP,PUBLIC, same text-> {}   <- rsfbclient's statement cache:",
        one(&mut tra, "SELECT ORIGIN FROM CUSTOMERS")?
    );
    println!("                                  the cached statement bound PUBLIC.CUSTOMERS at prepare time");
    println!(
        "  path APP,PUBLIC, new text -> {}   <- fresh prepare, fresh resolution",
        one(&mut tra, "SELECT ORIGIN FROM CUSTOMERS /* fresh */")?
    );

    // -- 3. SYSTEM can be moved but not removed.
    tra.execute("SET SEARCH_PATH TO APP", ())?;
    println!(
        "\nSET SEARCH_PATH TO APP      -> {}   (SYSTEM auto-appended)",
        ctx_path(&mut tra)?
    );

    // -- 4. Stored code binds its own schema, not the caller's path.
    tra.execute("SET SEARCH_PATH TO APP, PUBLIC", ())?;
    tra.execute(
        "CREATE PROCEDURE WHICH_ONE RETURNS (SRC VARCHAR(20)) AS \
         BEGIN SELECT ORIGIN FROM CUSTOMERS INTO :SRC; SUSPEND; END",
        (),
    )?;
    tra.commit()?;

    // The search path is attachment state: the new transaction below still
    // starts on APP,PUBLIC until we change it again.
    let mut tra = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    println!("\nprocedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)");
    tra.execute("SET SEARCH_PATH TO PUBLIC", ())?;
    println!("  after SET SEARCH_PATH TO PUBLIC:");
    println!(
        "    direct SELECT ... FROM CUSTOMERS -> {}",
        one(&mut tra, "SELECT ORIGIN FROM CUSTOMERS /* after flip */")?
    );
    println!(
        "    SELECT SRC FROM APP.WHICH_ONE    -> {}   <- unmoved",
        one(&mut tra, "SELECT SRC FROM APP.WHICH_ONE")?
    );
    println!(
        "    RDB$DEPENDENCIES records         -> {}",
        one(
            &mut tra,
            "SELECT TRIM(RDB$DEPENDED_ON_SCHEMA_NAME) || '.' || TRIM(RDB$DEPENDED_ON_NAME) \
             FROM RDB$DEPENDENCIES WHERE RDB$DEPENDENT_NAME = 'WHICH_ONE'"
        )?
    );

    // -- 5. Plans are schema-qualified (RDB$SQL.EXPLAIN instead of getPlan).
    println!("\nRDB$SQL.EXPLAIN('SELECT COUNT(*) FROM CUSTOMERS') with path PUBLIC:");
    let plan: Vec<(i64, Option<String>, Option<String>)> = tra.query(
        "SELECT PLAN_LINE, \
                TRIM(SCHEMA_NAME) || '.' || TRIM(OBJECT_NAME), \
                CAST(ACCESS_PATH AS VARCHAR(300)) \
         FROM RDB$SQL.EXPLAIN('SELECT COUNT(*) FROM CUSTOMERS') \
         ORDER BY PLAN_LINE",
        (),
    )?;
    for (line, object, path) in plan {
        println!(
            "  {:>2}  {:<40} {}",
            line,
            path.unwrap_or_default().trim(),
            object.map(|o| format!("<- {}", o)).unwrap_or_default()
        );
    }

    tra.commit()?;
    println!("\ndone.");
    Ok(())
}
