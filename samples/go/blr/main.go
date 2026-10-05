// blr - stored BLR read raw from the catalog and disassembled (Go twin of
// ../../cpp/blr.cpp; see ../../../blr-intermediate-language.md).
//
// Fetches RDB$FIELDS.RDB$COMPUTED_BLR of EMPLOYEE.FULL_NAME and
// RDB$PROCEDURES.RDB$PROCEDURE_BLR of GET_EMP_PROJ, hex-dumps them and
// decodes the opening bytes with opcode values transcribed from
// firebird/impl/blr.h.  With firebirdsql a sub_type 2 (BLR) blob scans
// straight into a []byte - the driver fetches the segments itself, no
// CAST needed - and charset=NONE keeps the bytes untransliterated.
//
// The instructive difference: a pure wire client must WRITE BLR, not only
// read it.  Every op_execute carries a blr_message describing the
// parameter row, and firebirdsql builds it itself (calcBlr in the driver's
// xsqlvar.go: blr_version5, blr_begin, blr_message 0, a field count, one
// type descriptor plus a blr_short null indicator per column, blr_end,
// blr_eoc) - the same message syntax this sample decodes at the head of
// GET_EMP_PROJ.  libfbclient-based twins never see that layer.
//
// Run:  go run ./blr [database]   (read-only)
package main

import (
	"fmt"
	"os"

	"github.com/mariuz/conceptual-architecture-for-firebird-paper/samples/go/fbsample"
)

// Opcode values from firebird/impl/blr.h.
const (
	blrText2       = 15
	blrShort       = 7
	blrVarying     = 37
	blrBegin       = 2
	blrMessage     = 4
	blrLiteral     = 21
	blrField       = 23
	blrConcatenate = 39
	blrVersion5    = 5
	blrEoc         = 76
)

func hexDump(b []byte, limit int) {
	for i := 0; i < len(b) && i < limit; i++ {
		sep := " "
		if i%16 == 15 {
			sep = "\n"
		}
		fmt.Printf("%02x%s", b[i], sep)
	}
	if len(b) > limit {
		fmt.Printf("... (%d bytes total)\n", len(b))
	} else {
		fmt.Printf("(%d bytes total)\n", len(b))
	}
}

// expr decodes one expression - enough opcodes for a computed column.
func expr(b []byte, p int, depth int) int {
	fmt.Printf("%*s", depth*3, "")
	op := b[p]
	p++
	switch op {
	case blrConcatenate:
		fmt.Println("blr_concatenate")
		p = expr(b, p, depth+1)
		p = expr(b, p, depth+1)
	case blrField:
		ctx, n := b[p], int(b[p+1])
		p += 2
		fmt.Printf("blr_field context %d, '%s'\n", ctx, b[p:p+n])
		p += n
	case blrLiteral:
		if b[p] != blrText2 {
			fmt.Printf("blr_literal dtype %d ...\n", b[p])
			return len(b)
		}
		cs := int(b[p+1]) | int(b[p+2])<<8
		n := int(b[p+3]) | int(b[p+4])<<8
		p += 5
		fmt.Printf("blr_literal blr_text2 charset %d, len %d, %q\n", cs, n, b[p:p+n])
		p += n
	default:
		fmt.Printf("opcode %d (decoder stops here)\n", op)
		return len(b)
	}
	return p
}

func main() {
	database := "employee"
	if len(os.Args) > 1 {
		database = os.Args[1]
	}
	db, err := fbsample.Attach(database, "charset=NONE")
	fbsample.Check(err)
	defer db.Close()

	fmt.Println("== computed column EMPLOYEE.FULL_NAME - RDB$FIELDS.RDB$COMPUTED_BLR")
	var blr []byte
	fbsample.Check(db.QueryRow(`SELECT f.RDB$COMPUTED_BLR FROM RDB$FIELDS f
	  JOIN RDB$RELATION_FIELDS rf ON f.RDB$FIELD_NAME = rf.RDB$FIELD_SOURCE
	  WHERE rf.RDB$RELATION_NAME = 'EMPLOYEE' AND rf.RDB$FIELD_NAME = 'FULL_NAME'`).Scan(&blr))
	hexDump(blr, 64)
	p := 0
	if blr[p] == blrVersion5 {
		fmt.Println("blr_version5")
	}
	p = expr(blr, p+1, 1)
	if p < len(blr) && blr[p] == blrEoc {
		fmt.Println("blr_eoc")
	}

	fmt.Println("\n== procedure GET_EMP_PROJ - RDB$PROCEDURES.RDB$PROCEDURE_BLR")
	fbsample.Check(db.QueryRow(`SELECT RDB$PROCEDURE_BLR FROM RDB$PROCEDURES
	  WHERE RDB$PROCEDURE_NAME = 'GET_EMP_PROJ'`).Scan(&blr))
	hexDump(blr, 32)
	if blr[0] == blrVersion5 && blr[1] == blrBegin {
		fmt.Println("blr_version5, blr_begin")
	}
	p = 2
	for blr[p] == blrMessage {
		msg := blr[p+1]
		count := int(blr[p+2]) | int(blr[p+3])<<8
		p += 4
		fmt.Printf("blr_message %d, %d fields:", msg, count)
		for i := 0; i < count; i++ {
			switch blr[p] {
			case blrShort:
				fmt.Printf(" blr_short(scale %d)", blr[p+1])
				p += 2
			case blrText2:
				fmt.Printf(" blr_text2(cs %d, len %d)", int(blr[p+1])|int(blr[p+2])<<8, int(blr[p+3])|int(blr[p+4])<<8)
				p += 5
			case blrVarying:
				fmt.Printf(" blr_varying(len %d)", int(blr[p+1])|int(blr[p+2])<<8)
				p += 3
			default:
				fmt.Printf(" dtype %d?", blr[p])
				i = count
			}
		}
		fmt.Println()
	}
	fmt.Printf("... %d more bytes - see isql SET BLOB ALL for the full dump\n", len(blr)-p)
}
