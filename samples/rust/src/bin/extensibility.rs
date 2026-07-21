//! extensibility.rs — the extensibility scenario in Rust.
//!
//! The rsfbclient twin of ../cpp/extensibility.cpp: calling native code
//! through SQL, UDR end to end.  The shipped example UDR module
//! (plugins/udr/libudrcpp_example.so) is bound to SQL objects with
//! EXTERNAL NAME '<module>!<entry>' ENGINE udr, then called like any other
//! procedure/function — engine -> udr_engine plugin -> native module, all
//! of it reachable from any driver because the whole surface is SQL.
//! RDB$CONFIG names the plugin filling each role, and RDB$PROCEDURES /
//! RDB$FUNCTIONS record the module!entry binding as ordinary metadata.
//! See ../../extensibility.md.
//!
//! Run (see ../README.md):  cargo run --bin extensibility

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

fn main() -> Result<(), FbError> {
    let mut conn = connect("extensibility")?;
    let mut tr = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;

    // 1. Bind SQL names to entry points in the shipped native module.
    tr.execute(
        "recreate procedure gen_rows (start_n integer not null, \
                                      end_n integer not null) \
           returns (n integer not null) \
           external name 'udrcpp_example!gen_rows' engine udr",
        (),
    )?;
    tr.execute(
        "recreate function sum_args (n1 integer, n2 integer, n3 integer) \
           returns integer \
           external name 'udrcpp_example!sum_args' engine udr",
        (),
    )?;
    tr.commit_retaining()?;

    // 2. Call them: native C++ running inside the server, plain SQL here.
    println!("select n from gen_rows(1, 5):");
    let rows: Vec<(i64,)> = tr.query("select n from gen_rows(1, 5)", ())?;
    for (n,) in &rows {
        println!("  {}", n);
    }

    let (sum,): (i64,) = tr
        .query_first("select sum_args(19, 20, 3) from rdb$database", ())?
        .unwrap();
    println!("\nselect sum_args(19, 20, 3):  {}", sum);

    // 3. The binding is ordinary metadata...
    println!("\nexternal routines recorded in the system tables:");
    let routines: Vec<(String,)> = tr.query(
        "select trim(rdb$procedure_name) || '  ->  ' || \
                trim(rdb$entrypoint) || '  (engine ' || \
                trim(rdb$engine_name) || ')' \
         from rdb$procedures where rdb$engine_name = 'UDR' \
         union all \
         select trim(rdb$function_name) || '  ->  ' || \
                trim(rdb$entrypoint) || '  (engine ' || \
                trim(rdb$engine_name) || ')' \
         from rdb$functions where rdb$engine_name = 'UDR'",
        (),
    )?;
    for (line,) in &routines {
        println!("  {}", line);
    }

    // 4. ...and the plugin roster itself is SQL-visible via RDB$CONFIG.
    println!("\nplugins filling each role (rdb$config):");
    let plugins: Vec<(String, Option<String>)> = tr.query(
        "select trim(rdb$config_name), rdb$config_value \
         from rdb$config \
         where rdb$config_name in ('Providers', 'AuthServer', \
               'UserManager', 'WireCryptPlugin', 'TracePlugin', \
               'DefaultProfilerPlugin') \
         order by rdb$config_id",
        (),
    )?;
    for (role, plugin) in &plugins {
        println!("  {:<24} {}", role, plugin.as_deref().unwrap_or("<null>"));
    }

    tr.commit()?;
    println!("\ndone.");
    Ok(())
}
