{
  transactions.pas — the transactions-and-concurrency scenario in Pascal.

  The fbintf twin of ../cpp/transactions_demo.cpp: two attachments,
  SNAPSHOT vs READ COMMITTED visibility of a concurrent commit, then a
  NO WAIT update conflict.  The TPB is the instructive middle ground
  between the raw byte array the OO-API sample builds and fb-cpp's
  builder methods: StartTransaction takes the same isc_tpb_* constants,
  but as a Pascal open array — [isc_tpb_write, isc_tpb_nowait,
  isc_tpb_concurrency] — with reference-counted ITransaction interfaces
  doing the lifetime work.  The conflict surfaces as an EIBInterBaseError
  exception carrying the same isc_deadlock / isc_update_conflict codes.
  See ../../transactions-and-concurrency.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/transactions && samples/fpc/bin/transactions
}
program transactions;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A, B: IAttachment;
    TrA, TrB: ITransaction;
    R: IResultset;

function Balance(att: IAttachment; tr: ITransaction): integer;
begin
  Result := att.OpenCursorAtStart(tr,
    'SELECT BALANCE FROM ACCOUNTS WHERE ID = 1')[0].AsInteger;
end;

begin
  A := AttachOrCreate(DbConn('transactions'));
  B := FirebirdAPI.OpenDatabase(DbConn('transactions'), DefaultDPB);

  { Scratch table (idempotent). }
  try
    A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
      isc_tpb_rec_version], 'DROP TABLE ACCOUNTS');
  except on EIBInterBaseError do ;
  end;
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'CREATE TABLE ACCOUNTS (ID INT NOT NULL PRIMARY KEY, BALANCE INT)');
  A.ExecImmediate([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], 'INSERT INTO ACCOUNTS VALUES (1, 100)');

  { -- 1. Visibility: SNAPSHOT cannot see a commit made after it starts;
          READ COMMITTED can. }
  TrA := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency], taCommit);
  writeln('snapshot sees balance ', Balance(A, TrA), ' at start');

  TrB := B.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], taCommit);
  B.ExecuteSQL(TrB, 'UPDATE ACCOUNTS SET BALANCE = 150 WHERE ID = 1', []);
  TrB.Commit;
  writeln('B committed BALANCE = 150');

  writeln('snapshot still sees ', Balance(A, TrA), '   <- stable view');
  TrA.Commit;

  TrA := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], taCommit);
  writeln('read committed sees ', Balance(A, TrA), '   <- follows commits');
  TrA.Commit;

  { -- 2. Conflict: two NO WAIT updates of the same row. }
  TrA := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency], taCommit);
  TrB := B.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency], taRollback);
  A.ExecuteSQL(TrA, 'UPDATE ACCOUNTS SET BALANCE = BALANCE + 1 WHERE ID = 1', []);
  writeln('A updated the row (uncommitted)');
  try
    B.ExecuteSQL(TrB, 'UPDATE ACCOUNTS SET BALANCE = BALANCE + 2 WHERE ID = 1', []);
    writeln('BUG: conflicting update succeeded');
  except
    on E: EIBInterBaseError do
      writeln('B''s conflicting update failed as designed:'#10'    gds ',
        E.IBErrorCode, ': ', E.Message);
  end;
  TrB.Rollback;
  TrA.Commit;

  writeln('done.');
end.
