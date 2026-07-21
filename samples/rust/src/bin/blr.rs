//! blr.rs — companion to ../../blr-intermediate-language.md
//!
//! The rsfbclient twin of ../cpp/blr.cpp: read stored BLR *raw* from the
//! catalog of the stock employee database — the computed column
//! EMPLOYEE.FULL_NAME (RDB$FIELDS.RDB$COMPUTED_BLR) and the procedure
//! GET_EMP_PROJ (RDB$PROCEDURES.RDB$PROCEDURE_BLR) — hex-dump it, and
//! decode the first bytes with the opcode values from firebird/impl/blr.h.
//!
//! Two driver notes.  Where C++ walks the blob with openBlob/getSegment,
//! here each BLR program simply arrives as one Vec<u8>.  And rsfbclient
//! only understands blob subtypes 0 and 1 — subtype 2 (BLR) makes its row
//! reader bail with "Unsupported column type" — so the queries CAST the
//! column to BLOB SUB_TYPE BINARY on the server first.  The bytes are the
//! same either way; blr.h is the contract.
//!
//! Read-only against employee.
//!
//! Run (see ../README.md):  cargo run --bin blr

use fb_handson_rust::{password, user};
use rsfbclient::{prelude::*, FbError};

// The real opcode values from firebird/impl/blr.h — the header the engine
// itself compiles.
const BLR_VERSION5: u8 = 5;
const BLR_BEGIN: u8 = 2;
const BLR_MESSAGE: u8 = 4;
const BLR_SHORT: u8 = 7;
const BLR_TEXT2: u8 = 15;
const BLR_LITERAL: u8 = 21;
const BLR_FIELD: u8 = 23;
const BLR_CONCATENATE: u8 = 39;
const BLR_EOC: u8 = 76;

fn hex_dump(b: &[u8], limit: usize) {
    for (i, byte) in b.iter().take(limit).enumerate() {
        print!("{:02x}{}", byte, if i % 16 == 15 { "\n" } else { " " });
    }
    if b.len() > limit {
        println!("... ({} bytes total)", b.len());
    } else {
        println!("({} bytes total)", b.len());
    }
}

/// Decode one *expression* — enough opcodes for a computed column.
fn expr(b: &[u8], i: &mut usize, depth: usize) {
    print!("{:1$}", "", depth * 3);
    let op = b[*i];
    *i += 1;
    match op {
        BLR_CONCATENATE => {
            println!("blr_concatenate");
            expr(b, i, depth + 1);
            expr(b, i, depth + 1);
        }
        BLR_FIELD => {
            let ctx = b[*i];
            let len = b[*i + 1] as usize;
            *i += 2;
            println!(
                "blr_field context {}, '{}'",
                ctx,
                String::from_utf8_lossy(&b[*i..*i + len])
            );
            *i += len;
        }
        BLR_LITERAL => {
            if b[*i] == BLR_TEXT2 {
                *i += 1;
                let cs = b[*i] as u32 | (b[*i + 1] as u32) << 8;
                let len = b[*i + 2] as usize | (b[*i + 3] as usize) << 8;
                *i += 4;
                println!(
                    "blr_literal blr_text2 charset {}, len {}, \"{}\"",
                    cs,
                    len,
                    String::from_utf8_lossy(&b[*i..*i + len])
                );
                *i += len;
            } else {
                println!("blr_literal dtype {} ...", b[*i]);
                *i = b.len();
            }
        }
        other => {
            println!("opcode {} (decoder stops here)", other);
            *i = b.len();
        }
    }
}

fn main() -> Result<(), FbError> {
    let database = std::env::args().nth(1).unwrap_or_else(|| "employee".into());
    let mut conn = rsfbclient::builder_native()
        .with_dyn_link()
        .with_remote()
        .host("localhost")
        .db_name(&database)
        .user(user())
        .pass(password())
        .connect()?;

    println!("== computed column EMPLOYEE.FULL_NAME — RDB$FIELDS.RDB$COMPUTED_BLR");
    let (blr,): (Vec<u8>,) = conn
        .query_first(
            "SELECT CAST(f.RDB$COMPUTED_BLR AS BLOB SUB_TYPE BINARY) FROM RDB$FIELDS f \
             JOIN RDB$RELATION_FIELDS rf ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE \
             WHERE rf.RDB$RELATION_NAME = 'EMPLOYEE' \
             AND rf.RDB$FIELD_NAME = 'FULL_NAME'",
            (),
        )?
        .unwrap();
    hex_dump(&blr, 64);

    let mut i = 0usize;
    println!(
        "{}",
        if blr[i] == BLR_VERSION5 { "blr_version5" } else { "unexpected version!" }
    );
    i += 1;
    expr(&blr, &mut i, 1);
    println!(
        "{}",
        if i < blr.len() && blr[i] == BLR_EOC { "blr_eoc" } else { "(no blr_eoc?)" }
    );

    println!("\n== procedure GET_EMP_PROJ — RDB$PROCEDURES.RDB$PROCEDURE_BLR");
    let (blr,): (Vec<u8>,) = conn
        .query_first(
            "SELECT CAST(RDB$PROCEDURE_BLR AS BLOB SUB_TYPE BINARY) FROM RDB$PROCEDURES \
             WHERE RDB$PROCEDURE_NAME = 'GET_EMP_PROJ'",
            (),
        )?
        .unwrap();
    hex_dump(&blr, 32);

    // The opening bytes: version, begin, then the message declarations
    // (the wire-format row layouts this doc and the protocol doc share).
    let mut i = 0usize;
    print!("{}, ", if blr[i] == BLR_VERSION5 { "blr_version5" } else { "?" });
    i += 1;
    println!("{}", if blr[i] == BLR_BEGIN { "blr_begin" } else { "?" });
    i += 1;
    while blr[i] == BLR_MESSAGE {
        i += 1;
        let msg = blr[i];
        i += 1;
        let count = blr[i] as usize | (blr[i + 1] as usize) << 8;
        i += 2;
        print!("blr_message {}, {} fields:", msg, count);
        for _ in 0..count {
            if blr[i] == BLR_SHORT {
                print!(" blr_short(scale {})", blr[i + 1]);
                i += 2;
            } else if blr[i] == BLR_TEXT2 {
                print!(
                    " blr_text2(cs {}, len {})",
                    blr[i + 1] as u32 | (blr[i + 2] as u32) << 8,
                    blr[i + 3] as u32 | (blr[i + 4] as u32) << 8
                );
                i += 5;
            } else {
                print!(" dtype {}?", blr[i]);
                break;
            }
        }
        println!();
    }
    println!(
        "... {} more bytes — see isql SET BLOB ALL for the full dump",
        blr.len() - i
    );
    Ok(())
}
