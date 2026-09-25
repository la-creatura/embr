#ifndef EMBR_CORE_AST_H
#define EMBR_CORE_AST_H

#include "diagnostics.h"
#include "types.h"
#include "fwd.h"

#include <vector>
#include <memory>
#include <utility>
#include <string>

namespace embr {

struct Expr {
    enum class Kind { Number, Int, String, Var, Unary, Binary, Call, Array, Map, Index, Lambda, And, Or };
    Kind        kind;
    SourceRange range;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};

struct NumberExpr : Expr { double      v; NumberExpr(double      v,SourceRange r):Expr(Kind::Number),v(v)           {range=r;} };
// a numeric literal with no decimal point
struct IntExpr    : Expr { int64_t     v; IntExpr(int64_t        v,SourceRange r):Expr(Kind::Int),   v(v)           {range=r;} };
struct StringExpr : Expr { std::string v; StringExpr(std::string v,SourceRange r):Expr(Kind::String),v(std::move(v)){range=r;} };
struct VarExpr    : Expr { std::string name; VarExpr(std::string n,SourceRange r):Expr(Kind::Var),name(std::move(n)){range=r;} };

struct UnaryExpr  : Expr { std::string op;      ExprPtr right;            UnaryExpr():Expr(Kind::Unary) {} };
struct BinaryExpr : Expr { std::string op;      ExprPtr left,right;      BinaryExpr():Expr(Kind::Binary){} };
struct LogicExpr  : Expr { ExprPtr left, right;                           LogicExpr(Kind k) : Expr(k)   {} };

struct CallExpr   : Expr { ExprPtr callee;      std::vector<ExprPtr> args; CallExpr():Expr(Kind::Call)  {} };
struct ArrayExpr  : Expr { std::vector<ExprPtr> elements;                 ArrayExpr():Expr(Kind::Array) {} };
struct MapExpr    : Expr { std::vector<std::pair<ExprPtr,ExprPtr>> entries; MapExpr():Expr(Kind::Map)   {} };
struct IndexExpr  : Expr { ExprPtr object,index;                          IndexExpr():Expr(Kind::Index) {} };

// body heap-owned unlike FnStmt owned by program vector
struct LambdaExpr : Expr {
    std::vector<Param>          params;
    TypeSet                     returnType = TypeSet::Any();
    const std::vector<StmtPtr>* body;
    std::vector<StmtPtr>        ownedBody;
    LambdaExpr() : Expr(Kind::Lambda) {}
};

struct Stmt {
    enum class Kind { Assign, MultiAssign, Local, If, Fn, Return, While, For, Break, Continue, Import, Expr, Try };
    Kind kind; SourceRange range;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};

struct AssignStmt : Stmt { ExprPtr target,value; AssignStmt():Stmt(Kind::Assign){} };

// a, b, c = expr
// see LocalStmt for the declaring local equivalent, which additionally supports types
struct MultiAssignStmt : Stmt {
    std::vector<std::string> names;
    ExprPtr value;
    MultiAssignStmt():Stmt(Kind::MultiAssign){}
};

// one binding in a local declaration
// a name, plus an optional type constraint 
// isAuto means the type is inferred from the unpacked/assigned value at declaration time and then enforced on every later assignment
struct DestructureTarget {
    std::string name;
    TypeSet     type = TypeSet::Any();
    bool        isAuto = false;
};

// local [type] name = expr
// local [type] name1, [type] name2, ... = expr
// local [type|auto] [name1, name2, ...] = expr
// targets.size()==1 && !bracketed for no unpacking
// everything else destructures value via the array/map unpacking rules. see Runner::unpackForDestructure in tree_walker.h
struct LocalStmt : Stmt {
    std::vector<DestructureTarget> targets;
    bool    bracketed = false;
    ExprPtr value;
    LocalStmt():Stmt(Kind::Local){}
};
struct IfStmt     : Stmt { ExprPtr cond; std::vector<StmtPtr> thenBlock,elseBlock; IfStmt():Stmt(Kind::If){} };
struct FnStmt     : Stmt {
    std::string        name;
    std::vector<Param> params;
    TypeSet            returnType = TypeSet::Any();
    std::vector<StmtPtr> body;
    FnStmt():Stmt(Kind::Fn){}
};
struct ReturnStmt : Stmt { ExprPtr value; ReturnStmt():Stmt(Kind::Return){} };
struct WhileStmt  : Stmt { ExprPtr cond; std::vector<StmtPtr> body; WhileStmt():Stmt(Kind::While){} };
// for varName in iterable ... end
// for keyName, varName in iterable ... end
struct ForStmt : Stmt {
    std::string keyName;  // empty unless the two-variable map form was used
    std::string varName;
    ExprPtr iterable;
    std::vector<StmtPtr> body;
    ForStmt():Stmt(Kind::For){}
};
struct BreakStmt    : Stmt { BreakStmt():Stmt(Kind::Break){} };
struct ContinueStmt : Stmt { ContinueStmt():Stmt(Kind::Continue){} };
struct ExprStmt   : Stmt { ExprPtr expr; ExprStmt():Stmt(Kind::Expr){} };
// try ... catch e ... end
// catchVar is bound to the raised error's message for the duration of catchBlock only then goes out of scope
struct TryStmt : Stmt {
    std::vector<StmtPtr> tryBlock;
    std::string          catchVar;
    std::vector<StmtPtr> catchBlock;
    TryStmt():Stmt(Kind::Try){}
};
struct ImportStmt : Stmt { std::string path; ImportStmt():Stmt(Kind::Import){} };

} // namespace embr

#endif // EMBR_CORE_AST_H
