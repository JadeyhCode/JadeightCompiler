// lexer.h — J8 词法分析器
#pragma once

#include <string>
#include <vector>

#include "common.h"

namespace j8 {

enum class Tok {
    End, Ident, Int, Float, Str, Char,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    Comma, Semicolon, Colon, Dot, Arrow, Question,
    Plus, Minus, Star, Slash, Percent,
    Amp, Pipe, Caret, Tilde, Bang, Lt, Gt, Shl, Shr,
    AmpAmp, PipePipe, EqEq, BangEq, Le, Ge,
    Assign, PlusEq, MinusEq, StarEq, SlashEq, PercentEq,
    AmpEq, PipeEq, CaretEq, ShlEq, ShrEq,
    Inc, Dec,
    KwFn, KwReturn, KwIf, KwElse, KwWhile, KwDo, KwFor, KwBreak, KwContinue,
    KwStruct, KwProtocol, KwImpl, KwComponent, KwSystem, KwExtern,
    KwConst, KwNew, KwDelete, KwNull, KwTrue, KwFalse, KwEnt, KwVoid,
    KwSizeof, KwWorld,
    Pragma,   // #pragma ...
    Err,
};

struct Token {
    Tok kind = Tok::Err;
    SourceLoc loc;
    std::string text;
    uint64_t intVal = 0;
    double floatVal = 0.0;
};

class Lexer {
public:
    explicit Lexer(const std::string& src) : src_(src) {}
    std::vector<Token> tokenize();   // 出错时返回部分 token 并置 ok_=false

    bool ok() const { return ok_; }

private:
    std::string src_;
    size_t pos_ = 0;
    int line_ = 1;
    int col_ = 1;
    bool ok_ = true;

    char peek(size_t off = 0) const;
    char advance();
    void skipWsAndComments();
    Token make(Tok k);
    Token lexIdentOrKeyword();
    Token lexNumber();
    Token lexString();
    Token lexChar();
};

} // namespace j8
