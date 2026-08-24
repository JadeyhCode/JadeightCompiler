// parser.h — J8 语法分析器
#pragma once

#include <set>
#include <vector>

#include "ast.h"
#include "lexer.h"

namespace j8 {

class Parser {
public:
    explicit Parser(std::vector<Token> toks) : toks_(std::move(toks)) {}

    std::unique_ptr<Program> parseProgram();
    bool ok() const { return ok_; }

private:
    std::vector<Token> toks_;
    size_t pos_ = 0;
    bool ok_ = true;

    const Token& peek(size_t off = 0) const;
    const Token& next();
    bool at(Tok k, size_t off = 0) const;
    bool accept(Tok k);
    bool expect(Tok k, const char* what);
    bool isTypeToken(const Token& t) const;

    // 声明
    DeclPtr parseFunctionOrDecl();
    DeclPtr parseStruct();
    DeclPtr parseProtocol();
    DeclPtr parseImpl();
    DeclPtr parseComponent();
    DeclPtr parseSystem();
    DeclPtr parseExtern();
    bool parseParamList(std::vector<Param>& out, bool& hasThis);
    bool parseTypeName(std::string& out, Type*& resolved);
    bool parseFieldList(std::vector<Field>& out);

    // 语句
    StmtPtr parseStmt();
    StmtPtr parseBlock();
    StmtPtr parseIf();
    StmtPtr parseWhile();
    StmtPtr parseDoWhile();
    StmtPtr parseFor();
    StmtPtr parseVarDecl(bool isConst);
    StmtPtr parseReturn();
    StmtPtr parseExprStmt();

    // 表达式（C 优先级）
    ExprPtr parseExpr();
    ExprPtr parseAssign();
    ExprPtr parseTernary();
    ExprPtr parseLogOr();
    ExprPtr parseLogAnd();
    ExprPtr parseBitOr();
    ExprPtr parseBitXor();
    ExprPtr parseBitAnd();
    ExprPtr parseEquality();
    ExprPtr parseRelational();
    ExprPtr parseShift();
    ExprPtr parseAdditive();
    ExprPtr parseMultiplicative();
    ExprPtr parseUnary();
    ExprPtr parsePostfix();
    ExprPtr applyPostfix(ExprPtr e);
    ExprPtr parsePrimary();
    ExprPtr parseCallArgs(ExprPtr callee);
    ExprPtr parseGenericCall(const Token& identTok);
    ExprPtr parseNew();
    ExprPtr parseSizeof();

    // pragma 状态（附着到下一个 for）
    bool pendingUnroll_ = false;
    int pendingUnrollFactor_ = 0;

    // 已声明的类型名（struct/protocol/component）——用于 (T)expr 转型前瞻消歧，
    // 避免把 (var) 误判为转型
    std::set<std::string> typeNames_;

    ExprPtr binary(ExprPtr lhs, Tok op, ExprPtr rhs, const SourceLoc& loc);
};

} // namespace j8
