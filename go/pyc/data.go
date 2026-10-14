// Package pyc reads CPython .pyc files: marshalled objects, code objects and
// the module header.
package pyc

import "fmt"

// Buffer is a random-access byte reader (the C++ PycBuffer/PycFile).
type Buffer struct {
	data []byte
	pos  int
}

// NewBuffer wraps data.
func NewBuffer(data []byte) *Buffer { return &Buffer{data: data} }

func (b *Buffer) AtEof() bool  { return b.pos >= len(b.data) }
func (b *Buffer) Pos() int     { return b.pos }
func (b *Buffer) SetPos(p int) { b.pos = p }
func (b *Buffer) Len() int     { return len(b.data) }

// Byte returns the next byte or panics with ErrEOF.
func (b *Buffer) Byte() int {
	if b.pos >= len(b.data) {
		panic(ErrEOF{})
	}
	c := int(b.data[b.pos])
	b.pos++
	return c
}

// Bytes returns the next n bytes.
func (b *Buffer) Bytes(n int) []byte {
	if n < 0 || b.pos+n > len(b.data) {
		panic(ErrEOF{})
	}
	out := b.data[b.pos : b.pos+n]
	b.pos += n
	return out
}

func (b *Buffer) Get16() int {
	r := b.Byte()
	r |= b.Byte() << 8
	return r
}

// Get32 reads a little-endian signed 32 bit value.
func (b *Buffer) Get32() int {
	r := uint32(b.Byte())
	r |= uint32(b.Byte()) << 8
	r |= uint32(b.Byte()) << 16
	r |= uint32(b.Byte()) << 24
	return int(int32(r))
}

func (b *Buffer) Get64() int64 {
	var r uint64
	for i := 0; i < 8; i++ {
		r |= uint64(b.Byte()) << (8 * uint(i))
	}
	return int64(r)
}

// ErrEOF is panicked when the stream ends unexpectedly.
type ErrEOF struct{}

func (ErrEOF) Error() string { return "Unexpected end of stream" }

// Errorf panics with a formatted error (recovered by the command line tools).
func Errorf(format string, a ...interface{}) { panic(fmt.Errorf(format, a...)) }
