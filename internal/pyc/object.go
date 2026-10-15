package pyc

import (
	"fmt"
	"io"
	"math"
	"strings"
)

// Marshal type codes.
const (
	TypeNull               = '0'
	TypeNone               = 'N'
	TypeFalse              = 'F'
	TypeTrue               = 'T'
	TypeStopIter           = 'S'
	TypeEllipsis           = '.'
	TypeInt                = 'i'
	TypeInt64              = 'I'
	TypeFloat              = 'f'
	TypeBinaryFloat        = 'g'
	TypeComplex            = 'x'
	TypeBinaryComplex      = 'y'
	TypeLong               = 'l'
	TypeString             = 's'
	TypeInterned           = 't'
	TypeStringRef          = 'R'
	TypeObRef              = 'r'
	TypeTuple              = '('
	TypeList               = '['
	TypeDict               = '{'
	TypeCode               = 'c'
	TypeCode2              = 'C'
	TypeUnicode            = 'u'
	TypeUnknown            = '?'
	TypeSet                = '<'
	TypeFrozenSet          = '>'
	TypeASCII              = 'a'
	TypeASCIIInterned      = 'A'
	TypeSmallTuple         = ')'
	TypeShortASCII         = 'z'
	TypeShortASCIIInterned = 'Z'
	TypeSlice              = ':'
)

// Object is any marshalled value. A nil Object is TYPE_NULL.
type Object interface {
	Type() int
	IsEqual(o Object) bool
	load(b *Buffer, m *Module)
}

// TypeOf returns the type of o, TypeNull for nil.
func TypeOf(o Object) int {
	if o == nil {
		return TypeNull
	}
	return o.Type()
}

// Simple is None/True/False/Ellipsis/StopIteration.
type Simple struct{ typ int }

func (s *Simple) Type() int             { return s.typ }
func (s *Simple) IsEqual(o Object) bool { return Object(s) == o }
func (s *Simple) load(*Buffer, *Module) {}

var (
	None          Object = &Simple{TypeNone}
	Ellipsis      Object = &Simple{TypeEllipsis}
	StopIteration Object = &Simple{TypeStopIter}
	False         Object = &Simple{TypeFalse}
	True          Object = &Simple{TypeTrue}
)

// Int is a 32 bit integer.
type Int struct {
	typ   int
	Value int
}

func (i *Int) Type() int { return i.typ }
func (i *Int) IsEqual(o Object) bool {
	x, ok := o.(*Int)
	return ok && x.typ == i.typ && x.Value == i.Value
}
func (i *Int) load(b *Buffer, m *Module) { i.Value = b.Get32() }

// Long is an arbitrary-precision integer stored as 15 bit digits.
type Long struct {
	typ   int
	Size  int
	Value []int
}

func (l *Long) Type() int { return l.typ }
func (l *Long) IsEqual(o Object) bool {
	x, ok := o.(*Long)
	if !ok || x.typ != l.typ || x.Size != l.Size || len(x.Value) != len(l.Value) {
		return false
	}
	for i := range l.Value {
		if l.Value[i] != x.Value[i] {
			return false
		}
	}
	return true
}
func (l *Long) load(b *Buffer, m *Module) {
	if l.typ == TypeInt64 {
		lo := uint32(b.Get32())
		hi := uint32(b.Get32())
		l.Value = []int{int(lo & 0xFFFF), int(lo >> 16 & 0xFFFF), int(hi & 0xFFFF), int(hi >> 16 & 0xFFFF)}
		if hi&0x80000000 != 0 {
			l.Size = -4
		} else {
			l.Size = 4
		}
		return
	}
	l.Size = b.Get32()
	n := l.Size
	if n < 0 {
		n = -n
	}
	for i := 0; i < n; i++ {
		l.Value = append(l.Value, b.Get16())
	}
}

// Repr prints the long as a hexadecimal literal.
func (l *Long) Repr(m *Module) string {
	if l.Size == 0 {
		if m.VerCompare(3, 0) >= 0 {
			return "0x0"
		}
		return "0x0L"
	}
	var bits []uint32
	shift := uint(0)
	var temp uint32
	for _, bit := range l.Value {
		temp |= uint32(bit&0xFFFF) << shift
		shift += 15
		if shift >= 32 {
			bits = append(bits, temp)
			shift -= 32
			temp = uint32(bit&0xFFFF) >> (15 - shift)
		}
	}
	if temp != 0 {
		bits = append(bits, temp)
	}
	var sb strings.Builder
	if l.Size < 0 {
		sb.WriteByte('-')
	}
	sb.WriteString("0x")
	if len(bits) == 0 {
		sb.WriteString("0")
	} else {
		fmt.Fprintf(&sb, "%X", bits[len(bits)-1])
		for i := len(bits) - 2; i >= 0; i-- {
			fmt.Fprintf(&sb, "%08X", bits[i])
		}
	}
	if m.VerCompare(3, 0) < 0 {
		sb.WriteByte('L')
	}
	return sb.String()
}

// Float is a textual float (marshal 'f') or, with Imag set, a textual complex.
type Float struct {
	typ     int
	Value   string
	Imag    string
	complex bool
}

func (f *Float) Type() int { return f.typ }
func (f *Float) IsEqual(o Object) bool {
	x, ok := o.(*Float)
	return ok && x.typ == f.typ && x.Value == f.Value && x.Imag == f.Imag
}
func (f *Float) load(b *Buffer, m *Module) {
	n := b.Byte()
	f.Value = string(b.Bytes(n))
	if f.complex {
		n = b.Byte()
		f.Imag = string(b.Bytes(n))
	}
}

// CFloat is a binary float or complex.
type CFloat struct {
	typ     int
	Value   float64
	Imag    float64
	complex bool
}

func (f *CFloat) Type() int { return f.typ }
func (f *CFloat) IsEqual(o Object) bool {
	x, ok := o.(*CFloat)
	return ok && x.typ == f.typ && x.Value == f.Value && x.Imag == f.Imag
}
func (f *CFloat) load(b *Buffer, m *Module) {
	f.Value = math.Float64frombits(uint64(b.Get64()))
	if f.complex {
		f.Imag = math.Float64frombits(uint64(b.Get64()))
	}
}

// String is a marshalled string/bytes/unicode value.
type String struct {
	typ   int
	Value string
}

func (s *String) Type() int { return s.typ }
func (s *String) IsEqual(o Object) bool {
	x, ok := o.(*String)
	return ok && x.typ == s.typ && x.Value == s.Value
}
func (s *String) Length() int              { return len(s.Value) }
func (s *String) StartsWith(p string) bool { return strings.HasPrefix(s.Value, p) }

func (s *String) load(b *Buffer, m *Module) {
	if s.typ == TypeStringRef {
		o := m.GetIntern(b.Get32())
		s.typ = o.typ
		s.Value = o.Value
		return
	}
	var length int
	if s.typ == TypeShortASCII || s.typ == TypeShortASCIIInterned {
		length = b.Byte()
	} else {
		length = b.Get32()
	}
	if length < 0 {
		Errorf("bad string length")
	}
	s.Value = string(b.Bytes(length))
	if s.typ == TypeASCII || s.typ == TypeASCIIInterned || s.typ == TypeShortASCII || s.typ == TypeShortASCIIInterned {
		for i := 0; i < len(s.Value); i++ {
			if s.Value[i] == 0 {
				break
			}
			if s.Value[i]&0x80 != 0 {
				Errorf("Invalid bytes in ASCII string")
			}
		}
	}
	if s.typ == TypeInterned || s.typ == TypeASCIIInterned || s.typ == TypeShortASCIIInterned {
		m.intern(s)
	}
}

// Print writes the Python literal for the string.
func (s *String) Print(w io.Writer, m *Module, triple bool, parentFStringQuote string) {
	var prefix byte
	switch s.typ {
	case TypeString:
		if m.StrIsUnicode() {
			prefix = 'b'
		}
	case TypeUnicode:
		if !m.StrIsUnicode() {
			prefix = 'u'
		}
	case TypeInterned:
		if m.InternIsBytes() {
			prefix = 'b'
		}
	case TypeASCII, TypeASCIIInterned, TypeShortASCII, TypeShortASCIIInterned:
	default:
		Errorf("Invalid string type")
	}
	if prefix != 0 {
		w.Write([]byte{prefix})
	}
	if s.Value == "" {
		io.WriteString(w, "''")
		return
	}
	useQuotes := false
	if parentFStringQuote == "" {
		for i := 0; i < len(s.Value); i++ {
			ch := s.Value[i]
			if ch == '\'' {
				useQuotes = true
			} else if ch == '"' {
				useQuotes = false
				break
			}
		}
	} else {
		useQuotes = parentFStringQuote[0] == '"'
	}
	quote := func() {
		if parentFStringQuote != "" {
			return
		}
		if triple {
			if useQuotes {
				io.WriteString(w, `"""`)
			} else {
				io.WriteString(w, "'''")
			}
		} else if useQuotes {
			io.WriteString(w, `"`)
		} else {
			io.WriteString(w, "'")
		}
	}
	quote()
	for i := 0; i < len(s.Value); i++ {
		ch := s.Value[i]
		switch {
		case ch < 0x20 || ch == 0x7F:
			switch ch {
			case '\r':
				io.WriteString(w, `\r`)
			case '\n':
				if triple {
					io.WriteString(w, "\n")
				} else {
					io.WriteString(w, `\n`)
				}
			case '\t':
				io.WriteString(w, `\t`)
			default:
				fmt.Fprintf(w, `\x%02x`, ch)
			}
		case ch >= 0x80:
			if s.typ == TypeUnicode {
				w.Write([]byte{ch})
			} else {
				fmt.Fprintf(w, `\x%02x`, ch)
			}
		default:
			switch {
			case !useQuotes && ch == '\'':
				io.WriteString(w, `\'`)
			case useQuotes && ch == '"':
				io.WriteString(w, `\"`)
			case ch == '\\':
				io.WriteString(w, `\\`)
			case parentFStringQuote != "" && ch == '{':
				io.WriteString(w, "{{")
			case parentFStringQuote != "" && ch == '}':
				io.WriteString(w, "}}")
			default:
				w.Write([]byte{ch})
			}
		}
	}
	quote()
}

// Sequence is implemented by tuples, lists and sets (and code-object tables).
type Sequence interface {
	Object
	Size() int
	Get(i int) Object
	Values() []Object
}

// Seq is a tuple, list, set or frozenset.
type Seq struct {
	typ    int
	values []Object
}

func (s *Seq) Type() int        { return s.typ }
func (s *Seq) Size() int        { return len(s.values) }
func (s *Seq) Get(i int) Object { return s.values[i] }
func (s *Seq) Values() []Object { return s.values }
func (s *Seq) IsEqual(o Object) bool {
	x, ok := o.(*Seq)
	if !ok || x.typ != s.typ || len(x.values) != len(s.values) {
		return false
	}
	for i := range s.values {
		a, b := s.values[i], x.values[i]
		if a == nil || b == nil {
			if a != b {
				return false
			}
			continue
		}
		if !a.IsEqual(b) {
			return false
		}
	}
	return true
}
func (s *Seq) load(b *Buffer, m *Module) {
	var n int
	if s.typ == TypeSmallTuple {
		n = b.Byte()
	} else {
		n = b.Get32()
	}
	if n < 0 {
		Errorf("bad sequence length")
	}
	s.values = make([]Object, 0, minInt(n, 1<<16))
	for i := 0; i < n; i++ {
		s.values = append(s.values, LoadObject(b, m))
	}
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}

// NewTuple builds an empty tuple.
func NewTuple() *Seq { return &Seq{typ: TypeTuple} }

// Slice is a constant folded slice object (marshal 5, Python 3.14+).
type Slice struct {
	Start, Stop, Step Object
}

func (s *Slice) Type() int { return TypeSlice }
func (s *Slice) IsEqual(o Object) bool {
	x, ok := o.(*Slice)
	if !ok {
		return false
	}
	eq := func(a, b Object) bool {
		if a == nil || b == nil {
			return a == nil && b == nil
		}
		return a.IsEqual(b)
	}
	return eq(s.Start, x.Start) && eq(s.Stop, x.Stop) && eq(s.Step, x.Step)
}
func (s *Slice) load(b *Buffer, m *Module) {
	s.Start = LoadObject(b, m)
	s.Stop = LoadObject(b, m)
	s.Step = LoadObject(b, m)
}

// DictItem is a key/value pair.
type DictItem struct{ Key, Val Object }

// Dict is a marshalled dict.
type Dict struct{ Items []DictItem }

func (d *Dict) Type() int { return TypeDict }
func (d *Dict) IsEqual(o Object) bool {
	x, ok := o.(*Dict)
	if !ok || len(x.Items) != len(d.Items) {
		return false
	}
	for i := range d.Items {
		if !d.Items[i].Key.IsEqual(x.Items[i].Key) || !d.Items[i].Val.IsEqual(x.Items[i].Val) {
			return false
		}
	}
	return true
}
func (d *Dict) load(b *Buffer, m *Module) {
	for {
		k := LoadObject(b, m)
		if k == nil {
			break
		}
		v := LoadObject(b, m)
		d.Items = append(d.Items, DictItem{k, v})
	}
}

func createObject(t int) Object {
	switch t {
	case TypeNull:
		return nil
	case TypeNone:
		return None
	case TypeFalse:
		return False
	case TypeTrue:
		return True
	case TypeStopIter:
		return StopIteration
	case TypeEllipsis:
		return Ellipsis
	case TypeInt:
		return &Int{typ: t}
	case TypeInt64, TypeLong:
		return &Long{typ: t}
	case TypeFloat:
		return &Float{typ: t}
	case TypeComplex:
		return &Float{typ: t, complex: true}
	case TypeBinaryFloat:
		return &CFloat{typ: t}
	case TypeBinaryComplex:
		return &CFloat{typ: t, complex: true}
	case TypeString, TypeInterned, TypeStringRef, TypeUnicode, TypeASCII,
		TypeASCIIInterned, TypeShortASCII, TypeShortASCIIInterned:
		return &String{typ: t}
	case TypeTuple, TypeSmallTuple, TypeList, TypeSet, TypeFrozenSet:
		return &Seq{typ: t}
	case TypeDict:
		return &Dict{}
	case TypeCode, TypeCode2:
		return &Code{typ: t}
	case TypeSlice:
		return &Slice{}
	}
	fmt.Fprintf(errOut, "CreateObject: Got unsupported type 0x%X\n", t)
	return nil
}

// LoadObject reads one marshalled object.
func LoadObject(b *Buffer, m *Module) Object {
	t := b.Byte()
	if t == TypeObRef {
		return m.GetRef(b.Get32())
	}
	obj := createObject(t & 0x7F)
	if obj != nil {
		if t&0x80 != 0 {
			m.refObject(obj)
		}
		obj.load(b, m)
	}
	return obj
}

// NewString makes a plain (byte) string object.
func NewString(v string) *String { return &String{typ: TypeString, Value: v} }

// NewInt makes an int object.
func NewInt(v int) *Int { return &Int{typ: TypeInt, Value: v} }
