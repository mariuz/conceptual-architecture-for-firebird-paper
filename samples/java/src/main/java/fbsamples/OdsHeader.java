//
// OdsHeader.java - the header page read three ways (Java twin of
// ../../../../../cpp/ods_header.cpp; see ../../../../../../on-disk-structure.md).
//
// The same acts: create a scratch database through the server, ask
// MON$DATABASE for the server's view, detach, then read page 0 straight
// from the file at the byte offsets src/jrd/ods.h pins with static_asserts,
// and finish with a page-type census (byte 0 of every page).  Between the
// two sits the database-info API, reached through Jaybird's own GDS-ng
// layer: FbDatabase.getDatabaseInfo() sends isc_info_* items and returns
// the raw clumplet buffer (item, 2-byte length, little-endian VAX value),
// decoded here with Jaybird's VaxEncoding - including the 64-bit
// transaction markers (items 104-107) and fb_info_db_guid.  The file act is
// a FileChannel + ByteBuffer.order(LITTLE_ENDIAN); no driver involved.
// It reads the server-owned file, so run it on the server machine with
// read access to it (a member of the firebird group).
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=OdsHeader
//
package fbsamples;

import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.rows;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.channels.FileChannel;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.nio.file.StandardOpenOption;
import java.sql.Connection;
import java.sql.SQLException;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.VaxEncoding;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class OdsHeader {

    private static final String[] PAGE_TYPES = {"undefined", "pag_header", "pag_pages (PIP)",
        "pag_transactions (TIP)", "pag_pointer", "pag_data", "pag_root",
        "pag_index (b-tree)", "pag_blob", "pag_ids (generators)", "pag_scns"};

    /** The info-API view: raw isc_database_info clumplets, decoded by hand. */
    private static void infoView(Connection con) throws SQLException {
        FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();
        byte[] items = {
            ISCConstants.isc_info_ods_version, ISCConstants.isc_info_ods_minor_version,
            ISCConstants.isc_info_page_size, ISCConstants.isc_info_allocation,
            (byte) ISCConstants.isc_info_oldest_transaction, (byte) ISCConstants.isc_info_oldest_active,
            (byte) ISCConstants.isc_info_oldest_snapshot, (byte) ISCConstants.isc_info_next_transaction,
            (byte) ISCConstants.fb_info_db_guid, ISCConstants.isc_info_end};
        byte[] buf = db.getDatabaseInfo(items, 512);
        StringBuilder sb = new StringBuilder("  ");
        int pos = 0;
        while (pos < buf.length && buf[pos] != ISCConstants.isc_info_end) {
            int item = buf[pos] & 0xFF;
            int len = VaxEncoding.iscVaxInteger(buf, pos + 1, 2);
            int at = pos + 3;
            String value = item == ISCConstants.fb_info_db_guid
                    ? new String(buf, at, len, StandardCharsets.US_ASCII)
                    : Long.toString(VaxEncoding.iscVaxLong(buf, at, len));
            String name = switch (item) {
                case ISCConstants.isc_info_ods_version -> "ods_version";
                case ISCConstants.isc_info_ods_minor_version -> "ods_minor";
                case ISCConstants.isc_info_page_size -> "page_size";
                case ISCConstants.isc_info_allocation -> "allocation";
                case ISCConstants.isc_info_oldest_transaction -> "oit";
                case ISCConstants.isc_info_oldest_active -> "oat";
                case ISCConstants.isc_info_oldest_snapshot -> "ost";
                case ISCConstants.isc_info_next_transaction -> "next";
                case ISCConstants.fb_info_db_guid -> "guid";
                default -> "item" + item;
            };
            sb.append(name).append('[').append(item).append(",").append(len).append("B]=").append(value);
            sb.append(item == ISCConstants.isc_info_allocation ? "\n  " : "  ");
            pos = at + len;
        }
        System.out.println(sb.toString().stripTrailing());
    }

    private static String guid(ByteBuffer h, int o) {
        // hdr_guid: Win32 GUID layout (Data1..3 little-endian, Data4 bytes)
        StringBuilder sb = new StringBuilder(String.format("{%08X-%04X-%04X-",
                h.getInt(o), h.getShort(o + 4), h.getShort(o + 6)));
        for (int i = 8; i < 16; i++) {
            sb.append(String.format("%02X", h.get(o + i)));
            if (i == 9) {
                sb.append('-');
            }
        }
        return sb.append('}').toString();
    }

    private static void fileView(Path file) throws IOException {
        try (FileChannel ch = FileChannel.open(file, StandardOpenOption.READ)) {
            ByteBuffer h = ByteBuffer.allocate(152).order(ByteOrder.LITTLE_ENDIAN);  // sizeof(Ods::header_page)
            ch.read(h, 0);
            int type = h.get(0);
            int pageSize = Short.toUnsignedInt(h.getShort(16));
            int ods = Short.toUnsignedInt(h.getShort(18));
            int flags = Short.toUnsignedInt(h.getShort(22));
            System.out.println("pag_type      @0   = " + type + " (" + PAGE_TYPES[type] + ")");
            System.out.println("pag_flags     @1   = " + h.get(1));
            System.out.println("hdr_page_size @16  = " + pageSize);
            System.out.printf("hdr_ods_version @18 = 0x%04x -> ODS %d (FIREBIRD flag 0x8000 %s), minor @20 = %d%n",
                    ods, ods & 0x7fff, (ods & 0x8000) != 0 ? "set" : "clear", h.getShort(20));
            System.out.printf("hdr_flags     @22  = 0x%02x (%s%s%s)%n", flags,
                    (flags & 0x2) != 0 ? "force_write " : "", (flags & 0x8) != 0 ? "no_reserve " : "",
                    (flags & 0x10) != 0 ? "SQL_dialect_3" : "");
            System.out.println("hdr_PAGES     @28  = " + Integer.toUnsignedLong(h.getInt(28))
                    + "   <- pointer page of RDB$PAGES (catalog bootstrap anchor)");
            System.out.println("hdr_next_transaction   @40 = " + h.getLong(40));
            System.out.println("hdr_oldest_transaction @48 = " + h.getLong(48) + " (OIT)");
            System.out.println("hdr_oldest_active      @56 = " + h.getLong(56) + " (OAT)");
            System.out.println("hdr_oldest_snapshot    @64 = " + h.getLong(64) + " (OST)");
            System.out.println("hdr_guid      @84  = " + guid(h, 84));

            // Page-type census: byte 0 of every page.
            long pages = ch.size() / pageSize;
            int[] counts = new int[PAGE_TYPES.length];
            ByteBuffer one = ByteBuffer.allocate(1);
            for (long p = 0; p < pages; p++) {
                one.clear();
                ch.read(one, p * pageSize);
                int t = one.get(0);
                counts[t >= 0 && t < counts.length ? t : 0]++;
            }
            System.out.println();
            System.out.println("-- page-type census: " + pages + " pages of " + pageSize + " bytes --");
            for (int t = 0; t < counts.length; t++) {
                if (counts[t] > 0) {
                    System.out.printf("  type %2d  %-22s %5d%n", t, PAGE_TYPES[t], counts[t]);
                }
            }
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("ods", args);
            try (Connection con = attachOrCreate(path)) {
                System.out.println("-- server's view (MON$DATABASE) --");
                Object[] r = rows(con, "select mon$page_size, mon$ods_major, mon$ods_minor,"
                        + " mon$oldest_transaction, mon$oldest_active, mon$oldest_snapshot,"
                        + " mon$next_transaction from mon$database").get(0);
                System.out.printf("page_size ods_major ods_minor oit oat ost next = %s %s %s %s %s %s %s%n",
                        r[0], r[1], r[2], r[3], r[4], r[5], r[6]);

                System.out.println();
                System.out.println("-- the same through the info API (FbDatabase.getDatabaseInfo) --");
                infoView(con);
            }

            System.out.println();
            System.out.println("-- header page, parsed from " + path + " (offsets per ods.h) --");
            fileView(Path.of(path));
            System.out.println("done.");
        });
    }

    private OdsHeader() {
    }
}
