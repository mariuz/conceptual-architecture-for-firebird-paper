{
  api_styles.pas — the same query through both client-API bindings of fbintf.

  The fbintf twin of ../cpp/api_styles.cpp, companion to
  ../../client-apis-and-drivers.md.  The C++ sample runs one SELECT through
  fbclient's two C APIs: the legacy ISC API (flat isc_* functions, status
  vectors, XSQLDA) and the modern OO API (IMaster interfaces).  fbintf's
  angle on the same duality: it VENDORS both bindings — TFB25ClientAPI
  built on the isc_* entry points and TFB30ClientAPI built on IMaster —
  behind the one IFirebirdAPI interface, auto-selecting the OO API when
  fbclient provides it.  Here we force-load the legacy binding next to the
  default one and run the identical query through both: the same Pascal
  code drives XSQLDA descriptors in one half and IMessageMetadata in the
  other, against the same Y-valve in the same process.

  Build & run (see ../README.md):
      make -C samples/fpc bin/api_styles && samples/fpc/bin/api_styles
}
program api_styles;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBClientAPI, FB25ClientAPI, FBHandsOn;

const
  SQL = 'select rdb$get_context(''SYSTEM'', ''ENGINE_VERSION'') from rdb$database';

type
  {IB.pas picks TFB30ClientAPI whenever fbclient exports fb_get_master_interface.
   This subclass of the same loader refuses the OO API, so the selection logic
   falls back to the legacy isc_* binding — exactly what fbintf itself would do
   against a Firebird 2.5 client library.}
  TLegacyOnlyLibrary = class(TFBLibrary)
  protected
    function GetFirebird3API: IFirebirdAPI; override;
    function GetLegacyFirebirdAPI: IFirebirdAPI; override;
  end;

function TLegacyOnlyLibrary.GetFirebird3API: IFirebirdAPI;
begin
  Result := nil;                {decline the OO API}
end;

function TLegacyOnlyLibrary.GetLegacyFirebirdAPI: IFirebirdAPI;
begin
  Result := TFB25ClientAPI.Create(self);
end;

procedure RunQuery(const tag: AnsiString; api: IFirebirdAPI; const conn: AnsiString);
var DPB: IDPB;
    Att: IAttachment;
    Tr: ITransaction;
begin
  writeln(tag, ' GetImplementationVersion = ', api.GetImplementationVersion,
    ', OO master interface used = ', api.HasMasterIntf);
  DPB := api.AllocateDPB;                    {fbintf builds the DPB either way;}
  DPB.Add(isc_dpb_user_name).AsString := HandsOnUser;    {the legacy half emits}
  DPB.Add(isc_dpb_password).AsString := HandsOnPassword; {the hand-packed tag/}
  DPB.Add(isc_dpb_lc_ctype).AsString := 'UTF8';          {len/value bytes}
  Att := api.OpenDatabase(conn, DPB);
  Tr := Att.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln(tag, ' engine version = ',
    Att.OpenCursorAtStart(Tr, SQL)[0].AsString);
  Tr.Commit;
  Att.Disconnect;
end;

var legacyLib: IFirebirdLibrary;
    conn: AnsiString;

begin
  conn := 'localhost:employee';
  if ParamCount >= 1 then conn := ParamStr(1);

  writeln('fbintf ', FBIntf_Version,
    ': one Pascal API, both fbclient API styles underneath');
  writeln;

  {The ISC-API binding: isc_attach_database, isc_dsql_* and XSQLDA under the
   same Pascal interfaces.  It reports itself as "2.5" — the API level it
   models — although it drives the very same libfbclient as the OO half.}
  legacyLib := TLegacyOnlyLibrary.Create;
  RunQuery('[legacy ISC]', legacyLib.GetFirebirdAPI, conn);
  writeln;

  {The default binding: fbintf found fb_get_master_interface and went OO.}
  RunQuery('[OO API   ]', FirebirdAPI, conn);
  writeln;

  writeln('same engine, same Y-valve, two API styles behind one IFirebirdAPI. done.');
end.
