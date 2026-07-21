//
// common.js — shared boilerplate for the per-document hands-on samples.
//
// Every sample demonstrates one companion document of the paper; they all
// need the same skeleton — connection options, promisified attach/query/
// transaction — so that skeleton lives here.  node-firebird is a pure
// JavaScript wire-protocol implementation (no fbclient involved); see
// ../../firebird-wire-protocol.md for what happens on the socket.
//
'use strict';

const Firebird = require('node-firebird');

// The live demo server used throughout the companion documents.
const DEFAULTS = {
    host: process.env.FB_HOST || 'localhost',
    port: Number(process.env.FB_PORT) || 3050,
    database: process.env.FB_DATABASE || 'employee',
    user: process.env.ISC_USER || 'SYSDBA',
    password: process.env.ISC_PASSWORD || 'masterkey',
    // The stock employee.fdb has database charset NONE; with the driver
    // default (UTF8) node-firebird 2.11 mis-sizes NONE-charset VARCHARs.
    // See https://github.com/hgourvest/node-firebird/issues/422
    encoding: 'NONE',
};

// Raw TPB isolation arrays (see ../../transactions-and-concurrency.md):
// SNAPSHOT (concurrency) / READ COMMITTED rec_version / consistency.
const ISOLATION = {
    SNAPSHOT: Firebird.ISOLATION_REPEATABLE_READ,        // [2]
    READ_COMMITTED: Firebird.ISOLATION_READ_COMMITTED,   // [15, 17|18]
    TABLE_STABILITY: Firebird.ISOLATION_SERIALIZABLE,    // [1]
};

// A thin promise wrapper over one attachment.
class Conn {
    constructor(db) { this.db = db; }

    query(sql, params = []) {
        return new Promise((resolve, reject) =>
            this.db.query(sql, params, (err, rows) => err ? reject(err) : resolve(rows)));
    }

    // Start an explicit transaction; returns a Tx with query/commit/rollback.
    transaction(isolation = ISOLATION.SNAPSHOT) {
        return new Promise((resolve, reject) =>
            this.db.transaction(isolation, (err, tx) =>
                err ? reject(err) : resolve(new Tx(tx))));
    }

    detach() {
        return new Promise((resolve, reject) =>
            this.db.detach(err => err ? reject(err) : resolve()));
    }
}

class Tx {
    constructor(tx) { this.tx = tx; }

    query(sql, params = []) {
        return new Promise((resolve, reject) =>
            this.tx.query(sql, params, (err, rows) => err ? reject(err) : resolve(rows)));
    }

    commit() {
        return new Promise((resolve, reject) =>
            this.tx.commit(err => err ? reject(err) : resolve()));
    }

    rollback() {
        return new Promise((resolve, reject) =>
            this.tx.rollback(err => err ? reject(err) : resolve()));
    }
}

// attach() → Promise<Conn>.  Overrides are merged over DEFAULTS, so
// attach({ database: '/tmp/fbhandson/x.fdb' }) does what it says.
function attach(overrides = {}) {
    const options = { ...DEFAULTS, ...overrides };
    return new Promise((resolve, reject) =>
        Firebird.attach(options, (err, db) => err ? reject(err) : resolve(new Conn(db))));
}

// attachOrCreate() → Promise<Conn> for scratch databases under /tmp.
function attachOrCreate(overrides = {}) {
    const options = { ...DEFAULTS, ...overrides };
    return new Promise((resolve, reject) =>
        Firebird.attachOrCreate(options, (err, db) => err ? reject(err) : resolve(new Conn(db))));
}

// Buffers come back for NONE-charset text columns; render anything printable.
function s(v) {
    if (v === null || v === undefined) return '<null>';
    if (Buffer.isBuffer(v)) return v.toString('utf8').trim();
    if (typeof v === 'string') return v.trim();
    return String(v);
}

module.exports = { Firebird, DEFAULTS, ISOLATION, attach, attachOrCreate, s };
