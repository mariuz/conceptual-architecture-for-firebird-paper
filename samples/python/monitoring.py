#
# monitoring.py - the MON$ hierarchy and its stable snapshot (Python twin of
# ../cpp/monitoring.cpp; see ../../monitoring-and-tuning.md).
#
# Walks MON$DATABASE -> MON$ATTACHMENTS -> MON$TRANSACTIONS ->
# MON$STATEMENTS down to this very connection, reads its own counters
# through the MON$STAT_ID join, runs a 10 000-row full-scan workload INSIDE
# the same transaction and shows the counters frozen (the first MON$ select
# took a stable snapshot), then refreshed in a new transaction.
#
# firebird-driver's connection runs every statement in its main transaction
# until commit(), so the frozen snapshot needs no ceremony.  It also offers
# the second, older monitoring channel side by side: the attachment info
# calls (con.info.fetches, con.info.get_table_access_stats() - the
# isc_info_* counters behind gstat-style per-table reads) are answered live
# by the engine, NOT from a snapshot, so they move while MON$ stands still.
#
# Run:  python3 monitoring.py [database]
#
from fbsample import attach_or_create, db_path, execute, query, run, scalar

COUNTERS = ('SELECT R.MON$RECORD_SEQ_READS, R.MON$RECORD_IDX_READS, '
            '       R.MON$RECORD_INSERTS, I.MON$PAGE_FETCHES '
            'FROM MON$ATTACHMENTS A '
            'JOIN MON$RECORD_STATS R ON R.MON$STAT_ID = A.MON$STAT_ID '
            'JOIN MON$IO_STATS I     ON I.MON$STAT_ID = A.MON$STAT_ID '
            'WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION')


def counters(con, label):
    seq, idx, ins, fetches = query(con, COUNTERS)[0]
    print(f'{label:<38} seq_reads={seq:<6} idx_reads={idx:<5} '
          f'inserts={ins:<6} page_fetches={fetches}')


def info(con, label, rel_id):
    # per-table counters are keyed by relation id (isc_info_read_seq_count)
    work = [s for s in con.info.get_table_access_stats() if s.table_id == rel_id]
    seq = (work[0].sequential if work else None) or 0
    print(f'{label:<38} info.fetches={con.info.fetches}  '
          f'MON_WORK sequential reads={seq}')


def main():
    with attach_or_create(db_path('monitoring')) as con:
        # Workload table: 10000 rows to scan.
        execute(con, 'RECREATE TABLE MON_WORK (ID INT NOT NULL PRIMARY KEY, VAL INT)')
        execute(con, 'EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN '
                     '  WHILE (I < 10000) DO BEGIN INSERT INTO MON_WORK VALUES (:I, :I); '
                     '  I = I + 1; END '
                     'END')

        rel_id = scalar(con, "SELECT RDB$RELATION_ID FROM RDB$RELATIONS "
                             "WHERE RDB$RELATION_NAME = 'MON_WORK'")
        con.commit()

        # -- 1. the hierarchy, one level per query, one consistent snapshot --
        print('== MON$DATABASE: transaction markers ==')
        oit, oat, nxt, bufs = query(con, 'SELECT MON$OLDEST_TRANSACTION, MON$OLDEST_ACTIVE, '
                                         '       MON$NEXT_TRANSACTION, MON$PAGE_BUFFERS '
                                         'FROM MON$DATABASE')[0]
        print(f'OIT={oit} OAT={oat} NEXT={nxt} page_buffers={bufs}')

        print('\n== MON$ATTACHMENTS -> MON$TRANSACTIONS -> MON$STATEMENTS (me) ==')
        for att, user, tra, state, sql in query(con,
                'SELECT A.MON$ATTACHMENT_ID, A.MON$USER, T.MON$TRANSACTION_ID, '
                '       S.MON$STATE, CAST(SUBSTRING(S.MON$SQL_TEXT FROM 1 FOR 40) '
                '       AS VARCHAR(40)) '
                'FROM MON$ATTACHMENTS A '
                'JOIN MON$TRANSACTIONS T ON T.MON$ATTACHMENT_ID = A.MON$ATTACHMENT_ID '
                'JOIN MON$STATEMENTS S   ON S.MON$TRANSACTION_ID = T.MON$TRANSACTION_ID '
                'WHERE A.MON$ATTACHMENT_ID = CURRENT_CONNECTION'):
            print(f'attachment {att} ({user.strip()}), tx {tra}, state {state}: {sql}')

        # -- 2. the snapshot property, measured on our own counters ---------
        print()
        counters(con, 'MON$ snapshot 1:')
        info(con, 'info calls, before the workload:', rel_id)

        print('\n... running workload: SELECT COUNT(*) full scan + indexed lookup ...')
        print(f"count = {scalar(con, 'SELECT COUNT(*) FROM MON_WORK')}, "
              f"point = {scalar(con, 'SELECT VAL FROM MON_WORK WHERE ID = 4242')}")

        print()
        counters(con, 'same transaction: STILL snapshot 1:')
        info(con, 'info calls, same moment: live:', rel_id)
        con.commit()

        print()
        counters(con, 'new transaction: fresh snapshot:')
        con.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
