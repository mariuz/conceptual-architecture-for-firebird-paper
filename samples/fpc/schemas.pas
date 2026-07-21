{
  schemas.pas — schemas and name resolution, replayed through fbintf.

  The fbintf twin of ../cpp/schemas.cpp:

   1. RDB$SCHEMAS and the default search path ("PUBLIC", "SYSTEM").
   2. Two same-named tables (PUBLIC.CUSTOMERS / APP.CUSTOMERS); the same
      unqualified SELECT resolves differently as SET SEARCH_PATH changes.
   3. SYSTEM is auto-appended when omitted from SET SEARCH_PATH.
   4. A stored procedure created while APP leads the path binds
      APP.CUSTOMERS — and keeps meaning that after the session's path
      flips to PUBLIC (stored code resolves in its OWN schema, never the
      caller's path).  RDB$DEPENDENCIES records the resolution.
   5. IStatement.GetPlan shows the schema-qualified plan — fbintf exposes
      the plan directly, no info-buffer decoding needed.

  See ../../schemas-and-name-resolution.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/schemas && samples/fpc/bin/schemas
}
program schemas;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

const CleanupDDL: array[0..3] of AnsiString = (
  'DROP PROCEDURE APP.WHICH_ONE',
  'DROP TABLE PUBLIC.CUSTOMERS',
  'DROP TABLE APP.CUSTOMERS',
  'DROP SCHEMA APP');

var A: IAttachment;
    Tr: ITransaction;
    Stmt: IStatement;
    R: IResultSet;
    i: integer;

function NewTr: ITransaction;
begin
  Result := A.StartTransaction([isc_tpb_write, isc_tpb_nowait,
    isc_tpb_read_committed, isc_tpb_rec_version], taCommit);
end;

function Value(const sql: AnsiString): AnsiString;
begin
  Result := A.OpenCursorAtStart(Tr, sql)[0].AsString;
end;

function CtxPath: AnsiString;
begin
  Result := Value('SELECT RDB$GET_CONTEXT(''SYSTEM'',''SEARCH_PATH'') FROM RDB$DATABASE');
end;

begin
  A := AttachOrCreate(DbConn('schemas'));

  { -- Idempotent cleanup + setup. }
  Tr := NewTr;
  for i := low(CleanupDDL) to high(CleanupDDL) do
    try
      A.ExecImmediate(Tr, CleanupDDL[i]);
    except on EIBInterBaseError do ;
    end;
  A.ExecImmediate(Tr, 'CREATE SCHEMA APP');
  A.ExecImmediate(Tr, 'CREATE TABLE PUBLIC.CUSTOMERS (ID INT, ORIGIN VARCHAR(20))');
  A.ExecImmediate(Tr, 'CREATE TABLE APP.CUSTOMERS    (ID INT, ORIGIN VARCHAR(20))');
  Tr.Commit;

  { -- 1. The catalog and the default path. }
  Tr := NewTr;
  A.ExecuteSQL(Tr, 'INSERT INTO PUBLIC.CUSTOMERS VALUES (1, ''from PUBLIC'')', []);
  A.ExecuteSQL(Tr, 'INSERT INTO APP.CUSTOMERS    VALUES (2, ''from APP'')', []);
  write('schemas in RDB$SCHEMAS      : ');
  R := A.OpenCursor(Tr, 'SELECT TRIM(RDB$SCHEMA_NAME) FROM RDB$SCHEMAS ORDER BY 1');
  while R.FetchNext do
    write(R[0].AsString, '  ');
  writeln;
  writeln('default search path         : ', CtxPath);

  { -- 2. Same statement, two resolutions. }
  writeln;
  writeln('SELECT ORIGIN FROM CUSTOMERS, as the path changes:');
  writeln('  path PUBLIC,SYSTEM        -> ', Value('SELECT ORIGIN FROM CUSTOMERS'));
  A.ExecuteSQL(Tr, 'SET SEARCH_PATH TO APP, PUBLIC', []);
  writeln('  path APP,PUBLIC           -> ', Value('SELECT ORIGIN FROM CUSTOMERS'));

  { -- 3. SYSTEM can be moved but not removed. }
  A.ExecuteSQL(Tr, 'SET SEARCH_PATH TO APP', []);
  writeln;
  writeln('SET SEARCH_PATH TO APP      -> ', CtxPath, '   (SYSTEM auto-appended)');

  { -- 4. Stored code binds its own schema, not the caller's path. }
  A.ExecuteSQL(Tr, 'SET SEARCH_PATH TO APP, PUBLIC', []);
  A.ExecImmediate(Tr,
    'CREATE PROCEDURE WHICH_ONE RETURNS (SRC VARCHAR(20)) AS ' +
    'BEGIN SELECT ORIGIN FROM CUSTOMERS INTO :SRC; SUSPEND; END');
  Tr.Commit;

  Tr := NewTr;
  writeln;
  writeln('procedure created with path APP,PUBLIC (lands in APP, binds APP.CUSTOMERS)');
  A.ExecuteSQL(Tr, 'SET SEARCH_PATH TO PUBLIC', []);
  writeln('  after SET SEARCH_PATH TO PUBLIC:');
  writeln('    direct SELECT ... FROM CUSTOMERS -> ',
    Value('SELECT ORIGIN FROM CUSTOMERS'));
  writeln('    SELECT SRC FROM APP.WHICH_ONE    -> ',
    Value('SELECT SRC FROM APP.WHICH_ONE'), '   <- unmoved');
  writeln('    RDB$DEPENDENCIES records         -> ',
    Value('SELECT TRIM(RDB$DEPENDED_ON_SCHEMA_NAME) || ''.'' || TRIM(RDB$DEPENDED_ON_NAME)' +
      ' FROM RDB$DEPENDENCIES WHERE RDB$DEPENDENT_NAME = ''WHICH_ONE'''));

  { -- 5. Plans are schema-qualified. }
  Stmt := A.Prepare(Tr, 'SELECT COUNT(*) FROM CUSTOMERS');
  writeln;
  writeln('plan for unqualified SELECT :', Stmt.GetPlan);

  Tr.Commit;
  writeln;
  writeln('done.');
end.
