// ha - the one HA primitive that is pure client-side SQL: a database
// SHADOW (Go twin of ../../cpp/ha.cpp; see ../../../high-availability.md).
//
// On a fresh scratch database: CREATE SHADOW, see it registered in
// RDB$FILES (flag 1 = shadow), stat() both files - server and sample share
// a host here - as 5000 rows are inserted, then DROP SHADOW 1 DELETE FILE.
// CREATE/DROP SHADOW are ordinary DSQL, so the pure-Go firebirdsql needs
// nothing special: fbsample.Recreate starts from a fresh database
// (dropping a database drops its shadow too) and os.Stat plays stat().
// The driver's MaintenanceManager also carries the shadow's *service*
// verbs (ActivateShadow, KillShadow - gfix -activate / -kill), the
// recovery half of the story that needs a lost main file to show.
// Replica promotion and sync_replica need server-side configuration and
// stay as text in the document.
//
// Run:  go run ./ha [database]
package main

import (
	"fmt"
	"os"
	"strings"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

var (
	mainFile   = fbsample.DBPath("ha") // server-side paths
	shadowFile = strings.TrimSuffix(mainFile, ".fdb") + ".shd"
)

func fileSize(path string) int64 {
	st, err := os.Stat(path)
	if err != nil {
		return -1
	}
	return st.Size()
}

func showFiles(when string) {
	fmt.Printf("%-28s main = %8d bytes, shadow = %8d bytes\n",
		when, fileSize(mainFile), fileSize(shadowFile))
}

func main() {
	db, err := fbsample.Recreate(mainFile)
	fbsample.Check(err)
	defer db.Close()

	_, err = db.Exec("CREATE TABLE HA_LOG (ID INT NOT NULL PRIMARY KEY, PAYLOAD VARCHAR(200))")
	fbsample.Check(err)

	// 1. Create the synchronous page-level mirror.
	_, err = db.Exec(fmt.Sprintf("CREATE SHADOW 1 '%s'", shadowFile))
	fbsample.Check(err)
	fmt.Println("CREATE SHADOW 1 done - the engine dumped every page to the mirror")
	fmt.Println()

	// The shadow is registered in the metadata like any other file.
	rows, err := fbsample.Rows(db, `SELECT RDB$FILE_NAME, RDB$SHADOW_NUMBER, RDB$FILE_FLAGS
	                                  FROM RDB$FILES ORDER BY RDB$SHADOW_NUMBER`)
	fbsample.Check(err)
	for _, r := range rows {
		fmt.Printf("RDB$FILES: %s  shadow_number=%v  flags=%v\n",
			strings.TrimSpace(fbsample.Text(r[0])), fbsample.Text(r[1]), fbsample.Text(r[2]))
	}
	fmt.Println()
	showFiles("after CREATE SHADOW:")

	// 2. Write load: every page write now goes to both files.
	_, err = db.Exec(`EXECUTE BLOCK AS DECLARE I INT = 0; BEGIN
	                    WHILE (I < 5000) DO BEGIN
	                      INSERT INTO HA_LOG VALUES (:I, LPAD('', 200, 'x')); I = I + 1;
	                    END
	                  END`)
	fbsample.Check(err)
	showFiles("after 5000 inserts:")

	// 3. Retire the mirror.
	_, err = db.Exec("DROP SHADOW 1 DELETE FILE")
	fbsample.Check(err)
	fmt.Println("\nDROP SHADOW 1 DELETE FILE done")
	showFiles("after DROP SHADOW:")

	left, err := fbsample.Scalar(db, "SELECT COUNT(*) FROM RDB$FILES")
	fbsample.Check(err)
	fmt.Println("\nRDB$FILES rows left:", fbsample.Text(left))
	fmt.Println("done.")
}
