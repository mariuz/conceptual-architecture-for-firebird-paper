{
  events.pas — Firebird event notification, in Pascal.

  The fbintf twin of ../events_demo.cpp: two attachments to the same
  database over TCP — a LISTENER that registers interest in
  'demo_event' and a POSTER that executes PSQL blocks containing
  POST_EVENT.  Where the C++ twin implements IEventCallback by hand
  (event block, isc_event_counts, requeue), fbintf wraps the whole
  machinery in IEvents: GetEventHandler + AsyncWaitForEvent arm the
  auxiliary connection, the callback fires on fbintf's event thread,
  and ExtractEventCounts computes the delivered deltas — the same
  isc_event_counts arithmetic, pre-packaged.

  The demo shows the three defining semantics of Firebird events:
    1. delivery happens at COMMIT, not when POST_EVENT executes;
    2. a ROLLBACK discards pending posts — nothing is delivered;
    3. multiple posts of one name in one transaction are delivered
       once, with a count.

  See ../../firebird-events.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/events && samples/fpc/bin/events
}
program events;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses
  {$IFDEF UNIX} cthreads, {$ENDIF}
  SysUtils, IB, FBHandsOn;

type
  {AsyncWaitForEvent wants a method of object; the handler runs on
   fbintf's event thread, so it only sets a flag the main thread polls.}
  TListener = class
    Fired: boolean;
    procedure OnEvent(Sender: IEvents);
  end;

procedure TListener.OnEvent(Sender: IEvents);
begin
  Fired := true;
end;

var L: TListener;
    LA, PA: IAttachment;
    EH: IEvents;
    Tr: ITransaction;
    got, early: integer;
    rc: integer;

{Poll for the callback: -1 = nothing delivered before the timeout,
 otherwise the delivered count for 'demo_event' (the callback disarms
 the wait, so the caller must rearm afterwards).}
function WaitFired(timeoutMs: integer): integer;
var waited, i, total: integer;
    counts: TEventCounts;
begin
  waited := 0;
  while (not L.Fired) and (waited < timeoutMs) do
  begin
    Sleep(50);
    Inc(waited, 50);
  end;
  if not L.Fired then Exit(-1);
  L.Fired := false;
  total := 0;
  counts := EH.ExtractEventCounts;
  for i := 0 to High(counts) do
    if counts[i].EventName = 'demo_event' then Inc(total, counts[i].Count);
  Result := total;
end;

begin
  rc := 0;
  L := TListener.Create;
  LA := AttachOrCreate(DbConn('events'));
  PA := FirebirdAPI.OpenDatabase(DbConn('events'), DefaultDPB);

  { Register interest, then prove the plumbing with one committed primer
    post.  Deliveries are one-shot: each callback disarms the wait and
    AsyncWaitForEvent must be called again — fbintf's ExtractEventCounts
    keeps the previous counts as the baseline, so only new posts count. }
  EH := LA.GetEventHandler('demo_event');
  EH.AsyncWaitForEvent(L.OnEvent);
  Tr := PA.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  PA.ExecuteSQL(Tr, 'EXECUTE BLOCK AS BEGIN POST_EVENT ''demo_event''; END', []);
  Tr.Commit;
  got := WaitFired(3000);
  if got >= 0 then
    EH.AsyncWaitForEvent(L.OnEvent);   { delivery disarmed the wait: rearm }
  writeln('listener registered for ''demo_event'' (primer post delivered, count ',
    got, ')');

  { -- 1. POST_EVENT then ROLLBACK: nothing may be delivered. }
  Tr := PA.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taRollback);
  PA.ExecuteSQL(Tr, 'EXECUTE BLOCK AS BEGIN POST_EVENT ''demo_event''; END', []);
  Tr.Rollback;
  got := WaitFired(1500);
  if got < 0 then
    writeln('after POST_EVENT + ROLLBACK: delivered count = 0  '
      + '(correct - rollback swallows posts)')
  else
  begin
    writeln('after POST_EVENT + ROLLBACK: delivered count = ', got, '  (UNEXPECTED)');
    rc := 1;
    EH.AsyncWaitForEvent(L.OnEvent);
  end;

  { -- 2. Three POST_EVENTs in one transaction, then COMMIT:
          one delivery with count 3. }
  Tr := PA.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  PA.ExecuteSQL(Tr, 'EXECUTE BLOCK AS BEGIN POST_EVENT ''demo_event''; '
    + 'POST_EVENT ''demo_event''; POST_EVENT ''demo_event''; END', []);
  writeln('3 x POST_EVENT executed, not yet committed - waiting briefly...');

  early := WaitFired(1000);
  if early < 0 then
    writeln('before COMMIT: delivered count = 0  (correct - delivery is commit-time)')
  else
  begin
    writeln('before COMMIT: delivered count = ', early, '  (UNEXPECTED)');
    rc := 1;
    EH.AsyncWaitForEvent(L.OnEvent);
  end;

  Tr.Commit;
  got := WaitFired(3000);
  if got < 0 then got := 0;
  if got = 3 then
    writeln('after COMMIT: delivered count = ', got, '  (correct - one delivery, count 3)')
  else
  begin
    writeln('after COMMIT: delivered count = ', got, '  (UNEXPECTED)');
    rc := 1;
  end;

  EH.Cancel;
  EH := nil;
  PA.Disconnect;
  LA.Disconnect;
  L.Free;

  if rc = 0 then writeln('PASS') else writeln('FAIL');
  writeln('done.');
  Halt(rc);
end.
