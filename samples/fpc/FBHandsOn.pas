{
  FBHandsOn.pas — shared defaults for the Free Pascal hands-on twins.

  These samples mirror the OO-API programs in ../cpp (and their fb-cpp,
  node-firebird and rsfbclient siblings) using MWA Software's Firebird
  Pascal API, fbintf (https://github.com/MWASoftware/fbintf, vendored at
  ../../extern/fbintf).  fbintf wraps the same client library the C++
  samples use behind COM-style Pascal interfaces — IAttachment,
  ITransaction, IStatement, IResultset — with reference counting doing
  the lifetime work RAII does in the C++ twins.

  Everything runs against the local server with scratch databases under
  /tmp/fbhandson (SYSDBA/masterkey, overridable via ISC_USER/ISC_PASSWORD).
}
unit FBHandsOn;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

interface

uses SysUtils, IB;

function HandsOnUser: AnsiString;
function HandsOnPassword: AnsiString;

{Server path of a topic's scratch database.}
function DbPath(const topic: AnsiString): AnsiString;

{Connection string (inet, localhost) for a topic's scratch database;
 argv[1] overrides the whole database path.}
function DbConn(const topic: AnsiString): AnsiString;

{Standard DPB: credentials + UTF8 connection charset.}
function DefaultDPB: IDPB;

{Attach to the database, creating it on first run.}
function AttachOrCreate(const connString: AnsiString): IAttachment;

implementation

function HandsOnUser: AnsiString;
begin
  Result := GetEnvironmentVariable('ISC_USER');
  if Result = '' then Result := 'SYSDBA';
end;

function HandsOnPassword: AnsiString;
begin
  Result := GetEnvironmentVariable('ISC_PASSWORD');
  if Result = '' then Result := 'masterkey';
end;

function DbPath(const topic: AnsiString): AnsiString;
begin
  if ParamCount >= 1 then
    Result := ParamStr(1)
  else
    Result := '/tmp/fbhandson/' + topic + '_fpc.fdb';
end;

function DbConn(const topic: AnsiString): AnsiString;
begin
  Result := 'localhost:' + DbPath(topic);
end;

function DefaultDPB: IDPB;
begin
  Result := FirebirdAPI.AllocateDPB;
  Result.Add(isc_dpb_user_name).setAsString(HandsOnUser);
  Result.Add(isc_dpb_password).setAsString(HandsOnPassword);
  Result.Add(isc_dpb_lc_ctype).setAsString('UTF8');
end;

function AttachOrCreate(const connString: AnsiString): IAttachment;
begin
  Result := FirebirdAPI.OpenDatabase(connString, DefaultDPB, false);
  if Result = nil then
    Result := FirebirdAPI.CreateDatabase(connString, DefaultDPB);
end;

end.
