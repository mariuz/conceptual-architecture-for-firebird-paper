//
// blr.js — companion to ../../blr-intermediate-language.md
//
// The JavaScript twin of ../cpp/blr.cpp: reads the stored BLR of the
// computed column EMPLOYEE.FULL_NAME raw from RDB$FIELDS.RDB$COMPUTED_BLR.
// node-firebird returns non-text blobs as a function that streams Buffer
// chunks, so the exact catalog bytes arrive over the wire protocol; the
// same mini-disassembler as the C++ sample then walks the prefix-encoded
// tree.  Opcode values are transcribed from firebird/impl/blr.h.
//
'use strict';

const { attach } = require('./common');

// The handful of opcodes a computed-column expression needs (impl/blr.h).
const OP = { version5: 5, concatenate: 39, field: 23, literal: 21,
             text2: 15, eoc: 76 };

// row.COLUMN is a function(cb) for blob columns; collect the chunks.
function readBlob(fieldFn) {
    return new Promise((resolve, reject) =>
        fieldFn((err, name, emitter) => {
            if (err) return reject(err);
            const chunks = [];
            emitter.on('data', c => chunks.push(c));
            emitter.on('error', reject);
            emitter.on('end', () => resolve(Buffer.concat(chunks)));
        }));
}

function hexDump(buf, limit = 64) {
    for (let i = 0; i < Math.min(buf.length, limit); i += 16)
        console.log(buf.subarray(i, Math.min(i + 16, buf.length, limit))
            .toString('hex').match(/../g).join(' '));
    console.log(`(${buf.length} bytes total)`);
}

// Decode one expression; returns the next offset.  Prefix encoding: each
// operator is followed immediately by its operands.
function expr(b, at, depth) {
    const pad = ' '.repeat(depth * 3);
    const op = b[at++];
    switch (op) {
        case OP.concatenate:
            console.log(pad + 'blr_concatenate');
            at = expr(b, at, depth + 1);
            return expr(b, at, depth + 1);
        case OP.field: {
            const ctx = b[at], len = b[at + 1];
            const name = b.subarray(at + 2, at + 2 + len).toString('latin1');
            console.log(`${pad}blr_field context ${ctx}, '${name}'`);
            return at + 2 + len;
        }
        case OP.literal: {
            if (b[at] !== OP.text2) throw new Error('literal dtype ' + b[at]);
            const cs = b.readUInt16LE(at + 1), len = b.readUInt16LE(at + 3);
            const text = b.subarray(at + 5, at + 5 + len).toString('latin1');
            console.log(`${pad}blr_literal blr_text2 charset ${cs}, len ${len}, "${text}"`);
            return at + 5 + len;
        }
        default:
            throw new Error('decoder stops at opcode ' + op);
    }
}

(async () => {
    const conn = await attach();   // employee, read-only
    try {
        console.log('== computed column EMPLOYEE.FULL_NAME — RDB$FIELDS.RDB$COMPUTED_BLR');
        const rows = await conn.query(
            'SELECT f.RDB$COMPUTED_BLR AS BLR FROM RDB$FIELDS f'
            + ' JOIN RDB$RELATION_FIELDS rf ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE'
            + " WHERE rf.RDB$RELATION_NAME = 'EMPLOYEE'"
            + " AND rf.RDB$FIELD_NAME = 'FULL_NAME'");
        const blr = await readBlob(rows[0].BLR);
        hexDump(blr);

        console.log(blr[0] === OP.version5 ? 'blr_version5' : 'unexpected version!');
        const at = expr(blr, 1, 1);
        console.log(blr[at] === OP.eoc ? 'blr_eoc' : '(no blr_eoc?)');

        console.log('\n== procedure GET_EMP_PROJ — first bytes of RDB$PROCEDURE_BLR');
        const proc = await conn.query('SELECT RDB$PROCEDURE_BLR AS BLR'
            + " FROM RDB$PROCEDURES WHERE RDB$PROCEDURE_NAME = 'GET_EMP_PROJ'");
        hexDump(await readBlob(proc[0].BLR), 32);
        console.log('(isql SET BLOB ALL disassembles the whole stream server-side)');
    } finally {
        await conn.detach();
    }
})().catch(e => { console.error(e.message || e); process.exit(1); });
