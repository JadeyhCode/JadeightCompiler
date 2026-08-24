// optimize.cpp — J8 优化器实现
#include "optimize.h"

#include <algorithm>
#include <functional>

namespace j8 {

// ==================== AST 深拷贝 ====================
Expr* cloneExpr(const Expr* e) {
    if (!e) return nullptr;
    auto* n = new Expr(e->kind, e->loc);
    n->type = e->type;
    n->lvalue = e->lvalue;
    n->intVal = e->intVal;
    n->floatVal = e->floatVal;
    n->strVal = e->strVal;
    n->name = e->name;
    n->varId = e->varId;
    n->unOp = e->unOp;
    n->binOp = e->binOp;
    n->asOp = e->asOp;
    n->lhs = ExprPtr(cloneExpr(e->lhs.get()));
    n->rhs = ExprPtr(cloneExpr(e->rhs.get()));
    n->cond = ExprPtr(cloneExpr(e->cond.get()));
    n->thenExpr = ExprPtr(cloneExpr(e->thenExpr.get()));
    n->elseExpr = ExprPtr(cloneExpr(e->elseExpr.get()));
    for (auto& a : e->args) n->args.push_back(ExprPtr(cloneExpr(a.get())));
    n->callee = e->callee;
    n->tmplName = e->tmplName;
    n->postfix = e->postfix;
    n->compIdx = e->compIdx;
    n->constVal = e->constVal;
    return n;
}

Stmt* cloneStmt(const Stmt* s) {
    if (!s) return nullptr;
    auto* n = new Stmt(s->kind, s->loc);
    for (auto& b : s->body) n->body.push_back(StmtPtr(cloneStmt(b.get())));
    n->single = StmtPtr(cloneStmt(s->single.get()));
    n->cond = ExprPtr(cloneExpr(s->cond.get()));
    n->init = ExprPtr(cloneExpr(s->init.get()));
    if (s->forVar) {
        n->forVar = std::make_unique<VarDeclStmt>();
        n->forVar->name = s->forVar->name;
        n->forVar->typeName = s->forVar->typeName;
        n->forVar->type = s->forVar->type;
        n->forVar->init = ExprPtr(cloneExpr(s->forVar->init.get()));
        n->forVar->isConst = s->forVar->isConst;
        n->forVar->varId = s->forVar->varId;
        for (auto& fi : s->forVar->fieldInits) n->forVar->fieldInits.push_back(ExprPtr(cloneExpr(fi.get())));
    }
    n->step = ExprPtr(cloneExpr(s->step.get()));
    n->expr = ExprPtr(cloneExpr(s->expr.get()));
    if (s->varDecl) {
        n->varDecl = std::make_unique<VarDeclStmt>();
        n->varDecl->name = s->varDecl->name;
        n->varDecl->typeName = s->varDecl->typeName;
        n->varDecl->type = s->varDecl->type;
        n->varDecl->init = ExprPtr(cloneExpr(s->varDecl->init.get()));
        n->varDecl->isConst = s->varDecl->isConst;
        n->varDecl->varId = s->varDecl->varId;
        for (auto& fi : s->varDecl->fieldInits) n->varDecl->fieldInits.push_back(ExprPtr(cloneExpr(fi.get())));
    }
    n->hasUnrollPragma = s->hasUnrollPragma;
    n->unrollFactor = s->unrollFactor;
    n->loopId = s->loopId;
    return n;
}

// ==================== 工具 ====================
bool exprHasSideEffects(Expr* e) {
    if (!e) return false;
    switch (e->kind) {
        case ExprKind::Call: case ExprKind::MethodCall: case ExprKind::GenericCall:
        case ExprKind::New: case ExprKind::EcsCall: case ExprKind::BuiltinCall:
        case ExprKind::Assign: case ExprKind::CompoundAssign: case ExprKind::IncDec:
            return true;
        default:
            return exprHasSideEffects(e->lhs.get()) || exprHasSideEffects(e->rhs.get()) ||
                   exprHasSideEffects(e->cond.get()) || exprHasSideEffects(e->thenExpr.get()) ||
                   exprHasSideEffects(e->elseExpr.get());
    }
}

bool Optimizer::isLiteral(Expr* e) {
    return e && (e->kind == ExprKind::IntLit || e->kind == ExprKind::FloatLit ||
                 e->kind == ExprKind::BoolLit || e->kind == ExprKind::CharLit ||
                 e->kind == ExprKind::NullLit);
}

uint64_t Optimizer::litValue(Expr* e) {
    if (!e) return 0;
    if (e->kind == ExprKind::FloatLit) return static_cast<uint64_t>(static_cast<int64_t>(e->floatVal));
    return e->intVal;
}

Expr* Optimizer::makeLitFromConst(Expr* orig, ConstVal c) {
    if (c.isFloat || (orig->type && orig->type->isFloat())) {
        auto* n = new Expr(ExprKind::FloatLit, orig->loc);
        n->floatVal = c.isFloat ? c.f : static_cast<double>(static_cast<int64_t>(c.i));
        n->type = orig->type;
        n->constVal = ConstVal::Float(n->floatVal);
        return n;
    }
    auto* n = new Expr(ExprKind::IntLit, orig->loc);
    n->intVal = c.i;
    n->type = orig->type;
    n->constVal = c;
    return n;
}

// ==================== 常量折叠 + 强度削减 ====================
Expr* Optimizer::foldExpr(Expr* e) {
    if (!e) return nullptr;
    if (e->lhs) { Expr* _r = foldExpr(e->lhs.get()); if (_r != e->lhs.get()) e->lhs = ExprPtr(_r); };
    if (e->rhs) { Expr* _r = foldExpr(e->rhs.get()); if (_r != e->rhs.get()) e->rhs = ExprPtr(_r); };
    if (e->cond) { Expr* _r = foldExpr(e->cond.get()); if (_r != e->cond.get()) e->cond = ExprPtr(_r); };
    if (e->thenExpr) { Expr* _r = foldExpr(e->thenExpr.get()); if (_r != e->thenExpr.get()) e->thenExpr = ExprPtr(_r); };
    if (e->elseExpr) { Expr* _r = foldExpr(e->elseExpr.get()); if (_r != e->elseExpr.get()) e->elseExpr = ExprPtr(_r); };
    for (auto& a : e->args) { Expr* _r = foldExpr(a.get()); if (_r != a.get()) a = ExprPtr(_r); };

    // 常量折叠
    if (e->kind == ExprKind::Binary || e->kind == ExprKind::Unary ||
        e->kind == ExprKind::Ternary || e->kind == ExprKind::Cast) {
        e->constVal = evalConst(e);
        if (e->constVal && e->constVal->valid) {
            return makeLitFromConst(e, *e->constVal);
        }
    }

    // 强度削减（无符号整数，且乘数/除数为 2 的幂）
    if (e->kind == ExprKind::Binary && e->type && e->type->isInt() && !e->type->isSigned()) {
        if (e->binOp == BinaryOp::Mul && isLiteral(e->rhs.get()) && !(e->lhs->constVal && e->lhs->constVal->valid)) {
            uint64_t c = litValue(e->rhs.get());
            if (c != 0 && (c & (c - 1)) == 0) {
                int shift = 0;
                while ((1ULL << shift) < c) ++shift;
                auto* n = new Expr(ExprKind::Binary, e->loc);
                n->binOp = BinaryOp::Shl;
                n->type = e->type;
                n->lhs = std::move(e->lhs);
                auto* k = new Expr(ExprKind::IntLit, e->loc);
                k->intVal = static_cast<uint64_t>(shift);
                k->type = e->type;
                n->rhs = ExprPtr(k);
                return n;
            }
        }
        if (e->binOp == BinaryOp::Div && isLiteral(e->rhs.get()) && !(e->lhs->constVal && e->lhs->constVal->valid)) {
            uint64_t c = litValue(e->rhs.get());
            if (c != 0 && (c & (c - 1)) == 0) {
                int shift = 0;
                while ((1ULL << shift) < c) ++shift;
                auto* n = new Expr(ExprKind::Binary, e->loc);
                n->binOp = BinaryOp::Shr;
                n->type = e->type;
                n->lhs = std::move(e->lhs);
                auto* k = new Expr(ExprKind::IntLit, e->loc);
                k->intVal = static_cast<uint64_t>(shift);
                k->type = e->type;
                n->rhs = ExprPtr(k);
                return n;
            }
        }
        if (e->binOp == BinaryOp::Mod && isLiteral(e->rhs.get()) && !(e->lhs->constVal && e->lhs->constVal->valid)) {
            uint64_t c = litValue(e->rhs.get());
            if (c != 0 && (c & (c - 1)) == 0) {
                auto* n = new Expr(ExprKind::Binary, e->loc);
                n->binOp = BinaryOp::And;
                n->type = e->type;
                n->lhs = std::move(e->lhs);
                auto* k = new Expr(ExprKind::IntLit, e->loc);
                k->intVal = c - 1;
                k->type = e->type;
                n->rhs = ExprPtr(k);
                return n;
            }
        }
    }
    return e;
}

// 用已知常量标记 VarRef（后续 foldExpr 会转成字面量）
static void markKnownVars(Expr* e, const std::map<std::string, ConstVal>& known) {
    if (!e) return;
    if (e->kind == ExprKind::VarRef) {
        auto it = known.find(e->name);
        if (it != known.end()) e->constVal = it->second;
        return;
    }
    markKnownVars(e->lhs.get(), known);
    markKnownVars(e->rhs.get(), known);
    markKnownVars(e->cond.get(), known);
    markKnownVars(e->thenExpr.get(), known);
    markKnownVars(e->elseExpr.get(), known);
    for (auto& a : e->args) markKnownVars(a.get(), known);
}

// 把 constVal 有效的 VarRef 替换成字面量节点（unroll 的逐迭代常量替换；
// 仅标记 constVal 不够——codegen 仍会读槽，而展开后槽未初始化）
static Expr* substVarConsts(Expr* e) {
    if (!e) return nullptr;
    if (e->kind == ExprKind::VarRef && e->constVal && e->constVal->valid) {
        auto* n = new Expr(ExprKind::IntLit, e->loc);
        n->intVal = e->constVal->i;
        n->type = e->type;
        n->constVal = e->constVal;
        return n;
    }
    if (e->lhs) { Expr* r = substVarConsts(e->lhs.get()); if (r != e->lhs.get()) e->lhs = ExprPtr(r); }
    if (e->rhs) { Expr* r = substVarConsts(e->rhs.get()); if (r != e->rhs.get()) e->rhs = ExprPtr(r); }
    if (e->cond) { Expr* r = substVarConsts(e->cond.get()); if (r != e->cond.get()) e->cond = ExprPtr(r); }
    if (e->thenExpr) { Expr* r = substVarConsts(e->thenExpr.get()); if (r != e->thenExpr.get()) e->thenExpr = ExprPtr(r); }
    if (e->elseExpr) { Expr* r = substVarConsts(e->elseExpr.get()); if (r != e->elseExpr.get()) e->elseExpr = ExprPtr(r); }
    for (auto& a : e->args) { Expr* r = substVarConsts(a.get()); if (r != a.get()) a = ExprPtr(r); }
    return e;
}

// 语句内做常量替换（unroll 用）
static void substVarsInStmt(Stmt* s) {
    if (!s) return;
    if (s->expr) { Expr* r = substVarConsts(s->expr.get()); if (r != s->expr.get()) s->expr = ExprPtr(r); }
    if (s->cond) { Expr* r = substVarConsts(s->cond.get()); if (r != s->cond.get()) s->cond = ExprPtr(r); }
    if (s->init) { Expr* r = substVarConsts(s->init.get()); if (r != s->init.get()) s->init = ExprPtr(r); }
    if (s->step) { Expr* r = substVarConsts(s->step.get()); if (r != s->step.get()) s->step = ExprPtr(r); }
    if (s->varDecl && s->varDecl->init) { Expr* r = substVarConsts(s->varDecl->init.get()); if (r != s->varDecl->init.get()) s->varDecl->init = ExprPtr(r); }
    if (s->forVar && s->forVar->init) { Expr* r = substVarConsts(s->forVar->init.get()); if (r != s->forVar->init.get()) s->forVar->init = ExprPtr(r); }
    for (auto& b : s->body) substVarsInStmt(b.get());
    substVarsInStmt(s->single.get());
}

// 清除表达式内所有 VarRef 的 constVal 标记（循环条件/步进禁止用传播常量折叠）
static void stripVarConsts(Expr* e) {
    if (!e) return;
    if (e->kind == ExprKind::VarRef) { e->constVal.reset(); return; }
    stripVarConsts(e->lhs.get());
    stripVarConsts(e->rhs.get());
    stripVarConsts(e->cond.get());
    stripVarConsts(e->thenExpr.get());
    stripVarConsts(e->elseExpr.get());
    for (auto& a : e->args) stripVarConsts(a.get());
}

void Optimizer::foldFunctionBody(Stmt* s) {
    if (!s) return;
    if (s->kind == StmtKind::ExprStmt && s->expr) {
        { Expr* _r = foldExpr(s->expr.get()); if (_r != s->expr.get()) s->expr = ExprPtr(_r); };
    } else if (s->kind == StmtKind::Return && s->expr) {
        { Expr* _r = foldExpr(s->expr.get()); if (_r != s->expr.get()) s->expr = ExprPtr(_r); };
    } else if (s->kind == StmtKind::VarDecl && s->varDecl) {
        if (s->varDecl->init) { Expr* _r = foldExpr(s->varDecl->init.get()); if (_r != s->varDecl->init.get()) s->varDecl->init = ExprPtr(_r); };
        for (auto& fi : s->varDecl->fieldInits) { Expr* _r = foldExpr(fi.get()); if (_r != fi.get()) fi = ExprPtr(_r); };
    } else if (s->kind == StmtKind::If) {
        if (s->cond) { Expr* _r = foldExpr(s->cond.get()); if (_r != s->cond.get()) s->cond = ExprPtr(_r); };
        foldFunctionBody(s->single.get());
        for (auto& b : s->body) foldFunctionBody(b.get());
    } else if (s->kind == StmtKind::While || s->kind == StmtKind::DoWhile) {
        if (s->cond) { Expr* _r = foldExpr(s->cond.get()); if (_r != s->cond.get()) s->cond = ExprPtr(_r); };
        foldFunctionBody(s->single.get());
    } else if (s->kind == StmtKind::For) {
        if (s->init) { Expr* _r = foldExpr(s->init.get()); if (_r != s->init.get()) s->init = ExprPtr(_r); };
        if (s->forVar && s->forVar->init) { Expr* _r = foldExpr(s->forVar->init.get()); if (_r != s->forVar->init.get()) s->forVar->init = ExprPtr(_r); };
        if (s->cond) { Expr* _r = foldExpr(s->cond.get()); if (_r != s->cond.get()) s->cond = ExprPtr(_r); };
        if (s->step) { Expr* _r = foldExpr(s->step.get()); if (_r != s->step.get()) s->step = ExprPtr(_r); };
        foldFunctionBody(s->single.get());
    } else if (s->kind == StmtKind::Block) {
        for (auto& b : s->body) foldFunctionBody(b.get());
    }
}

// ==================== 常量传播 ====================
void Optimizer::propagateBlock(Stmt* s, std::map<std::string, ConstVal>& known) {
    if (!s) return;
    if (s->kind == StmtKind::Block) {
        propagateStmts(s->body, known);
        return;
    }
    // 单语句分支：把单语句包成块处理，结果回写
    std::vector<StmtPtr> tmp;
    tmp.push_back(std::move(s->single));
    propagateStmts(tmp, known);
    s->single = std::move(tmp[0]);
}

void Optimizer::propagateStmts(std::vector<StmtPtr>& stmts, std::map<std::string, ConstVal>& known) {
    for (auto& s : stmts) {
        if (!s) continue;
        switch (s->kind) {
            case StmtKind::VarDecl: {
                VarDeclStmt* vd = s->varDecl.get();
                if (vd && vd->init) {
                    markKnownVars(vd->init.get(), known);
                    { Expr* _r = foldExpr(vd->init.get()); if (_r != vd->init.get()) vd->init = ExprPtr(_r); };
                    if (vd->init->constVal && vd->init->constVal->valid && vd->fieldInits.empty()) {
                        known[vd->name] = *vd->init->constVal;
                    }
                }
                break;
            }
            case StmtKind::ExprStmt: {
                Expr* ex = s->expr.get();
                if (!ex) break;
                if (ex->kind == ExprKind::Assign && ex->lhs &&
                    ex->lhs->kind == ExprKind::VarRef) {
                    markKnownVars(ex->rhs.get(), known);
                    { Expr* _r = foldExpr(ex->rhs.get()); if (_r != ex->rhs.get()) ex->rhs = ExprPtr(_r); };
                    if (ex->rhs->constVal && ex->rhs->constVal->valid) {
                        known[ex->lhs->name] = *ex->rhs->constVal;
                    } else {
                        known.erase(ex->lhs->name);
                        known.clear();   // 保守：非平凡赋值可能影响全局状态
                    }
                } else if (ex->kind == ExprKind::CompoundAssign ||
                           ex->kind == ExprKind::IncDec) {
                    markKnownVars(ex->rhs.get(), known);
                    if (ex->lhs) markKnownVars(ex->lhs.get(), known);
                    if (ex->rhs) { Expr* _r = foldExpr(ex->rhs.get()); if (_r != ex->rhs.get()) ex->rhs = ExprPtr(_r); };
                    if (ex->lhs && ex->lhs->kind == ExprKind::VarRef) known.erase(ex->lhs->name);
                    known.clear();
                } else {
                    markKnownVars(ex, known);
                    { Expr* _r = foldExpr(ex); if (_r != ex) { s->expr = ExprPtr(_r); ex = _r; } }
                    if (exprHasSideEffects(ex)) known.clear();
                }
                break;
            }
            case StmtKind::If: case StmtKind::While: case StmtKind::DoWhile:
            case StmtKind::For: {
                // 初始化只执行一次：可用传播后的已知常量折叠
                if (s->init) { markKnownVars(s->init.get(), known); { Expr* _r = foldExpr(s->init.get()); if (_r != s->init.get()) s->init = ExprPtr(_r); }; }
                if (s->forVar && s->forVar->init) { markKnownVars(s->forVar->init.get(), known); { Expr* _r = foldExpr(s->forVar->init.get()); if (_r != s->forVar->init.get()) s->forVar->init = ExprPtr(_r); }; }
                // 循环条件/步进每轮迭代都会重新求值：禁止用循环前的已知常量折叠，
                // 仅折叠不含变量的字面量子表达式（strip 掉 VarRef 上的陈旧标记后折叠）。
                if (s->cond) {
                    stripVarConsts(s->cond.get());
                    { Expr* _r = foldExpr(s->cond.get()); if (_r != s->cond.get()) s->cond = ExprPtr(_r); };
                }
                if (s->step) {
                    stripVarConsts(s->step.get());
                    { Expr* _r = foldExpr(s->step.get()); if (_r != s->step.get()) s->step = ExprPtr(_r); };
                }
                // 循环体可能执行多次，进入时的 known 在迭代间会失效：
                // 从空 known 开始（体内直写常量的传播仍是逐迭代健全的，
                // 因为非常量赋值/控制流交汇处都会清空 known）。
                std::map<std::string, ConstVal> childKnown;
                propagateBlock(s->single.get(), childKnown);
                for (auto& b : s->body) {
                    std::map<std::string, ConstVal> ck;
                    propagateStmts(b->body, ck);
                }
                known.clear();   // 循环可能改写任意变量（保守）
                break;
            }
            case StmtKind::Return: {
                if (s->expr) {
                    markKnownVars(s->expr.get(), known);
                    { Expr* _r = foldExpr(s->expr.get()); if (_r != s->expr.get()) s->expr = ExprPtr(_r); };
                }
                break;
            }
            default: break;
        }
    }
}

void Optimizer::propagateStmts(std::vector<StmtPtr>& stmts) {
    std::map<std::string, ConstVal> known;
    propagateStmts(stmts, known);
}

// ==================== DCE ====================
bool Optimizer::isSideEffectFree(Expr* e) {
    return !exprHasSideEffects(e);
}

void Optimizer::dceStmts(std::vector<StmtPtr>& stmts, FuncInstance* inst) {
    std::set<std::string> used;
    std::function<void(Expr*)> scanE = [&](Expr* e) {
        if (!e) return;
        if (e->kind == ExprKind::VarRef) used.insert(e->name);
        scanE(e->lhs.get());
        scanE(e->rhs.get());
        scanE(e->cond.get());
        scanE(e->thenExpr.get());
        scanE(e->elseExpr.get());
        for (auto& a : e->args) scanE(a.get());
    };
    std::function<void(Stmt*)> scanS = [&](Stmt* s) {
        if (!s) return;
        scanE(s->cond.get());
        scanE(s->init.get());
        scanE(s->step.get());
        scanE(s->expr.get());
        if (s->varDecl) scanE(s->varDecl->init.get());
        if (s->forVar) scanE(s->forVar->init.get());
        for (auto& b : s->body) scanS(b.get());
        if (s->single) scanS(s->single.get());
    };
    for (auto& s : stmts) scanS(s.get());

    std::vector<StmtPtr> out;
    bool unreachable = false;
    for (auto& s : stmts) {
        if (!s) continue;
        if (unreachable) { s.reset(); continue; }
        bool keep = true;
        switch (s->kind) {
            case StmtKind::VarDecl: {
                if (!used.count(s->varDecl->name) &&
                    (!s->varDecl->init || isSideEffectFree(s->varDecl->init.get())) &&
                    s->varDecl->fieldInits.empty()) {
                    keep = false;
                }
                break;
            }
            case StmtKind::ExprStmt: {
                if (s->expr && !exprHasSideEffects(s->expr.get()) &&
                    !(s->expr->constVal && s->expr->constVal->valid)) {
                    keep = false;
                }
                break;
            }
            case StmtKind::Block: {
                dceStmts(s->body, inst);
                if (s->body.empty()) keep = false;
                break;
            }
            case StmtKind::If: {
                if (s->cond && s->cond->constVal && s->cond->constVal->valid) {
                    // 条件为常量：只保留对应分支
                    bool taken = s->cond->constVal->isFloat ? s->cond->constVal->f != 0
                                                            : s->cond->constVal->i != 0;
                    if (taken) {
                        if (s->single) {
                            std::vector<StmtPtr> tmp;
                            tmp.push_back(std::move(s->single));
                            dceStmts(tmp, inst);
                            s->kind = StmtKind::Block;
                            s->body = std::move(tmp);
                            s->single = nullptr;
                            keep = true;
                        } else {
                            keep = false;
                        }
                        s->cond = nullptr;
                    } else {
                        if (!s->body.empty()) {
                            for (auto& b : s->body) dceStmts(b->body, inst);
                            s->kind = StmtKind::Block;
                            s->body = std::move(s->body);
                            s->single = nullptr;
                            s->cond = nullptr;
                            keep = true;
                        } else {
                            keep = false;
                        }
                    }
                } else {
                    if (s->single) {
                        std::vector<StmtPtr> tmp;
                        tmp.push_back(std::move(s->single));
                        dceStmts(tmp, inst);
                        s->single = tmp.empty() ? nullptr : std::move(tmp[0]);
                    }
                    for (auto& b : s->body) dceStmts(b->body, inst);
                }
                break;
            }
            case StmtKind::While: case StmtKind::DoWhile: case StmtKind::For: {
                if (s->single) {
                    std::vector<StmtPtr> tmp;
                    tmp.push_back(std::move(s->single));
                    dceStmts(tmp, inst);
                    s->single = tmp.empty() ? nullptr : std::move(tmp[0]);
                }
                for (auto& b : s->body) dceStmts(b->body, inst);
                // 条件恒假且无循环变量的 while/for：可删除
                if ((s->kind == StmtKind::While || s->kind == StmtKind::For) &&
                    s->cond && s->cond->constVal && s->cond->constVal->valid &&
                    s->cond->constVal->isFloat == false &&
                    s->cond->constVal->i == 0) {
                    keep = false;
                }
                break;
            }
            default: break;
        }
        if (s->kind == StmtKind::Return || s->kind == StmtKind::Break ||
            s->kind == StmtKind::Continue) {
            unreachable = true;
        }
        if (keep) out.push_back(std::move(s));
    }
    stmts = std::move(out);
}

// ==================== 循环展开 ====================
bool Optimizer::loopHasBreakContinue(const Stmt* s, bool& found) {
    if (!s || found) return found;
    if (s->kind == StmtKind::Break || s->kind == StmtKind::Continue) { found = true; return found; }
    if (s->kind == StmtKind::For || s->kind == StmtKind::While || s->kind == StmtKind::DoWhile) {
        return found;   // 嵌套循环不深入
    }
    for (auto& b : s->body) loopHasBreakContinue(b.get(), found);
    loopHasBreakContinue(s->single.get(), found);
    return found;
}

bool Optimizer::loopModifiesVar(const Stmt* s, const std::string& var) {
    if (!s) return false;
    if (s->kind == StmtKind::ExprStmt && s->expr) {
        Expr* e = s->expr.get();
        if (e->lhs && e->lhs->kind == ExprKind::VarRef && e->lhs->name == var &&
            (e->kind == ExprKind::Assign || e->kind == ExprKind::CompoundAssign ||
             e->kind == ExprKind::IncDec)) {
            return true;
        }
    }
    if (s->kind == StmtKind::For && s->forVar && s->forVar->name == var) return true;
    for (auto& b : s->body) if (loopModifiesVar(b.get(), var)) return true;
    if (s->single && loopModifiesVar(s->single.get(), var)) return true;
    return false;
}

// 表达式是否引用变量 var（循环展开后变量值丢失的检查用）
static bool exprUsesVar(const Expr* e, const std::string& var) {
    if (!e) return false;
    if (e->kind == ExprKind::VarRef) return e->name == var;
    return exprUsesVar(e->lhs.get(), var) || exprUsesVar(e->rhs.get(), var) ||
           exprUsesVar(e->cond.get(), var) || exprUsesVar(e->thenExpr.get(), var) ||
           exprUsesVar(e->elseExpr.get(), var);
}

// 语句是否引用变量 var（含嵌套）。
// 后续 for 若声明同名循环变量（遮蔽），其整个循环体引用的都是新变量，
// 与外层 var 无关，直接跳过（循环变量未被外部使用才能安全展开）。
static bool stmtUsesVar(const Stmt* s, const std::string& var) {
    if (!s) return false;
    if (s->kind == StmtKind::For && s->forVar && s->forVar->name == var) return false;
    if (exprUsesVar(s->cond.get(), var) || exprUsesVar(s->init.get(), var) ||
        exprUsesVar(s->step.get(), var) || exprUsesVar(s->expr.get(), var)) return true;
    if (s->varDecl && exprUsesVar(s->varDecl->init.get(), var)) return true;
    if (s->forVar && exprUsesVar(s->forVar->init.get(), var)) return true;
    for (auto& b : s->body) if (stmtUsesVar(b.get(), var)) return true;
    if (s->single && stmtUsesVar(s->single.get(), var)) return true;
    return false;
}

void Optimizer::unrollLoop(Stmt* s, std::vector<StmtPtr>& stmts, size_t idx) {
    if (s->kind != StmtKind::For) return;
    if (!s->forVar || !s->forVar->init || !s->cond || !s->step) return;
    Expr* initE = s->forVar->init.get();
    if (!isLiteral(initE)) return;
    if (initE->type && initE->type->isFloat()) return;

    // 循环变量在循环之后（同一块内）仍被引用：不能展开，否则最终值丢失
    for (size_t j = idx + 1; j < stmts.size(); ++j) {
        if (stmtUsesVar(stmts[j].get(), s->forVar->name)) return;
    }

    Expr* cond = s->cond.get();
    if (cond->kind != ExprKind::Binary) return;
    BinaryOp cop = cond->binOp;
    if (cop != BinaryOp::Lt && cop != BinaryOp::Le && cop != BinaryOp::Gt && cop != BinaryOp::Ge) return;
    if (cond->lhs->kind != ExprKind::VarRef || cond->lhs->name != s->forVar->name) return;
    Expr* boundE = cond->rhs.get();
    if (!isLiteral(boundE)) return;

    Expr* step = s->step.get();
    int64_t stride = 0;
    if (step->kind == ExprKind::IncDec && step->lhs && step->lhs->kind == ExprKind::VarRef &&
        step->lhs->name == s->forVar->name) {
        stride = (step->unOp == UnaryOp::PreInc || step->unOp == UnaryOp::PostInc) ? 1 : -1;
    } else if (step->kind == ExprKind::Binary && step->lhs &&
               step->lhs->kind == ExprKind::VarRef && step->lhs->name == s->forVar->name) {
        if (step->binOp == BinaryOp::Add && isLiteral(step->rhs.get())) {
            stride = static_cast<int64_t>(litValue(step->rhs.get()));
        } else if (step->binOp == BinaryOp::Sub && isLiteral(step->rhs.get())) {
            stride = -static_cast<int64_t>(litValue(step->rhs.get()));
        }
    } else if (step->kind == ExprKind::CompoundAssign && step->lhs &&
               step->lhs->kind == ExprKind::VarRef && step->lhs->name == s->forVar->name) {
        if (step->asOp == AssignOp::Add && isLiteral(step->rhs.get())) {
            stride = static_cast<int64_t>(litValue(step->rhs.get()));
        } else if (step->asOp == AssignOp::Sub && isLiteral(step->rhs.get())) {
            stride = -static_cast<int64_t>(litValue(step->rhs.get()));
        }
    } else if (step->kind == ExprKind::Assign && step->lhs &&
               step->lhs->kind == ExprKind::VarRef && step->lhs->name == s->forVar->name) {
        // C 风格步进：i = i + 1 / i = i - 1 / i = i++ 等
        Expr* r = step->rhs.get();
        if (r && r->kind == ExprKind::Binary && r->lhs &&
            r->lhs->kind == ExprKind::VarRef && r->lhs->name == s->forVar->name &&
            isLiteral(r->rhs.get())) {
            if (r->binOp == BinaryOp::Add) stride = static_cast<int64_t>(litValue(r->rhs.get()));
            else if (r->binOp == BinaryOp::Sub) stride = -static_cast<int64_t>(litValue(r->rhs.get()));
        } else if (r && r->kind == ExprKind::IncDec && r->lhs &&
                   r->lhs->kind == ExprKind::VarRef && r->lhs->name == s->forVar->name) {
            stride = (r->unOp == UnaryOp::PreInc || r->unOp == UnaryOp::PostInc) ? 1 : -1;
        }
    }
    if (stride == 0) return;

    int64_t c0 = static_cast<int64_t>(litValue(initE));
    int64_t c1 = static_cast<int64_t>(litValue(boundE));
    int64_t n = 0;
    if (stride > 0) {
        if (cop == BinaryOp::Lt) n = c1 > c0 ? (c1 - c0 + stride - 1) / stride : 0;
        else if (cop == BinaryOp::Le) n = c1 >= c0 ? (c1 - c0 + stride) / stride : 0;
        else if (cop == BinaryOp::Gt) n = c0 > c1 ? (c0 - c1 + stride - 1) / stride : 0;
        else n = c0 >= c1 ? (c0 - c1 + stride) / stride : 0;
    } else {
        int64_t st = -stride;
        if (cop == BinaryOp::Gt) n = c0 > c1 ? (c0 - c1 + st - 1) / st : 0;
        else if (cop == BinaryOp::Ge) n = c0 >= c1 ? (c0 - c1 + st) / st : 0;
        else if (cop == BinaryOp::Lt) n = c1 > c0 ? (c1 - c0 + st - 1) / st : 0;
        else n = c1 >= c0 ? (c1 - c0 + st) / st : 0;
    }
    if (n <= 0) {
        // 循环体不执行：保留循环变量声明（可能后续使用）
        s->kind = StmtKind::VarDecl;
        s->varDecl = std::move(s->forVar);
        s->forVar = nullptr;
        s->init = nullptr;
        s->cond = nullptr;
        s->step = nullptr;
        s->single = nullptr;
        return;
    }

    if (opts_.noUnroll) return;
    bool hasBC = false;
    loopHasBreakContinue(s->single.get(), hasBC);
    if (hasBC) return;
    if (loopModifiesVar(s->single.get(), s->forVar->name)) return;

    int limit = opts_.unrollLimit;
    if (s->hasUnrollPragma && s->unrollFactor > 0) limit = s->unrollFactor;

    if (n <= static_cast<int64_t>(limit)) {
        // 完全展开：body 复制 n 次，循环变量替换为常量
        auto* block = new Stmt(StmtKind::Block, s->loc);
        for (int64_t k = 0; k < n; ++k) {
            int64_t iv = c0 + k * stride;
            Stmt* copy = cloneStmt(s->single.get());
            std::map<std::string, ConstVal> known;
            known[s->forVar->name] = ConstVal::Int(static_cast<uint64_t>(iv));
            if (copy->kind == StmtKind::Block) {
                for (auto& st : copy->body) markKnownVarsInStmt(st.get(), known);
                for (auto& st : copy->body) substVarsInStmt(st.get());
                for (auto& st : copy->body) foldFunctionBody(st.get());
            } else {
                markKnownVarsInStmt(copy, known);
                substVarsInStmt(copy);
                foldFunctionBody(copy);
            }
            block->body.push_back(StmtPtr(copy));
        }
        s->kind = StmtKind::Block;
        s->body = std::move(block->body);
        s->single = nullptr;
        s->forVar = nullptr;
        s->init = nullptr;
        s->cond = nullptr;
        s->step = nullptr;
        delete block;
        return;
    }

    // 部分展开（#pragma unroll N）：先全展开余数，再按因子复制主体
    if (s->hasUnrollPragma && s->unrollFactor >= 2) {
        int k = s->unrollFactor;
        int64_t r = n % k;
        auto* block = new Stmt(StmtKind::Block, s->loc);

        // 余数迭代：r 份全展开（用常量替换循环变量）
        for (int64_t j = 0; j < r; ++j) {
            int64_t iv = c0 + j * stride;
            Stmt* copy = cloneStmt(s->single.get());
            std::map<std::string, ConstVal> known;
            known[s->forVar->name] = ConstVal::Int(static_cast<uint64_t>(iv));
            if (copy->kind == StmtKind::Block) {
                for (auto& st : copy->body) markKnownVarsInStmt(st.get(), known);
                for (auto& st : copy->body) substVarsInStmt(st.get());
                for (auto& st : copy->body) foldFunctionBody(st.get());
            } else {
                markKnownVarsInStmt(copy, known);
                substVarsInStmt(copy);
                foldFunctionBody(copy);
            }
            block->body.push_back(StmtPtr(copy));
        }

        // 主循环：for (i = c0 + r*stride; cond; i = i + stride*k) { body; i+=stride; ... }
        auto* newFor = new Stmt(StmtKind::For, s->loc);
        newFor->forVar = std::make_unique<VarDeclStmt>();
        newFor->forVar->name = s->forVar->name;
        newFor->forVar->typeName = s->forVar->typeName;
        newFor->forVar->type = s->forVar->type;
        auto* newInit = new Expr(ExprKind::IntLit, s->loc);
        newInit->intVal = static_cast<uint64_t>(c0 + r * stride);
        newInit->type = s->forVar->type;      // codegen 需要类型宽度
        newFor->forVar->init = ExprPtr(newInit);
        newFor->cond = ExprPtr(cloneExpr(cond));
        // 主循环步进 = i = i + stride（必须是赋值；体内已有 (k-1) 次 i+=stride，
        // 合计每轮推进 k*stride）
        auto* newStep = new Expr(ExprKind::Assign, s->loc);
        newStep->type = s->forVar->type;
        auto* sv = new Expr(ExprKind::VarRef, s->loc);
        sv->name = s->forVar->name;
        sv->type = s->forVar->type;
        newStep->lhs = ExprPtr(sv);
        auto* stepRhs = new Expr(ExprKind::Binary, s->loc);
        stepRhs->binOp = BinaryOp::Add;
        stepRhs->type = s->forVar->type;
        auto* sv2 = new Expr(ExprKind::VarRef, s->loc);
        sv2->name = s->forVar->name;
        sv2->type = s->forVar->type;
        stepRhs->lhs = ExprPtr(sv2);
        auto* sk = new Expr(ExprKind::IntLit, s->loc);
        sk->intVal = static_cast<uint64_t>(stride);
        sk->type = s->forVar->type;
        stepRhs->rhs = ExprPtr(sk);
        newStep->rhs = ExprPtr(stepRhs);
        newFor->step = ExprPtr(newStep);

        auto* mainBody = new Stmt(StmtKind::Block, s->loc);
        for (int j = 0; j < k; ++j) {
            mainBody->body.push_back(StmtPtr(cloneStmt(s->single.get())));
            if (j < k - 1) {
                auto* inc = new Stmt(StmtKind::ExprStmt, s->loc);
                auto* ca = new Expr(ExprKind::CompoundAssign, s->loc);
                ca->asOp = AssignOp::Add;
                ca->type = s->forVar->type;
                auto* v = new Expr(ExprKind::VarRef, s->loc);
                v->name = s->forVar->name;
                v->type = s->forVar->type;
                auto* dv = new Expr(ExprKind::IntLit, s->loc);
                dv->intVal = static_cast<uint64_t>(stride);
                dv->type = s->forVar->type;
                ca->lhs = ExprPtr(v);
                ca->rhs = ExprPtr(dv);
                inc->expr = ExprPtr(ca);
                mainBody->body.push_back(StmtPtr(inc));
            }
        }
        newFor->single = StmtPtr(mainBody);
        block->body.push_back(StmtPtr(newFor));

        s->kind = StmtKind::Block;
        s->body = std::move(block->body);
        s->single = nullptr;
        s->forVar = nullptr;
        s->init = nullptr;
        s->cond = nullptr;
        s->step = nullptr;
        delete block;
        return;
    }
}

void Optimizer::markKnownVarsInStmt(Stmt* s, const std::map<std::string, ConstVal>& known) {
    if (!s) return;
    if (s->kind == StmtKind::ExprStmt && s->expr) {
        markKnownVars(s->expr.get(), known);
    } else if (s->kind == StmtKind::Return && s->expr) {
        markKnownVars(s->expr.get(), known);
    } else if (s->kind == StmtKind::VarDecl && s->varDecl && s->varDecl->init) {
        markKnownVars(s->varDecl->init.get(), known);
    } else if (s->kind == StmtKind::If) {
        markKnownVars(s->cond.get(), known);
        markKnownVarsInStmt(s->single.get(), known);
        for (auto& b : s->body) markKnownVarsInStmt(b.get(), known);
    } else if (s->kind == StmtKind::While || s->kind == StmtKind::DoWhile ||
               s->kind == StmtKind::For) {
        markKnownVars(s->cond.get(), known);
        markKnownVarsInStmt(s->single.get(), known);
    } else if (s->kind == StmtKind::Block) {
        for (auto& b : s->body) markKnownVarsInStmt(b.get(), known);
    }
}

void Optimizer::unrollStmts(std::vector<StmtPtr>& stmts) {
    for (size_t i = 0; i < stmts.size(); ++i) {
        auto& s = stmts[i];
        if (!s) continue;
        if (s->kind == StmtKind::For) {
            unrollLoop(s.get(), stmts, i);
            if (s->kind == StmtKind::Block) unrollStmts(s->body);
        } else if (s->kind == StmtKind::Block) {
            unrollStmts(s->body);
        } else if (s->kind == StmtKind::If || s->kind == StmtKind::While ||
                   s->kind == StmtKind::DoWhile) {
            if (s->single) {
                std::vector<StmtPtr> tmp;
                tmp.push_back(std::move(s->single));
                unrollStmts(tmp);
                s->single = std::move(tmp[0]);
            }
            unrollStmts(s->body);
        }
    }
}

// ==================== 主入口 ====================
void Optimizer::run(std::vector<FuncInstance*>& instances) {
    if (opts_.optLevel <= 0) return;
    for (FuncInstance* inst : instances) {
        if (!inst || !inst->compileBody) continue;
        Stmt* body = inst->compileBody;
        if (body->kind != StmtKind::Block) continue;

        foldFunctionBody(body);
        if (opts_.optLevel >= 2) {
            propagateStmts(body->body);
            unrollStmts(body->body);
            foldFunctionBody(body);
            propagateStmts(body->body);
        }
        dceStmts(body->body, inst);
    }
}

} // namespace j8
