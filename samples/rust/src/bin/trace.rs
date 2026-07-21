//! trace.rs — the trace facility, seen from a driver that cannot reach it.
//!
//! The C++ twin (../cpp/trace.cpp) and the JavaScript twin
//! (../nodejs/trace.js) both run a complete user trace session through the
//! Services API: isc_action_svc_trace_start with an inline configuration,
//! the TraceLog streamed back line by line, a second service connection
//! stopping the session.  rsfbclient exposes NO Services API — no
//! attachServiceManager, no trace_start/stop/list — so a user trace
//! session simply cannot be driven from this driver; that is the honest
//! delta of this sample.
//!
//! What it does instead is play both remaining roles:
//!
//!   - the observed side: the same marker query the twins run, so a
//!     session started externally (fbtracemgr -start ..., or either twin)
//!     will show this program's ATTACH / statement / DETACH in its stream;
//!   - the poor man's print_perf: the per-statement counters a trace
//!     session would log are recovered as before/after deltas from
//!     MON$RECORD_STATS and MON$IO_STATS — monitoring answers "what has
//!     accumulated", trace answers "what just happened"; with a delta
//!     around a single statement the first approximates the second.
//! See ../../trace-and-audit.md.
//!
//! Run (see ../README.md):  cargo run --bin trace

use fb_handson_rust::connect;
use rsfbclient::{prelude::*, FbError, SimpleTransaction};

const STATS: &str = "select r.MON$RECORD_SEQ_READS, r.MON$RECORD_IDX_READS, \
     i.MON$PAGE_READS, i.MON$PAGE_FETCHES \
     from MON$ATTACHMENTS a \
     join MON$RECORD_STATS r on r.MON$STAT_ID = a.MON$STAT_ID \
     join MON$IO_STATS i on i.MON$STAT_ID = a.MON$STAT_ID \
     where a.MON$ATTACHMENT_ID = current_connection";

/// One fresh MON$ snapshot of this attachment's counters (its own
/// transaction: MON$ snapshots are per-transaction).
fn stats(conn: &mut rsfbclient::SimpleConnection) -> Result<(i64, i64, i64, i64), FbError> {
    let mut t = SimpleTransaction::new(conn, TransactionConfiguration::default())?;
    let row = t.query_first(STATS, ())?.unwrap();
    t.commit()?;
    Ok(row)
}

fn main() -> Result<(), FbError> {
    let mut conn = connect("trace")?;

    println!("rsfbclient has no Services API: no isc_action_svc_trace_start, no");
    println!("TraceLog stream — a user trace session cannot be started from this");
    println!("driver.  (The C++ and JavaScript twins run the full session; from");
    println!("here the same session would be:  fbtracemgr -se localhost:service_mgr");
    println!("-start -name hands-on-rust -config <file>.)");
    println!();

    // The identity a trace session's ATTACH_DATABASE line would print.
    {
        let mut t = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
        let (att, user, proto): (i64, String, String) = t
            .query_first(
                "select MON$ATTACHMENT_ID, trim(MON$USER), \
                 coalesce(trim(MON$REMOTE_PROTOCOL), '<internal>') \
                 from MON$ATTACHMENTS where MON$ATTACHMENT_ID = current_connection",
                (),
            )?
            .unwrap();
        t.commit()?;
        println!(
            "the observed side, as a trace ATTACH line would identify it:\n  attachment {} | {} | {}\n",
            att, user, proto
        );
    }

    // The counters a trace session's print_perf would log for one
    // statement, recovered as attachment-level MON$ deltas around it.
    let before = stats(&mut conn)?;

    let mut tra = SimpleTransaction::new(&mut conn, TransactionConfiguration::default())?;
    let (n,): (i64,) = tra
        .query_first(
            "SELECT COUNT(*) FROM RDB$RELATIONS /* traced from Rust! */",
            (),
        )?
        .unwrap();
    tra.commit()?;
    println!("[marker] SELECT COUNT(*) FROM RDB$RELATIONS says: {}", n);

    let after = stats(&mut conn)?;
    println!("\nper-statement counters, trace-style, as MON$ deltas around the marker:");
    println!(
        "  record seq reads: +{:<6} record idx reads: +{}",
        after.0 - before.0,
        after.1 - before.1
    );
    println!(
        "  page reads      : +{:<6} page fetches    : +{}",
        after.2 - before.2,
        after.3 - before.3
    );
    println!("  (attachment-level deltas, so the MON$ queries' own work is included —");
    println!("   trace would attribute counters to exactly one statement)");

    println!("\ndone.");
    Ok(())
}
