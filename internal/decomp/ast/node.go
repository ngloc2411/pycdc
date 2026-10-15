// Package ast defines the Python syntax tree built by the decompiler.
package ast

import "github.com/ngloc2411/pycdc/internal/pyc"

// Node types.
const (
	NODE_INVALID = iota
	NODE_NODELIST
	NODE_OBJECT
	NODE_UNARY
	NODE_BINARY
	NODE_COMPARE
	NODE_SLICE
	NODE_STORE
	NODE_RETURN
	NODE_NAME
	NODE_DELETE
	NODE_FUNCTION
	NODE_CLASS
	NODE_CALL
	NODE_IMPORT
	NODE_TUPLE
	NODE_LIST
	NODE_SET
	NODE_MAP
	NODE_SUBSCR
	NODE_PRINT
	NODE_CONVERT
	NODE_KEYWORD
	NODE_RAISE
	NODE_EXEC
	NODE_BLOCK
	NODE_COMPREHENSION
	NODE_LOADBUILDCLASS
	NODE_AWAITABLE
	NODE_FORMATTEDVALUE
	NODE_JOINEDSTR
	NODE_CONST_MAP
	NODE_ANNOTATED_VAR
	NODE_CHAINSTORE
	NODE_TERNARY
	NODE_KW_NAMES_MAP
	NODE_MATCH_PATTERN
	NODE_NAMEDEXPR
	NODE_LOCALS
)

// Node is an AST node. A nil Node is the C++ null PycRef.
type Node interface {
	Type() int
	Processed() bool
	SetProcessed()
}

type base struct {
	typ       int
	processed bool
}

func (b *base) Type() int       { return b.typ }
func (b *base) Processed() bool { return b.processed }
func (b *base) SetProcessed()   { b.processed = true }

// TypeOf is PycRef<Node>::type() (TYPE_NULL for null).
func TypeOf(n Node) int {
	if n == nil {
		return pyc.TypeNull
	}
	return n.Type()
}

type KV struct{ K, V Node }

type NodeList struct {
	base
	nodes []Node
}

func NewNodeList(nodes []Node) *NodeList {
	return &NodeList{base: base{typ: NODE_NODELIST}, nodes: nodes}
}
func (n *NodeList) Nodes() []Node { return n.nodes }
func (n *NodeList) Append(x Node) { n.nodes = append(n.nodes, x) }
func (n *NodeList) RemoveFirst()  { n.nodes = n.nodes[1:] }
func (n *NodeList) RemoveLast()   { n.nodes = n.nodes[:len(n.nodes)-1] }

type ChainStore struct {
	base
	nodes []Node
	src   Node
}

func NewChainStore(nodes []Node, src Node) *ChainStore {
	return &ChainStore{base: base{typ: NODE_CHAINSTORE}, nodes: nodes, src: src}
}
func (n *ChainStore) Nodes() []Node { return n.nodes }
func (n *ChainStore) Append(x Node) { n.nodes = append(n.nodes, x) }
func (n *ChainStore) RemoveFirst()  { n.nodes = n.nodes[1:] }
func (n *ChainStore) RemoveLast()   { n.nodes = n.nodes[:len(n.nodes)-1] }
func (n *ChainStore) Src() Node     { return n.src }

type NamedExpr struct {
	base
	name, value Node
}

func NewNamedExpr(name, value Node) *NamedExpr {
	return &NamedExpr{base: base{typ: NODE_NAMEDEXPR}, name: name, value: value}
}
func (n *NamedExpr) Name() Node  { return n.name }
func (n *NamedExpr) Value() Node { return n.value }

type Object struct {
	base
	obj pyc.Object
}

func NewObject(o pyc.Object) *Object {
	return &Object{base: base{typ: NODE_OBJECT}, obj: o}
}
func (n *Object) Object() pyc.Object { return n.obj }

const (
	UN_POSITIVE = iota
	UN_NEGATIVE
	UN_INVERT
	UN_NOT
	UN_UNPACK
)

type Unary struct {
	base
	op      int
	operand Node
}

func NewUnary(operand Node, op int) *Unary {
	return &Unary{base: base{typ: NODE_UNARY}, op: op, operand: operand}
}
func (n *Unary) Operand() Node { return n.operand }
func (n *Unary) Op() int       { return n.op }
func (n *Unary) OpStr() string {
	return []string{"+", "-", "~", "not ", "*"}[n.op]
}

const (
	BIN_ATTR = iota
	BIN_POWER
	BIN_MULTIPLY
	BIN_DIVIDE
	BIN_FLOOR_DIVIDE
	BIN_MODULO
	BIN_ADD
	BIN_SUBTRACT
	BIN_LSHIFT
	BIN_RSHIFT
	BIN_AND
	BIN_XOR
	BIN_OR
	BIN_LOG_AND
	BIN_LOG_OR
	BIN_MAT_MULTIPLY
	BIN_IP_ADD
	BIN_IP_SUBTRACT
	BIN_IP_MULTIPLY
	BIN_IP_DIVIDE
	BIN_IP_MODULO
	BIN_IP_POWER
	BIN_IP_LSHIFT
	BIN_IP_RSHIFT
	BIN_IP_AND
	BIN_IP_XOR
	BIN_IP_OR
	BIN_IP_FLOOR_DIVIDE
	BIN_IP_MAT_MULTIPLY
	BIN_INVALID
)

const (
	CMP_LESS = iota
	CMP_LESS_EQUAL
	CMP_EQUAL
	CMP_NOT_EQUAL
	CMP_GREATER
	CMP_GREATER_EQUAL
	CMP_IN
	CMP_NOT_IN
	CMP_IS
	CMP_IS_NOT
	CMP_EXCEPTION
	CMP_BAD
)

const (
	SLICE0 = iota
	SLICE1
	SLICE2
	SLICE3
)

// Binary also represents Compare and Slice (distinguished by Type()).
type Binary struct {
	base
	op          int
	left, right Node
}

func NewBinary(left, right Node, op int) *Binary {
	return &Binary{base: base{typ: NODE_BINARY}, op: op, left: left, right: right}
}
func NewCompare(left, right Node, op int) *Binary {
	return &Binary{base: base{typ: NODE_COMPARE}, op: op, left: left, right: right}
}
func NewSlice(op int, left, right Node) *Binary {
	return &Binary{base: base{typ: NODE_SLICE}, op: op, left: left, right: right}
}
func (n *Binary) Left() Node      { return n.left }
func (n *Binary) Right() Node     { return n.right }
func (n *Binary) Op() int         { return n.op }
func (n *Binary) IsInplace() bool { return n.op >= BIN_IP_ADD }
func (n *Binary) OpStr() string {
	if n.typ == NODE_COMPARE {
		return []string{" < ", " <= ", " == ", " != ", " > ", " >= ", " in ", " not in ", " is ", " is not ",
			"<EXCEPTION MATCH>", "<BAD>"}[n.op]
	}
	return []string{".", " ** ", " * ", " / ", " // ", " % ", " + ", " - ",
		" << ", " >> ", " & ", " ^ ", " | ", " and ", " or ", " @ ",
		" += ", " -= ", " *= ", " /= ", " %= ", " **= ", " <<= ",
		" >>= ", " &= ", " ^= ", " |= ", " //= ", " @= ", " <INVALID> "}[n.op]
}

type Store struct {
	base
	src, dest Node
}

func NewStore(src, dest Node) *Store {
	return &Store{base: base{typ: NODE_STORE}, src: src, dest: dest}
}
func (n *Store) Src() Node  { return n.src }
func (n *Store) Dest() Node { return n.dest }

const (
	RET_RETURN = iota
	RET_YIELD
	RET_YIELD_FROM
)

type Return struct {
	base
	value   Node
	rettype int
}

func NewReturn(value Node, rettype int) *Return {
	return &Return{base: base{typ: NODE_RETURN}, value: value, rettype: rettype}
}
func (n *Return) Value() Node  { return n.value }
func (n *Return) RetType() int { return n.rettype }

type Name struct {
	base
	name *pyc.String
}

func NewName(name *pyc.String) *Name {
	return &Name{base: base{typ: NODE_NAME}, name: name}
}
func (n *Name) Name() *pyc.String { return n.name }

type Delete struct {
	base
	value Node
}

func NewDelete(v Node) *Delete { return &Delete{base: base{typ: NODE_DELETE}, value: v} }
func (n *Delete) Value() Node  { return n.value }

type Function struct {
	base
	code               Node
	defargs, kwdefargs []Node
}

func NewFunction(code Node, def, kwdef []Node) *Function {
	return &Function{base: base{typ: NODE_FUNCTION}, code: code, defargs: def, kwdefargs: kwdef}
}
func (n *Function) Code() Node        { return n.code }
func (n *Function) Defargs() []Node   { return n.defargs }
func (n *Function) Kwdefargs() []Node { return n.kwdefargs }

type Class struct {
	base
	code, bases, name Node
}

func NewClass(code, bases, name Node) *Class {
	return &Class{base: base{typ: NODE_CLASS}, code: code, bases: bases, name: name}
}
func (n *Class) Code() Node  { return n.code }
func (n *Class) Bases() Node { return n.bases }
func (n *Class) Name() Node  { return n.name }

type Call struct {
	base
	fn        Node
	pparams   []Node
	kwparams  []KV
	vr, kw    Node
	decorator bool
}

func NewCall(fn Node, pp []Node, kw []KV) *Call {
	return &Call{base: base{typ: NODE_CALL}, fn: fn, pparams: pp, kwparams: kw}
}
func (n *Call) Func() Node          { return n.fn }
func (n *Call) Pparams() []Node     { return n.pparams }
func (n *Call) Kwparams() []KV      { return n.kwparams }
func (n *Call) Var() Node           { return n.vr }
func (n *Call) KW() Node            { return n.kw }
func (n *Call) HasVar() bool        { return n.vr != nil }
func (n *Call) HasKW() bool         { return n.kw != nil }
func (n *Call) SetVar(v Node)       { n.vr = v }
func (n *Call) SetKW(v Node)        { n.kw = v }
func (n *Call) IsDecorator() bool   { return n.decorator }
func (n *Call) SetDecorator(d bool) { n.decorator = d }

type Import struct {
	base
	name     Node
	stores   []*Store
	fromlist Node
	level    int
}

func NewImport(name, fromlist Node) *Import {
	return &Import{base: base{typ: NODE_IMPORT}, name: name, fromlist: fromlist}
}
func (n *Import) Name() Node        { return n.name }
func (n *Import) Stores() []*Store  { return n.stores }
func (n *Import) AddStore(s *Store) { n.stores = append(n.stores, s) }
func (n *Import) Fromlist() Node    { return n.fromlist }
func (n *Import) Level() int        { return n.level }
func (n *Import) SetLevel(l int)    { n.level = l }

type Tuple struct {
	base
	values        []Node
	requireParens bool
}

func NewTuple(values []Node) *Tuple {
	return &Tuple{base: base{typ: NODE_TUPLE}, values: values, requireParens: true}
}
func (n *Tuple) Values() []Node          { return n.values }
func (n *Tuple) Add(x Node)              { n.values = append(n.values, x) }
func (n *Tuple) SetRequireParens(b bool) { n.requireParens = b }
func (n *Tuple) RequireParens() bool     { return n.requireParens }

type List struct {
	base
	values []Node
}

func NewList(v []Node) *List   { return &List{base: base{typ: NODE_LIST}, values: v} }
func (n *List) Values() []Node { return n.values }

type Set struct {
	base
	values []Node
}

func NewSet(v []Node) *Set    { return &Set{base: base{typ: NODE_SET}, values: v} }
func (n *Set) Values() []Node { return n.values }

type Map struct {
	base
	values []KV
}

func NewMap() *Map           { return &Map{base: base{typ: NODE_MAP}} }
func (n *Map) Add(k, v Node) { n.values = append(n.values, KV{k, v}) }
func (n *Map) Values() []KV  { return n.values }

type KwNamesMap struct {
	base
	values []KV
}

func NewKwNamesMap() *KwNamesMap    { return &KwNamesMap{base: base{typ: NODE_KW_NAMES_MAP}} }
func (n *KwNamesMap) Add(k, v Node) { n.values = append(n.values, KV{k, v}) }
func (n *KwNamesMap) Values() []KV  { return n.values }

type ConstMap struct {
	base
	keys   Node
	values []Node
}

func NewConstMap(keys Node, values []Node) *ConstMap {
	return &ConstMap{base: base{typ: NODE_CONST_MAP}, keys: keys, values: values}
}
func (n *ConstMap) Keys() Node     { return n.keys }
func (n *ConstMap) Values() []Node { return n.values }

type Subscr struct {
	base
	name, key Node
}

func NewSubscr(name, key Node) *Subscr {
	return &Subscr{base: base{typ: NODE_SUBSCR}, name: name, key: key}
}
func (n *Subscr) Name() Node { return n.name }
func (n *Subscr) Key() Node  { return n.key }

type Print struct {
	base
	values []Node
	stream Node
	eol    bool
}

func NewPrint(value, stream Node) *Print {
	p := &Print{base: base{typ: NODE_PRINT}, stream: stream}
	if value != nil {
		p.values = append(p.values, value)
	} else {
		p.eol = true
	}
	return p
}
func (n *Print) Values() []Node { return n.values }
func (n *Print) Stream() Node   { return n.stream }
func (n *Print) Eol() bool      { return n.eol }
func (n *Print) Add(v Node)     { n.values = append(n.values, v) }
func (n *Print) SetEol(b bool)  { n.eol = b }

type Convert struct {
	base
	name Node
}

func NewConvert(n Node) *Convert { return &Convert{base: base{typ: NODE_CONVERT}, name: n} }
func (n *Convert) Name() Node    { return n.name }

const (
	KW_PASS = iota
	KW_BREAK
	KW_CONTINUE
)

type Keyword struct {
	base
	key int
}

func NewKeyword(k int) *Keyword    { return &Keyword{base: base{typ: NODE_KEYWORD}, key: k} }
func (n *Keyword) Key() int        { return n.key }
func (n *Keyword) WordStr() string { return []string{"pass", "break", "continue"}[n.key] }

type Raise struct {
	base
	params []Node
}

func NewRaise(p []Node) *Raise  { return &Raise{base: base{typ: NODE_RAISE}, params: p} }
func (n *Raise) Params() []Node { return n.params }

type Exec struct {
	base
	stmt, glob, loc Node
}

func NewExec(s, g, l Node) *Exec {
	return &Exec{base: base{typ: NODE_EXEC}, stmt: s, glob: g, loc: l}
}
func (n *Exec) Statement() Node { return n.stmt }
func (n *Exec) Globals() Node   { return n.glob }
func (n *Exec) Locals() Node    { return n.loc }

// Block kinds.
const (
	BLK_MAIN = iota
	BLK_IF
	BLK_ELSE
	BLK_ELIF
	BLK_TRY
	BLK_CONTAINER
	BLK_EXCEPT
	BLK_FINALLY
	BLK_WHILE
	BLK_FOR
	BLK_WITH
	BLK_ASYNCFOR
	BLK_MATCH
	BLK_CASE
)

// InitCond values for CondBlock.
const (
	UNINITED = iota
	POPPED
	PRE_POPPED
)

// Block merges the C++ Block, CondBlock, IterBlock, ContainerBlock,
// WithBlock, MatchBlock and CaseBlock classes.
type Block struct {
	base
	blktype int
	end     int
	nodes   []Node
	inited  int

	// CondBlock
	cond      Node
	negative  bool
	exceptVar Node
	// IterBlock
	iter, idx, condition Node
	comp                 bool
	start                int
	// ContainerBlock
	finally, except int
	elseBlock       *Block
	// WithBlock
	expr, vr Node
	// MatchBlock
	subject Node
	// CaseBlock
	pattern *MatchPattern
	guard   Node
}

func NewBlock(blktype, end, inited int) *Block {
	return &Block{base: base{typ: NODE_BLOCK}, blktype: blktype, end: end, inited: inited}
}
func NewCondBlock(blktype, end int, cond Node, negative bool) *Block {
	b := NewBlock(blktype, end, 0)
	b.cond, b.negative = cond, negative
	return b
}
func NewIterBlock(blktype, start, end int, iter Node) *Block {
	b := NewBlock(blktype, end, 0)
	b.iter, b.start = iter, start
	return b
}
func NewContainerBlock(finally, except int) *Block {
	b := NewBlock(BLK_CONTAINER, 0, 0)
	b.finally, b.except = finally, except
	return b
}
func NewWithBlock(end int) *Block { return NewBlock(BLK_WITH, end, 0) }
func NewMatchBlock(subject Node) *Block {
	b := NewBlock(BLK_MATCH, 0, 1)
	b.subject = subject
	return b
}
func NewCaseBlock(pat *MatchPattern, end int) *Block {
	b := NewBlock(BLK_CASE, end, 1)
	b.pattern = pat
	return b
}

func (b *Block) BlkType() int  { return b.blktype }
func (b *Block) End() int      { return b.end }
func (b *Block) SetEnd(e int)  { b.end = e }
func (b *Block) Nodes() []Node { return b.nodes }
func (b *Block) Size() int     { return len(b.nodes) }
func (b *Block) RemoveFirst()  { b.nodes = b.nodes[1:] }
func (b *Block) RemoveLast()   { b.nodes = b.nodes[:len(b.nodes)-1] }
func (b *Block) Append(n Node) { b.nodes = append(b.nodes, n) }
func (b *Block) TypeStr() string {
	return []string{"", "if", "else", "elif", "try", "CONTAINER", "except",
		"finally", "while", "for", "with", "async for", "match", "case"}[b.blktype]
}
func (b *Block) Inited() int { return b.inited }
func (b *Block) Init()       { b.inited = 1 }
func (b *Block) InitV(i int) { b.inited = i }

// CondBlock
func (b *Block) Cond() Node          { return b.cond }
func (b *Block) Negative() bool      { return b.negative }
func (b *Block) ExceptVar() Node     { return b.exceptVar }
func (b *Block) SetExceptVar(v Node) { b.exceptVar = v }

// IterBlock
func (b *Block) Iter() Node              { return b.iter }
func (b *Block) Index() Node             { return b.idx }
func (b *Block) Condition() Node         { return b.condition }
func (b *Block) IsComprehension() bool   { return b.comp }
func (b *Block) Start() int              { return b.start }
func (b *Block) SetIndex(i Node)         { b.idx = i; b.Init() }
func (b *Block) SetCondition(c Node)     { b.condition = c }
func (b *Block) SetComprehension(c bool) { b.comp = c }
func (b *Block) SetIter(i Node)          { b.iter = i }

// ContainerBlock
func (b *Block) HasFinally() bool      { return b.finally != 0 }
func (b *Block) HasExcept() bool       { return b.except != 0 }
func (b *Block) Finally() int          { return b.finally }
func (b *Block) Except() int           { return b.except }
func (b *Block) SetExcept(e int)       { b.except = e }
func (b *Block) ElseBlock() *Block     { return b.elseBlock }
func (b *Block) SetElseBlock(e *Block) { b.elseBlock = e }

// WithBlock
func (b *Block) Expr() Node     { return b.expr }
func (b *Block) Var() Node      { return b.vr }
func (b *Block) SetExpr(e Node) { b.expr = e; b.Init() }
func (b *Block) SetVar(v Node)  { b.vr = v }

// MatchBlock / CaseBlock
func (b *Block) Subject() Node          { return b.subject }
func (b *Block) Pattern() *MatchPattern { return b.pattern }
func (b *Block) Guard() Node            { return b.guard }
func (b *Block) SetGuard(g Node)        { b.guard = g }

const (
	KIND_LIST = iota
	KIND_SET
	KIND_DICT
	KIND_GENERATOR
)

type Comprehension struct {
	base
	result     Node
	generators []*Block
	kind       int
}

func NewComprehension(result Node) *Comprehension {
	return &Comprehension{base: base{typ: NODE_COMPREHENSION}, result: result, kind: KIND_LIST}
}
func (n *Comprehension) Kind() int            { return n.kind }
func (n *Comprehension) SetKind(k int)        { n.kind = k }
func (n *Comprehension) Result() Node         { return n.result }
func (n *Comprehension) Generators() []*Block { return n.generators }
func (n *Comprehension) AddGenerator(g *Block) {
	n.generators = append([]*Block{g}, n.generators...)
}

type LoadBuildClass struct {
	base
	obj pyc.Object
}

func NewLoadBuildClass(o pyc.Object) *LoadBuildClass {
	return &LoadBuildClass{base: base{typ: NODE_LOADBUILDCLASS}, obj: o}
}
func (n *LoadBuildClass) Object() pyc.Object { return n.obj }

type Awaitable struct {
	base
	expr Node
}

func NewAwaitable(e Node) *Awaitable {
	return &Awaitable{base: base{typ: NODE_AWAITABLE}, expr: e}
}
func (n *Awaitable) Expression() Node { return n.expr }

// FormattedValue conversion flags.
const (
	FV_NONE            = 0
	FV_STR             = 1
	FV_REPR            = 2
	FV_ASCII           = 3
	FV_CONVERSION_MASK = 3
	FV_HAVE_FMT_SPEC   = 4
)

type FormattedValue struct {
	base
	val        Node
	conversion int
	formatSpec Node
}

func NewFormattedValue(val Node, conv int, spec Node) *FormattedValue {
	return &FormattedValue{base: base{typ: NODE_FORMATTEDVALUE}, val: val, conversion: conv, formatSpec: spec}
}
func (n *FormattedValue) Val() Node        { return n.val }
func (n *FormattedValue) Conversion() int  { return n.conversion }
func (n *FormattedValue) FormatSpec() Node { return n.formatSpec }

type JoinedStr struct {
	base
	values []Node
}

func NewJoinedStr(v []Node) *JoinedStr {
	return &JoinedStr{base: base{typ: NODE_JOINEDSTR}, values: v}
}
func (n *JoinedStr) Values() []Node { return n.values }

type AnnotatedVar struct {
	base
	name, ann Node
}

func NewAnnotatedVar(name, ann Node) *AnnotatedVar {
	return &AnnotatedVar{base: base{typ: NODE_ANNOTATED_VAR}, name: name, ann: ann}
}
func (n *AnnotatedVar) Name() Node       { return n.name }
func (n *AnnotatedVar) Annotation() Node { return n.ann }

type Ternary struct {
	base
	ifBlock, ifExpr, elseExpr Node
}

func NewTernary(ifBlock, ifExpr, elseExpr Node) *Ternary {
	return &Ternary{base: base{typ: NODE_TERNARY}, ifBlock: ifBlock, ifExpr: ifExpr, elseExpr: elseExpr}
}
func (n *Ternary) IfBlock() Node  { return n.ifBlock }
func (n *Ternary) IfExpr() Node   { return n.ifExpr }
func (n *Ternary) ElseExpr() Node { return n.elseExpr }

// Match pattern kinds.
const (
	PAT_WILDCARD = iota
	PAT_CAPTURE
	PAT_VALUE
	PAT_SEQUENCE
	PAT_STAR
	PAT_MAPPING
	PAT_CLASS
	PAT_OR
	PAT_AS
)

type MatchPattern struct {
	base
	kind        int
	expr        Node
	name        *pyc.String
	subs        []*MatchPattern
	keys        []Node
	kwNames     []*pyc.String
	nPositional int
}

func NewMatchPattern(kind int) *MatchPattern {
	return &MatchPattern{base: base{typ: NODE_MATCH_PATTERN}, kind: kind}
}
func (p *MatchPattern) Kind() int               { return p.kind }
func (p *MatchPattern) Expr() Node              { return p.expr }
func (p *MatchPattern) Name() *pyc.String       { return p.name }
func (p *MatchPattern) Subs() []*MatchPattern   { return p.subs }
func (p *MatchPattern) Keys() []Node            { return p.keys }
func (p *MatchPattern) KwNames() []*pyc.String  { return p.kwNames }
func (p *MatchPattern) NumPositional() int      { return p.nPositional }
func (p *MatchPattern) SetExpr(e Node)          { p.expr = e }
func (p *MatchPattern) SetName(n *pyc.String)   { p.name = n }
func (p *MatchPattern) Add(s *MatchPattern)     { p.subs = append(p.subs, s) }
func (p *MatchPattern) AddKey(k Node)           { p.keys = append(p.keys, k) }
func (p *MatchPattern) AddKwName(n *pyc.String) { p.kwNames = append(p.kwNames, n) }
func (p *MatchPattern) SetNumPositional(n int)  { p.nPositional = n }

// NewLocals is the node pushed by LOAD_LOCALS.
func NewLocals() Node { return &NodeList{base: base{typ: NODE_LOCALS}} }
