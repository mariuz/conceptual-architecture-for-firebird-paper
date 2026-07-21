{
  replication.pas — the client-visible half of Firebird replication.

  The fbintf twin of ../cpp/replication.cpp: the publication.  All of it
  is plain DDL plus system tables — no replication.conf, no restart:

    ALTER DATABASE ENABLE PUBLICATION              -> RDB$PUBLICATIONS
    ALTER DATABASE INCLUDE TABLE ... / INCLUDE ALL -> RDB$PUBLICATION_TABLES

  The journal/segment transport behind it (Publisher -> ChangeLog ->
  Applier) needs server-side replication.conf and stays as text in the
  document's validated walk-through.  See ../../replication-architecture.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/replication && samples/fpc/bin/replication
}
program replication;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;

procedure ExecIgnore(const sql: AnsiString);
begin
  try
    A.ExecImmediate(Tr, sql);
  except on EIBInterBaseError do ;
  end;
end;

procedure PubState(const when: AnsiString);
var R: IResultSet;
    n: integer;
begin
  writeln('-- ', when);
  R := A.OpenCursor(Tr,
    'SELECT TRIM(RDB$PUBLICATION_NAME), RDB$ACTIVE_FLAG, RDB$AUTO_ENABLE' +
    '  FROM RDB$PUBLICATIONS');
  while R.FetchNext do
    writeln(Format('   publication %-14s active=%d  auto_enable=%d',
      [R[0].AsString, R[1].AsInteger, R[2].AsInteger]));
  R := A.OpenCursor(Tr,
    'SELECT TRIM(RDB$TABLE_SCHEMA_NAME), TRIM(RDB$TABLE_NAME)' +
    '  FROM RDB$PUBLICATION_TABLES ORDER BY RDB$TABLE_NAME');
  n := 0;
  while R.FetchNext do
  begin
    writeln('   published table: ', R[0].AsString, '.', R[1].AsString);
    inc(n);
  end;
  if n = 0 then
    writeln('   (no tables in the publication)');
  writeln;
end;

begin
  A := AttachOrCreate(DbConn('replication'));
  Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], taCommit);

  { Idempotent reset: back to a clean, unpublished state. }
  ExecIgnore('ALTER DATABASE EXCLUDE ALL FROM PUBLICATION');
  ExecIgnore('ALTER DATABASE DISABLE PUBLICATION');
  ExecIgnore('DROP TABLE REPL_ORDERS');
  ExecIgnore('DROP TABLE REPL_SCRATCH');
  A.ExecImmediate(Tr,
    'CREATE TABLE REPL_ORDERS (ID INT NOT NULL PRIMARY KEY, ITEM VARCHAR(30))');
  A.ExecImmediate(Tr, 'CREATE TABLE REPL_SCRATCH (N INT)');   { note: no key }
  Tr.CommitRetaining;

  PubState('initial state (publication exists but is inactive)');

  A.ExecImmediate(Tr, 'ALTER DATABASE ENABLE PUBLICATION');
  Tr.CommitRetaining;
  PubState('after ENABLE PUBLICATION');

  A.ExecImmediate(Tr, 'ALTER DATABASE INCLUDE TABLE REPL_ORDERS TO PUBLICATION');
  Tr.CommitRetaining;
  PubState('after INCLUDE TABLE REPL_ORDERS');

  A.ExecImmediate(Tr, 'ALTER DATABASE INCLUDE ALL TO PUBLICATION');
  Tr.CommitRetaining;
  PubState('after INCLUDE ALL (auto-enable: future tables join automatically)');

  { The monitoring view of the same facts, on this attachment. }
  writeln('MON$DATABASE.MON$REPLICA_MODE = ', A.OpenCursorAtStart(Tr,
    'SELECT MON$REPLICA_MODE FROM MON$DATABASE')[0].AsInteger,
    '   (0 = not a replica: this side publishes)');

  Tr.Commit;
  writeln;
  writeln('done.');
end.
