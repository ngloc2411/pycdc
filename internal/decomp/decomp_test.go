package decomp_test

import (
	"flag"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"

	"github.com/ngloc2411/pycdc/internal/decomp"
	"github.com/ngloc2411/pycdc/internal/pyc"
)

// The fixtures are shared with the original C++ test-suite:
//
//	tests/compiled   .pyc files that must decompile to equivalent source
//	tests/xfail      .pyc files that are known not to round-trip yet
//	tests/tokenized  token stream of the original source (see scripts/token_dump)
//	tests/golden     exact decompiler output (run `go test ./internal/decomp -update`)
const testRoot = "../../tests"

var update = flag.Bool("update", false, "rewrite tests/golden from the current output")

func decompileFile(t *testing.T, path string) (string, error) {
	t.Helper()
	m, err := pyc.LoadFile(path)
	if err != nil {
		t.Fatalf("load %s: %v", path, err)
	}
	return decomp.DecompileModule(m, filepath.Base(path))
}

// TestGolden pins the exact output for every compiled fixture.
func TestGolden(t *testing.T) {
	files, _ := filepath.Glob(filepath.Join(testRoot, "compiled", "*.pyc"))
	if len(files) == 0 {
		t.Skip("no fixtures found")
	}
	for _, f := range files {
		name := strings.TrimSuffix(filepath.Base(f), ".pyc")
		t.Run(name, func(t *testing.T) {
			got, err := decompileFile(t, f)
			if err != nil {
				t.Fatalf("decompile: %v", err)
			}
			golden := filepath.Join(testRoot, "golden", name+".py")
			if *update {
				os.MkdirAll(filepath.Dir(golden), 0o755)
				if err := os.WriteFile(golden, []byte(got), 0o644); err != nil {
					t.Fatal(err)
				}
				return
			}
			want, err := os.ReadFile(golden)
			if err != nil {
				t.Fatalf("missing golden file (run with -update): %v", err)
			}
			if got != string(want) {
				t.Errorf("output differs from %s", golden)
			}
		})
	}
}

// TestXfail runs the known-bad fixtures. They may produce wrong source or even
// a fatal error (the C++ build crashes on some); a Go panic must still be
// contained by DecompileModule, which this test checks.
func TestXfail(t *testing.T) {
	files, _ := filepath.Glob(filepath.Join(testRoot, "xfail", "*.pyc"))
	for _, f := range files {
		t.Run(strings.TrimSuffix(filepath.Base(f), ".pyc"), func(t *testing.T) {
			if _, err := decompileFile(t, f); err != nil {
				t.Logf("expected failure: %v", err)
			}
		})
	}
}

// TestTokens is the upstream check: the decompiled source must tokenize to the
// same stream as the original. It needs python3 (scripts/token_dump).
func TestTokens(t *testing.T) {
	py, err := exec.LookPath("python3")
	if err != nil {
		t.Skip("python3 not available")
	}
	dump := filepath.Join("..", "..", "scripts", "token_dump")
	files, _ := filepath.Glob(filepath.Join(testRoot, "compiled", "*.pyc"))
	for _, f := range files {
		base := strings.TrimSuffix(filepath.Base(f), ".pyc")
		t.Run(base, func(t *testing.T) {
			// compiled files are NAME.<major>.<minor>.pyc; the token file is NAME.txt
			parts := strings.Split(base, ".")
			if len(parts) < 3 {
				t.Skip("unexpected fixture name")
			}
			name := strings.Join(parts[:len(parts)-2], ".")
			want, err := os.ReadFile(filepath.Join(testRoot, "tokenized", name+".txt"))
			if err != nil {
				t.Skipf("no token file: %v", err)
			}
			src, derr := decompileFile(t, f)
			if derr != nil {
				t.Fatalf("decompile: %v", derr)
			}
			tmp := filepath.Join(t.TempDir(), base+".src.py")
			if err := os.WriteFile(tmp, []byte(src), 0o644); err != nil {
				t.Fatal(err)
			}
			out, err := exec.Command(py, dump, tmp).Output()
			if err != nil {
				t.Fatalf("token_dump: %v", err)
			}
			if string(out) != string(want) {
				t.Errorf("token stream differs from tokenized/%s.txt", name)
			}
		})
	}
}
