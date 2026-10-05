// services - the Services API from client code: a service attachment, an
// SPB-driven action, and the 1 KB ring-buffer polling loop (Go twin of
// ../../cpp/services.cpp; see ../../../services-api.md).
//
// The sample attaches to localhost:service_mgr, asks the server for its
// version and environment, then starts a VERBOSE backup of a scratch
// database.  The backup runs BURP_main - the real gbak - on a server
// thread; its output arrives through the 1 KB svc_stdout ring buffer,
// drained here with repeated isc_info_svc_line queries.  Every path in
// the SPB is a SERVER path: the .fbk lands on the server's filesystem,
// owned by the server's user.
//
// firebirdsql speaks the Services protocol itself (op_service_attach /
// op_service_start / op_service_info), no libfbclient and no embedded
// service manager to attach by mistake.  Its BackupManager.Backup(db, fbk,
// options, verboseChan) would hide the loop behind a channel, so the
// sample keeps it visible: the action SPB is written with the driver's
// exported XPBWriter (the isc_* tag values are spelled out, the driver
// does not export them) and ServiceManager.GetString() is one
// isc_info_svc_line query per call - the poll the C++ twin counts.
//
// Run:  go run ./services [database [backup-file]]
package main

import (
	"fmt"
	"os"
	"os/user"
	"strconv"
	"strings"
	"syscall"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
	"github.com/nakagami/firebirdsql"
)

// SPB tags from ibase.h (not exported by the driver).
const (
	iscActionSvcBackup = 1   // isc_action_svc_backup
	iscSpbBkpFile      = 5   // isc_spb_bkp_file
	iscSpbDbname       = 106 // isc_spb_dbname
	iscSpbVerbose      = 107 // isc_spb_verbose
)

func main() {
	dbPath := fbsample.DBPath("services")
	bkPath := fbsample.Scratch + "/services_go.fbk"
	if len(os.Args) > 2 {
		bkPath = os.Args[2]
	}

	// 0. Make sure the scratch database exists, with one table in it.
	db, err := fbsample.Create(dbPath)
	fbsample.Check(err)
	db.Exec("create table t (id int, v varchar(20))") // already there is fine
	db.Close()

	// 1. Attach to the service manager: credentials in the SPB_ATTACH.
	svc, err := firebirdsql.NewServiceManager(fbsample.ServiceAddr(), fbsample.User,
		fbsample.Password, fbsample.ServiceOptions())
	fbsample.Check(err)
	defer svc.Close()
	fmt.Printf("service       : %s:service_mgr (wire cipher %s)\n", fbsample.ServiceAddr(), svc.WireCipher())

	// 2. Information requests: no action, just server facts.
	version, err := svc.GetServerVersionString()
	fbsample.Check(err)
	arch, _ := svc.GetArchitecture()
	home, _ := svc.GetHomeDir()
	secdb, _ := svc.GetSecurityDatabasePath()
	fmt.Println("server version:", version)
	fmt.Println("architecture  :", arch)
	fmt.Println("home directory:", home)
	fmt.Println("security db   :", secdb)

	// 3. Start action_backup - the services[] table dispatches it to
	//    BURP_main, i.e. gbak itself, on a server thread.
	spb := firebirdsql.NewXPBWriterFromTag(iscActionSvcBackup)
	spb.PutString(iscSpbDbname, dbPath)  // server path!
	spb.PutString(iscSpbBkpFile, bkPath) // server path!
	spb.PutTag(iscSpbVerbose)
	fbsample.Check(svc.ServiceStart(spb.Bytes()))
	fmt.Println("backup started (verbose) - draining the 1 KB ring buffer:")

	// 4. The polling loop: each GetString is one isc_info_svc_line query;
	//    the producer BLOCKS whenever the buffer is full, so a client that
	//    stops polling stalls the backup.  (A "data not ready" answer is
	//    re-asked inside GetString after 10 ms, so polls counts calls.)
	lines, polls := 0, 0
	for {
		line, end, err := svc.GetString()
		fbsample.Check(err)
		polls++
		if end || line == "" {
			break
		}
		fmt.Println("  " + strings.TrimRight(line, " "))
		lines++
	}
	fmt.Printf("done: %d gbak lines drained in %d GetString() polls\n", lines, polls)

	owner := "?"
	if st, err := os.Stat(bkPath); err == nil {
		uid := st.Sys().(*syscall.Stat_t).Uid
		owner = strconv.Itoa(int(uid))
		if u, err := user.LookupId(owner); err == nil {
			owner = u.Username
		}
		fmt.Printf("the file %s now exists on the SERVER: %d bytes, owned by %s\n", bkPath, st.Size(), owner)
	} else {
		fmt.Printf("the file %s is on the SERVER (not visible from here: %v)\n", bkPath, err)
	}
}
