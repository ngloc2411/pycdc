# Decompyle++ 
***A Python Byte-code Disassembler/Decompiler***

Decompyle++ aims to translate compiled Python byte-code back into valid
and human-readable Python source code. While other projects have achieved
this with varied success, Decompyle++ is unique in that it seeks to
support byte-code from any version of Python.

Decompyle++ includes both a byte-code disassembler (pycdas) and a 
decompiler (pycdc).

Decompyle++ is written in Go. It was ported from the original C++
implementation by Michael Hansen and Darryl Pogue (https://github.com/zrax/pycdc)
and is byte-identical to it on all test fixtures.

## Layout
| Path | Contents |
| --- | --- |
| `cmd/pycdas`, `cmd/pycdc` | the disassembler and decompiler executables |
| `internal/pyc` | .pyc/marshal reader: objects, code objects, module header, disassembly dump |
| `internal/opcode` | per-version opcode tables |
| `internal/decomp` | decompiler: `builder*.go`, `pre_*.go`, `ops_*.go` (bytecode -> AST), `scan_*.go` (control-flow pre-scans), `match.go` (match statements), `printer.go` (AST -> source), `ast/` (node types) |
| `internal/cli` | shared command-line helpers |
| `tests/` | compiled fixtures (`compiled`, `xfail`, `tokenized`) and golden outputs (`golden`, `golden_dis`) |

## Building
* `go build ./cmd/pycdc ./cmd/pycdas`
* Run tests: `go test ./...` (refresh golden files with `go test ./internal/... -update`)

## Usage
**To run pycdas**, the PYC Disassembler:
`./pycdas [PATH TO PYC FILE]`
The byte-code disassembly is printed to stdout.

**To run pycdc**, the PYC Decompiler: 
`./pycdc [PATH TO PYC FILE]`
The decompiled Python source is printed to stdout.
Any errors are printed to stderr.

**Marshalled code objects**:
Both tools support Python marshalled code objects, as output from `marshal.dumps(compile(...))`.

To use this feature, specify `-c -v <version>` on the command line - the version must be specified as the objects themselves do not contain version metadata.

## Authors, Licence, Credits
Decompyle++ is the work of Michael Hansen and Darryl Pogue.

Additional contributions from:
* charlietang98
* Kunal Parmar
* Olivier Iffrig
* Zlodiy

It is released under the terms of the GNU General Public License, version 3;
See LICENSE file for details.
