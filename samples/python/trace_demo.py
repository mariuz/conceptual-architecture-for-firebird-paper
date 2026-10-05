#
# trace_demo.py - a complete user trace session through the Services API
# (Python twin of ../cpp/trace.cpp; see ../../trace-and-audit.md).  Not
# named trace.py: that would shadow the standard library module.
#
#   service A: trace start with an inline configuration, then the session's
#              TraceLog streamed back line by line;
#   worker:    attaches to the traced database and runs one marker query;
#   service B: lists the active sessions, then stops ours, ending A's stream.
#
# firebird-driver wraps the whole trace family as typed methods on
# Server.trace: start(config=, name=) sends isc_action_svc_trace_start with
# the text in isc_spb_trc_cfg and returns the session id (the driver reads
# the "Trace session ID n started" line for us), readline_timed() drains the
# isc_info_svc_line stream, sessions decodes trace_list, and
# stop(session_id=) sends isc_action_svc_trace_stop - no raw SPB tags at all.
# One gap on Firebird 6: driver 2.0.3's trace_list parser rejects the new
# "plugins:" line, so sessions raises InterfaceError (caught and shown).
# The service attaches go through fbsample.server(), which registers the
# host: connect_server() otherwise takes the bare name as a config and
# silently attaches the in-process (embedded) service_mgr.
#
# Run:  python3 trace_demo.py [database]
#
import threading
import time

from firebird.driver import TIMEOUT, InterfaceError

from fbsample import attach, attach_or_create, db_path, run, scalar, server


TRACE_CFG = """database = {path}
{{
  enabled = true
  log_connections = true
  log_transactions = true
  log_statement_finish = true
  print_plan = true
  print_perf = true
  time_threshold = 0
}}
"""


def observed_then_stop(path, session_id):
    time.sleep(0.8)
    with attach(path) as db:
        print('[worker] marker query says:',
              scalar(db, 'SELECT COUNT(*) FROM RDB$RELATIONS /* traced! */'))
        db.commit()
    time.sleep(1.2)
    with server() as srv_b:
        try:
            for sid, s in srv_b.trace.sessions.items():
                print(f'[list ] session {sid} name={s.name!r} user={s.user} flags={s.flags}')
        except InterfaceError as e:            # driver 2.0.3 vs Firebird 6's list
            print('[list ] driver could not parse trace_list:', str(e).strip())
        print('[stop ]', srv_b.trace.stop(session_id=session_id).strip())


def main():
    path = db_path('trace')
    with attach_or_create(path):              # must exist before the session
        pass

    with server() as srv_a:
        session_id = srv_a.trace.start(config=TRACE_CFG.format(path=path),
                                       name='hands-on-py')
        print(f'[trace] Trace session ID {session_id} started')
        worker = threading.Thread(target=observed_then_stop, args=(path, session_id))
        worker.start()
        # readline_timed() queries isc_info_svc_line, so lines arrive live;
        # plain iteration (readline) asks for isc_info_svc_to_eof and would
        # hand the whole log over in bulk only once the session has ended.
        while (line := srv_a.readline_timed(1)) is not None:
            if line is not TIMEOUT and line.strip():
                print('[trace]', line.rstrip())
        worker.join()
    print('done.')


if __name__ == '__main__':
    run(main)
