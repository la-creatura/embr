#ifndef EMBR_CORE_FWD_H
#define EMBR_CORE_FWD_H

// forward declarations that break the circular dependency between ScriptFn/Value (core/value.h) and Expr/Stmt (core/ast.h)
// a script function body is a pointer to a statement list, and the AST doesn't need to know about Value at all
// Value needs a name for "pointer to statement list" and "pointer to capture frame" before either of those types actually exist yet

#include <memory>
#include <string>

namespace embr {

class Interpreter;
struct Expr;
struct Stmt;
using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;

struct CaptureFrame;
struct Value;

// defined in core/value.h right after Value
// declared here so Value's formatAsString can call it before it's defined
inline std::string valueRepr(const Value& v);

} // namespace embr

#endif // EMBR_CORE_FWD_H
