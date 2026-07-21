{
  ods_header.pas — the on-disk structure, read two ways.

  The fbintf twin of ../cpp/ods_header.cpp: creates a scratch database
  through the server, asks MON$DATABASE for the server's own view of the
  numbers, then detaches, opens the database FILE with a TFileStream and
  parses the header page (page 0) raw bytes at the offsets src/jrd/ods.h
  pins with static_asserts: page type, page size, ODS version, the
  hdr_PAGES bootstrap anchor and the four transaction markers
  (Next/OIT/OAT/OST).  Then a page-type census — byte 0 of every page —
  shows "one file, many pages".

  A third view the node and rust twins could not reach: fbintf wraps the
  isc_database_info call as IAttachment.GetDBInformation (typed
  IDBInfoItem results) plus the GetODSMajorVersion/GetODSMinorVersion
  conveniences, so the same facts also arrive through the client API
  without any SQL.  Run it on the machine the server runs on (the server
  writes /tmp/fbhandson/ods_header_fpc.fdb; we read it).
  See ../../on-disk-structure.md.

  Build & run (see ../README.md):
      make -C samples/fpc bin/ods_header && samples/fpc/bin/ods_header
}
program ods_header;

{$mode delphi}
{$codepage UTF8}
{$interfaces COM}

uses SysUtils, Classes, IB, FBHandsOn;

var A: IAttachment;
    Tr: ITransaction;
    R: IResultset;
    Info: IDBInformation;
    F: TFileStream;
    H: array[0..151] of byte;      { sizeof(Ods::header_page) }
    pageSize: cardinal;
    odsRaw, flags: word;
    counts: array[0..10] of cardinal;
    pageNo: int64;
    i, t: integer;
    b: byte;

function U16(o: integer): word;
begin
  Move(H[o], Result, 2);
end;

function U32(o: integer): cardinal;
begin
  Move(H[o], Result, 4);
end;

function U64(o: integer): uint64;
begin
  Move(H[o], Result, 8);
end;

function PageTypeName(pt: integer): AnsiString;
const names: array[0..10] of AnsiString = ('undefined', 'pag_header',
    'pag_pages (PIP)', 'pag_transactions (TIP)', 'pag_pointer', 'pag_data',
    'pag_root', 'pag_index (b-tree)', 'pag_blob', 'pag_ids (generators)',
    'pag_scns');
begin
  if (pt >= 0) and (pt <= 10) then Result := names[pt]
  else Result := '???';
end;

begin
  { 1. Create (or reuse) the scratch database and generate a little
       transaction history so the TIP markers move off their floor. }
  A := AttachOrCreate(DbConn('ods_header'));
  for i := 1 to 3 do
  begin
    Tr := A.StartTransaction([isc_tpb_write, isc_tpb_nowait, isc_tpb_concurrency],
      taCommit);
    A.OpenCursorAtStart(Tr, 'select 1 from rdb$database');
    Tr.Commit;
  end;

  Tr := A.StartTransaction([isc_tpb_read, isc_tpb_nowait, isc_tpb_concurrency],
    taCommit);
  writeln('-- server''s view (MON$DATABASE) --');
  R := A.OpenCursorAtStart(Tr,
    'select mon$page_size, mon$ods_major, mon$ods_minor,' +
    '       mon$oldest_transaction, mon$oldest_active,' +
    '       mon$oldest_snapshot, mon$next_transaction ' +
    'from mon$database');
  for i := 0 to R.getCount - 1 do
    writeln(Format('  %-24s = %s', [R[i].Name, R[i].AsString]));
  Tr.Commit;

  { 1b. The same facts through the client info API — fbintf wraps
       isc_database_info; no SQL involved. }
  writeln;
  writeln('-- client API view (GetODSMajorVersion / GetDBInformation) --');
  writeln('  GetODSMajorVersion       = ', A.GetODSMajorVersion,
    ', GetODSMinorVersion = ', A.GetODSMinorVersion);
  Info := A.GetDBInformation([isc_info_page_size, isc_info_allocation]);
  writeln('  isc_info_page_size       = ', Info.Find(isc_info_page_size).AsInteger,
    ', isc_info_allocation = ', Info.Find(isc_info_allocation).AsInteger, ' pages');
  { Honest gap: fbintf's typed reply parser has no mapping for the 64-bit
    transaction-marker tags (isc_info_oldest_transaction..next, 104..107) -
    they fall into its untyped "special item" bucket where getAsInteger
    refuses and getRawBytes returns a truncated clumplet.  The markers are
    covered by MON$DATABASE above and by the raw header bytes below. }
  writeln('  (fbintf gap: info tags 104..107 - the OIT/OAT/OST/next markers -');
  writeln('   have no typed decoding; MON$DATABASE and the disk header cover them)');

  A.Disconnect;
  A := nil;

  { 2. Now the same facts straight from the bytes on disk. }
  F := TFileStream.Create(DbPath('ods_header'), fmOpenRead or fmShareDenyNone);
  F.ReadBuffer(H, SizeOf(H));

  writeln;
  writeln('-- header page, parsed from ', DbPath('ods_header'),
    ' (offsets per ods.h) --');
  writeln(Format('pag_type      @0   = %d (%s)', [H[0], PageTypeName(H[0])]));
  writeln(Format('pag_flags     @1   = %d', [H[1]]));

  pageSize := U16(16);
  odsRaw := U16(18);
  writeln(Format('hdr_page_size @16  = %d', [pageSize]));
  writeln(Format('hdr_ods_version @18 = 0x%.4x -> ODS %d (FIREBIRD flag 0x8000 %s), minor @20 = %d',
    [odsRaw, odsRaw and $7fff,
     BoolToStr(odsRaw and $8000 <> 0, 'set', 'clear'), U16(20)]));

  flags := U16(22);
  writeln(Format('hdr_flags     @22  = 0x%.2x (%s%s%s)', [flags,
    BoolToStr(flags and $2 <> 0, 'force_write ', ''),
    BoolToStr(flags and $8 <> 0, 'no_reserve ', ''),
    BoolToStr(flags and $10 <> 0, 'SQL_dialect_3', '')]));
  writeln(Format('hdr_PAGES     @28  = %d   <- pointer page of RDB$PAGES (catalog bootstrap anchor)',
    [U32(28)]));
  writeln('hdr_next_transaction   @40 = ', U64(40));
  writeln('hdr_oldest_transaction @48 = ', U64(48), ' (OIT)');
  writeln('hdr_oldest_active      @56 = ', U64(56), ' (OAT)');
  writeln('hdr_oldest_snapshot    @64 = ', U64(64), ' (OST)');

  { hdr_guid: Win32 GUID layout }
  writeln(Format('hdr_guid      @84  = {%.8X-%.4X-%.4X-%.2X%.2X-%.2X%.2X%.2X%.2X%.2X%.2X}',
    [U32(84), U16(88), U16(90), H[92], H[93], H[94], H[95], H[96], H[97],
     H[98], H[99]]));

  { 3. Page-type census: byte 0 of every page in the file. }
  FillChar(counts, SizeOf(counts), 0);
  pageNo := 0;
  while pageNo * pageSize < F.Size do
  begin
    F.Position := pageNo * pageSize;
    if F.Read(b, 1) <> 1 then break;
    if b <= 10 then Inc(counts[b]) else Inc(counts[0]);
    Inc(pageNo);
  end;
  F.Free;

  writeln;
  writeln(Format('-- page-type census: %d pages of %d bytes --',
    [pageNo, pageSize]));
  for t := 1 to 10 do
    if counts[t] > 0 then
      writeln(Format('  type %2d  %-22s %5d', [t, PageTypeName(t), counts[t]]));
  writeln('done.');
end.
