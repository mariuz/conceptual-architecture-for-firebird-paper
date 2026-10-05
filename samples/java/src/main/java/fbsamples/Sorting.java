//
// Sorting.java - the TempCacheLimit threshold made visible (Java twin of
// ../../../../../cpp/sorting.cpp; see ../../../../../../sorting-and-temp-space.md).
//
// The same experiment: 200,000 rows with a 400-byte ASCII key (~82 MB of
// sort data) sorted whole, against a 20,000-row (~8 MB) variant of the same
// ORDER BY.  While each query runs, a watcher thread on its OWN attachment
// polls database-level MON$MEMORY_USAGE (a fresh transaction per poll - a
// fresh MON$ snapshot) and looks for the unlinked fb_sort_* scratch files.
// Jaybird's pure-Java wire protocol blocks only the thread waiting on its
// socket, so the watcher keeps polling while the main thread sits in the
// fetch that performs the sort.  The plan comes from
// FirebirdPreparedStatement in both forms: the legacy PLAN SORT (...) and
// the explained tree, whose Sort node carries the record and key lengths.
// The scratch half has the same privilege gap as the Python and Go twins:
// the files exist only in the server's /proc/<pid>/fd, which needs the
// server's uid or root, so the fallback watches the free space of the
// scratch filesystem (/tmp, the default TempDirectories; FIREBIRD_TMP
// overrides) through java.nio.file.FileStore - an unlinked file still
// occupies its blocks.
//
// Run:  cd samples/java && mvn -q compile exec:exec -Dsample=Sorting
//
package fbsamples;

import static fbsamples.FbSample.attach;
import static fbsamples.FbSample.attachOrCreate;
import static fbsamples.FbSample.dbPath;
import static fbsamples.FbSample.execute;
import static fbsamples.FbSample.scalar;

import java.io.IOException;
import java.nio.file.FileStore;
import java.nio.file.Files;
import java.nio.file.Path;
import java.sql.Connection;
import java.sql.PreparedStatement;
import java.sql.ResultSet;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.stream.Stream;

import org.firebirdsql.jdbc.FirebirdPreparedStatement;

public final class Sorting {

    private static final String MEM_SQL = "select m.mon$memory_allocated from mon$database d"
            + " join mon$memory_usage m on m.mon$stat_id = d.mon$stat_id";
    private static final Path SCRATCH_FS = Path.of(System.getenv().getOrDefault("FIREBIRD_TMP", "/tmp"));

    /** Polls MON$ and the scratch side until stopped, keeping the peaks. */
    private static final class Watcher extends Thread {
        final AtomicBoolean running = new AtomicBoolean(true);
        final AtomicLong peakMem = new AtomicLong();
        final AtomicLong peakFiles = new AtomicLong();
        final AtomicLong peakScratch = new AtomicLong();
        final AtomicLong peakDrop = new AtomicLong();
        volatile boolean procVisible = true;
        volatile Exception failure;
        private final String path;
        private final long pid;
        private final long freeBefore;

        Watcher(String path, long pid) throws IOException {
            this.path = path;
            this.pid = pid;
            this.freeBefore = store().getUsableSpace();
        }

        private static FileStore store() throws IOException {
            return Files.getFileStore(SCRATCH_FS);
        }

        /** Sizes of the server's open fb_sort_* files, or null when /proc is closed to us. */
        private long[] viaProc() {
            Path fds = Path.of("/proc", Long.toString(pid), "fd");
            long files = 0;
            long bytes = 0;
            try (Stream<Path> s = Files.list(fds)) {
                for (Path fd : (Iterable<Path>) s::iterator) {
                    try {
                        if (Files.readSymbolicLink(fd).toString().contains("fb_sort")) {
                            files++;
                            bytes += Files.size(fd);
                        }
                    } catch (IOException ignored) {
                        // fd closed between list and stat
                    }
                }
            } catch (IOException | SecurityException e) {
                return null;
            }
            return new long[] {files, bytes};
        }

        @Override
        public void run() {
            try (Connection mon = attach(path)) {
                mon.setAutoCommit(false);
                while (running.get()) {
                    long[] seen = procVisible ? viaProc() : null;
                    if (seen != null) {
                        peakFiles.accumulateAndGet(seen[0], Math::max);
                        peakScratch.accumulateAndGet(seen[1], Math::max);
                    } else {
                        procVisible = false;
                        peakDrop.accumulateAndGet(freeBefore - store().getUsableSpace(), Math::max);
                    }
                    peakMem.accumulateAndGet(((Number) scalar(mon, MEM_SQL)).longValue(), Math::max);
                    mon.commit();                    // next poll: new tx => new MON$ snapshot
                }
            } catch (Exception e) {
                failure = e;
            }
        }
    }

    public static void main(String[] args) {
        FbSample.run(() -> {
            String path = dbPath("sorting", args);
            try (Connection con = attachOrCreate(path)) {
                execute(con, "recreate table bulk (id integer, pad varchar(400) character set ascii)");
                execute(con, "execute block as declare i integer = 0; begin"
                        + "  while (i < 200000) do begin"
                        + "    insert into bulk values (:i, rpad(uuid_to_char(gen_uuid()), 400, 'x'));"
                        + "    i = i + 1;"
                        + "  end "
                        + "end");
                System.out.println("bulk: 200000 rows, 400-byte ASCII key -> ~82 MB of sort data");

                long pid = ((Number) scalar(con, "select mon$server_pid from mon$attachments"
                        + " where mon$attachment_id = current_connection")).longValue();
                long memIdle = ((Number) scalar(con, MEM_SQL)).longValue();
                System.out.printf("server pid %d, database memory allocated while idle: %d bytes%n", pid, memIdle);

                List<String[]> cases = List.of(
                        new String[] {"big sort (200k rows, ~82 MB)",
                            "select first 1 id from bulk order by pad desc"},
                        new String[] {"small sort (20k rows, ~8 MB)",
                            "select first 1 id from bulk where mod(id, 10) = 0 order by pad desc"});

                for (String[] c : cases) {
                    Watcher w = new Watcher(path, pid);
                    w.start();
                    Object top;
                    try (PreparedStatement ps = con.prepareStatement(c[1])) {
                        FirebirdPreparedStatement fps = ps.unwrap(FirebirdPreparedStatement.class);
                        System.out.println();
                        System.out.println(c[0]);
                        System.out.println("  " + fps.getExecutionPlan().strip());
                        for (String line : fps.getExplainedExecutionPlan().split("\n")) {
                            if (line.contains("Sort")) {
                                System.out.println("  " + line.strip());
                            }
                        }
                        try (ResultSet rs = ps.executeQuery()) {   // the sort happens here
                            top = rs.next() ? rs.getObject(1) : null;
                        }
                    } finally {
                        w.running.set(false);
                        w.join();
                    }
                    if (w.failure != null) {
                        throw w.failure;
                    }
                    System.out.println("  top row id = " + top);
                    if (w.procVisible) {
                        System.out.printf("  peak fb_sort_* scratch: %d file(s), %d bytes%n",
                                w.peakFiles.get(), w.peakScratch.get());
                    } else {
                        System.out.printf("  /proc/%d/fd not readable; peak drop of free space on %s: %d bytes%n",
                                pid, SCRATCH_FS, Math.max(0, w.peakDrop.get()));
                    }
                    System.out.printf("  peak database MON$MEMORY_ALLOCATED: %d bytes (+%d over idle)%n",
                            w.peakMem.get(), w.peakMem.get() - memIdle);
                }
            }
            System.out.println();
            System.out.println("done.");
        });
    }

    private Sorting() {
    }
}
