#
# numerics.py - exact and inexact numbers end to end (Python twin of
# ../cpp/numerics.cpp; see ../../numeric-and-precision-arithmetic.md).
#
#   1. (0.1 + 0.2) - 0.3 in DOUBLE PRECISION vs DECFLOAT(34): binary floating
#      point leaves a residue, decimal floating point is 0.
#   2. NUMERIC(18,4) on the wire: SQL_INT64 with scale -4, and the message
#      buffer holds the scaled integer 123456789.
#   3. INT128 at 2^127-1, and the overflow error one step beyond it.
#   4. The DECFLOAT Division_by_zero trap (on by default), cleared with
#      SET DECFLOAT TRAPS TO - and again, without SQL, through the DPB.
#
# Python is the one twin language whose standard library has the engine's
# exact types: firebird-driver decodes NUMERIC to decimal.Decimal, INT128 to
# an arbitrary-precision int and DECFLOAT to Decimal (Infinity included), so
# nothing is squeezed through a binary double the way node-firebird and
# rsfbclient do.  The raw message bytes, though, are not public API: the
# sample reaches them through the Statement's private _out_desc/_out_buffer
# (the ItemMetadata the driver unpacks from), which is the honest gap.
#
# Run:  python3 numerics.py [database]
#
from firebird.driver import (DatabaseError, DecfloatTraps,
                             connect, driver_config)

from fbsample import (PASSWORD, USER, attach_or_create, db_path, dsn,
                      execute, run, scalar)


def first_line(exc):
    return str(exc).splitlines()[0].strip()


def main():
    path = db_path('numerics')
    with attach_or_create(path) as db:
        # -- 1. Exactness: the residue of (0.1 + 0.2) - 0.3.
        d = scalar(db, 'SELECT (CAST(0.1 AS DOUBLE PRECISION) + 0.2) - 0.3 FROM RDB$DATABASE')
        f = scalar(db, 'SELECT (CAST(0.1 AS DECFLOAT(34)) + 0.2) - 0.3 FROM RDB$DATABASE')
        print(f'(0.1+0.2)-0.3 in DOUBLE PRECISION : {d!r}  ({type(d).__name__})')
        print(f'(0.1+0.2)-0.3 in DECFLOAT(34)     : {f!r}  (== 0: {f == 0})\n')

        # -- 2. NUMERIC(18,4) on the wire: scaled integer + scale in metadata.
        cur = db.cursor()
        cur.execute('SELECT CAST(12345.6789 AS NUMERIC(18,4)) FROM RDB$DATABASE')
        value = cur.fetchone()[0]
        meta = cur._stmt._out_desc[0]                 # private: driver internals
        raw_bytes = bytes(cur._stmt._out_buffer[meta.offset:meta.offset + meta.length])
        raw = int.from_bytes(raw_bytes, 'little', signed=True)
        print(f'NUMERIC(18,4) wire format: type={meta.datatype.value} '
              f'(SQL_{meta.datatype.name}), length={meta.length}, scale={meta.scale}')
        print('message bytes (little-endian)  :', raw_bytes.hex(' '))
        print('raw integer                    :', raw)
        print(f'value = raw * 10^scale         : {raw} * 10^{meta.scale} = {value!r}')
        print('cursor.description             :', cur.description[0][1:])
        cur.close()
        print()

        # -- 3. INT128: the full range, and one step past it.
        m = scalar(db, 'SELECT CAST(170141183460469231731687303715884105727 AS INT128) '
                       'FROM RDB$DATABASE')
        print(f'INT128 max  : {m} (== 2**127-1: {m == 2**127 - 1})')
        try:
            scalar(db, 'SELECT CAST(170141183460469231731687303715884105727 AS INT128) + 1 '
                       'FROM RDB$DATABASE')
            print('BUG: overflow not detected')
        except DatabaseError as e:
            print(f'INT128 max+1: {first_line(e)} (sqlcode {e.sqlcode})\n')

        # -- 4. DECFLOAT division by zero: trapped by default, Infinity untrapped.
        one_by_zero = 'SELECT CAST(1 AS DECFLOAT(16)) / 0 FROM RDB$DATABASE'
        try:
            scalar(db, one_by_zero)
            print('BUG: default trap did not fire')
        except DatabaseError as e:
            print('1/0 with default traps     :', first_line(e))
        execute(db, 'SET DECFLOAT TRAPS TO', commit=False)   # session-level
        print('1/0 with traps cleared     :', repr(scalar(db, one_by_zero)))
        db.commit()

    # 4b. The same session setting, carried in the DPB instead of SQL: a
    # registered database config with decfloat_traps -> isc_dpb_decfloat_traps.
    cfg = driver_config.register_database('numerics-no-zero-trap')
    cfg.database.value = dsn(path)
    cfg.decfloat_traps.value = [DecfloatTraps.INEXACT]    # anything but Division_by_zero
    with connect('numerics-no-zero-trap', user=USER, password=PASSWORD) as db:
        traps = scalar(db, "SELECT RDB$GET_CONTEXT('SYSTEM', 'DECFLOAT_TRAPS') FROM RDB$DATABASE")
        print(f'1/0 with DPB traps={traps!r} :', repr(scalar(db, one_by_zero)))
    print('\ndone.')


if __name__ == '__main__':
    run(main)
