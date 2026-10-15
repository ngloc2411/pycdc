# Decompyle++ 
***A Python Byte-code Disassembler/Decompiler***

Decompyle++ aims to translate compiled Python byte-code back into valid
and human-readable Python source code. While other projects have achieved
this with varied success, Decompyle++ is unique in that it seeks to
support byte-code from any version of Python.

Decompyle++ includes both a byte-code disassembler (pycdas) and a 
decompiler (pycdc).

Decompyle++ now ships two implementations of the same tool: a Go port (the
main one, at the repository root) that is byte-identical to the original C++
version on all test fixtures, and the original C++ sources under `cpp/`.
If you wish to contribute, please fork us on github at 
https://github.com/zrax/pycdc

## Layout
| Path | Contents |
| --- | --- |
| `pyc/` | .pyc/marshal reader: objects, code objects, module header |
| `opcode/` | per-version opcode tables |
| `decomp/` | decompiler: `build.go` (bytecode -> AST), `scan_*.go` (control-flow pre-scans), `match.go` (match statements), `printer.go` (AST -> source), `ast/` (node types) |
| `internal/cli` | shared command-line helpers |
| `cmd/pycdas`, `cmd/pycdc` | the disassembler and decompiler executables |
| `cpp/` | original C++ implementation (CMake) |
| `tests/` | compiled fixtures (`compiled`, `xfail`, `tokenized`) and Go golden outputs (`golden`) |

## Building Decompyle++ (Go)
* `go build ./cmd/pycdc ./cmd/pycdas`
* Run tests: `go test ./...` (refresh golden files with `go test ./decomp -update`)

## Building Decompyle++ (C++, in `cpp/`)
* Generate a project or makefile with [CMake](http://www.cmake.org) from the `cpp/` directory (See CMake's documentation for details)
  * The following options can be passed to CMake to control debug features:

    | Option | Description |
    | --- | --- |
    | `-DCMAKE_BUILD_TYPE=Debug` | Produce debugging symbols |
    | `-DENABLE_BLOCK_DEBUG=ON` | Enable block debugging output |
    | `-DENABLE_STACK_DEBUG=ON` | Enable stack debugging output |

* Build the generated project or makefile
  * For projects (e.g. MSVC), open the generated project file and build it
  * For makefiles, just run `make`
  * To run tests (on \*nix or MSYS), run `make check JOBS=4` (optional
    `FILTER=xxxx` to run only certain tests)

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
