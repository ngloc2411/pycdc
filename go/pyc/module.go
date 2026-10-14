package pyc

import (
	"fmt"
	"os"
)

const (
	magic_1_0   = 0x999902
	magic_1_1   = 0x999903
	magic_1_3   = 0xa0d2e89
	magic_1_4   = 0xa0d1704
	magic_1_5   = 0xa0d4e99
	magic_1_6   = 0xa0dc4fc
	magic_2_0   = 0xa0dc687
	magic_2_1   = 0xa0deb2a
	magic_2_2   = 0xa0ded2d
	magic_2_3   = 0xa0df23b
	magic_2_4   = 0xa0df26d
	magic_2_5   = 0xa0df2b3
	magic_2_6   = 0xa0df2d1
	magic_2_7   = 0xa0df303
	magic_3_0   = 0xa0d0c3a
	magic_3_1   = 0xa0d0c4e
	magic_3_2   = 0xa0d0c6c
	magic_3_3   = 0xa0d0c9e
	magic_3_4   = 0xa0d0cee
	magic_3_5   = 0xa0d0d16
	magic_3_5_3 = 0xa0d0d17
	magic_3_6   = 0xa0d0d33
	magic_3_7   = 0xa0d0d42
	magic_3_8   = 0xa0d0d55
	magic_3_9   = 0xa0d0d61
	magic_3_10  = 0xa0d0d6f
	magic_3_11  = 0xa0d0da7
	magic_3_12  = 0xa0d0dcb
	magic_3_13  = 0xa0d0df3
	magic_3_14  = 0xa0d0e2b
)

// Module is a loaded .pyc: version information plus the top-level code object.
type Module struct {
	maj, min int
	unicode  bool
	Code     *Code
	interns  []*String
	refs     []Object
}

func (m *Module) setVersion(magic uint32) {
	m.unicode = false
	switch magic {

	case magic_1_0:
		m.maj, m.min, m.unicode = 1, 0, false
	case magic_1_1:
		m.maj, m.min, m.unicode = 1, 1, false
	case magic_1_3:
		m.maj, m.min, m.unicode = 1, 3, false
	case magic_1_4:
		m.maj, m.min, m.unicode = 1, 4, false
	case magic_1_5:
		m.maj, m.min, m.unicode = 1, 5, false
	case magic_1_6:
		m.maj, m.min, m.unicode = 1, 6, false
	case magic_1_6 + 1:
		m.maj, m.min, m.unicode = 1, 6, true
	case magic_2_0:
		m.maj, m.min, m.unicode = 2, 0, false
	case magic_2_0 + 1:
		m.maj, m.min, m.unicode = 2, 0, true
	case magic_2_1:
		m.maj, m.min, m.unicode = 2, 1, false
	case magic_2_1 + 1:
		m.maj, m.min, m.unicode = 2, 1, true
	case magic_2_2:
		m.maj, m.min, m.unicode = 2, 2, false
	case magic_2_2 + 1:
		m.maj, m.min, m.unicode = 2, 2, true
	case magic_2_3:
		m.maj, m.min, m.unicode = 2, 3, false
	case magic_2_3 + 1:
		m.maj, m.min, m.unicode = 2, 3, true
	case magic_2_4:
		m.maj, m.min, m.unicode = 2, 4, false
	case magic_2_4 + 1:
		m.maj, m.min, m.unicode = 2, 4, true
	case magic_2_5:
		m.maj, m.min, m.unicode = 2, 5, false
	case magic_2_5 + 1:
		m.maj, m.min, m.unicode = 2, 5, true
	case magic_2_6:
		m.maj, m.min, m.unicode = 2, 6, false
	case magic_2_6 + 1:
		m.maj, m.min, m.unicode = 2, 6, true
	case magic_2_7:
		m.maj, m.min, m.unicode = 2, 7, false
	case magic_2_7 + 1:
		m.maj, m.min, m.unicode = 2, 7, true
	case magic_3_0 + 1:
		m.maj, m.min, m.unicode = 3, 0, true
	case magic_3_1 + 1:
		m.maj, m.min, m.unicode = 3, 1, true
	case magic_3_2:
		m.maj, m.min, m.unicode = 3, 2, true
	case magic_3_3:
		m.maj, m.min, m.unicode = 3, 3, true
	case magic_3_4:
		m.maj, m.min, m.unicode = 3, 4, true
	case magic_3_5:
		m.maj, m.min, m.unicode = 3, 5, true
	case magic_3_5_3:
		m.maj, m.min, m.unicode = 3, 5, true
	case magic_3_6:
		m.maj, m.min, m.unicode = 3, 6, true
	case magic_3_7:
		m.maj, m.min, m.unicode = 3, 7, true
	case magic_3_8:
		m.maj, m.min, m.unicode = 3, 8, true
	case magic_3_9:
		m.maj, m.min, m.unicode = 3, 9, true
	case magic_3_10:
		m.maj, m.min, m.unicode = 3, 10, true
	case magic_3_11:
		m.maj, m.min, m.unicode = 3, 11, true
	case magic_3_12:
		m.maj, m.min, m.unicode = 3, 12, true
	case magic_3_13:
		m.maj, m.min, m.unicode = 3, 13, true
	case magic_3_14:
		m.maj, m.min, m.unicode = 3, 14, true
	default:
		m.maj, m.min = -1, -1
	}
}

// NewModule returns an empty module.
func NewModule() *Module { return &Module{maj: -1, min: -1} }

// NewModuleVersion builds a module of a known version (used by tests).
func NewModuleVersion(maj, min int) *Module {
	return &Module{maj: maj, min: min, unicode: maj >= 3}
}

func (m *Module) IsValid() bool   { return m.maj >= 0 && m.min >= 0 }
func (m *Module) MajorVer() int   { return m.maj }
func (m *Module) MinorVer() int   { return m.min }
func (m *Module) IsUnicode() bool { return m.unicode }

// VerCompare compares the version with maj.min (negative, zero, positive).
func (m *Module) VerCompare(maj, min int) int {
	if m.maj == maj {
		return m.min - min
	}
	return m.maj - maj
}

func (m *Module) StrIsUnicode() bool {
	return m.maj >= 3 || m.Code.Flags&CoFutureUnicodeLiterals != 0
}

func (m *Module) InternIsBytes() bool {
	return m.maj < 3 && m.Code.Flags&CoFutureUnicodeLiterals != 0
}

func (m *Module) intern(s *String)   { m.interns = append(m.interns, s) }
func (m *Module) refObject(o Object) { m.refs = append(m.refs, o) }

func (m *Module) GetIntern(ref int) *String {
	if ref < 0 || ref >= len(m.interns) {
		Errorf("Intern index out of range")
	}
	return m.interns[ref]
}

func (m *Module) GetRef(ref int) Object {
	if ref < 0 || ref >= len(m.refs) {
		Errorf("Ref index out of range")
	}
	return m.refs[ref]
}

// IsSupportedVersion reports whether marshalled files of that version load.
func IsSupportedVersion(major, minor int) bool {
	switch major {
	case 1:
		return minor >= 0 && minor <= 6
	case 2:
		return minor >= 0 && minor <= 7
	case 3:
		return minor >= 0 && minor <= 14
	}
	return false
}

// LoadFile reads a .pyc file.
func LoadFile(filename string) (m *Module, err error) {
	data, err := os.ReadFile(filename)
	if err != nil {
		return nil, err
	}
	return LoadBytes(data)
}

// LoadBytes parses the bytes of a .pyc file.
func LoadBytes(data []byte) (m *Module, err error) {
	defer recoverErr(&err)
	m = NewModule()
	in := NewBuffer(data)
	m.setVersion(uint32(in.Get32()))
	if !m.IsValid() {
		return nil, fmt.Errorf("Bad MAGIC!")
	}
	flags := 0
	if m.VerCompare(3, 7) >= 0 {
		flags = in.Get32()
	}
	if flags&1 != 0 {
		in.Get32()
		in.Get32()
	} else {
		in.Get32()
		if m.VerCompare(3, 3) >= 0 {
			in.Get32()
		}
	}
	c, _ := LoadObject(in, m).(*Code)
	if c == nil {
		return nil, fmt.Errorf("file does not contain a code object")
	}
	m.Code = c
	return m, nil
}

// LoadMarshalled reads a bare marshalled code object of a given version.
func LoadMarshalled(filename string, major, minor int) (m *Module, err error) {
	defer recoverErr(&err)
	if !IsSupportedVersion(major, minor) {
		return nil, fmt.Errorf("Unsupported version %d.%d", major, minor)
	}
	data, err := os.ReadFile(filename)
	if err != nil {
		return nil, err
	}
	m = NewModule()
	m.maj, m.min, m.unicode = major, minor, major >= 3
	c, _ := LoadObject(NewBuffer(data), m).(*Code)
	if c == nil {
		return nil, fmt.Errorf("file does not contain a code object")
	}
	m.Code = c
	return m, nil
}

func recoverErr(err *error) {
	if r := recover(); r != nil {
		if e, ok := r.(error); ok {
			*err = e
			return
		}
		*err = fmt.Errorf("%v", r)
	}
}
