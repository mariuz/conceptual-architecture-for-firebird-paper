#
# temporal.py - WITH TIME ZONE storage seen from the client (Python twin of
# ../cpp/temporal.cpp; see ../../temporal-and-time-zones.md).
#
# 1./2. TIMESTAMP '2026-07-18 12:00:00 America/New_York' and the same wall
#       time at a bare -05:00 offset, fetched natively and then re-encoded
#       into the 12-byte ISC_TIMESTAMP_TZ struct (UTC instant + zone id).
# 3.    AT TIME ZONE across a DST boundary; equality by UTC instant.
# 4.    The session time zone: SET TIME ZONE, and the DPB alternative.
#
# firebird-driver is the zone-*keeping* attitude: a TIMESTAMP WITH TIME ZONE
# arrives as an aware datetime.datetime whose tzinfo is a dateutil zone that
# remembers the Firebird name (tzinfo._timezone_), decoded by the same
# IUtil::decodeTimeStampTz the C++ sample calls by hand.  The cursor hides
# the raw message buffer, so the wire struct is shown by handing the value
# back to IUtil::encodeTimeStampTz (the driver's internal _util) - a round
# trip, not a peek at the fetched bytes.  Note that the UTC offset Python
# reports comes from the *client's* tzdata (/usr/share/zoneinfo), not from
# the engine's ICU.
#
# Run:  python3 temporal.py [database]
#
import struct

from firebird.driver import connect, core

from fbsample import (PASSWORD, USER, attach_or_create, db_path, dsn, execute,
                      query, run, scalar)

ZONE = "select rdb$get_context('SYSTEM', 'SESSION_TIMEZONE') from rdb$database"
NOW = 'select current_timestamp from rdb$database'


def wire(value):
    """ISC_TIMESTAMP_TZ = {ISC_DATE days; ISC_TIME time; USHORT zone id}."""
    return struct.unpack('<iIH', core._util.encode_timestamp_tz(value)[:10])


def main():
    path = db_path('temporal')
    with attach_or_create(path) as db:
        # -- 1./2. Named zone vs bare offset.
        row = query(db, "select timestamp '2026-07-18 12:00:00 America/New_York',"
                        "       timestamp '2026-07-18 12:00:00 -05:00' from rdb$database")[0]
        for label, ts in zip(('named-zone', 'offset'), row):
            days, time, zone_id = wire(ts)
            print(f'{label} literal:')
            print(f'  python value: {ts.isoformat()}  zone={ts.tzinfo._timezone_}'
                  f'  ({type(ts.tzinfo).__name__})')
            print(f'  as the struct: UTC days={days} time={time}  zone id={zone_id}')

        # -- 3. The same wall time across a DST boundary.
        for season, date in (('winter', '2026-01-18'), ('summer', '2026-07-18')):
            utc = scalar(db, f"select timestamp '{date} 12:00:00 America/New_York'"
                             " at time zone 'Etc/UTC' from rdb$database")
            print(f'{"" if season == "summer" else chr(10)}NY 12:00 in UTC, {season}:', utc)
        print('10:00 -02:00 = 09:00 -03:00 ?', scalar(
            db, "select iif(time '10:00:00 -02:00' = time '09:00:00 -03:00',"
                " 'EQUAL', 'different') from rdb$database").rstrip())

        # -- 4. The session time zone governs "now".
        print(f'\nsession zone: {scalar(db, ZONE)}   CURRENT_TIMESTAMP: {scalar(db, NOW)}')
        execute(db, "set time zone 'Asia/Tokyo'")
        print(f'session zone: {scalar(db, ZONE)}         CURRENT_TIMESTAMP: {scalar(db, NOW)}')
        db.commit()

    # The same choice made at attach time: connect(session_time_zone=...)
    # puts isc_dpb_session_time_zone in the DPB.
    with connect(dsn(path), user=USER, password=PASSWORD,
                 session_time_zone='America/Sao_Paulo') as db:
        print(f'DPB zone    : {scalar(db, ZONE)}  CURRENT_TIMESTAMP: {scalar(db, NOW)}')
        db.commit()
    print('\ndone.')


if __name__ == '__main__':
    run(main)
