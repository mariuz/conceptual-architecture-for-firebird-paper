#
# migration.py - the type-mapping table made concrete (Python twin of
# ../cpp/migration.cpp; see ../../migration-and-interoperability.md).
#
# A probe table with the types migrations trip over - INT128, NUMERIC(38,8),
# DECFLOAT(34), TIMESTAMP WITH TIME ZONE, BOOLEAN, CHAR(16) OCTETS (UUID) -
# inspected the ways a migration tool sees it: the DESCRIBED metadata (the
# SQL_* wire codes, read from the OO API's IMessageMetadata that the driver
# wraps, next to the DB-API cursor.description the driver publishes), the
# NATIVE face (what Python type each column arrives as), and the TEXT face
# (server-side CAST to VARCHAR, the universal fallback).
#
# firebird-driver is the far end of the wrapper spectrum from node-firebird
# and rsfbclient: every one of these types arrives natively and exactly -
# INT128, NUMERIC(38,8) and DECFLOAT(34) as decimal.Decimal, TIMESTAMP WITH
# TIME ZONE as an aware datetime whose tzinfo is the named zone, OCTETS as
# bytes.  The one lag is in the DB-API description, whose type_code is None
# for INT128, float for DECFLOAT and str for OCTETS although the values come
# back as Decimal and bytes.
#
# Run:  python3 migration.py [database]
#
import uuid

from fbsample import attach_or_create, db_path, execute, query, run

COLUMNS = ('C_INT128', 'C_NUM', 'C_DEC', 'C_TSTZ', 'C_BOOL', 'C_UUID', 'C_VC')


def main():
    with attach_or_create(db_path('migration')) as con:
        execute(con, 'RECREATE TABLE TYPE_PROBE ('
                     '  C_INT128 INT128,'
                     '  C_NUM    NUMERIC(38,8),'
                     '  C_DEC    DECFLOAT(34),'
                     '  C_TSTZ   TIMESTAMP WITH TIME ZONE,'
                     '  C_BOOL   BOOLEAN,'
                     '  C_UUID   CHAR(16) CHARACTER SET OCTETS,'
                     '  C_VC     VARCHAR(20))')
        execute(con, 'INSERT INTO TYPE_PROBE VALUES ('
                     '  170141183460469231731687303715884105727,'
                     '  123456789012345678901234567890.12345678,'
                     '  1.234567890123456789012345678901234E+10,'
                     "  TIMESTAMP '2026-07-21 12:00:00 Europe/Bucharest',"
                     "  TRUE, GEN_UUID(), 'naïve ütf8 text')")

        # -- 1. what DESCRIBE tells a driver: the wire type codes -----------
        print('described output metadata of SELECT * FROM TYPE_PROBE:\n')
        print(f'{"column":<8} {"code":>6} {"wire type":<16} {"length":>6} {"scale":>5}'
              '   cursor.description type_code')
        cur = con.cursor()
        cur.execute('SELECT * FROM TYPE_PROBE')
        meta = cur._stmt._istmt.get_output_metadata()
        for i, d in enumerate(cur.description):
            t = meta.get_type(i)
            print(f'{meta.get_field(i):<8} {int(t):>6} {"SQL_" + t.name:<16} '
                  f'{meta.get_length(i):>6} {meta.get_scale(i):>5}   {getattr(d[1], "__name__", d[1])}')
        meta.release()
        print('(the DB-API type_code lags the fetch path: None for INT128, float for'
              '\n DECFLOAT, str for OCTETS - the values below arrive as Decimal and bytes)')

        # -- 2. the native face: Python values, no text in between ---------
        print('\nsame row fetched natively:\n')
        row = cur.fetchone()
        cur.close()
        for name, value in zip(COLUMNS, row):
            shown = (f'{value} [{value.tzinfo}]' if name == 'C_TSTZ' else
                     uuid.UUID(bytes=value) if name == 'C_UUID' else value)
            print(f'  {name:<8} -> {type(value).__name__:<8} {shown}')

        # -- 3. the text face: engine-rendered strings, the ETL fallback ----
        print('\nthe text face (CAST ... AS VARCHAR server-side):\n')
        casts = ', '.join(f'CAST({c} AS VARCHAR(64))' for c in COLUMNS[:5])
        for name, text in zip(COLUMNS, query(con, f'SELECT {casts}, '
                                                  'UUID_TO_CHAR(C_UUID), C_VC FROM TYPE_PROBE')[0]):
            print(f'  {name:<8} = {text}')
        con.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
