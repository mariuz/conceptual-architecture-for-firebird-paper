{
  ha.pas — the one HA primitive that is pure client-side SQL: a SHADOW.

  The fbintf twin of ../cpp/ha.cpp: creates a shadow on a scratch
  database, proves the mirror file appears (RDB$FILES plus a stat() of
  the file — server and sample run on the same host here), shows the
  shadow growing in lock-step with the main file as rows are inserted,
  and drops it again.  The other primitives (replica promotion with
  gfix -replica none, sync_replica) need server-side config and stay
  as text in the document.  See ../../high-availability.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/ha && samples/fpc/bin/ha
}
program ha;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, BaseUnix, IB, FBHandsOn;

const SHAD = '/tmp/fbhandson/ha_fpc.shd';

var A: IAttachment;
    Tr: ITransaction;

function FSize(const path: AnsiString): int64;
var st: Stat;
begin
  if FpStat(path, st) = 0 then Result := st.st_size else Result := -1;
end;

procedure ShowFiles(const when: AnsiString);
begin
  writeln(Format('%-28s main = %8d bytes, shadow = %8d bytes',
    [when, FSize(DbPath('ha')), FSize(SHAD)]));
end;

var R: IResultSet;

begin
  A := AttachOrCreate(DbConn('ha'));

  { Idempotent cleanup from earlier runs. }
  try
    A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
      'DROP SHADOW 1 DELETE FILE');
  except on EIBInterBaseError do ;
  end;
  try
    A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
      'DROP TABLE HA_LOG');
  except on EIBInterBaseError do ;
  end;
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    'CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))');

  { -- 1. Create the synchronous page-level mirror. }
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  A.ExecuteSQL(Tr, 'CREATE SHADOW 1 ''' + SHAD + '''', []);
  Tr.CommitRetaining;
  writeln('CREATE SHADOW 1 done - the engine dumped every page to the mirror');
  writeln;

  { The shadow is registered in the metadata like any other file. }
  R := A.OpenCursorAtStart(Tr,
    'SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS '
    + 'FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER');
  while not R.IsEof do
  begin
    writeln(Format('%-28s shadow_number=%s flags=%s',
      [TrimRight(R[0].AsString), R[1].AsString, R[2].AsString]));
    R.FetchNext;
  end;
  writeln;
  ShowFiles('after CREATE SHADOW:');

  { -- 2. Write load: every page write now goes to both files. }
  A.ExecuteSQL(Tr,
    'EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN '
    + '  WHILE (I < 5000) DO BEGIN '
    + '    INSERT INTO HA_LOG VALUES (:I, LPAD('''', 200, ''x'')); I = I + 1; '
    + '  END '
    + 'END', []);
  Tr.CommitRetaining;
  ShowFiles('after 5000 inserts:');

  { -- 3. Retire the mirror. }
  A.ExecuteSQL(Tr, 'DROP SHADOW 1 DELETE FILE', []);
  Tr.Commit;
  writeln;
  writeln('DROP SHADOW 1 DELETE FILE done');
  ShowFiles('after DROP SHADOW:');

  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln;
  writeln('RDB$FILES rows left: ',
    A.OpenCursorAtStart(Tr, 'SELECT COUNT(*) FROM RDB$FILES')[0].AsString);
  Tr.Commit;
  writeln('done.');
end.
