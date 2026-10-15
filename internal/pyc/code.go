package pyc

import "os"

var errOut = os.Stderr

// ExceptionTableEntry is one 3.11+ exception table row.
type ExceptionTableEntry struct {
	StartOffset int // inclusive
	EndOffset   int // exclusive
	Target      int
	StackDepth  int
	PushLasti   bool
}

// Code flags.
const (
	CoOptimized             = 0x1
	CoNewlocals             = 0x2
	CoVarargs               = 0x4
	CoVarkeywords           = 0x8
	CoNested                = 0x10
	CoGenerator             = 0x20
	CoNofree                = 0x40
	CoCoroutine             = 0x80
	CoIterableCoroutine     = 0x100
	CoAsyncGenerator        = 0x200
	CoGeneratorAllowed      = 0x1000
	CoFutureDivision        = 0x20000
	CoFutureAbsoluteImport  = 0x40000
	CoFutureWithStatement   = 0x80000
	CoFuturePrintFunction   = 0x100000
	CoFutureUnicodeLiterals = 0x200000
	CoFutureBarryAsBDFL     = 0x400000
	CoFutureGeneratorStop   = 0x800000
	CoFutureAnnotations     = 0x1000000
	CoNoMonitoringEvents    = 0x2000000
)

// Code is a code object.
type Code struct {
	typ                                                  int
	ArgCount, PosOnlyArgCount, KwOnlyArgCount, NumLocals int
	StackSize, Flags                                     int
	Code                                                 *String
	Consts, Names, LocalNames                            Sequence
	LocalKinds                                           *String
	FreeVars, CellVars                                   Sequence
	FileName, Name, QualName                             *String
	FirstLine                                            int
	LnTable, ExceptTable                                 *String
	globalsUsed, nonlocalsUsed                           []*String
}

func (c *Code) Type() int             { return c.typ }
func (c *Code) IsEqual(o Object) bool { return Object(c) == o }

func asString(o Object) *String {
	s, _ := o.(*String)
	if s == nil {
		Errorf("expected string in code object")
	}
	return s
}
func asSeq(o Object) Sequence {
	s, _ := o.(Sequence)
	if s == nil {
		Errorf("expected sequence in code object")
	}
	return s
}

func (c *Code) load(b *Buffer, m *Module) {
	if m.VerCompare(1, 3) >= 0 && m.VerCompare(2, 3) < 0 {
		c.ArgCount = b.Get16()
	} else if m.VerCompare(2, 3) >= 0 {
		c.ArgCount = b.Get32()
	}
	if m.VerCompare(3, 8) >= 0 {
		c.PosOnlyArgCount = b.Get32()
	}
	if m.MajorVer() >= 3 {
		c.KwOnlyArgCount = b.Get32()
	}
	if m.VerCompare(1, 3) >= 0 && m.VerCompare(2, 3) < 0 {
		c.NumLocals = b.Get16()
	} else if m.VerCompare(2, 3) >= 0 && m.VerCompare(3, 11) < 0 {
		c.NumLocals = b.Get32()
	}
	if m.VerCompare(1, 5) >= 0 && m.VerCompare(2, 3) < 0 {
		c.StackSize = b.Get16()
	} else if m.VerCompare(2, 3) >= 0 {
		c.StackSize = b.Get32()
	}
	if m.VerCompare(1, 3) >= 0 && m.VerCompare(2, 3) < 0 {
		c.Flags = b.Get16()
	} else if m.VerCompare(2, 3) >= 0 {
		c.Flags = b.Get32()
	}
	if m.VerCompare(3, 8) < 0 {
		if uint32(c.Flags)&0xF0000000 != 0 {
			Errorf("Cannot remap unexpected flags")
		}
		c.Flags = (c.Flags & 0xFFFF) | ((c.Flags & 0xFFF0000) << 4)
	}
	c.Code = asString(LoadObject(b, m))
	c.Consts = asSeq(LoadObject(b, m))
	c.Names = asSeq(LoadObject(b, m))
	if m.VerCompare(1, 3) >= 0 {
		c.LocalNames = asSeq(LoadObject(b, m))
	} else {
		c.LocalNames = NewTuple()
	}
	if m.VerCompare(3, 11) >= 0 {
		c.LocalKinds = asString(LoadObject(b, m))
	} else {
		c.LocalKinds = &String{typ: TypeString}
	}
	if m.VerCompare(2, 1) >= 0 && m.VerCompare(3, 11) < 0 {
		c.FreeVars = asSeq(LoadObject(b, m))
		c.CellVars = asSeq(LoadObject(b, m))
	} else {
		c.FreeVars = NewTuple()
		c.CellVars = NewTuple()
	}
	c.FileName = asString(LoadObject(b, m))
	c.Name = asString(LoadObject(b, m))
	if m.VerCompare(3, 11) >= 0 {
		c.QualName = asString(LoadObject(b, m))
	} else {
		c.QualName = &String{typ: TypeString}
	}
	if m.VerCompare(1, 5) >= 0 && m.VerCompare(2, 3) < 0 {
		c.FirstLine = b.Get16()
	} else if m.VerCompare(2, 3) >= 0 {
		c.FirstLine = b.Get32()
	}
	if m.VerCompare(1, 5) >= 0 {
		c.LnTable = asString(LoadObject(b, m))
	} else {
		c.LnTable = &String{typ: TypeString}
	}
	if m.VerCompare(3, 11) >= 0 {
		c.ExceptTable = asString(LoadObject(b, m))
	} else {
		c.ExceptTable = &String{typ: TypeString}
	}
}

// GetConst returns consts[idx].
func (c *Code) GetConst(idx int) Object { return c.Consts.Get(idx) }

// GetName returns names[idx].
func (c *Code) GetName(idx int) *String { return c.Names.Get(idx).(*String) }

// GetLocal returns the local variable name idx.
func (c *Code) GetLocal(idx int) *String { return c.LocalNames.Get(idx).(*String) }

// GetCellVar names the cell (or free) variable idx.
func (c *Code) GetCellVar(m *Module, idx int) *String {
	if m.VerCompare(3, 11) >= 0 {
		return c.GetLocal(idx)
	}
	if idx >= c.CellVars.Size() {
		return c.FreeVars.Get(idx - c.CellVars.Size()).(*String)
	}
	return c.CellVars.Get(idx).(*String)
}

// IsFreeVar reports whether cell index idx names a captured free variable.
func (c *Code) IsFreeVar(m *Module, idx int) bool {
	if m.VerCompare(3, 11) >= 0 {
		if idx < 0 || idx >= len(c.LocalKinds.Value) {
			return false
		}
		return c.LocalKinds.Value[idx]&0x80 != 0
	}
	return idx >= c.CellVars.Size()
}

func (c *Code) Globals() []*String   { return c.globalsUsed }
func (c *Code) MarkGlobal(n *String) { c.globalsUsed = append(c.globalsUsed, n) }
func (c *Code) Nonlocals() []*String { return c.nonlocalsUsed }
func (c *Code) MarkNonlocal(n *String) {
	for _, x := range c.nonlocalsUsed {
		if x.Value == n.Value {
			return
		}
	}
	c.nonlocalsUsed = append(c.nonlocalsUsed, n)
}

func parseVarint(b *Buffer) int {
	x := b.Byte()
	val := x & 0x3F
	for x&0x40 != 0 {
		val <<= 6
		x = b.Byte()
		val |= x & 0x3F
	}
	return val
}

// ExceptionTableEntries decodes the 3.11+ exception table.
func (c *Code) ExceptionTableEntries() []ExceptionTableEntry {
	b := NewBuffer([]byte(c.ExceptTable.Value))
	var out []ExceptionTableEntry
	for !b.AtEof() {
		start := parseVarint(b) * 2
		length := parseVarint(b) * 2
		target := parseVarint(b) * 2
		dl := parseVarint(b)
		out = append(out, ExceptionTableEntry{start, start + length, target, dl >> 1, dl&1 != 0})
	}
	return out
}
