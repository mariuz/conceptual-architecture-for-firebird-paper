{
  protocol.pas — the negotiated wire session, reported by the engine.

  The fbintf twin of ../fb-cpp/protocol.cpp: attach over inet and let the
  client library run the whole op_connect / Srp256 / op_crypt handshake,
  then ask the engine what was actually negotiated.  fbintf has no wire
  implementation of its own — unlike node-firebird and rsfbclient-rust,
  which re-implement the protocol in JavaScript/Rust and negotiate older
  versions and Arc4 — it auto-selects the Firebird 3+ OO client API and
  drives libfbclient, the very library the C++ twins link.  So the
  MON$ATTACHMENTS row here shows the SAME session the C++ clients get:
  the newest protocol version, Srp256, ChaCha64.
  See ../../firebird-wire-protocol.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/protocol && samples/fpc/bin/protocol
}
program protocol;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    R: IResultSet;

function S(d: ISQLData): AnsiString;
begin
  if d.IsNull then Result := '(none)' else Result := d.AsString;
end;

begin
  { The duality behind one attach call: fbintf ships bindings for both the
    legacy 2.5 API and the 3.0+ OO API and picks at load time.
    GetImplementationVersion reports the loaded client library's version -
    3.0 or later means the OO API (IProvider/IAttachment) is in charge. }
  writeln('fbintf ', FBIntf_Version, ' loaded libfbclient ',
    FirebirdAPI.GetImplementationVersion,
    ' -> Firebird 3+ OO API selected (HasMasterIntf = ',
    FirebirdAPI.HasMasterIntf, ')');

  A := AttachOrCreate(DbConn('protocol'));
  writeln('attached to ', DbConn('protocol'));
  writeln;

  { Ask the ENGINE (not the driver) what this attachment negotiated. }
  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_read_committed,
    isc_tpb_rec_version], taCommit);
  R := A.OpenCursorAtStart(Tr,
    'SELECT RDB$GET_CONTEXT(''SYSTEM'', ''ENGINE_VERSION''), TRIM(CURRENT_USER),' +
    '       MON$AUTH_METHOD, MON$REMOTE_VERSION, MON$WIRE_CRYPT_PLUGIN,' +
    '       MON$REMOTE_PROTOCOL, MON$CLIENT_VERSION' +
    '  FROM MON$ATTACHMENTS WHERE MON$ATTACHMENT_ID = CURRENT_CONNECTION');
  writeln('MON$ATTACHMENTS, as the server recorded the handshake:');
  writeln('   engine version : ', S(R[0]));
  writeln('   authenticated  : ', S(R[1]));
  writeln('   auth method    : ', S(R[2]), '   <- layer 1: SRP proof, password never sent');
  writeln('   wire protocol  : ', S(R[3]), '   <- highest version both sides speak');
  writeln('   wire crypt     : ', S(R[4]), '   <- layer 2: from the session key SRP left behind');
  writeln('   network        : ', S(R[5]));
  writeln('   client version : ', S(R[6]));
  Tr.Commit;
  writeln;

  { fbintf caches the same facts client-side on the attachment interface. }
  writeln('the same facts from IAttachment, without a query:');
  writeln('   GetRemoteProtocol       : ', A.GetRemoteProtocol);
  writeln('   GetAuthenticationMethod : ', A.GetAuthenticationMethod);
  writeln('   GetSecurityDatabase     : ', A.GetSecurityDatabase);
  writeln('   GetODSMajorVersion      : ', A.GetODSMajorVersion);
  writeln;

  writeln('note: the JS and Rust twins re-implement this handshake themselves and');
  writeln('negotiate older protocol versions and Arc4; fbintf rides libfbclient,');
  writeln('so its wire session is identical to the C++ clients'' - see above.');

  A.Disconnect;
  writeln('detached. done.');
end.
