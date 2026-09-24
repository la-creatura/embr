#ifndef EMBR_CORE_LEXER_H
#define EMBR_CORE_LEXER_H

#include "diagnostics.h"
#include "token.h"

#include <string>
#include <vector>
#include <cctype>
#include <unordered_map>

namespace embr {

class Lexer {
public:
    explicit Lexer(const std::string& src) : s_(src), map_(src) {}

    std::vector<Token> tokenize() {
        std::vector<Token> out;
        while (!eof()) {
            skipWs();
            if (eof()) break;
            char c = peek();
            if (std::isalpha((unsigned char)c) || c == '_') out.push_back(lexIdent());
            else if (std::isdigit(c))                       out.push_back(lexNum());
            else if (c == '"')                              out.push_back(lexStr());
            else                                            out.push_back(lexSym());
        }
        out.push_back({TokenType::Eof, "", line_, col_});
        return out;
    }

    const SourceMap& sourceMap() const { return map_; }

private:
    const std::string& s_;
    SourceMap          map_;
    size_t pos_  = 0;
    int    line_ = 1, col_ = 1;

    bool eof()  const { return pos_ >= s_.size(); }
    char peek() const { return eof() ? '\0' : s_[pos_]; }
    char get() {
        char c = s_[pos_++];
        if (c == '\n') { ++line_; col_ = 1; } else ++col_;
        return c;
    }

    void skipWs() {
        while (!eof()) {
            char c = peek();
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n') get();
            else if (c == '#') { while (!eof() && peek() != '\n') get(); }
            else break;
        }
    }

    Token lexIdent() {
        int sl = line_, sc = col_; std::string t;
        while (!eof() && (std::isalnum((unsigned char)peek()) || peek() == '_')) t += get();
        static const std::unordered_map<std::string, TokenType> kw = {
            {"if",TokenType::If},        {"else",TokenType::Else},    {"end",TokenType::End},
            {"fn",TokenType::Fn},        {"return",TokenType::Return},{"while",TokenType::While},
            {"import",TokenType::Import},{"local",TokenType::Local},
            {"and",TokenType::And},      {"or",TokenType::Or},
        };
        auto it = kw.find(t);
        return {it != kw.end() ? it->second : TokenType::Identifier, t, sl, sc};
    }

    Token lexNum() {
        int sl = line_, sc = col_; std::string t; bool dot = false;
        while (!eof() && (std::isdigit(peek()) || peek() == '.')) {
            if (peek() == '.') {
                if (dot) raiseError("lexer","multiple decimal points",{sl,sc,line_,col_},map_);
                dot = true;
            }
            t += get();
        }
        if (t.back() == '.')
            raiseError("lexer","trailing decimal point",{sl,sc,line_,col_},map_);
        return {TokenType::Number, t, sl, sc};
    }

    Token lexStr() {
        int sl = line_, sc = col_; get(); std::string t;
        while (!eof()) {
            char c = get();
            if (c == '"') return {TokenType::String, t, sl, sc};
            if (c == '\\') {
                if (eof()) raiseError("lexer","unterminated escape",{sl,sc,line_,col_},map_);
                switch (char nx = get()) {
                    case 'n':t+='\n';break; case 't':t+='\t';break;
                    case 'r':t+='\r';break; case '0':t+='\0';break;
                    case '"':t+='"'; break; case '\\':t+='\\';break;
                    default: raiseError("lexer",std::string("unknown escape \\")+nx,
                                        {sl,sc,line_,col_},map_);
                }
            } else t += c;
        }
        raiseError("lexer","unterminated string literal",{sl,sc,line_,col_},map_);
    }

    Token lexSym() {
        int sl = line_, sc = col_; char c = get();
        switch (c) {
            case '+': if(peek()=='='){get();return{TokenType::PlusEq,      "+=",sl,sc};} return{TokenType::Plus,   "+",sl,sc};
            case '-': if(peek()=='>'){get();return{TokenType::RArrow,      "->",sl,sc};}
                      if(peek()=='='){get();return{TokenType::MinusEq,     "-=",sl,sc};} return{TokenType::Minus,  "-",sl,sc};
            case '*': if(peek()=='='){get();return{TokenType::StarEq,      "*=",sl,sc};} return{TokenType::Star,   "*",sl,sc};
            case '/': if(peek()=='='){get();return{TokenType::SlashEq,     "/=",sl,sc};} return{TokenType::Slash,  "/",sl,sc};
            case '%': return {TokenType::Percent, "%",sl,sc};

            case '(': return {TokenType::LParen,  "(",sl,sc};
            case ')': return {TokenType::RParen,  ")",sl,sc};
            case '[': return {TokenType::LBracket,"[",sl,sc};
            case ']': return {TokenType::RBracket,"]",sl,sc};
            case '{': return {TokenType::LCurly,  "{",sl,sc};
            case '}': return {TokenType::RCurly,  "}",sl,sc};

            case ',': return {TokenType::Comma,   ",",sl,sc};
            case '.': return {TokenType::Dot,     ".",sl,sc};
            case ':': return {TokenType::Colon,   ":",sl,sc};

            case '=': if(peek()=='='){get();return{TokenType::EqualEqual,  "==",sl,sc};} return{TokenType::Equal,  "=",sl,sc};
            case '>': if(peek()=='='){get();return{TokenType::GreaterEqual,">=",sl,sc};} return{TokenType::Greater,">",sl,sc};
            case '<': if(peek()=='='){get();return{TokenType::LessEqual,   "<=",sl,sc};} return{TokenType::Less,   "<",sl,sc};
            case '!': if(peek()=='='){get();return{TokenType::BangEqual,   "!=",sl,sc};} return{TokenType::Bang,   "!",sl,sc};

        }
        raiseError("lexer", std::string("unexpected character: ")+c, {sl,sc,line_,col_}, map_);
    }
};

} // namespace embr

#endif // EMBR_CORE_LEXER_H
