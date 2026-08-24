// optimize.h — J8 优化器：常量折叠、常量传播、循环展开、DCE、强度削减
#pragma once

#include "ast.h"
#include "sema.h"

namespace j8 {

class Optimizer {
public:
    Optimizer(Sema* sema, CompileOptions& opts) : sema_(sema), opts_(opts) {}

    // 对所有实例的函数体执行优化
    void run(std::vector<FuncInstance*>& instances);

private:
    Sema* sema_;
    CompileOptions& opts_;

    // 常量折叠 + 强度削减（就地改写表达式）
    Expr* foldExpr(Expr* e);
    void foldFunctionBody(Stmt* s);
    // 常量传播（块内数据流）
    void propagateStmts(std::vector<StmtPtr>& stmts, std::map<std::string, ConstVal>& known);
    void propagateStmts(std::vector<StmtPtr>& stmts);
    void propagateBlock(Stmt* s, std::map<std::string, ConstVal>& known);
    void markKnownVarsInStmt(Stmt* s, const std::map<std::string, ConstVal>& known);
    // DCE
    void dceStmts(std::vector<StmtPtr>& stmts, FuncInstance* inst);
    bool isSideEffectFree(Expr* e);
    // 循环展开
    void unrollStmts(std::vector<StmtPtr>& stmts);
    void unrollLoop(Stmt* s, std::vector<StmtPtr>& stmts, size_t idx);
    bool loopHasBreakContinue(const Stmt* s, bool& found);
    bool loopModifiesVar(const Stmt* s, const std::string& var);

    // 工具
    bool isLiteral(Expr* e);
    uint64_t litValue(Expr* e);
    Expr* makeLitFromConst(Expr* orig, ConstVal c);
};

// 判断表达式是否有副作用（DCE 用）
bool exprHasSideEffects(Expr* e);

} // namespace j8
