#
# memory_pools.py - the pool hierarchy made visible from SQL via
# MON$MEMORY_USAGE (Python twin of ../cpp/memory_pools.cpp; see
# ../../memory-management.md).
#
# The six-group summary with the parent-redirection signature (child pools
# with real MON$MEMORY_USED and zero MON$MEMORY_ALLOCATED), one connection's
# database -> attachment -> transaction chain, and a transaction pool
# growing under an uncommitted 3000-row UPDATE, then vanishing at rollback -
# watched from a second attachment, because a MON$ snapshot is frozen per
# transaction (so every observation here commits the monitor's transaction).
# firebird-driver supplies the ids the C++ sample asks SQL for
# (current_connection / current_transaction) as info calls: con.info.id and
# TransactionManager.info.id; and con.info.current_memory reads the
# database-level usage counter through isc_info_current_memory - no MON$
# snapshot involved, so it differs from the MON$DATABASE row only by the
# churn between the two reads.
#
# Run:  python3 memory_pools.py [database]
#
from fbsample import attach, attach_or_create, db_path, execute, query, run


def level_summary(mon):
    rows = query(mon, 'select MON$STAT_GROUP, count(*), sum(MON$MEMORY_USED), '
                      '       sum(MON$MEMORY_ALLOCATED), '
                      '       count(nullif(MON$MEMORY_ALLOCATED, 0)) '
                      'from MON$MEMORY_USAGE group by 1 order by 1')
    mon.commit()
    print('GROUP POOLS USED       ALLOCATED  WITH_OWN_EXTENTS')
    for g, n, used, alloc, own in rows:
        print(f'{g:<5} {n:<5} {used:<10} {alloc:<10} {own}')


def pool_row(mon, label, join, params=()):
    """One row of the worker's pool chain, from a fresh monitor snapshot."""
    rows = query(mon, 'select MON$MEMORY_USED, MON$MEMORY_ALLOCATED '
                      'from MON$MEMORY_USAGE ' + join, params)
    mon.commit()
    if rows:
        print(f'  {label:<24} used={rows[0][0]:<10} allocated={rows[0][1]}')
        return rows[0][0]
    return None


def main():
    path = db_path('memory_pools')
    with attach_or_create(path) as worker, attach(path) as mon:
        execute(worker, 'recreate table t (id int, pad varchar(200))')
        execute(worker, "execute block as declare i int = 0; begin"
                        "  while (i < 3000) do begin"
                        "    insert into t values (:i, rpad('x', 200, 'x')); i = i + 1;"
                        "  end "
                        "end")

        print('-- per-level summary (0=db 1=att 2=tra 3=stmt 5=cmp; '
              'used > 0 with allocated = 0: parent redirection)')
        level_summary(mon)

        # The worker's own chain: database -> attachment -> transaction.
        tm = worker.transaction_manager()
        tm.begin()
        att, tra = worker.info.id, tm.info.id
        by_att = ('join MON$ATTACHMENTS using (MON$STAT_ID) '
                  'where MON$ATTACHMENT_ID = ?', (att,))
        by_tra = ('join MON$TRANSACTIONS using (MON$STAT_ID) '
                  'where MON$TRANSACTION_ID = ?', (tra,))

        print(f'\n-- worker\'s pool chain (attachment {att}, transaction {tra}; before the update)')
        pool_row(mon, 'database pool:', 'join MON$DATABASE using (MON$STAT_ID)')
        print(f'  {"(con.info.current_memory":<24} {worker.info.current_memory})')
        pool_row(mon, 'worker attachment pool:', *by_att)
        pool_row(mon, 'worker transaction pool:', *by_tra)

        # Grow the transaction pool: the undo log of an uncommitted UPDATE
        # lives in the transaction's pool.
        execute(worker, "update t set pad = rpad('y', 200, 'y')", tr=tm)

        print('\n-- after an uncommitted 3000-row UPDATE in that transaction')
        att_before = pool_row(mon, 'worker attachment pool:', *by_att)
        tra_pool = pool_row(mon, 'worker transaction pool:', *by_tra)

        tm.rollback()                        # bulk-free: the whole pool goes at once
        tm.close()
        print('\n-- after rollback (transaction pool destroyed with its undo log)')
        att_after = pool_row(mon, 'worker attachment pool:', *by_att)
        print(f'  attachment used fell by {att_before - att_after}; '
              f'the dead transaction pool held {tra_pool}')


if __name__ == '__main__':
    run(main)
