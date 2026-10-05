#
# blr.py - stored BLR read raw from the catalog and disassembled (Python twin
# of ../cpp/blr.cpp; see ../../blr-intermediate-language.md).
#
# Fetches RDB$FIELDS.RDB$COMPUTED_BLR for EMPLOYEE.FULL_NAME and
# RDB$PROCEDURES.RDB$PROCEDURE_BLR for GET_EMP_PROJ from the stock employee
# database, hex-dumps them and decodes the opening bytes: the computed
# column's whole prefix-encoded expression tree, the procedure's message
# declarations.  Read-only.
#
# firebird-driver hands a non-text blob (BLR is sub_type 2) back as plain
# `bytes` - no blob handle, no segments, no server-side CAST.  The driver
# has no BLR opcode table, so like the C++ twin's #include the sample takes
# firebird/impl/blr.h literally: it parses the installed header's #defines
# at startup and falls back to a transcribed table if no header is found.
#
# Run:  python3 blr.py [database]
#
import os
import re
import sys

from fbsample import attach, run, scalar

HEADERS = ['/opt/firebird/include/firebird/impl/blr.h',
           '/usr/include/firebird/impl/blr.h']
TRANSCRIBED = {'blr_text2': 15, 'blr_short': 7, 'blr_version5': 5, 'blr_eoc': 76,
               'blr_begin': 2, 'blr_message': 4, 'blr_literal': 21,
               'blr_field': 23, 'blr_concatenate': 39}


def opcodes():
    """{name: value} for every blr_* #define in blr.h (or the fallback)."""
    for h in HEADERS:
        if os.path.exists(h):
            text = open(h, encoding='latin-1').read()
            table = {m[1]: int(m[2]) for m in re.finditer(
                r'#define\s+(blr_\w+)\s+\(unsigned char\)\s*(\d+)', text)}
            print(f'(opcodes: {len(table)} blr_* defines parsed from {h})\n')
            return table
    print('(opcodes: transcribed table - no blr.h found)\n')
    return TRANSCRIBED


B = opcodes()


def hex_dump(b, limit):
    shown = b[:limit]
    lines = [' '.join(f'{x:02x}' for x in shown[i:i + 16])
             for i in range(0, len(shown), 16)]
    tail = f'... ({len(b)} bytes total)' if len(b) > limit else f'({len(b)} bytes total)'
    if len(shown) % 16:
        lines[-1] += ' ' + tail
    else:
        lines.append(tail)
    print('\n'.join(lines))


def u16(b, i):
    return b[i] | (b[i + 1] << 8)


def expr(b, i, depth):
    """Decode one expression at b[i]; return the offset after it."""
    pad = '   ' * depth
    op = b[i]
    i += 1
    if op == B['blr_concatenate']:
        print(pad + 'blr_concatenate')
        i = expr(b, i, depth + 1)
        return expr(b, i, depth + 1)
    if op == B['blr_field']:
        ctx, n = b[i], b[i + 1]
        print(f"{pad}blr_field context {ctx}, '{b[i + 2:i + 2 + n].decode()}'")
        return i + 2 + n
    if op == B['blr_literal'] and b[i] == B['blr_text2']:
        cs, n = u16(b, i + 1), u16(b, i + 3)
        print(f'{pad}blr_literal blr_text2 charset {cs}, len {n},'
              f' "{b[i + 5:i + 5 + n].decode()}"')
        return i + 5 + n
    print(f'{pad}opcode {op} (decoder stops here)')
    return len(b)


def main():
    database = sys.argv[1] if len(sys.argv) > 1 else 'employee'
    with attach(database, charset='NONE') as con:
        print('== computed column EMPLOYEE.FULL_NAME — RDB$FIELDS.RDB$COMPUTED_BLR')
        blr = scalar(con, "SELECT f.RDB$COMPUTED_BLR FROM RDB$FIELDS f"
                          " JOIN RDB$RELATION_FIELDS rf"
                          "   ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE"
                          " WHERE rf.RDB$RELATION_NAME = 'EMPLOYEE'"
                          "   AND rf.RDB$FIELD_NAME = 'FULL_NAME'")
        print(f'(python type: {type(blr).__name__})')
        hex_dump(blr, 64)
        print('blr_version5' if blr[0] == B['blr_version5'] else 'unexpected version!')
        i = expr(blr, 1, 1)
        print('blr_eoc' if i < len(blr) and blr[i] == B['blr_eoc'] else '(no blr_eoc?)')

        print('\n== procedure GET_EMP_PROJ — RDB$PROCEDURES.RDB$PROCEDURE_BLR')
        blr = scalar(con, "SELECT RDB$PROCEDURE_BLR FROM RDB$PROCEDURES"
                          " WHERE RDB$PROCEDURE_NAME = 'GET_EMP_PROJ'")
        hex_dump(blr, 32)
        print(('blr_version5' if blr[0] == B['blr_version5'] else '?') + ', ' +
              ('blr_begin' if blr[1] == B['blr_begin'] else '?'))
        i = 2
        while blr[i] == B['blr_message']:
            msg, count = blr[i + 1], u16(blr, i + 2)
            i += 4
            fields = []
            for _ in range(count):
                if blr[i] == B['blr_short']:
                    fields.append(f'blr_short(scale {blr[i + 1]})')
                    i += 2
                elif blr[i] == B['blr_text2']:
                    fields.append(f'blr_text2(cs {u16(blr, i + 1)}, len {u16(blr, i + 3)})')
                    i += 5
                else:
                    fields.append(f'dtype {blr[i]}?')
                    break
            print(f'blr_message {msg}, {count} fields: ' + ' '.join(fields))
        print(f'... {len(blr) - i} more bytes — see isql SET BLOB ALL for the full dump')
        con.commit()


if __name__ == '__main__':
    run(main)
