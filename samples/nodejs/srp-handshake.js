//
// srp-handshake.js — the Firebird wire-protocol handshake, from scratch.
//
// A dependency-free Node.js script (BigInt + node:crypto + net only) that
// performs a complete, real connection to a Firebird 3..6 server and prints
// every step:
//
//   op_connect     -> protocol negotiation + Srp256 client public key, one packet
//   op_cond_accept <- server picks the protocol, returns SRP salt + B  (cleartext)
//   op_cont_auth   -> client proof M, computed exactly like src/auth/
//                     SecureRemotePassword/srp.cpp in the Firebird tree
//   op_response    <- authentication accepted                          (cleartext)
//   op_crypt       -> start Arc4 wire encryption keyed by the SRP session key K
//   op_attach      -> attach to the database (now ENCRYPTED; no password in the DPB)
//   op_detach      -> goodbye
//
// The SRP exchange runs in cleartext (that is by design — the whole point of SRP
// is that eavesdropping it reveals nothing); encryption begins only afterwards,
// keyed by the shared secret the two sides just derived without ever sending it.
//
// Every SRP intermediate value is printed, and every place where Firebird's
// implementation deviates from RFC 2945 / RFC 5054 is marked "DEVIATION".
// See ../../firebird-wire-protocol.md for the full write-up.
//
// Run:  node srp-handshake.js [host] [port] [database] [user] [password]
//
'use strict';
const net = require('net');
const crypto = require('crypto');

const HOST = process.argv[2] || 'localhost';
const PORT = Number(process.argv[3]) || 3050;
const DB = process.argv[4] || 'employee';
const USER = (process.argv[5] || 'SYSDBA').toUpperCase(); // unquoted identifiers are uppercased
const PASS = process.argv[6] || 'masterkey';

//--------------------------------------------------------------- SRP math ---
// Group: fixed 1024-bit prime, g = 2 (src/auth/SecureRemotePassword/srp.cpp).
// This is Tom Wu's demo group, NOT one of the RFC 5054 appendix-A groups.
const N = BigInt('0x' +
    'E67D2E994B2F900C3F41F08F5BB2627ED0D49EE1FE767A52EFCD565CD6E76881' +
    '2C3E1E9CE8F0A8BEA6CB13CD29DDEBF7A96D4A93B55D488DF099A15C89DCB064' +
    '0738EB2CBDD9A8F7BAB561AB1B0DC1C6CDABF303264A08D1BCA932D1F1EE428B' +
    '619D970F342ABA9A65793B8B2F041AE5364350C16F735F56ECBCA87BD57B29E7');
const g = 2n;

const sha1 = (...b) => crypto.createHash('sha1').update(Buffer.concat(b.map(toBuf))).digest();
const sha256 = (...b) => crypto.createHash('sha256').update(Buffer.concat(b.map(toBuf))).digest();
const toBuf = (x) => Buffer.isBuffer(x) ? x : bigToBuf(x);
const bigToHex = (b) => { let h = b.toString(16); return h.length % 2 ? '0' + h : h; };
const bigToBuf = (b) => Buffer.from(bigToHex(b), 'hex');   // minimal big-endian, no leading zeros
const bufToBig = (buf) => buf.length ? BigInt('0x' + buf.toString('hex')) : 0n;
const mod = (a, m) => ((a % m) + m) % m;                   // C++ BigInteger % is non-negative

function modPow(base, exp, m) {
    let r = 1n; base = mod(base, m);
    while (exp > 0n) {
        if (exp & 1n) r = (r * base) % m;
        base = (base * base) % m;
        exp >>= 1n;
    }
    return r;
}

// k = SHA1(N | PAD(g))  — matches RFC 5054 (pad g with zeros to |N| = 128).
const k = bufToBig(sha1(bigToBuf(N), Buffer.alloc(128 - 1), bigToBuf(g)));

// x = SHA1(salt | SHA1(user ':' password))   [RFC 2945 x = SHA1(s | SHA1(I ':' p))]
const userHash = (user, salt, password) =>
    bufToBig(sha1(salt, sha1(Buffer.from(user + ':' + password))));

function clientProofSrp256(user, salt, A, B, a) {
    // u = SHA1(A | B) over MINIMAL bytes.
    // DEVIATION from RFC 5054, which specifies u = SHA1(PAD(A) | PAD(B)).
    const u = bufToBig(sha1(bigToBuf(A), bigToBuf(B)));

    // S = (B - k*g^x) ^ (a + u*x) mod N   — standard SRP-6 client side,
    // with Firebird reducing the exponent (a + u*x) mod N.
    const x = userHash(user, salt, PASS);
    const S = modPow(mod(B - k * modPow(g, x, N), N), mod(a + u * x, N), N);

    // K = SHA1(S): the session key is ALWAYS SHA-1 (20 bytes) — even in
    // Srp256, only the proof below uses SHA-256. This key becomes the wire
    // encryption key (Arc4 uses it directly; ChaCha stretches it via SHA-256).
    const K = sha1(bigToBuf(S));

    // M = H(n1, n2, salt, A, B, K) with H = SHA-256 for Srp256 (the whole
    // point of the plugin, per doc/README.SecureRemotePassword.html).
    // DEVIATION: RFC 2945 says n1 = H(N) XOR H(g); Firebird computes
    //            n1 = H(N) ^ H(g) mod N — modular EXPONENTIATION. The code
    //            comment in srp.cpp writes "H(H(prime) ^ H(g), ...)"; the '^'
    //            was implemented with BigInteger::modPow.
    // The inner hashes n1, n2 stay SHA-1 in every Srp* variant.
    const n1 = modPow(bufToBig(sha1(bigToBuf(N))), bufToBig(sha1(bigToBuf(g))), N);
    const n2 = bufToBig(sha1(Buffer.from(user)));
    const M = sha256(bigToBuf(n1), bigToBuf(n2), salt, bigToBuf(A), bigToBuf(B), K);

    return { u, x, S, K, M };
}

//----------------------------------------------------------------- Arc4 -----
// RC4, byte-for-byte identical to Firebird's Cypher (src/plugins/crypt/arc4/
// Arc4.cpp). Each direction is an independent keystream seeded with the same
// session key K, so our `enc` tracks the server's decryptor and vice versa.
class Arc4 {
    constructor(key) {
        this.s = Array.from({ length: 256 }, (_, i) => i);
        this.i = 0; this.j = 0;
        for (let k1 = 0, k2 = 0; k1 < 256; k1++) {
            k2 = (k2 + key[k1 % key.length] + this.s[k1]) & 0xff;
            [this.s[k1], this.s[k2]] = [this.s[k2], this.s[k1]];
        }
    }
    transform(buf) {
        const out = Buffer.allocUnsafe(buf.length);
        for (let n = 0; n < buf.length; n++) {
            this.i = (this.i + 1) & 0xff;
            this.j = (this.j + this.s[this.i]) & 0xff;
            [this.s[this.i], this.s[this.j]] = [this.s[this.j], this.s[this.i]];
            out[n] = buf[n] ^ this.s[(this.s[this.i] + this.s[this.j]) & 0xff];
        }
        return out;
    }
}

//------------------------------------------------------------ XDR helpers ---
class Writer {
    constructor() { this.parts = []; }
    int(v) { const b = Buffer.alloc(4); b.writeInt32BE(v); this.parts.push(b); return this; }
    bytes(buf) {                       // XDR opaque: length, data, pad to 4
        this.int(buf.length);
        this.parts.push(buf, Buffer.alloc((4 - buf.length % 4) % 4));
        return this;
    }
    str(s) { return this.bytes(Buffer.from(s)); }
    buf() { return Buffer.concat(this.parts); }
}

class Reader {                          // async cursor over the (optionally decrypted) TCP stream
    constructor(socket) {
        this.chunks = []; this.len = 0; this.waiter = null; this.dec = null;
        socket.on('data', (d) => {
            this.chunks.push(this.dec ? this.dec.transform(d) : d);
            this.len += d.length; this.waiter?.();
        });
        socket.on('error', (e) => { throw e; });
    }
    enableDecryption(cipher) { this.dec = cipher; }   // must be called with the inbound buffer empty
    async take(n) {
        while (this.len < n) await new Promise((r) => (this.waiter = r));
        const all = Buffer.concat(this.chunks);
        this.chunks = [all.subarray(n)]; this.len = all.length - n;
        return all.subarray(0, n);
    }
    async int() { return (await this.take(4)).readInt32BE(); }
    async bytes() {
        const n = await this.int();
        const data = await this.take(n);
        await this.take((4 - n % 4) % 4);
        return data;
    }
}

// Wire opcodes and tags used below (src/remote/protocol.h)
const op = { connect: 1, accept: 3, reject: 4, response: 9, attach: 19, detach: 21,
             cont_auth: 92, crypt: 96, accept_data: 94, cond_accept: 98 };
const CNCT = { user: 1, host: 4, user_verification: 6, specific_data: 7,
               plugin_name: 8, login: 9, plugin_list: 10, client_crypt: 11 };
const WIRE_CRYPT_ENABLED = 1;

function userIdBlock(login, pluginName, pluginList, specificData) {
    const p = [];
    const tlv = (tag, data) => { data = Buffer.from(data); p.push(Buffer.from([tag, data.length]), data); };
    tlv(CNCT.login, login);
    tlv(CNCT.plugin_name, pluginName);
    tlv(CNCT.plugin_list, pluginList);
    // specific data (SRP client key A as hex text) in <=254-byte chunks,
    // each prefixed with a sequence byte (serialize.js:addMultiblockPart)
    for (let i = 0; i * 254 < specificData.length; i++) {
        const c = Buffer.from(specificData.slice(i * 254, i * 254 + 254));
        p.push(Buffer.from([CNCT.specific_data, c.length + 1, i]), c);
    }
    // client's wire-crypt stance (4-byte LE): ENABLED — the default server
    // config (WireCrypt = Enabled) refuses a DISABLED client with
    // isc_wirecrypt_incompatible.
    p.push(Buffer.from([CNCT.client_crypt, 4, WIRE_CRYPT_ENABLED, 0, 0, 0]));
    tlv(CNCT.user, 'demo');
    tlv(CNCT.host, 'localhost');
    p.push(Buffer.from([CNCT.user_verification, 0]));
    return Buffer.concat(p);
}

async function readResponse(r) {        // op_response: handle, blob id, data, status vector
    const handle = await r.int();
    await r.take(8);                    // blob id
    await r.bytes();                    // response data
    const text = [];                    // status vector: (tag, value)* isc_arg_end
    for (;;) {
        const t = await r.int();
        if (t === 0) break;                                       // isc_arg_end
        else if (t === 1 || t === 4 || t === 19) {                // gds code / number / warning
            const c = await r.int();
            if (t === 1 && c !== 0) text.push('gds ' + c);
        } else text.push((await r.bytes()).toString());           // string / interpreted / sql_state
    }
    if (text.length) throw new Error('server error: ' + text.join(', '));
    return handle;
}

//---------------------------------------------------------------- the run ---
(async () => {
    const socket = net.connect(PORT, HOST);
    await new Promise((res, rej) => socket.on('connect', res).on('error', rej));
    const r = new Reader(socket);
    console.log(`connected to ${HOST}:${PORT}`);

    // -- 1. op_connect: offer protocols 13..20, present Srp256 client key A --
    const a = mod(bufToBig(crypto.randomBytes(128)), N);   // private key (SRP_KEY_SIZE = 128)
    const A = modPow(g, a, N);
    console.log('\n[SRP] client private key a  =', bigToHex(a).slice(0, 32) + '…');
    console.log('[SRP] client public key  A  =', bigToHex(A).slice(0, 32) + '…');

    const offered = [13, 16, 17, 18, 19, 20];              // FB3 .. FB6
    const w = new Writer()
        .int(op.connect).int(op.attach)
        .int(3)                                            // CONNECT_VERSION3
        .int(1)                                            // arch_generic
        .str(DB)
        .int(offered.length)
        .bytes(userIdBlock(USER, 'Srp256', 'Srp256,Srp', bigToHex(A)));
    offered.forEach((v, i) =>
        w.int(v | 0x8000).int(1).int(3).int(3).int(i + 2)); // version, arch, min/max ptype_batch_send, weight
    socket.write(w.buf());

    // -- 2. op_cond_accept: negotiated protocol + plugin data (salt, B) --
    const reply = await r.int();
    if (reply === op.reject) throw new Error('op_reject: no protocol/plugin in common');
    const version = (await r.int()) & 0x7fff, arch = await r.int(), ptype = await r.int();
    console.log(`\nserver replied op ${reply} ` +
        `(${reply === op.cond_accept ? 'op_cond_accept' : 'op_accept_data'}):`);
    console.log(`  protocol version ${version}, architecture ${arch}, packet type ${ptype}`);
    if (reply !== op.cond_accept && reply !== op.accept_data)
        throw new Error('unexpected opcode ' + reply);

    const data = await r.bytes();                          // 2-byte LE lengths: salt, then B as hex
    const plugin = (await r.bytes()).toString();
    const authenticated = await r.int();
    await r.bytes();                                       // p_acpt_keys (wire crypt negotiation)
    console.log(`  plugin ${plugin}, authenticated=${authenticated}`);

    const saltLen = data.readUInt16LE(0);
    const salt = data.subarray(2, 2 + saltLen);            // 32 random bytes made printable
    const keyLen = data.readUInt16LE(2 + saltLen);
    const B = BigInt('0x' + data.subarray(4 + saltLen, 4 + saltLen + keyLen).toString());
    console.log('[SRP] salt (server)         =', salt.toString().slice(0, 32) + '…');
    console.log('[SRP] server public key B   =', bigToHex(B).slice(0, 32) + '…');

    // -- 3. compute session key + proof, send op_cont_auth (still cleartext) --
    const { u, x, S, K, M } = clientProofSrp256(USER, salt, A, B, a);
    console.log('[SRP] scramble u = SHA1(A|B)=', bigToHex(u));
    console.log('[SRP] x = SHA1(s|SHA1(u:p)) =', bigToHex(x));
    console.log('[SRP] secret S              =', bigToHex(S).slice(0, 32) + '…');
    console.log('[SRP] session key K=SHA1(S) =', K.toString('hex'), '(SHA-1 even in Srp256!)');
    console.log('[SRP] proof M (SHA-256)     =', M.toString('hex'));

    socket.write(new Writer()
        .int(op.cont_auth)
        .str(M.toString('hex'))                            // p_data: proof as hex text
        .str(plugin).str('Srp256,Srp').str('')             // plugin name, list, keys
        .buf());

    if (await r.int() !== op.response) throw new Error('expected op_response');
    await readResponse(r);
    console.log('\nauthentication ACCEPTED — no password ever crossed the wire');

    // -- 4. op_crypt: turn on Arc4, keyed by the session key we just derived --
    // Sent in cleartext; the decryptor is armed immediately (the server's
    // response to op_crypt already arrives encrypted), the encryptor after.
    const enc = new Arc4(K), dec = new Arc4(K);
    r.enableDecryption(dec);                               // inbound buffer is empty here
    socket.write(new Writer().int(op.crypt).str('Arc4').str('Symmetric').buf());
    if (await r.int() !== op.response) throw new Error('expected op_response to op_crypt');
    await readResponse(r);
    // from now on everything we send must be encrypted
    const send = (buf) => socket.write(enc.transform(buf));
    console.log('wire encryption ON (Arc4 keyed by K) — the session key doubles as the cipher key');

    // -- 5. op_attach with a minimal DPB (no credentials needed any more) --
    const lc = Buffer.from('NONE'), un = Buffer.from(USER);
    const dpb = Buffer.concat([Buffer.from([1]),                       // isc_dpb_version1
        Buffer.from([48, lc.length]), lc,                             // isc_dpb_lc_ctype
        Buffer.from([28, un.length]), un]);                           // isc_dpb_user_name
    send(new Writer().int(op.attach).int(0).str(DB).bytes(dpb).buf());
    if (await r.int() !== op.response) throw new Error('expected op_response to op_attach');
    const handle = await readResponse(r);
    console.log(`attached to '${DB}' over the encrypted channel, attachment handle ${handle}`);

    // -- 6. detach politely --
    send(new Writer().int(op.detach).int(handle).buf());
    if (await r.int() === op.response) await readResponse(r);
    console.log('detached. bye');
    socket.end();
})().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
