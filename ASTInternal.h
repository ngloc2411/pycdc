#ifndef _PYC_ASTINTERNAL_H
#define _PYC_ASTINTERNAL_H

/* State shared between the AST builder (ASTree.cpp) and the source printer
   (ASTPrint.cpp). */
extern bool cleanBuild;               /* the last build/print finished without errors */
extern bool inLambda;                 /* printing a lambda body */
extern bool lambdaBare;               /* print a sole lambda argument without parentheses */
extern bool genexprBare;              /* print a sole generator argument without parentheses */
extern bool printDocstringAndGlobals; /* a def's docstring/globals still have to be printed */
extern bool printClassDocstring;      /* a class docstring may still appear */

extern int cur_indent;                /* current output indentation level */

#include <ostream>
#include "pyc_object.h"
#include "pyc_module.h"
void start_line(int indent, std::ostream& pyc_output);
void end_line(std::ostream& pyc_output);
bool print_docstring(PycRef<PycObject> obj, int indent, PycModule* mod,
                     std::ostream& pyc_output);

#endif
