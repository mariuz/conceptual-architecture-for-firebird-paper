//! deployment.rs — the deployment-and-operations scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/deployment.cpp: a deployment is usually
//! inspected from the server's shell, but the engine publishes its own view
//! of it to any SQL client, three layers deep — MON$DATABASE (the physical
//! facts), RDB$CONFIG (the EFFECTIVE configuration, firebird.conf merged
//! with databases.conf per database) and the SYSTEM context namespace.
//! Read-only apart from the scratch database the shared helper creates.
//! See ../../deployment-and-operations.md.
//!
//! Run (see ../README.md):  cargo run --bin deployment

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction, SystemInfos};

fn line(tr: &mut SimpleTransaction, label: &str, sql: &str) -> Result<(), FbError> {
    let (v,): (Option<String>,) = tr.query_first(sql, ())?.unwrap();
    println!("  {:<22} {}", label, v.unwrap_or_else(|| "<null>".into()));
    Ok(())
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("deployment")?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    println!("== MON$DATABASE: the database as deployed ==");
    line(&mut tr, "database file",
        "SELECT MON$DATABASE_NAME FROM MON$DATABASE")?;
    line(&mut tr, "ODS version",
        "SELECT MON$ODS_MAJOR || '.' || MON$ODS_MINOR FROM MON$DATABASE")?;
    line(&mut tr, "page size",
        "SELECT CAST(MON$PAGE_SIZE AS VARCHAR(20)) FROM MON$DATABASE")?;
    line(&mut tr, "page buffers",
        "SELECT CAST(MON$PAGE_BUFFERS AS VARCHAR(20)) FROM MON$DATABASE")?;
    line(&mut tr, "sweep interval",
        "SELECT CAST(MON$SWEEP_INTERVAL AS VARCHAR(20)) FROM MON$DATABASE")?;
    line(&mut tr, "forced writes",
        "SELECT CAST(MON$FORCED_WRITES AS VARCHAR(20)) FROM MON$DATABASE")?;
    line(&mut tr, "SQL dialect",
        "SELECT CAST(MON$SQL_DIALECT AS VARCHAR(20)) FROM MON$DATABASE")?;
    line(&mut tr, "crypt state",
        "SELECT CAST(MON$CRYPT_STATE AS VARCHAR(20)) FROM MON$DATABASE")?;

    let (total,): (i64,) = tr.query_first("SELECT COUNT(*) FROM RDB$CONFIG", ())?.unwrap();
    println!("\n== RDB$CONFIG: effective configuration (selected of {} settings) ==", total);
    let cfg: Vec<(String, Option<String>, bool)> = tr.query(
        "SELECT TRIM(RDB$CONFIG_NAME), RDB$CONFIG_VALUE, RDB$CONFIG_IS_SET \
         FROM RDB$CONFIG \
         WHERE RDB$CONFIG_NAME IN ('ServerMode', 'DefaultDbCachePages', \
           'DatabaseAccess', 'WireCrypt', 'MaxParallelWorkers', 'SecurityDatabase') \
         ORDER BY RDB$CONFIG_NAME",
        (),
    )?;
    for (name, value, is_set) in &cfg {
        println!("  {:<22} {:<38} set in config: {}",
            name, value.clone().unwrap_or_else(|| "<null>".into()), is_set);
    }

    println!("\n== settings explicitly set in config files ==");
    let set: Vec<(String, Option<String>, Option<String>)> = tr.query(
        "SELECT TRIM(RDB$CONFIG_NAME), RDB$CONFIG_VALUE, RDB$CONFIG_SOURCE \
         FROM RDB$CONFIG WHERE RDB$CONFIG_IS_SET ORDER BY RDB$CONFIG_ID",
        (),
    )?;
    for (name, value, source) in &set {
        println!("  {:<22} {:<38} ({})",
            name,
            value.clone().unwrap_or_else(|| "<null>".into()),
            source.as_deref().map(str::trim).unwrap_or("<null>"));
    }

    println!("\n== SYSTEM context: this engine, this session ==");
    for var in ["ENGINE_VERSION", "DB_NAME", "NETWORK_PROTOCOL",
                "WIRE_CRYPT_PLUGIN", "CLIENT_ADDRESS"] {
        let sql = format!(
            "SELECT RDB$GET_CONTEXT('SYSTEM','{}') FROM RDB$DATABASE", var);
        line(&mut tr, var, &sql)?;
    }
    tr.commit()?;

    // An honest driver-side delta: rsfbclient's SystemInfos::server_engine()
    // parses ENGINE_VERSION into an enum that stops at V5, so against this
    // Firebird 6 server the typed accessor refuses what the raw context
    // variable above showed happily — deployments outlive driver assumptions.
    match conn.server_engine() {
        Ok(v) => println!("\nrsfbclient server_engine(): {:?}", v),
        Err(e) => println!("\nrsfbclient server_engine() on this server: {}", e),
    }

    println!("done.");
    Ok(())
}
