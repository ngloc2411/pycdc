// Package op holds the version independent opcode numbering and the per
// version byte -> opcode maps.
package opcode

import "fmt"

// Name returns the opcode name.
func Name(opcode int) string {
	if opcode < 0 {
		return "<INVALID>"
	}
	if opcode < PYC_LAST_OPCODE {
		return names[opcode]
	}
	return fmt.Sprintf("<%d>", opcode)
}

// ByteToOpcode maps a raw code byte of Python maj.min to an Opcode.
func ByteToOpcode(maj, min, b int) int {
	m := maps[[2]int{maj, min}]
	if m == nil || b < 0 || b > 255 {
		return PYC_INVALID_OPCODE
	}
	return int(m[b]) - 1 // stored +1 so that 0 means "invalid byte"
}
