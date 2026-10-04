#ifndef EMBR_CORE_PARSER_H
#define EMBR_CORE_PARSER_H

#include <algorithm>
#include <initializer_list>
#include "diagnostics.h"
#include "types.h"
#include "token.h"
#include "ast.h"

#include <vector>
#include <memory>
#include <iostream>
#include <string>
#include <charconv>

namespace embr {

class Parser {
public:
    Parser(std::vector<Token> toks, SourceMap src)
        : toks_(std::move(toks)), src_(std::move(src)) {}

    // by default parse errors are printed to std::cerr as they are found (the parser recovers and keeps
    // going, so one bad statement doesn't hide the rest). a tool that wants them as data, `embr check`,
    // an editor/LSP integration, passes a vector here and they are appended to it instead, nothing printed
    void collectErrors(std::vector<EmbrError>* sink) { errors_ = sink; }

    std::vector<StmtPtr> parse() {
        std::vector<StmtPtr> out;
        while (!check(TokenType::Eof)) {
            try { out.push_back(stmt()); }
            catch (const EmbrError& e) { report(e); loopDepth_ = 0; sync(); }
            catch (const std::exception& e) {
                report(EmbrError(std::string("[parser] ") + e.what() + "\n", false, {}, e.what(), "parser"));
                loopDepth_ = 0; sync();
            }
        }
        return out;
    }

private:
    std::vector<EmbrError>* errors_ = nullptr;     // see collectErrors()
    void report(const EmbrError& e) { if (errors_) errors_->push_back(e); else std::cerr << e.what(); }

    std::vector<Token> toks_;
    SourceMap          src_;
    size_t             i_ = 0;
    int                loopDepth_ = 0; // reset across fn/lambda boundaries so break/continue can't cross them

    // hard limits that turn a native stack overflow (an uncatchable process kill) on pathological input into
    // an ordinary parser error. both are far above what real code needs and far below where an 8 MB stack gives out
    // parse()'s error recovery also resets loopDepth_ to 0, because a statement that throws midway skips the
    // matching restore. without that, a stray `continue` after an error was accepted with no loop to attach to
    static constexpr int kMaxNesting   = 2500;   // simultaneous recursive parse calls (stmt/expr/unary)
    static constexpr int kMaxExprDepth = 4000;   // height of any single expression tree
    int                nest_ = 0;

    struct NestGuard {
        Parser& p;
        explicit NestGuard(Parser& p_) : p(p_) {
            if (++p.nest_ > kMaxNesting) {
                --p.nest_;
                raiseError("parser", "nesting too deep (max " + std::to_string(kMaxNesting) + ")",
                           SourceRange::fromToken(p.peek()), p.src_);
            }
        }
        ~NestGuard() { --p.nest_; }
    };

    // records n's tree height from its children and enforces kMaxExprDepth
    void setDepth(Expr* n, std::initializer_list<const Expr*> kids) {
        int d = 0;
        for (const Expr* k : kids) if (k && k->depth > d) d = k->depth;
        n->depth = d + 1;
        if (n->depth > kMaxExprDepth)
            raiseError("parser", "expression too deeply nested or too long (max depth " +
                       std::to_string(kMaxExprDepth) + ")", n->range, src_);
    }

    Token& peek() { return toks_[i_]; }
    Token& prev() { return toks_[i_-1]; }
    bool match(TokenType t) { if (peek().type==t){++i_;return true;} return false; }
    bool check(TokenType t) const { return toks_[i_].type==t; }
    bool checkAt(size_t off, TokenType t) const {
        return i_+off < toks_.size() && toks_[i_+off].type == t;
    }
    Token consume(TokenType t, const char* msg) {
        if (match(t)) return prev();
        raiseError("parser", msg, SourceRange::fromToken(peek()), src_);
    }
    SourceRange tr(const Token& tok) const { return SourceRange::fromToken(tok); }

    void sync() {
        while (!check(TokenType::Eof)) {
            switch (peek().type) {
                case TokenType::If:     case TokenType::Elif:  case TokenType::Else:
                case TokenType::End:
                case TokenType::Fn:     case TokenType::While: case TokenType::For:
                case TokenType::Break:  case TokenType::Continue:
                case TokenType::Try:    case TokenType::Catch:
                case TokenType::Import:
                case TokenType::Local:  case TokenType::Return: return;
                default: ++i_;
            }
        }
    }

    TypeSet parseTypeName(const Token& t) {
        TypeSet ts = TypeSet::fromName(t.text);
        if (ts.isNone())
            raiseError("parser", "unknown type '" + t.text +
                       "' expected: num, str, arr, map, fn, ptr, any", tr(t), src_);
        return ts;
    }
    // name : type
    TypeSet parseTypeAnnotation() {
        if (!match(TokenType::Colon)) return TypeSet::Any();
        Token t = consume(TokenType::Identifier, "expected type name after ':'");
        return parseTypeName(t);
    }

    // caller already eatid (
    std::vector<Param> parseParamList() {
        std::vector<Param> params;
        if (!check(TokenType::RParen)) {
            do {
                // ...name[: type]
                // collects every remaining argument into an array bound to name. must be the last parameter
                if (match(TokenType::Ellipsis)) {
                    Token nm = consume(TokenType::Identifier, "expected parameter name after '...'");
                    TypeSet mask = parseTypeAnnotation();
                    params.push_back(Param::rest(nm.text, mask));
                    if (check(TokenType::Comma))
                        raiseError("parser", "variadic parameter '...' must be the last parameter",
                                   tr(peek()), src_);
                    break;
                }
                Token nm = consume(TokenType::Identifier, "expected parameter name");
                TypeSet mask = parseTypeAnnotation();
                params.push_back(Param::req(nm.text, mask));
            } while (match(TokenType::Comma));
        }
        return params;
    }

    // function -> type
    TypeSet parseReturnAnnotation() {
        if (!match(TokenType::RArrow)) return TypeSet::Any();
        Token t = consume(TokenType::Identifier, "expected return type after '->'");
        return parseTypeName(t);
    }

    StmtPtr stmt() {
        NestGuard guard(*this);
        if (match(TokenType::If))       return parseIf();
        if (match(TokenType::While))    return parseWhile();
        if (match(TokenType::For))      return parseFor();
        if (match(TokenType::Try))      return parseTry();
        if (match(TokenType::Break))    return parseBreak();
        if (match(TokenType::Continue)) return parseContinue();
        if (match(TokenType::Return))   return parseReturn();
        if (match(TokenType::Import))   return parseImport();
        if (match(TokenType::Local))    return parseLocal();
        if (match(TokenType::Fn))       return parseFnBlock();

        // name(params) = expr
        // IDENT ( [IDENT {, IDENT}] ) = (not ==)
        if (isFnAssignHead()) return parseFnAssign();

        // a, b, c = expr (reassigns existing bare names by unpacking expr)
        if (isMultiAssignHead()) return parseMultiAssign();

        auto lhs = expr();
        if (match(TokenType::Equal)) {
            auto rhs = expr();
            auto s = std::make_unique<AssignStmt>();
            s->target = std::move(lhs); s->value = std::move(rhs);
            return s;
        }

        // name +=, -=, *=, /= expr
        //  \/\/\/
        // name = name +, -, *, / expr
        {
            std::string binOp;
            if      (match(TokenType::PlusEq))  binOp = "+";
            else if (match(TokenType::MinusEq)) binOp = "-";
            else if (match(TokenType::StarEq))  binOp = "*";
            else if (match(TokenType::SlashEq)) binOp = "/";
            if (!binOp.empty()) {
                auto rhs = expr();
                auto bin = std::make_unique<BinaryExpr>();
                bin->op    = binOp;
                bin->range = SourceRange::merge(lhs->range, rhs->range);
                if (lhs->kind == Expr::Kind::Var) {
                    auto* v = static_cast<VarExpr*>(lhs.get());
                    bin->left  = std::make_unique<VarExpr>(v->name, v->range);
                    bin->right = std::move(rhs);
                    auto s = std::make_unique<AssignStmt>();
                    s->target  = std::move(lhs);
                    s->value   = std::move(bin);
                    return s;
                }
                raiseError("parser", "compound assignment target must be a simple variable",
                           lhs->range, src_);
            }
        }
        auto s = std::make_unique<ExprStmt>();
        s->range = lhs->range; s->expr = std::move(lhs);
        return s;
    }

    // scan tokens without consuming owo to detect fn assign
    bool isFnAssignHead() const {
        if (!checkAt(0, TokenType::Identifier)) return false;
        if (!checkAt(1, TokenType::LParen))     return false;
        size_t j = i_ + 2;

        auto skipParam = [&]() -> bool {
            // ident
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j;
            // : type
            if (j < toks_.size() && toks_[j].type == TokenType::Colon) {
                ++j; // skip :
                if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
                ++j; // skip type
            }
            return true;
        };

        // f() = ...
        if (j < toks_.size() && toks_[j].type == TokenType::RParen) {
            ++j;
            return j < toks_.size() && toks_[j].type == TokenType::Equal &&
                   (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
        }
        // params
        if (!skipParam()) return false;
        while (j < toks_.size() && toks_[j].type == TokenType::Comma) {
            ++j;
            if (!skipParam()) return false;
        }
        if (j >= toks_.size() || toks_[j].type != TokenType::RParen) return false;
        ++j;
        // -> type
        if (j < toks_.size() && toks_[j].type == TokenType::RArrow) {
            ++j; // skip ->
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j; // skip type
        }
        return j < toks_.size() && toks_[j].type == TokenType::Equal &&
               (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
    }

    // IDENT (, IDENT)+ = (not ==) at least one comma required, otherwise this is just the ordinary single-variable assignment path in stmt()
    bool isMultiAssignHead() const {
        if (!checkAt(0, TokenType::Identifier)) return false;
        size_t j = i_ + 1;
        if (j >= toks_.size() || toks_[j].type != TokenType::Comma) return false;
        while (j < toks_.size() && toks_[j].type == TokenType::Comma) {
            ++j;
            if (j >= toks_.size() || toks_[j].type != TokenType::Identifier) return false;
            ++j;
        }
        return j < toks_.size() && toks_[j].type == TokenType::Equal &&
               (j+1 >= toks_.size() || toks_[j+1].type != TokenType::Equal);
    }

    // a, b, c = expr
    StmtPtr parseMultiAssign() {
        SourceRange start = tr(peek());
        std::vector<std::string> names;
        names.push_back(consume(TokenType::Identifier, "expected variable name").text);
        while (match(TokenType::Comma))
            names.push_back(consume(TokenType::Identifier, "expected variable name").text);
        consume(TokenType::Equal, "expected '='");
        auto e = expr();
        auto s = std::make_unique<MultiAssignStmt>();
        s->names = std::move(names);
        s->range = SourceRange::merge(start, e->range);
        s->value = std::move(e);
        return s;
    }

    // name(params) = expr
    //  \/\/\/
    // fn name(params) return expr end
    StmtPtr parseFnAssign() {
        Token nameTok = toks_[i_++];
        SourceRange start = tr(nameTok);
        consume(TokenType::LParen, "expected '('");
        auto params = parseParamList();
        consume(TokenType::RParen, "expected ')'");
        TypeSet retType = parseReturnAnnotation();
        consume(TokenType::Equal,  "expected '='");
        auto bodyExpr = expr();
        // wrap in return stmt
        auto ret   = std::make_unique<ReturnStmt>(); ret->range = bodyExpr->range; ret->value = std::move(bodyExpr);
        auto fn    = std::make_unique<FnStmt>();
        fn->name   = nameTok.text; fn->params = std::move(params); fn->returnType = retType;
        fn->body.push_back(std::move(ret));
        fn->range  = SourceRange::merge(start, fn->body.back()->range);
        return fn;
    }

    // fn name(params) body end
    StmtPtr parseFnBlock() {
        SourceRange start = tr(prev());
        Token name = consume(TokenType::Identifier, "expected function name after 'fn'");
        consume(TokenType::LParen, "expected '('");
        auto params = parseParamList();
        consume(TokenType::RParen, "expected ')'");
        TypeSet retType = parseReturnAnnotation();
        std::vector<StmtPtr> body;
        int savedLoopDepth = loopDepth_; loopDepth_ = 0; // break/continue can't cross a fn boundary
        while (!check(TokenType::End) && !check(TokenType::Eof)) body.push_back(stmt());
        loopDepth_ = savedLoopDepth;
        Token endTok = consume(TokenType::End, "expected 'end' to close 'fn'");
        auto fn = std::make_unique<FnStmt>();
        fn->name = name.text; fn->params = std::move(params); fn->returnType = retType;
        fn->body = std::move(body);
        fn->range = SourceRange::merge(start, tr(endTok));
        return fn;
    }

    // one target in a local declaration list [type|auto] name
    // IDENT with another IDENT means the first is type and second is the variable (local int x)
    // IDENT with no other IDENT is just the variable name untyped
    DestructureTarget parseDestructureTarget() {
        DestructureTarget t;
        if (match(TokenType::Auto)) {
            t.isAuto = true;
            t.name = consume(TokenType::Identifier, "expected variable name after 'auto'").text;
            return t;
        }
        if (check(TokenType::Identifier) && checkAt(1, TokenType::Identifier)) {
            Token typeTok = toks_[i_++];
            t.type = parseTypeName(typeTok);
        }
        t.name = consume(TokenType::Identifier, "expected variable name").text;
        return t;
    }

    // local [type] name = expr
    // local [type] name1, [type] name2, ... = expr
    // local [type|auto] [name1, name2, ...] = expr
    StmtPtr parseLocal() {
        SourceRange start = tr(prev());
        auto s = std::make_unique<LocalStmt>();

        bool bracketNext =
            check(TokenType::LBracket) ||
            (check(TokenType::Auto)       && checkAt(1, TokenType::LBracket)) ||
            (check(TokenType::Identifier) && checkAt(1, TokenType::LBracket));

        if (bracketNext) {
            s->bracketed = true;
            bool     sharedAuto = false;
            TypeSet  sharedType = TypeSet::Any();
            if (match(TokenType::Auto)) {
                sharedAuto = true;
            } else if (check(TokenType::Identifier)) {
                Token typeTok = toks_[i_++];
                sharedType = parseTypeName(typeTok);
            }
            consume(TokenType::LBracket, "expected '['");
            do {
                DestructureTarget t;
                t.name   = consume(TokenType::Identifier, "expected variable name").text;
                t.isAuto = sharedAuto;
                t.type   = sharedType;
                s->targets.push_back(std::move(t));
            } while (match(TokenType::Comma));
            consume(TokenType::RBracket, "expected ']'");
        } else {
            s->targets.push_back(parseDestructureTarget());
            while (match(TokenType::Comma))
                s->targets.push_back(parseDestructureTarget());
        }

        consume(TokenType::Equal, "expected '=' in 'local' declaration");
        auto e = expr();
        s->range = SourceRange::merge(start, e->range);
        s->value = std::move(e);
        return s;
    }

    StmtPtr parseIf() {
        SourceRange start = tr(prev());
        auto s = parseIfHead(start);
        Token endTok = consume(TokenType::End, "expected 'end' to close 'if'");
        s->range = SourceRange::merge(s->range, tr(endTok));
        return s;
    }

    // parses "cond thenBlock" plus any chained elif/else
    // but doesn't consume the closing end
    // an elif chain desugars into nested IfStmt, all sharing the single end token the outermost caller (parseIf) consumes owo
    StmtPtr parseIfHead(SourceRange start) {
        auto cond = expr();
        std::vector<StmtPtr> thenB;
        while (!check(TokenType::Elif) && !check(TokenType::Else) &&
               !check(TokenType::End)  && !check(TokenType::Eof))
            thenB.push_back(stmt());

        auto s = std::make_unique<IfStmt>();
        s->cond = std::move(cond);
        s->thenBlock = std::move(thenB);
        s->range = start;

        if (match(TokenType::Elif)) {
            SourceRange elifStart = tr(prev());
            std::vector<StmtPtr> elseB;
            elseB.push_back(parseIfHead(elifStart));
            s->elseBlock = std::move(elseB);
        } else if (match(TokenType::Else)) {
            std::vector<StmtPtr> elseB;
            while (!check(TokenType::End) && !check(TokenType::Eof)) elseB.push_back(stmt());
            s->elseBlock = std::move(elseB);
        }
        return s;
    }

    StmtPtr parseReturn() {
        SourceRange start = tr(prev());
        auto e = expr();
        auto s = std::make_unique<ReturnStmt>();
        s->range = SourceRange::merge(start, e->range); s->value = std::move(e);
        return s;
    }

    StmtPtr parseWhile() {
        SourceRange start = tr(prev());
        auto cond = expr();
        ++loopDepth_;
        std::vector<StmtPtr> body;
        while (!check(TokenType::End) && !check(TokenType::Eof)) body.push_back(stmt());
        --loopDepth_;
        Token endTok = consume(TokenType::End, "expected 'end' to close 'while'");
        auto s = std::make_unique<WhileStmt>();
        s->cond = std::move(cond); s->body = std::move(body);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    // for varName in iterable ... end
    // for keyName, varName in iterable ... end
    StmtPtr parseFor() {
        SourceRange start = tr(prev());
        Token first = consume(TokenType::Identifier, "expected loop variable after 'for'");
        std::string keyName, varName;
        if (match(TokenType::Comma)) {
            Token second = consume(TokenType::Identifier, "expected second loop variable after ','");
            keyName = first.text;
            varName = second.text;
        } else {
            varName = first.text;
        }
        consume(TokenType::In, "expected 'in' after for-loop variable(s)");
        auto iter = expr();
        ++loopDepth_;
        std::vector<StmtPtr> body;
        while (!check(TokenType::End) && !check(TokenType::Eof)) body.push_back(stmt());
        --loopDepth_;
        Token endTok = consume(TokenType::End, "expected 'end' to close 'for'");
        auto s = std::make_unique<ForStmt>();
        s->keyName = std::move(keyName); s->varName = std::move(varName);
        s->iterable = std::move(iter); s->body = std::move(body);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    // try ... catch e ... end
    StmtPtr parseTry() {
        SourceRange start = tr(prev());
        std::vector<StmtPtr> tryBlock;
        while (!check(TokenType::Catch) && !check(TokenType::Eof)) tryBlock.push_back(stmt());
        consume(TokenType::Catch, "expected 'catch' to close 'try'");
        Token var = consume(TokenType::Identifier, "expected a variable name after 'catch'");
        std::vector<StmtPtr> catchBlock;
        while (!check(TokenType::End) && !check(TokenType::Eof)) catchBlock.push_back(stmt());
        Token endTok = consume(TokenType::End, "expected 'end' to close 'try'/'catch'");
        auto s = std::make_unique<TryStmt>();
        s->tryBlock = std::move(tryBlock);
        s->catchVar = var.text;
        s->catchBlock = std::move(catchBlock);
        s->range = SourceRange::merge(start, tr(endTok));
        return s;
    }

    StmtPtr parseBreak() {
        Token tok = prev();
        if (loopDepth_ == 0)
            raiseError("parser", "'break' used outside of a loop", tr(tok), src_);
        auto s = std::make_unique<BreakStmt>();
        s->range = tr(tok);
        return s;
    }

    StmtPtr parseContinue() {
        Token tok = prev();
        if (loopDepth_ == 0)
            raiseError("parser", "'continue' used outside of a loop", tr(tok), src_);
        auto s = std::make_unique<ContinueStmt>();
        s->range = tr(tok);
        return s;
    }

    // import <expr>
    // path is a full expression (not just a string literal) so a script can
    // compute its own import path exactly like it already can for
    // load_module(), e.g. `import "plugins/" + name`
    StmtPtr parseImport() {
        SourceRange start = tr(prev());
        auto pathExpr = expr();
        auto s = std::make_unique<ImportStmt>();
        s->range = SourceRange::merge(start, pathExpr->range);
        s->pathExpr = std::move(pathExpr);
        return s;
    }

    ExprPtr expr(int minPrec = 0) {
        NestGuard guard(*this);
        ExprPtr left = unary();
        while (true) {
            if (minPrec <= 0 && check(TokenType::Or)) {
                ++i_;
                auto right = expr(1); // and binds tighter than or
                auto log = std::make_unique<LogicExpr>(Expr::Kind::Or);
                log->range = SourceRange::merge(left->range, right->range);
                log->left  = std::move(left);
                log->right = std::move(right);
                setDepth(log.get(), {log->left.get(), log->right.get()});
                left = std::move(log);
                continue;
            }
            if (minPrec <= 1 && check(TokenType::And)) {
                ++i_;
                auto right = expr(2); // and is right-associative at prec 1
                auto log = std::make_unique<LogicExpr>(Expr::Kind::And);
                log->range = SourceRange::merge(left->range, right->range);
                log->left  = std::move(left);
                log->right = std::move(right);
                setDepth(log.get(), {log->left.get(), log->right.get()});
                left = std::move(log);
                continue;
            }

            int prec = infixPrec(peek().type);
            if (prec < minPrec) break;
            Token op = toks_[i_++];
            auto bin  = std::make_unique<BinaryExpr>();
            bin->op   = op.text;
            bin->left = std::move(left);
            bin->right = expr(prec + 1);
            bin->range = SourceRange::merge(bin->left->range, bin->right->range);
            setDepth(bin.get(), {bin->left.get(), bin->right.get()});
            left = std::move(bin);
        }
        return left;
    }

    // `&target` as a call argument, the `&` already consumed. the target must be a variable or an element of one
    ExprPtr refArg(const Token& amp) {
        auto target = unary();
        const Expr* root = target.get();
        while (root->kind == Expr::Kind::Index) root = static_cast<const IndexExpr*>(root)->object.get();
        if (root->kind != Expr::Kind::Var)
            raiseError("parser", "'&' needs a variable or an element of one, like &list or &grid[i]",
                       SourceRange::merge(tr(amp), target->range), src_);
        auto r = std::make_unique<RefExpr>();
        r->range = SourceRange::merge(tr(amp), target->range);
        setDepth(r.get(), {target.get()});
        r->target = std::move(target);
        return r;
    }

    ExprPtr unary() {
        NestGuard guard(*this);
        if (match(TokenType::Minus) || match(TokenType::Bang)) {
            Token op = prev(); auto right = unary();
            auto u = std::make_unique<UnaryExpr>();
            u->op = op.text; u->right = std::move(right);
            u->range = SourceRange::merge(tr(op), u->right->range);
            setDepth(u.get(), {u->right.get()});
            return u;
        }
        auto expr_ = primary();
        // handle postfix operators: [ ... ], identifier() and .identifier
        while (true) {
            if (match(TokenType::LParen)) {
                auto call = std::make_unique<CallExpr>();
                call->callee = std::move(expr_);

                if (!check(TokenType::RParen))
                    do {
                        call->args.push_back(match(TokenType::Amp) ? refArg(prev()) : expr());
                    } while (match(TokenType::Comma));

                Token rp = consume(TokenType::RParen, "expected ')'");
                call->range = SourceRange::merge(
                    call->callee->range,
                    tr(rp)
                );
                {
                    int d = call->callee->depth;
                    for (auto& a : call->args) d = std::max(d, a->depth);
                    call->depth = d + 1;
                    if (call->depth > kMaxExprDepth)
                        raiseError("parser", "expression too deeply nested or too long (max depth " +
                                   std::to_string(kMaxExprDepth) + ")", call->range, src_);
                }

                expr_ = std::move(call);
            }
            else if (match(TokenType::LBracket)) {
                auto idx = std::make_unique<IndexExpr>();
                idx->object = std::move(expr_);
                idx->index = expr();
                Token rb = consume(TokenType::RBracket, "expected ']'");
                idx->range = SourceRange::merge(idx->object->range, tr(rb));
                setDepth(idx.get(), {idx->object.get(), idx->index.get()});
                expr_ = std::move(idx);
            }
            else if (match(TokenType::Dot)) {
                Token id = consume(TokenType::Identifier, "expected identifier after '.'");
                auto idx = std::make_unique<IndexExpr>();
                idx->object = std::move(expr_);
                // convert the identifier into a string literal expression
                auto keyExpr = std::make_unique<StringExpr>(id.text, SourceRange::fromToken(id));
                idx->index = std::move(keyExpr);
                idx->range = SourceRange::merge(idx->object->range, SourceRange::fromToken(id));
                setDepth(idx.get(), {idx->object.get(), idx->index.get()});
                expr_ = std::move(idx);
            }
            else {
                break;
            }
        }
        return expr_;
    }

    ExprPtr primary() {
        Token tok = toks_[i_++];
        SourceRange r = tr(tok);
        switch (tok.type) {
            case TokenType::Number: {
                // no decimal point = an exact int64 literal
                // defaults to float parsing if it doesn't fit
                if (tok.text.find('.') == std::string::npos) {
                    int64_t iv = 0;
                    auto [ptr, ec] = std::from_chars(tok.text.data(), tok.text.data()+tok.text.size(), iv);
                    if (ec == std::errc() && ptr == tok.text.data()+tok.text.size())
                        return std::make_unique<IntExpr>(iv, r);
                }
                double v; auto [ptr,ec] = parse_double(tok.text.data(), tok.text.data()+tok.text.size(), v);
                if (ec != std::errc()) raiseError("parser","invalid number: "+tok.text, r, src_);
                return std::make_unique<NumberExpr>(v, r);
            }
            case TokenType::String:
                return std::make_unique<StringExpr>(tok.text, r);
            case TokenType::Identifier: {
                return std::make_unique<VarExpr>(tok.text, r);
            }
            case TokenType::LParen: {
                auto e = expr(); Token rp = consume(TokenType::RParen, "expected ')'");
                e->range = SourceRange::merge(r, tr(rp)); return e;
            }
            case TokenType::LBracket: {
                auto arr = std::make_unique<ArrayExpr>();
                if (!check(TokenType::RBracket))
                    do { arr->elements.push_back(expr()); }
                    while (match(TokenType::Comma) && !check(TokenType::RBracket));
                Token rb = consume(TokenType::RBracket, "expected ']'");
                arr->range = SourceRange::merge(r, tr(rb));
                { int d = 0; for (auto& el : arr->elements) d = std::max(d, el->depth);
                  arr->depth = d + 1;
                  if (arr->depth > kMaxExprDepth)
                      raiseError("parser", "expression too deeply nested or too long (max depth " +
                                 std::to_string(kMaxExprDepth) + ")", arr->range, src_); }
                return arr;
            }
            case TokenType::LCurly: {
                auto map = std::make_unique<MapExpr>();
                if (!check(TokenType::RCurly)) {
                    do {
                        ExprPtr key;
                        if (check(TokenType::Identifier) && checkAt(1, TokenType::Colon)) {
                            Token id = toks_[i_++];
                            key = std::make_unique<StringExpr>(id.text, tr(id));
                            consume(TokenType::Colon, "expected ':'");
                        } else { key = expr(); consume(TokenType::Colon, "expected ':'"); }
                        map->entries.emplace_back(std::move(key), expr());
                    } while (match(TokenType::Comma) && !check(TokenType::RCurly));
                }
                Token rc = consume(TokenType::RCurly, "expected '}'");
                map->range = SourceRange::merge(r, tr(rc));
                { int d = 0; for (auto& kv : map->entries) d = std::max({d, kv.first->depth, kv.second->depth});
                  map->depth = d + 1;
                  if (map->depth > kMaxExprDepth)
                      raiseError("parser", "expression too deeply nested or too long (max depth " +
                                 std::to_string(kMaxExprDepth) + ")", map->range, src_); }
                return map;
            }
            // fn(params) body end
            case TokenType::Fn: {
                consume(TokenType::LParen, "expected '(' after 'fn'");
                auto params = parseParamList();
                consume(TokenType::RParen, "expected ')'");
                TypeSet retType = parseReturnAnnotation();
                auto lam = std::make_unique<LambdaExpr>();
                lam->params = std::move(params);
                lam->returnType = retType;
                int savedLoopDepth = loopDepth_; loopDepth_ = 0; // break/continue can't cross a fn boundary
                while (!check(TokenType::End) && !check(TokenType::Eof))
                    lam->ownedBody.push_back(stmt());
                loopDepth_ = savedLoopDepth;
                Token endTok = consume(TokenType::End, "expected 'end' after fn expression body");
                lam->range  = SourceRange::merge(r, tr(endTok));
                return lam;
            }
            default:
                // primary() advanced past `tok` unconditionally above; if that was the Eof sentinel,
                // put it back so the error-recovery sync() in parse() doesn't read one past the end
                // of the token vector (found by tools/fuzz: a bare `return` at end of input).
                if (tok.type == TokenType::Eof) --i_;
                raiseError("parser", "unexpected token '" + tok.text + "'", r, src_);
        }
    }

    int infixPrec(TokenType t) const {
        switch (t) {
            case TokenType::EqualEqual:   case TokenType::BangEqual:
            case TokenType::Greater:      case TokenType::Less:
            case TokenType::GreaterEqual: case TokenType::LessEqual: return 2;
            case TokenType::Plus:         case TokenType::Minus:     return 3;
            case TokenType::Star:         case TokenType::Slash:
            case TokenType::Percent:                                 return 4;

            default: return -1;
        }
    }
};

} // namespace embr

#endif // EMBR_CORE_PARSER_H
