#ifndef _PYC_ASTMATCH_H
#define _PYC_ASTMATCH_H

#include <deque>
#include <vector>
#include "ASTNode.h"
#include "pyc_code.h"

/* Static analysis of the structural pattern matching ("match" statement)
 * bytecode emitted by Python 3.10+.  The patterns of a case are decoded by
 * symbolically executing the (very regular) pattern code, the bodies of the
 * case blocks are left to the regular statement decompiler. */

struct PyInstr {
    int pos;    // byte offset of the instruction (including EXTENDED_ARG prefix)
    int end;    // byte offset of the following instruction
    int op;     // Pyc::Opcode (CACHE entries are not part of the list)
    int arg;
};

struct MatchCase {
    PycRef<ASTMatchPattern> pattern;
    bool nonLast = false;       // the subject is kept on the stack for later cases
    int bodyStart = 0;          // offset right after the pattern (and its stores)
    int bodyEnd = -1;           // offset where the failure cleanup starts (-1: none)
    int nextPos = -1;           // offset where the next case / code after the match starts
};

class MatchAnalyzer {
public:
    MatchAnalyzer(PycRef<PycCode> code, PycModule* mod);

    /* Try to decode a case whose pattern code starts at byte offset `pos`. */
    bool parseCase(int pos, MatchCase& out);

    /* Is this a plausible first instruction of a `match` statement? */
    bool isMatchStart(int pos) const;

    int indexOf(int pos) const;
    const std::vector<PyInstr>& instrs() const { return m_ins; }
    int jumpTarget(const PyInstr& in) const;
    PycRef<PycCode> code() const { return m_code; }
    PycModule* mod() const { return m_mod; }

private:
    PycRef<PycCode> m_code;
    PycModule* m_mod;
    std::vector<PyInstr> m_ins;
    std::vector<int> m_index;   // byte offset -> instruction index (or -1)
};

#endif
