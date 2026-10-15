package pyc_test

import (
	"bytes"
	"flag"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ngloc2411/pycdc/internal/pyc"
)

var update = flag.Bool("update", false, "rewrite tests/golden_dis from the current output")

// TestDisassembly pins pycdas output for every compiled fixture.
func TestDisassembly(t *testing.T) {
	files, _ := filepath.Glob("../../tests/compiled/*.pyc")
	if len(files) == 0 {
		t.Skip("no fixtures found")
	}
	for _, f := range files {
		name := strings.TrimSuffix(filepath.Base(f), ".pyc")
		t.Run(name, func(t *testing.T) {
			m, err := pyc.LoadFile(f)
			if err != nil {
				t.Fatal(err)
			}
			var buf bytes.Buffer
			pyc.DumpObject(m.Code, m, 0, 0, &buf)
			golden := filepath.Join("..", "..", "tests", "golden_dis", name+".txt")
			if *update {
				os.MkdirAll(filepath.Dir(golden), 0o755)
				if err := os.WriteFile(golden, buf.Bytes(), 0o644); err != nil {
					t.Fatal(err)
				}
				return
			}
			want, err := os.ReadFile(golden)
			if err != nil {
				t.Fatalf("missing golden (run with -update): %v", err)
			}
			if !bytes.Equal(buf.Bytes(), want) {
				t.Errorf("disassembly differs from %s", golden)
			}
		})
	}
}
