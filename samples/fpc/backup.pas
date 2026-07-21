{
  backup.pas — a gbak backup + restore round trip through the Services API.

  The fbintf twin of ../cpp/backup.cpp, companion to
  ../../backup-and-recovery.md.  The same path `gbak -se` and fbsvcmgr use,
  no gbak binary needed on the client:

    1. create a scratch database with a table and a few rows;
    2. IServiceManager + isc_action_svc_backup (verbose), streaming gbak's
       log line by line with isc_info_svc_line;
    3. isc_action_svc_restore (replace) the same way;
    4. attach to the restored database and prove the rows survived.

  The backup runs while the source attachment is still open: gbak reads
  through a snapshot transaction — the "online" property of the document's
  gbak section.  Driver contrast: fbintf reaches the Services API natively
  (IServiceManager, IB.pas) just like the C++ OO API and node-firebird's
  pure-JS client; rsfbclient cannot, and its twin shells out to gbak.

  Build & run (see ../README.md):
      make -C samples/fpc bin/backup && samples/fpc/bin/backup
}
program backup;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const FBK = '/tmp/fbhandson/backup_fpc.fbk';               {server-side path}
      DB_REST = '/tmp/fbhandson/backup_fpc_restored.fdb';

{Drain one service's verbose output: query isc_info_svc_line until empty.}
procedure StreamServiceOutput(Svc: IServiceManager; const pfx: AnsiString);
var Req: ISRB;
    Res: IServiceQueryResults;
    Item: IServiceQueryResultItem;
    line: AnsiString;
begin
  repeat
    Req := Svc.AllocateSRB;
    Req.Add(isc_info_svc_line);
    Res := Svc.Query(Req);
    Item := Res.Find(isc_info_svc_line);
    if Item = nil then break;
    line := Item.AsString;                 {empty line = service done}
    if line <> '' then
      writeln(pfx, line);
  until line = '';
end;

var Att, RAtt: IAttachment;
    Tr: ITransaction;
    SPB: ISPB;
    Svc: IServiceManager;
    Req: ISRB;

begin
  { -- 1. scratch source database --------------------------------- }
  Att := AttachOrCreate(DbConn('backup'));
  Tr := Att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  try
    Att.ExecuteSQL(Tr, 'DROP TABLE BR_ITEMS', []);
  except on EIBInterBaseError do ;
  end;
  Att.ExecuteSQL(Tr,
    'CREATE TABLE BR_ITEMS (ID INT NOT NULL PRIMARY KEY, NAME VARCHAR(30))', []);
  Tr.CommitRetaining;
  Att.ExecuteSQL(Tr, 'INSERT INTO BR_ITEMS VALUES (1, ''alpha'')', []);
  Att.ExecuteSQL(Tr, 'INSERT INTO BR_ITEMS VALUES (2, ''beta'')', []);
  Att.ExecuteSQL(Tr, 'INSERT INTO BR_ITEMS VALUES (3, ''gamma'')', []);
  Tr.Commit;
  writeln('source ready: BR_ITEMS with 3 rows');

  { -- 2. attach to the service manager --------------------------- }
  SPB := FirebirdAPI.AllocateSPB;
  SPB.Add(isc_spb_user_name).AsString := HandsOnUser;
  SPB.Add(isc_spb_password).AsString := HandsOnPassword;
  Svc := FirebirdAPI.GetServiceManager('localhost', TCP, SPB);

  { -- 3. gbak backup through the service, verbose ---------------- }
  writeln;
  writeln('== backup: ', DbPath('backup'), ' -> ', FBK, ' ==');
  Req := Svc.AllocateSRB;
  Req.Add(isc_action_svc_backup);
  Req.Add(isc_spb_dbname).AsString := DbPath('backup');
  Req.Add(isc_spb_bkp_file).AsString := FBK;
  Req.Add(isc_spb_verbose);
  Svc.Start(Req);
  StreamServiceOutput(Svc, '  gbak> ');

  { -- 4. gbak restore (replace) through the same service --------- }
  writeln;
  writeln('== restore: ', FBK, ' -> ', DB_REST, ' ==');
  Req := Svc.AllocateSRB;
  Req.Add(isc_action_svc_restore);
  Req.Add(isc_spb_bkp_file).AsString := FBK;
  Req.Add(isc_spb_dbname).AsString := DB_REST;
  Req.Add(isc_spb_options).SetAsInteger(isc_spb_res_replace);
  Req.Add(isc_spb_verbose);
  Svc.Start(Req);
  StreamServiceOutput(Svc, '  gbak> ');
  Svc.Detach;

  { -- 5. prove the restored copy has the data -------------------- }
  RAtt := FirebirdAPI.OpenDatabase('localhost:' + DB_REST, DefaultDPB);
  Tr := RAtt.StartTransaction([isc_tpb_read, isc_tpb_nowait,
    isc_tpb_concurrency], taCommit);
  writeln;
  writeln('restored database says: ',
    RAtt.OpenCursorAtStart(Tr, 'SELECT COUNT(*) FROM BR_ITEMS')[0].AsString,
    ' rows, max name = ',
    RAtt.OpenCursorAtStart(Tr, 'SELECT MAX(NAME) FROM BR_ITEMS')[0].AsString);
  Tr.Commit;
  writeln('(Services API native in fbintf - node-firebird reimplements it in JS;');
  writeln(' rsfbclient has no services attach and must shell out to gbak.)');
  writeln('done.');
end.
