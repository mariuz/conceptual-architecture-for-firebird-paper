#
# api_styles.py - one SELECT through three API levels of the same libfbclient
# (Python twin of ../cpp/api_styles.cpp; see ../../client-apis-and-drivers.md).
#
# The C++ twin runs the engine-version query through the legacy ISC API
# (ibase.h: isc_* functions, status vectors, XSQLDA) and the OO API
# (firebird/Interface.h).  firebird-driver is itself a Path A driver - a
# ctypes binding of libfbclient's OO API - so this file can show all three
# rungs of the ladder in one process, against one loaded library:
#
#   [ISC API] plain ctypes calls of isc_* on the driver's own library handle:
#             a byte-built DPB, a 20-slot status vector rendered with
#             fb_interpret(), a hand-declared XSQLDA whose one XSQLVAR the
#             caller points at VARCHAR storage;
#   [OO API ] firebird.driver.interfaces, the driver's thin layer over
#             IMaster/IProvider/IAttachment/IStatement/IResultSet: DPB from
#             IXpbBuilder, errors as exceptions, IMessageMetadata offsets;
#   [driver ] the DB-API surface everyone actually uses: connect, cursor.
#
# Run:  python3 api_styles.py [database]
#
import struct
import sys
from ctypes import (POINTER, Structure, addressof, byref, c_char, c_int,
                    c_short, c_ssize_t, c_uint, c_ushort, c_void_p, cast,
                    create_string_buffer)

from firebird.driver import connect
from firebird.driver.fbapi import get_api
from firebird.driver.types import DPBItem, StateResult, XpbKind

from fbsample import PASSWORD, USER, dsn, run

SQL = "select rdb$get_context('SYSTEM', 'ENGINE_VERSION') from rdb$database"

# ---------------------------------------------------------------- ISC API --
ISC_STATUS_ARRAY = c_ssize_t * 20          # ISC_STATUS is intptr_t
FB_API_HANDLE = c_uint
SQL_VARYING, SQLDA_VERSION1, SQL_DIALECT_V6, DSQL_DROP = 448, 1, 3, 2


class XSQLVAR(Structure):                  # ibase.h, field for field
    _fields_ = [('sqltype', c_short), ('sqlscale', c_short),
                ('sqlsubtype', c_short), ('sqllen', c_short),
                ('sqldata', c_void_p), ('sqlind', POINTER(c_short)),
                ('sqlname_length', c_short), ('sqlname', c_char * 32),
                ('relname_length', c_short), ('relname', c_char * 32),
                ('ownname_length', c_short), ('ownname', c_char * 32),
                ('aliasname_length', c_short), ('aliasname', c_char * 32)]


class XSQLDA(Structure):
    _fields_ = [('version', c_short), ('sqldaid', c_char * 8), ('sqldabc', c_int),
                ('sqln', c_short), ('sqld', c_short), ('sqlvar', XSQLVAR * 1)]


class Varying(Structure):                  # VARCHAR storage: 2-byte len + bytes
    _fields_ = [('len', c_short), ('data', c_char * 512)]


def isc_style(lib, database):
    st = ISC_STATUS_ARRAY()

    def check(where):
        """The classic idiom: inspect the status vector after every call."""
        if st[0] == 1 and st[1]:
            msg, walk = create_string_buffer(512), cast(st, POINTER(c_ssize_t))
            print(f'ISC error in {where}:', file=sys.stderr)
            while lib.fb_interpret(msg, 512, byref(walk)):
                print('   ', msg.value.decode(), file=sys.stderr)
            sys.exit(1)

    # 1. The DPB by hand: version byte, then tag / length byte / payload.
    dpb = bytes([1])                                     # isc_dpb_version1
    for tag, value in ((28, USER), (29, PASSWORD)):      # user_name, password
        dpb += bytes([tag, len(value)]) + value.encode()
    db, tr, stmt = FB_API_HANDLE(0), FB_API_HANDLE(0), FB_API_HANDLE(0)
    name = database.encode()
    lib.isc_attach_database(st, c_short(len(name)), name, byref(db),
                            c_short(len(dpb)), dpb)
    check('isc_attach_database')
    lib.isc_start_transaction(st, byref(tr), c_short(1), byref(db), c_short(0), None)
    check('isc_start_transaction')

    # 2. The DSQL lifecycle: allocate, prepare (describes into the XSQLDA),
    #    execute, fetch.
    lib.isc_dsql_allocate_statement(st, byref(db), byref(stmt))
    check('isc_dsql_allocate_statement')
    out = XSQLDA(version=SQLDA_VERSION1, sqln=1)
    lib.isc_dsql_prepare(st, byref(tr), byref(stmt), c_ushort(0), SQL.encode(),
                         c_ushort(SQL_DIALECT_V6), byref(out))
    check('isc_dsql_prepare')

    # 3. Bind the one output column to caller-owned storage that honours
    #    the declared type, plus a null indicator.
    var, null_ind = Varying(), c_short(0)
    v = out.sqlvar[0]
    v.sqltype = SQL_VARYING + 1                          # +1: sqlind is used
    v.sqldata = addressof(var)
    v.sqlind = cast(addressof(null_ind), POINTER(c_short))

    lib.isc_dsql_execute(st, byref(tr), byref(stmt), c_ushort(SQL_DIALECT_V6), None)
    check('isc_dsql_execute')
    while lib.isc_dsql_fetch(st, byref(stmt), c_ushort(SQL_DIALECT_V6), byref(out)) == 0:
        print(f'[ISC API] engine version = {var.data[:var.len].decode()}')
    check('isc_dsql_fetch')

    lib.isc_dsql_free_statement(st, byref(stmt), c_ushort(DSQL_DROP))
    lib.isc_commit_transaction(st, byref(tr))
    lib.isc_detach_database(st, byref(db))
    check('isc_detach_database')


# ----------------------------------------------------------------- OO API --
def oo_style(api, database):
    provider = api.master.get_dispatcher()               # the Y-valve
    with api.util.get_xpb_builder(XpbKind.DPB) as dpb:   # IXpbBuilder, not bytes
        dpb.insert_string(DPBItem.USER_NAME, USER)
        dpb.insert_string(DPBItem.PASSWORD, PASSWORD)
        att = provider.attach_database(database, dpb.get_buffer())
    tra = att.start_transaction(bytes([3]))              # isc_tpb_version3
    stmt = att.prepare(tra, SQL, SQL_DIALECT_V6)         # errors raise DatabaseError
    meta = stmt.get_output_metadata()                    # replaces the XSQLDA
    msg = create_string_buffer(meta.get_message_length())
    rs = stmt.open_cursor(tra, None, None, meta, 0)
    while rs.fetch_next(msg) == StateResult.OK:
        off = meta.get_offset(0)
        (length,) = struct.unpack_from('<h', msg.raw, off)
        print(f'[OO API ] engine version = {msg.raw[off + 2:off + 2 + length].decode()}')
    rs.close()
    stmt.free()
    meta.release()
    tra.commit()
    att.detach()
    provider.release()


# ----------------------------------------------------------------- driver --
def driver_style(database):
    with connect(database, user=USER, password=PASSWORD) as con:
        cur = con.cursor()
        print(f'[driver ] engine version = {cur.execute(SQL).fetchone()[0]}')
        cur.close()
        con.commit()


def main():
    database = dsn(sys.argv[1] if len(sys.argv) > 1 else 'employee')
    api = get_api()                                      # loads libfbclient once
    print(f'libfbclient: {api.client_library_name}\n')
    isc_style(api.client_library, database)
    oo_style(api, database)
    driver_style(database)
    print('same engine, same Y-valve, three API levels. done.')


if __name__ == '__main__':
    run(main)
