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
    enum class Kind { Number, String, Var, Unary, Binary, Call, Array, Map, Index, Lambda, And, Or };
    Kind        kind;
    SourceRange range;
    explicit Expr(Kind k) : kind(k) {}
    virtual ~Expr() = default;
};

struct NumberExpr : Expr { double      v; NumberExpr(double      v,SourceRange r):Expr(Kind::Number),v(v)           {range=r;} };
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
    enum class Kind { Assign, Local, If, Fn, Return, While, Import, Expr };
    Kind kind; SourceRange range;
    explicit Stmt(Kind k) : kind(k) {}
    virtual ~Stmt() = default;
};

struct AssignStmt : Stmt { ExprPtr target,value; AssignStmt():Stmt(Kind::Assign){} };
struct LocalStmt  : Stmt { std::string name; ExprPtr value; LocalStmt():Stmt(Kind::Local){} };
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
struct ExprStmt   : Stmt { ExprPtr expr; ExprStmt():Stmt(Kind::Expr){} };
struct ImportStmt : Stmt { std::string path; ImportStmt():Stmt(Kind::Import){} };

} // namespace embr

#endif // EMBR_CORE_AST_H
