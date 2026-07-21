{
  parser_errors.pas — driving Firebird's SQL parser from Pascal.

  The fbintf twin of ../cpp/parser_errors.cpp: feeds IAttachment.Prepare
    1. a dynamic-SQL statement with a `?` placeholder — the parser turns
       it into a typed input parameter (the type comes from the column);
    2. two statements only a backtracking grammar accepts comfortably —
       FIRST as the FIRST-clause keyword and FIRST as a plain column name;
    3. syntax errors — the lexer/parser reports the offending token with
       its exact line and column ("Token unknown - line N, column M");
    4. a semantic error — position tracking survives past the parse into
       the DSQL pass ("Column unknown ... At line N, column M").

  fbintf surfaces the whole story: the statement's parsed shape through
  IStatement (SQLStatementType, SQLParams, MetaData), and a failed prepare
  as EIBInterBaseError whose Message is the formatted status vector and
  whose Status.CheckStatusVector([...]) still lets code test for the
  specific gds codes in their vector positions (the numeric line/column
  arguments themselves are only exposed pre-formatted into the text).
  Read-only against the stock employee database.
  See ../../grammar-and-parser.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/parser_errors && samples/fpc/bin/parser_errors
}
program parser_errors;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, IBErrorCodes, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    database: AnsiString;

procedure TryPrepare(const sql: AnsiString);
var S: IStatement;
    i: integer;
begin
  writeln('---- ', sql);
  try
    S := A.Prepare(Tr, sql);
    writeln('  parsed OK: type=', S.GetSQLStatementTypeName,
      ', input params=', S.SQLParams.Count,
      ', output columns=', S.MetaData.Count);
    for i := 0 to S.SQLParams.Count - 1 do
      writeln('    param ', i, ': ', S.SQLParams[i].GetSQLTypeName,
        ', length=', S.SQLParams[i].GetSize);
  except
    on E: EIBInterBaseError do
    begin
      writeln('  prepare failed, gds ', E.IBErrorCode, ':');
      writeln('    ', StringReplace(E.Message, #10, #10'    ', [rfReplaceAll]));
      if E.Status.CheckStatusVector([isc_dsql_token_unk_err]) then
        writeln('    (status vector carries isc_dsql_token_unk_err ',
          'with the line/column as its arguments)');
      if E.Status.CheckStatusVector([isc_dsql_field_err]) then
        writeln('    (status vector carries isc_dsql_field_err - a semantic, ',
          'not syntax, error)');
    end;
  end;
end;

begin
  if ParamCount >= 1 then database := ParamStr(1)
  else database := 'localhost:employee';

  A := FirebirdAPI.OpenDatabase(database, DefaultDPB);
  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);

  { 1. Dynamic SQL: the `?` becomes a typed parameter. }
  TryPrepare('SELECT first_name FROM employee WHERE emp_no = ?');

  { 2. One token, two grammatical roles: FIRST as row-limit clause... }
  TryPrepare('SELECT FIRST 1 emp_no FROM employee');
  { ...and FIRST as an ordinary identifier (non-reserved keyword). }
  TryPrepare('SELECT first FROM (SELECT 1 AS first FROM rdb$database)');

  { 3. Syntax errors with token position. }
  TryPrepare('SELEC 1 FROM rdb$database');
  TryPrepare('SELECT emp_no'#10'FROM employee'#10'WHERE ORDER BY 1');

  { 4. Semantic error — still carries line/column. }
  TryPrepare('SELECT frst_name'#10'FROM employee');

  Tr.Commit;
  writeln('done.');
end.
