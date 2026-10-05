#
# parser_errors.py - Firebird's SQL parser seen from the client (Python twin
# of ../cpp/parser_errors.cpp; see ../../grammar-and-parser.md).
#
# Six strings go to the parser through a prepare-only step, cursor.prepare():
# a `?` placeholder that comes back as a typed input parameter, FIRST in two
# grammatical roles (row-limit clause and plain column name), two syntax
# errors carrying the offending token's line/column, and a semantic error
# whose position survives past the parse.  firebird-driver's Statement
# publishes the statement type (Statement.type, a StatementType enum); the
# parameter and column descriptors are read here from the OO API's
# IMessageMetadata the driver wraps (Statement._istmt) - the public API only
# describes output columns, via cursor.description after execute.  A failed
# prepare raises DatabaseError with the formatted status vector plus sqlcode
# and the raw gds_codes, so syntax (isc_dsql_token_unk_err) and semantic
# (isc_dsql_field_err) failures can be told apart programmatically.
#
# Read-only against the stock employee database.
#
# Run:  python3 parser_errors.py
#
from firebird.driver import DatabaseError

from fbsample import employee, error_text, run

ISC_DSQL_FIELD_ERR = 335544578      # "Column unknown"
ISC_DSQL_TOKEN_UNK_ERR = 335544634  # "Token unknown"


def try_prepare(con, sql):
    """Feed one string to the parser; report the parsed shape or the error."""
    print('----', sql)
    cur = con.cursor()
    try:
        with cur.prepare(sql) as stmt:
            inp = stmt._istmt.get_input_metadata()
            out = stmt._istmt.get_output_metadata()
            print(f'  parsed OK: type={stmt.type.name}, '
                  f'input params={inp.get_count()}, output columns={out.get_count()}')
            for i in range(inp.get_count()):
                print(f'    param {i}: sqltype={int(inp.get_type(i))} '
                      f'({inp.get_type(i).name}), length={inp.get_length(i)}')
            inp.release()
            out.release()
    except DatabaseError as e:
        kind = ('syntax' if ISC_DSQL_TOKEN_UNK_ERR in e.gds_codes else
                'semantic' if ISC_DSQL_FIELD_ERR in e.gds_codes else '?')
        print(f'  prepare failed ({kind}; sqlcode {e.sqlcode}):')
        print(error_text(e))
    finally:
        cur.close()


def main():
    with employee(charset='NONE') as con:
        # 1. Dynamic SQL: the `?` becomes a typed parameter.
        try_prepare(con, 'SELECT first_name FROM employee WHERE emp_no = ?')

        # 2. One token, two grammatical roles: FIRST as row-limit clause...
        try_prepare(con, 'SELECT FIRST 1 emp_no FROM employee')
        # ...and FIRST as an ordinary identifier (non-reserved keyword).
        try_prepare(con, 'SELECT first FROM (SELECT 1 AS first FROM rdb$database)')

        # 3. Syntax errors with token position.
        try_prepare(con, 'SELEC 1 FROM rdb$database')
        try_prepare(con, 'SELECT emp_no\nFROM employee\nWHERE ORDER BY 1')

        # 4. Semantic error - still carries line/column.
        try_prepare(con, 'SELECT frst_name\nFROM employee')
        con.commit()
    print('done.')


if __name__ == '__main__':
    run(main)
