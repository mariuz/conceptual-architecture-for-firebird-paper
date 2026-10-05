//
// Blobs.java - segmented and stream blobs, both API levels (Java twin of
// ../../../../../cpp/blobs.cpp; see ../../../../../../blob-handling.md).
//
// The C++ scenario on Jaybird's two layers:
//   1. on the GDS-ng layer (FbDatabase.createBlobForOutput, FbBlob
//      .putSegment) - three explicit segments, the row receiving only the
//      8-byte blob id through an FbStatement parameter;
//   2. read back with FbBlob.getSegment(64) - and here the pure-Java
//      driver differs: op_get_segment asks the server to fill a 64-byte
//      buffer, the server packs as many length-prefixed segments as fit,
//      and Jaybird strips the prefixes, so all three segments arrive as
//      ONE 40-byte chunk (libfbclient unpacks that buffer one segment per
//      call, which is why the C++ twin sees the boundaries).  The blob
//      still knows them: FbBlob.getBlobInfo() returns the same
//      isc_info_blob_* clumplets the C++ sample decodes - 3 segments;
//   3. the JDBC way - Connection.createBlob() and three writes to its
//      setBinaryStream(): Jaybird creates STREAM blobs by default
//      (useStreamBlobs=true), so getBlobInfo reports type 1, and only a
//      stream blob can seek: FirebirdBlob.BlobInputStream.seek() jumps
//      into the middle without reading the start;
//   4. subtype text vs binary from the catalog;
//   5. BLOB_APPEND assembling a blob in SQL.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Blobs
//
package fbsamples;

import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;

import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.sql.Blob;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.sql.SQLException;

import org.firebirdsql.gds.ISCConstants;
import org.firebirdsql.gds.ng.DatatypeCoder;
import org.firebirdsql.gds.ng.FbBlob;
import org.firebirdsql.gds.ng.FbDatabase;
import org.firebirdsql.gds.ng.FbStatement;
import org.firebirdsql.gds.ng.FbTransaction;
import org.firebirdsql.gds.ng.fields.RowValue;
import org.firebirdsql.jdbc.FBBlob;
import org.firebirdsql.jdbc.FirebirdBlob;
import org.firebirdsql.jdbc.FirebirdConnection;

public final class Blobs {

    private static final String[] SEGMENTS = {"first segment", "second, longer segment", "third"};

    /** Decode the {item, 2-byte length, value} clumplets of a blob info reply. */
    private static long infoValue(byte[] buf, int item) {
        int p = 0;
        while (p < buf.length && buf[p] != ISCConstants.isc_info_end) {
            int it = buf[p++];
            int len = (buf[p] & 0xff) | (buf[p + 1] & 0xff) << 8;
            p += 2;
            if (it == item) {
                long v = 0;
                for (int i = 0; i < len && i < 4; i++) {
                    v |= (long) (buf[p + i] & 0xff) << (8 * i);
                }
                return v;
            }
            p += len;
        }
        return -1;
    }

    /** Read a stored blob with getSegment(64), then ask it for its statistics. */
    private static void walk(FbDatabase db, long blobId) throws SQLException {
        FbTransaction tx = db.startTransaction("set transaction read only");
        try {
            FbBlob blob = db.createBlobForInput(tx, blobId);
            blob.open();
            int n = 0;
            while (true) {
                byte[] seg = blob.getSegment(64);
                if (seg.length == 0 && blob.isEof()) {
                    break;
                }
                System.out.printf("  FbBlob.getSegment(64) #%d: %2d bytes  \"%s\"%n", ++n, seg.length,
                        new String(seg, StandardCharsets.UTF_8));
            }
            byte[] info = blob.getBlobInfo(new byte[] {
                ISCConstants.isc_info_blob_num_segments, ISCConstants.isc_info_blob_max_segment,
                ISCConstants.isc_info_blob_total_length, ISCConstants.isc_info_blob_type}, 64);
            System.out.printf("  blob info: %d segments, longest %d, total %d bytes, type %d (0=segmented, 1=stream)%n",
                    infoValue(info, ISCConstants.isc_info_blob_num_segments),
                    infoValue(info, ISCConstants.isc_info_blob_max_segment),
                    infoValue(info, ISCConstants.isc_info_blob_total_length),
                    infoValue(info, ISCConstants.isc_info_blob_type));
            blob.close();
        } finally {
            tx.commit();
        }
    }

    private static long blobIdOf(Connection con, int id) throws SQLException {
        try (PreparedStatement ps = con.prepareStatement("select note from docs where id = ?")) {
            ps.setInt(1, id);
            try (ResultSet rs = ps.executeQuery()) {
                rs.next();
                Blob b = rs.getBlob(1);
                return ((FBBlob) b).getBlobId();
            }
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            try (Connection con = FbSample.attachOrCreate(dbPath("blobs", args))) {
                execute(con, "recreate table docs ("
                        + " id integer primary key,"
                        + " note blob sub_type text character set utf8,"
                        + " data blob sub_type binary)");
                FbDatabase db = con.unwrap(FirebirdConnection.class).getFbDatabase();

                // -- 1. GDS-ng: three explicit putSegment calls -------------------
                FbTransaction tx = db.startTransaction("set transaction read write");
                try {
                    FbBlob out = db.createBlobForOutput(tx);
                    out.open();
                    for (String s : SEGMENTS) {
                        out.putSegment(s.getBytes(StandardCharsets.UTF_8));
                    }
                    out.close();
                    System.out.printf("id 1: wrote 3 segments with FbBlob.putSegment, blob id 0x%016x%n",
                            out.getBlobId());

                    FbStatement st = db.createStatement(tx);
                    try {
                        st.prepare("insert into docs (id, note) values (?, ?)");
                        DatatypeCoder coder = db.getDatatypeCoder();
                        st.execute(RowValue.of(st.getParameterDescriptor(),
                                coder.encodeInt(1), coder.encodeLong(out.getBlobId())));
                    } finally {
                        st.close();
                    }
                } finally {
                    tx.commit();
                }

                // -- 2. read it back: the boundaries survive ----------------------
                walk(db, blobIdOf(con, 1));

                // -- 3. JDBC: createBlob + setBinaryStream -> a stream blob -------
                System.out.println();
                Blob jb = con.createBlob();
                try (OutputStream os = jb.setBinaryStream(1)) {
                    for (String s : SEGMENTS) {
                        os.write(s.getBytes(StandardCharsets.UTF_8));
                    }
                }
                try (PreparedStatement ps = con.prepareStatement("insert into docs (id, note) values (3, ?)")) {
                    ps.setBlob(1, jb);
                    ps.executeUpdate();
                }
                System.out.println("id 3: the same three writes through JDBC Blob.setBinaryStream()");
                walk(db, blobIdOf(con, 3));
                con.setAutoCommit(false);
                try (PreparedStatement ps = con.prepareStatement("select note from docs where id = 3");
                     ResultSet rs = ps.executeQuery()) {
                    rs.next();
                    FirebirdBlob fb = (FirebirdBlob) rs.getBlob(1);
                    try (FirebirdBlob.BlobInputStream in =
                                 (FirebirdBlob.BlobInputStream) fb.getBinaryStream()) {
                        in.seek(13);
                        byte[] part = in.readNBytes(6);
                        System.out.println("  isSegmented() = " + fb.isSegmented()
                                + "; BlobInputStream.seek(13) then 6 bytes: \""
                                + new String(part, StandardCharsets.UTF_8) + "\"");
                    }
                }
                con.commit();
                con.setAutoCommit(true);

                // -- 4. subtype text vs binary, from the catalog ------------------
                System.out.println();
                System.out.println("-- column subtypes (RDB$FIELDS) --");
                Windows.print(con, "select trim(rf.rdb$field_name) as field, f.rdb$field_sub_type as subtype,"
                        + " trim(cs.rdb$character_set_name) as charset"
                        + " from rdb$relation_fields rf"
                        + " join rdb$fields f on rf.rdb$field_source = f.rdb$field_name"
                        + " left join rdb$character_sets cs on f.rdb$character_set_id = cs.rdb$character_set_id"
                        + " where rf.rdb$relation_name = 'DOCS' and f.rdb$field_type = 261 order by 1");

                // -- 5. BLOB_APPEND -------------------------------------------------
                execute(con, "insert into docs (id, note) values (2, "
                        + "blob_append(cast('' as blob sub_type text), 'part1-', 'part2-', 'part3'))");
                System.out.println();
                System.out.println("-- BLOB_APPEND result --");
                Windows.print(con, "select id, octet_length(note) as octets, char_length(note) as chars,"
                        + " cast(note as varchar(50)) as content from docs where id = 2");
            }
            System.out.println("done.");
        });
    }
}
