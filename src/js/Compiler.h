#pragma once

// The compiler from a function's syntax tree to the bytecode of
// Bytecode.h: one pass per FunctionNode, no intermediate form. It emits
// exactly the environment operations the tree-walking evaluator performs
// at the same points, and calls the same runtime mechanisms through its
// instructions, so the two tiers agree by construction.

#include "js/Ast.h"
#include "js/Bytecode.h"

#include <memory>
#include <string>

namespace sashfold::js {

class Heap;

// The compiled body of `node`, or null with `error` set: the compiler
// found something it cannot express (a feature not written yet, or an
// internal inconsistency — reported so that it is never silent). The heap
// interns the two names the compiler needs of its own.
std::unique_ptr<CodeBlock> compile_function_body(FunctionNode const& node, Heap& heap, std::string* error);

}
