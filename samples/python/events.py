#
# events.py - Firebird event notification: the three defining semantics
# (Python twin of ../events_demo.cpp; see ../../firebird-events.md).
#
# A LISTENER attachment registers interest in 'demo_event'; a POSTER runs
# PSQL blocks with POST_EVENT, showing that (1) a ROLLBACK swallows posts,
# (2) delivery happens at COMMIT, not when POST_EVENT executes, and (3) several
# posts in one transaction coalesce into one delivery with a count.
#
# firebird-driver packages the whole client-side dance in
# Connection.event_collector(names): begin() queues the interest (the
# auxiliary connection opens), the first delivery - the baseline - is
# swallowed internally, every later delivery runs isc_event_counts and
# re-queues the one-shot interest on the collector's own thread, and the
# deltas ACCUMULATE in a dict until flush().  wait(timeout) blocks until
# something arrives and returns that dict.  Under the hood it is the legacy
# ISC API (isc_event_block / isc_que_events / isc_event_counts), not the OO
# API's IAttachment::queEvents the C++ sample uses.
#
# Run:  python3 events.py [database]      (default: employee)
#
import sys

from fbsample import attach, execute, run

EVENT = 'demo_event'


def delivered(collector, timeout):
    """Wait up to `timeout` s; return the accumulated count and reset it."""
    count = collector.wait(timeout)[EVENT]
    collector.flush()
    return count


def main():
    database = sys.argv[1] if len(sys.argv) > 1 else 'employee'
    ok = True
    with attach(database) as listener, attach(database) as poster, \
            listener.event_collector([EVENT]) as collector:
        print(f"listener registered for '{EVENT}' (baseline consumed by EventCollector)")

        # 1. POST_EVENT then ROLLBACK: nothing may be delivered.
        execute(poster, f"EXECUTE BLOCK AS BEGIN POST_EVENT '{EVENT}'; END", commit=False)
        poster.rollback()
        got = delivered(collector, 1.5)
        ok &= got == 0
        print(f'after POST_EVENT + ROLLBACK: delivered count = {got}  '
              + ('(correct - rollback swallows posts)' if got == 0 else '(UNEXPECTED)'))

        # 2. Three POST_EVENTs in one transaction, then COMMIT: one delivery, count 3.
        execute(poster, 'EXECUTE BLOCK AS BEGIN ' + f"POST_EVENT '{EVENT}'; " * 3 + 'END',
                commit=False)
        print('3 x POST_EVENT executed, not yet committed - waiting briefly...')
        early = delivered(collector, 1.0)
        ok &= early == 0
        print(f'before COMMIT: delivered count = {early}  '
              + ('(correct - delivery is commit-time)' if early == 0 else '(UNEXPECTED)'))

        poster.commit()
        got = delivered(collector, 3.0)
        ok &= got == 3
        print(f'after COMMIT: delivered count = {got}  '
              + ('(correct - one delivery, count 3)' if got == 3 else '(UNEXPECTED)'))
    print('PASS' if ok else 'FAIL')
    if not ok:
        sys.exit(1)


if __name__ == '__main__':
    run(main)
