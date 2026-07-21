{
  security.pas — the security layers observed from client code.

  The fbintf twin of ../cpp/security.cpp.  In one run:
    1. WHO AM I    — the attachment's own MON$ATTACHMENTS row: auth plugin
                     (Srp256) and wire-crypt plugin (ChaCha64), i.e. layers
                     1 and 2 as the server recorded them.
    2. USERS       — SEC$USERS, the virtual view over the security database.
    3. LEAST PRIVILEGE — a temporary user plus a role carrying the
                     MONITOR_ANY_ATTACHMENT system privilege; connecting
                     with and without the role (isc_dpb_sql_role_name in
                     the DPB) changes how much of MON$ATTACHMENTS the same
                     user can see.
    4. FAILED LOGIN — the error chain a wrong password produces, as
                     EIBInterBaseError with its gds code.
  All DDL happens on a scratch database / the security database; the demo
  cleans up its user and role at the end.  See ../../security-architecture.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/security && samples/fpc/bin/security
}
program security;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const TmpUser = 'HANDSON_USER';
      TmpPass = 'Hands0nPw';

var Admin, Plain, Monitor, Bad: IAttachment;
    Tr, T2: ITransaction;
    R: IResultSet;

function NewTr(att: IAttachment): ITransaction;
begin
  Result := att.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
end;

{Run one statement in its own transaction, ignoring any error — needed for
 idempotent cleanup, because DROP USER reports "record not found" only at
 COMMIT (user management is deferred work, executed at commit time).}
procedure ExecIgnore(const sql: AnsiString);
var t: ITransaction;
begin
  t := NewTr(Admin);
  try
    Admin.ExecImmediate(t, sql);
    t.Commit;
  except on EIBInterBaseError do
    if t.InTransaction then t.Rollback;
  end;
end;

{One DDL batch, own transaction, FULL commit: user management is deferred
 work performed at commit, and continuing after COMMIT RETAINING of
 CREATE USER is not safe on this server.}
procedure ExecCommit(const sql: array of AnsiString);
var t: ITransaction;
    i: integer;
begin
  t := NewTr(Admin);
  for i := low(sql) to high(sql) do
    Admin.ExecImmediate(t, sql[i]);
  t.Commit;
end;

procedure WhoAmI(att: IAttachment; tr: ITransaction; const aLabel: AnsiString);
var r: IResultSet;
begin
  r := att.OpenCursorAtStart(tr,
    'select trim(mon$user), mon$auth_method, mon$wire_crypt_plugin,' +
    '       mon$remote_protocol, trim(coalesce(current_role, ''NONE''))' +
    '  from mon$attachments where mon$attachment_id = current_connection');
  writeln(Format('%-22s user=%s auth=%s wirecrypt=%s protocol=%s role=%s',
    [aLabel, r[0].AsString, r[1].AsString, r[2].AsString, r[3].AsString,
     r[4].AsString]));
end;

function VisibleAttachments(att: IAttachment; tr: ITransaction): integer;
begin
  Result := att.OpenCursorAtStart(tr,
    'select count(*) from mon$attachments where mon$system_flag = 0')[0].AsInteger;
end;

function UserDPB(const u, p, role: AnsiString): IDPB;
begin
  Result := FirebirdAPI.AllocateDPB;
  Result.Add(isc_dpb_user_name).AsString := u;
  Result.Add(isc_dpb_password).AsString := p;
  Result.Add(isc_dpb_lc_ctype).AsString := 'UTF8';
  if role <> '' then
    Result.Add(isc_dpb_sql_role_name).AsString := role;
end;

begin
  Admin := AttachOrCreate(DbConn('security'));

  { 1. Layers 1+2, as recorded for THIS attachment. }
  Tr := NewTr(Admin);
  WhoAmI(Admin, Tr, 'admin attachment:');
  Tr.Commit;

  { 2+3. A temporary user (security database) and a privileged role
    (this database).  Cleanup first, in case a prior run died. }
  ExecIgnore('drop user ' + TmpUser + ' using plugin Srp');
  ExecIgnore('drop role HANDSON_MONITOR');
  ExecCommit(['create user ' + TmpUser + ' password ''' + TmpPass +
    ''' using plugin Srp']);          { <- the user exists only now }
  ExecCommit([
    'create role HANDSON_MONITOR set system privileges to MONITOR_ANY_ATTACHMENT',
    'grant HANDSON_MONITOR to user ' + TmpUser]);

  Tr := NewTr(Admin);
  writeln;
  writeln('SEC$USERS (the security database, through the virtual view):');
  writeln(Format('    %-16s %-8s %s', ['USER', 'PLUGIN', 'ADMIN']));
  R := Admin.OpenCursor(Tr,
    'select trim(sec$user_name), trim(sec$plugin), sec$admin' +
    '  from sec$users order by 1');
  while R.FetchNext do
    writeln(Format('    %-16s %-8s %s',
      [R[0].AsString, R[1].AsString, R[2].AsString]));

  { 3. Same user, without and with the role: the system privilege decides
    how much of MON$ is visible.  (Admin stays attached, so a fully
    privileged viewer sees at least 2 attachments.) }
  writeln;
  writeln('admin sees ', VisibleAttachments(Admin, Tr),
    ' user attachments in MON$ATTACHMENTS');

  Plain := FirebirdAPI.OpenDatabase(DbConn('security'),
    UserDPB(TmpUser, TmpPass, ''));
  T2 := NewTr(Plain);
  WhoAmI(Plain, T2, 'user, no role:');
  writeln('  -> sees ', VisibleAttachments(Plain, T2),
    ' attachment(s): only its own');
  T2.Commit;
  Plain.Disconnect;

  Monitor := FirebirdAPI.OpenDatabase(DbConn('security'),
    UserDPB(TmpUser, TmpPass, 'HANDSON_MONITOR'));
  T2 := NewTr(Monitor);
  WhoAmI(Monitor, T2, 'user + role:');
  writeln('  -> sees ', VisibleAttachments(Monitor, T2),
    ' attachments: MONITOR_ANY_ATTACHMENT at work');
  T2.Commit;
  Monitor.Disconnect;

  { 4. The failed login, and its exact error chain. }
  writeln;
  writeln('failed login (wrong password) produces:');
  try
    Bad := FirebirdAPI.OpenDatabase(DbConn('security'),
      UserDPB(TmpUser, 'wrong-password', ''));
    writeln('BUG: login succeeded');
  except
    on E: EIBInterBaseError do
      writeln('    gds ', E.IBErrorCode, ': ', E.Message);
  end;

  Tr.Commit;

  { Cleanup, again one transaction per DDL batch with a full commit. }
  ExecCommit(['drop user ' + TmpUser + ' using plugin Srp',
              'drop role HANDSON_MONITOR']);
  writeln;
  writeln('temporary user and role dropped. done.');
end.
