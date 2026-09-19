// sema.cpp — J8 语义分析实现
#include "sema.h"
#include "lexer.h"
#include "parser.h"

#include <algorithm>
#include <cstring>
#include <sstream>

namespace j8 {

// ==================== 常量求值（优化器共用） ====================
static uint64_t maskWidth(Type* t) {
    int w = typeWidth(t);
    if (w >= 8) return ~0ULL;
    return (1ULL << (w * 8)) - 1;
}

static ConstVal normalizeConst(ConstVal c, Type* t) {
    if (!c.valid) return c;
    if (t && t->isFloat()) {
        if (!c.isFloat) c.f = static_cast<double>(static_cast<int64_t>(c.i));
        return c;
    }
    if (t) {
        uint64_t m = maskWidth(t);
        uint64_t v = c.isFloat ? static_cast<uint64_t>(static_cast<int64_t>(c.f)) : c.i;
        v &= m;
        if (t->isSigned() && (v & (1ULL << (typeWidth(t) * 8 - 1)))) {
            // 符号扩展
            v |= ~m;
        }
        c.i = v;
        c.isFloat = false;
    }
    return c;
}

ConstVal evalConst(const Expr* e) {
    ConstVal r = ConstVal::Invalid();
    if (!e) return r;
    switch (e->kind) {
        case ExprKind::IntLit: r = ConstVal::Int(e->intVal); break;
        case ExprKind::FloatLit: r = ConstVal::Float(e->floatVal); break;
        case ExprKind::BoolLit: case ExprKind::CharLit: r = ConstVal::Int(e->intVal); break;
        case ExprKind::NullLit: r = ConstVal::Int(0); break;
        case ExprKind::Sizeof: r = ConstVal::Int(e->intVal); break;
        case ExprKind::VarRef: {
            if (e->constVal && e->constVal->valid) r = *e->constVal;   // 优化器标记的已知常量
            break;
        }
        case ExprKind::Unary: {
            ConstVal a = evalConst(e->lhs.get());
            if (!a.valid) break;
            switch (e->unOp) {
                case UnaryOp::Neg:
                    if (a.isFloat) r = ConstVal::Float(-a.f);
                    else if (e->type && e->type->isSigned()) r = ConstVal::Int(static_cast<uint64_t>(0 - static_cast<int64_t>(a.i)));
                    else r = ConstVal::Int(0 - a.i);
                    break;
                case UnaryOp::Pos: r = a; break;
                case UnaryOp::Not: r = ConstVal::Int(a.isFloat ? (a.f == 0.0 ? 1 : 0) : (a.i == 0 ? 1 : 0)); break;
                case UnaryOp::BitNot: r = ConstVal::Int(~a.i); break;
                default: r = ConstVal::Invalid(); break;
            }
            break;
        }
        case ExprKind::Binary: {
            ConstVal a = evalConst(e->lhs.get());
            ConstVal b = evalConst(e->rhs.get());
            if (!a.valid || !b.valid) break;
            bool isFloat = e->type && e->type->isFloat();
            auto iop = [&](uint64_t (*f)(uint64_t, uint64_t)) {
                r = ConstVal::Int(f(a.i, b.i));
            };
            auto fop = [&](double (*f)(double, double)) {
                double av = a.isFloat ? a.f : static_cast<double>(static_cast<int64_t>(a.i));
                double bv = b.isFloat ? b.f : static_cast<double>(static_cast<int64_t>(b.i));
                r = ConstVal::Float(f(av, bv));
            };
            switch (e->binOp) {
                case BinaryOp::Add: isFloat ? fop([](double x, double y){return x+y;}) : iop([](uint64_t x, uint64_t y){return x+y;}); break;
                case BinaryOp::Sub: isFloat ? fop([](double x, double y){return x-y;}) : iop([](uint64_t x, uint64_t y){return x-y;}); break;
                case BinaryOp::Mul: isFloat ? fop([](double x, double y){return x*y;}) : iop([](uint64_t x, uint64_t y){return x*y;}); break;
                case BinaryOp::Div: {
                    if (isFloat) fop([](double x, double y){return y==0.0?0.0:x/y;});
                    else if (b.i == 0) r = ConstVal::Int(0);
                    else if (e->type && e->type->isSigned())
                        r = ConstVal::Int(static_cast<uint64_t>(static_cast<int64_t>(a.i) / static_cast<int64_t>(b.i)));
                    else iop([](uint64_t x, uint64_t y){return x/y;});
                    break;
                }
                case BinaryOp::Mod: {
                    if (b.i == 0) r = ConstVal::Int(0);
                    else if (e->type && e->type->isSigned())
                        r = ConstVal::Int(static_cast<uint64_t>(static_cast<int64_t>(a.i) % static_cast<int64_t>(b.i)));
                    else iop([](uint64_t x, uint64_t y){return x%y;});
                    break;
                }
                case BinaryOp::Shl: iop([](uint64_t x, uint64_t y){return x << (y & 63);}); break;
                case BinaryOp::Shr: {
                    if (e->type && e->type->isSigned())
                        r = ConstVal::Int(static_cast<uint64_t>(static_cast<int64_t>(a.i) >> (b.i & 63)));
                    else iop([](uint64_t x, uint64_t y){return x >> (y & 63);});
                    break;
                }
                case BinaryOp::And: iop([](uint64_t x, uint64_t y){return x&y;}); break;
                case BinaryOp::Or: iop([](uint64_t x, uint64_t y){return x|y;}); break;
                case BinaryOp::Xor: iop([](uint64_t x, uint64_t y){return x^y;}); break;
                case BinaryOp::Lt: {
                    if (isFloat) r = ConstVal::Int(a.f < b.f ? 1 : 0);
                    else if (e->type && e->type->isSigned()) r = ConstVal::Int(static_cast<int64_t>(a.i) < static_cast<int64_t>(b.i) ? 1 : 0);
                    else r = ConstVal::Int(a.i < b.i ? 1 : 0);
                    break;
                }
                case BinaryOp::Le: {
                    if (isFloat) r = ConstVal::Int(a.f <= b.f ? 1 : 0);
                    else if (e->type && e->type->isSigned()) r = ConstVal::Int(static_cast<int64_t>(a.i) <= static_cast<int64_t>(b.i) ? 1 : 0);
                    else r = ConstVal::Int(a.i <= b.i ? 1 : 0);
                    break;
                }
                case BinaryOp::Gt: {
                    if (isFloat) r = ConstVal::Int(a.f > b.f ? 1 : 0);
                    else if (e->type && e->type->isSigned()) r = ConstVal::Int(static_cast<int64_t>(a.i) > static_cast<int64_t>(b.i) ? 1 : 0);
                    else r = ConstVal::Int(a.i > b.i ? 1 : 0);
                    break;
                }
                case BinaryOp::Ge: {
                    if (isFloat) r = ConstVal::Int(a.f >= b.f ? 1 : 0);
                    else if (e->type && e->type->isSigned()) r = ConstVal::Int(static_cast<int64_t>(a.i) >= static_cast<int64_t>(b.i) ? 1 : 0);
                    else r = ConstVal::Int(a.i >= b.i ? 1 : 0);
                    break;
                }
                case BinaryOp::Eq: {
                    if (isFloat) r = ConstVal::Int(a.f == b.f ? 1 : 0);
                    else r = ConstVal::Int(a.i == b.i ? 1 : 0);
                    break;
                }
                case BinaryOp::Ne: {
                    if (isFloat) r = ConstVal::Int(a.f != b.f ? 1 : 0);
                    else r = ConstVal::Int(a.i != b.i ? 1 : 0);
                    break;
                }
                case BinaryOp::LogAnd: r = ConstVal::Int(((a.i != 0) || (a.isFloat && a.f != 0)) && ((b.i != 0) || (b.isFloat && b.f != 0)) ? 1 : 0); break;
                case BinaryOp::LogOr: r = ConstVal::Int(((a.i != 0) || (a.isFloat && a.f != 0)) || ((b.i != 0) || (b.isFloat && b.f != 0)) ? 1 : 0); break;
            }
            break;
        }
        case ExprKind::Ternary: {
            ConstVal c = evalConst(e->cond.get());
            if (!c.valid) break;
            bool t = c.isFloat ? c.f != 0 : c.i != 0;
            r = evalConst(t ? e->thenExpr.get() : e->elseExpr.get());
            break;
        }
        case ExprKind::Cast: {
            ConstVal a = evalConst(e->lhs.get());
            if (!a.valid || !e->type) break;
            if (e->type->isFloat()) {
                double v = a.isFloat ? a.f : static_cast<double>(static_cast<int64_t>(a.i));
                r = ConstVal::Float(v);
            } else {
                uint64_t v = a.isFloat ? static_cast<uint64_t>(static_cast<int64_t>(a.f)) : a.i;
                r = ConstVal::Int(v);
            }
            break;
        }
        default: break;
    }
    return normalizeConst(r, const_cast<Type*>(e->type));
}

// ==================== Sema 实现 ====================

int Sema::sizeOf(Type* t) {
    if (!t) return 0;
    if (t->kind == TypeKind::Struct) {
        Decl* d = structs[t->name];
        return d ? d->structSize : 0;
    }
    int w = typeWidth(t);
    return w > 0 ? w : 0;
}

int Sema::structSize(Decl* d) {
    int off = 0;
    for (auto& f : d->fields) {
        f.offset = off;
        Type* ft = resolveType(f.typeName);
        off += sizeOf(ft);
    }
    d->structSize = off;
    return off;
}

Type* Sema::resolveType(const std::string& typeName, const std::map<std::string, Type*>* env) {
    // 指针
    if (!typeName.empty() && typeName.back() == '*') {
        return Type::makePtr(resolveType(typeName.substr(0, typeName.size() - 1), env));
    }
    if (typeName == "ptr") return Type::makePtr(Type::make(TypeKind::Void));
    if (typeName == "void") return Type::make(TypeKind::Void);
    if (typeName == "u8" || typeName == "bool" || typeName == "char") return Type::make(TypeKind::U8);
    if (typeName == "u16") return Type::make(TypeKind::U16);
    if (typeName == "u32") return Type::make(TypeKind::U32);
    if (typeName == "u64") return Type::make(TypeKind::U64);
    if (typeName == "i8") return Type::make(TypeKind::I8);
    if (typeName == "i16") return Type::make(TypeKind::I16);
    if (typeName == "i32") return Type::make(TypeKind::I32);
    if (typeName == "i64") return Type::make(TypeKind::I64);
    if (typeName == "f64") return Type::make(TypeKind::F64);
    if (typeName == "f32") { Diag::error({0, 0}, "f32 不支持（v1 只支持 f64），请用 f64"); return Type::err(); }
    if (typeName == "ent") return Type::make(TypeKind::Ent);
    if (env) {
        auto it = env->find(typeName);
        if (it != env->end()) return it->second;
    }
    if (structs.count(typeName)) return Type::makeStruct(typeName);
    if (protocols.count(typeName)) return Type::makeExist(typeName);
    return Type::makeGeneric(typeName);
}

bool Sema::isConforming(const std::string& type, const std::string& proto) {
    return impls.count(proto + "::" + type) > 0;
}

FuncSym* Sema::findFunc(const std::string& name) {
    auto it = functions.find(name);
    if (it != functions.end()) return &it->second;
    return nullptr;
}

Decl* Sema::findImpl(const std::string& proto, const std::string& type) {
    auto it = impls.find(proto + "::" + type);
    return it == impls.end() ? nullptr : it->second;
}

// ==================== AST 构造器（helper 函数用） ====================
namespace {
struct AB {
    SourceLoc loc{0, 0};
    ExprPtr intLit(uint64_t v) {
        auto e = std::make_unique<Expr>(ExprKind::IntLit, loc);
        e->intVal = v;
        return e;
    }
    ExprPtr floatLit(double v) {
        auto e = std::make_unique<Expr>(ExprKind::FloatLit, loc);
        e->floatVal = v;
        return e;
    }
    ExprPtr strLit(const std::string& s) {
        auto e = std::make_unique<Expr>(ExprKind::StrLit, loc);
        e->strVal = s;
        return e;
    }
    ExprPtr var(const std::string& n) {
        auto e = std::make_unique<Expr>(ExprKind::VarRef, loc);
        e->name = n;
        return e;
    }
    template <typename... A>
    ExprPtr call(const std::string& fn, A&&... args) {
        auto e = std::make_unique<Expr>(ExprKind::Call, loc);
        e->callee = fn;
        (e->args.push_back(std::forward<A>(args)), ...);
        return e;
    }
    ExprPtr bin(BinaryOp op, ExprPtr l, ExprPtr r) {
        auto e = std::make_unique<Expr>(ExprKind::Binary, loc);
        e->binOp = op;
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }
    ExprPtr neg(ExprPtr x) {
        auto e = std::make_unique<Expr>(ExprKind::Unary, loc);
        e->unOp = UnaryOp::Neg;
        e->lhs = std::move(x);
        return e;
    }
    ExprPtr assign(ExprPtr l, ExprPtr r) {
        auto e = std::make_unique<Expr>(ExprKind::Assign, loc);
        e->lhs = std::move(l);
        e->rhs = std::move(r);
        return e;
    }
    ExprPtr cast(const std::string& t, ExprPtr x) {
        auto e = std::make_unique<Expr>(ExprKind::Cast, loc);
        e->name = t;
        e->lhs = std::move(x);
        return e;
    }
    ExprPtr index(ExprPtr a, ExprPtr i) {
        auto e = std::make_unique<Expr>(ExprKind::Index, loc);
        e->lhs = std::move(a);
        e->rhs = std::move(i);
        return e;
    }
    ExprPtr member(ExprPtr o, const std::string& f) {
        auto e = std::make_unique<Expr>(ExprKind::Member, loc);
        e->lhs = std::move(o);
        e->name = f;
        e->intVal = 1; // 视为箭头（指针访问）
        return e;
    }
    StmtPtr exprStmt(ExprPtr x) {
        auto s = std::make_unique<Stmt>(StmtKind::ExprStmt, loc);
        s->expr = std::move(x);
        return s;
    }
    StmtPtr ret(ExprPtr x) {
        auto s = std::make_unique<Stmt>(StmtKind::Return, loc);
        s->expr = std::move(x);
        return s;
    }
    StmtPtr retVoid() {
        auto s = std::make_unique<Stmt>(StmtKind::Return, loc);
        return s;
    }
    StmtPtr varDecl(const std::string& t, const std::string& n, ExprPtr init) {
        auto s = std::make_unique<Stmt>(StmtKind::VarDecl, loc);
        s->varDecl = std::make_unique<VarDeclStmt>();
        s->varDecl->typeName = t;
        s->varDecl->name = n;
        s->varDecl->init = std::move(init);
        return s;
    }
    StmtPtr varDeclArray(const std::string& t, const std::string& n, int size) {
        auto s = std::make_unique<Stmt>(StmtKind::VarDecl, loc);
        s->varDecl = std::make_unique<VarDeclStmt>();
        s->varDecl->typeName = t;
        s->varDecl->name = n;
        s->varDecl->arraySize = size;
        return s;
    }
    StmtPtr ifStmt(ExprPtr c, StmtPtr thenS, StmtPtr elseS = nullptr) {
        auto s = std::make_unique<Stmt>(StmtKind::If, loc);
        s->cond = std::move(c);
        s->single = std::move(thenS);
        if (elseS) {
            auto blk = std::make_unique<Stmt>(StmtKind::Block, loc);
            blk->body.push_back(std::move(elseS));
            s->body.push_back(std::move(blk));
        }
        return s;
    }
    StmtPtr whileStmt(ExprPtr c, StmtPtr body) {
        auto s = std::make_unique<Stmt>(StmtKind::While, loc);
        s->cond = std::move(c);
        s->single = std::move(body);
        return s;
    }
    template <typename... S>
    StmtPtr block(S&&... stmts) {
        auto s = std::make_unique<Stmt>(StmtKind::Block, loc);
        (s->body.push_back(std::forward<S>(stmts)), ...);
        return s;
    }
};
} // namespace

// ==================== helper 函数构造 ====================
void Sema::buildHelpers() {
    AB ab;
    auto addFn = [&](const std::string& name, const std::string& retT,
                     const std::vector<std::pair<std::string, std::string>>& params,
                     StmtPtr body) {
        auto d = std::make_unique<Decl>(DeclKind::Function, ab.loc);
        d->name = name;
        d->retTypeName = retT;
        for (auto& p : params) {
            Param pa;
            pa.name = p.first;
            pa.typeName = p.second;
            d->params.push_back(pa);
        }
        d->body = std::move(body);
        prog_->decls.push_back(std::move(d));
    };

    // __print_u32: 打印 u32 数字（逐位）
    addFn("__print_u32", "void", {{"v", "u32"}}, ab.block(
        ab.ifStmt(ab.bin(BinaryOp::Eq, ab.var("v"), ab.intLit(0)),
                  ab.block( ab.exprStmt(ab.call("printc", ab.intLit('0'))), ab.retVoid() )),
        ab.varDeclArray("u32", "d", 16),
        ab.varDecl("u32", "n", ab.intLit(0)),
        ab.whileStmt(ab.bin(BinaryOp::Ne, ab.var("v"), ab.intLit(0)), ab.block(
            ab.varDecl("u64", "t", ab.bin(BinaryOp::Mod, ab.var("v"), ab.intLit(10))),
            ab.exprStmt(ab.assign(ab.index(ab.var("d"), ab.var("n")), ab.var("t"))),
            ab.exprStmt(ab.assign(ab.var("v"), ab.bin(BinaryOp::Div, ab.var("v"), ab.intLit(10)))),
            ab.exprStmt(ab.assign(ab.var("n"), ab.bin(BinaryOp::Add, ab.var("n"), ab.intLit(1)))))),
        ab.whileStmt(ab.bin(BinaryOp::Gt, ab.var("n"), ab.intLit(0)), ab.block(
            ab.exprStmt(ab.assign(ab.var("n"), ab.bin(BinaryOp::Sub, ab.var("n"), ab.intLit(1)))),
            ab.exprStmt(ab.call("printc", ab.bin(BinaryOp::Add, ab.intLit('0'), ab.index(ab.var("d"), ab.var("n"))))))),
        ab.retVoid()));

    // __print_u64（用源码解析构建：AST 与用户代码完全一致。
    // 此前用 AB 构建器手搭 AST，其 while 循环在 O0 下存在边界异常（n=0 时循环体仍执行一次）。）
    {
        std::string src =
            "void __print_u64(u64 v) {"
            "  if (v == 0) { printc('0'); return; }"
            "  u64 d[24];"
            "  u32 n = 0;"
            "  while (v != 0) {"
            "    u64 t = v % 10;"
            "    d[n] = t;"
            "    v = v / 10;"
            "    n = n + 1;"
            "  }"
            "  n = n - 1;"
            "  printc((u8)(d[n] + 48));"
            "  while (n > 0) {"
            "    n = n - 1;"
            "    printc((u8)(d[n] + 48));"
            "  }"
            "}";
        Lexer lex(src);
        auto toks = lex.tokenize();
        Parser parser(std::move(toks));
        auto hp = parser.parseProgram();
        if (parser.ok() && !hp->decls.empty()) {
            prog_->decls.push_back(std::move(hp->decls[0]));
        }
    }

    // __print_i64
    addFn("__print_i64", "void", {{"v", "i64"}}, ab.block(
        ab.ifStmt(ab.bin(BinaryOp::Lt, ab.var("v"), ab.intLit(0)), ab.block(
            ab.exprStmt(ab.call("printc", ab.intLit('-'))),
            ab.exprStmt(ab.assign(ab.var("v"), ab.bin(BinaryOp::Sub, ab.intLit(0), ab.var("v")))))),
        ab.exprStmt(ab.call("__print_u64", ab.cast("u64", ab.var("v")))),
        ab.retVoid()));

    // __print_f64: 定点 3 位小数
    addFn("__print_f64", "void", {{"v", "f64"}}, ab.block(
        ab.ifStmt(ab.bin(BinaryOp::Lt, ab.var("v"), ab.floatLit(0.0)), ab.block(
            ab.exprStmt(ab.call("printc", ab.intLit('-'))),
            ab.exprStmt(ab.assign(ab.var("v"), ab.bin(BinaryOp::Sub, ab.floatLit(0.0), ab.var("v")))))),
        ab.varDecl("u32", "ip", ab.cast("u32", ab.var("v"))),
        ab.exprStmt(ab.call("__print_u32", ab.var("ip"))),
        ab.exprStmt(ab.call("printc", ab.intLit('.'))),
        ab.varDecl("u32", "fp", ab.cast("u32",
            ab.bin(BinaryOp::Mul,
                   ab.bin(BinaryOp::Sub, ab.var("v"), ab.cast("f64", ab.var("ip"))),
                   ab.floatLit(1000.0)))),
        ab.ifStmt(ab.bin(BinaryOp::Lt, ab.var("fp"), ab.intLit(100)),
                  ab.exprStmt(ab.call("printc", ab.intLit('0')))),
        ab.ifStmt(ab.bin(BinaryOp::Lt, ab.var("fp"), ab.intLit(10)),
                  ab.exprStmt(ab.call("printc", ab.intLit('0')))),
        ab.exprStmt(ab.call("__print_u32", ab.var("fp"))),
        ab.retVoid()));

    // __print_str
    addFn("__print_str", "void", {{"s", "char*"}}, ab.block(
        ab.varDecl("u32", "i", ab.intLit(0)),
        ab.whileStmt(ab.bin(BinaryOp::Ne, ab.index(ab.var("s"), ab.var("i")), ab.intLit(0)), ab.block(
            ab.exprStmt(ab.call("printc", ab.index(ab.var("s"), ab.var("i")))),
            ab.exprStmt(ab.assign(ab.var("i"), ab.bin(BinaryOp::Add, ab.var("i"), ab.intLit(1)))))),
        ab.retVoid()));

    // __print_bool
    addFn("__print_bool", "void", {{"b", "u8"}}, ab.block(
        ab.ifStmt(ab.var("b"),
                  ab.exprStmt(ab.call("__print_str", ab.strLit("true"))),
                  ab.exprStmt(ab.call("__print_str", ab.strLit("false")))),
        ab.retVoid()));

    // 数值转换 helper
    addFn("__f64_from_u64", "f64", {{"v", "u64"}}, ab.block(
        ab.varDecl("u32", "hi", ab.cast("u32", ab.bin(BinaryOp::Shr, ab.var("v"), ab.intLit(32)))),
        ab.varDecl("u32", "lo", ab.cast("u32", ab.bin(BinaryOp::And, ab.var("v"), ab.intLit(0xFFFFFFFF)))),
        ab.ret(ab.bin(BinaryOp::Add,
                      ab.bin(BinaryOp::Mul, ab.cast("f64", ab.var("hi")), ab.floatLit(4294967296.0)),
                      ab.cast("f64", ab.var("lo"))))));
    addFn("__f64_from_i64", "f64", {{"v", "i64"}}, ab.block(
        ab.ifStmt(ab.bin(BinaryOp::Lt, ab.var("v"), ab.intLit(0)),
                  ab.ret(ab.neg(ab.call("__f64_from_u64", ab.cast("u64", ab.bin(BinaryOp::Sub, ab.intLit(0), ab.var("v")))))),
                  ab.ret(ab.call("__f64_from_u64", ab.cast("u64", ab.var("v")))))));
    addFn("__u64_from_f64", "u64", {{"v", "f64"}}, ab.block(
        ab.ifStmt(ab.bin(BinaryOp::Lt, ab.var("v"), ab.floatLit(4294967296.0)),
                  ab.ret(ab.cast("u64", ab.cast("u32", ab.var("v")))),
                  ab.block(
                      ab.varDecl("u32", "hi", ab.cast("u32", ab.bin(BinaryOp::Div, ab.var("v"), ab.floatLit(4294967296.0)))),
                      ab.varDecl("u32", "lo", ab.cast("u32", ab.bin(BinaryOp::Sub, ab.var("v"), ab.bin(BinaryOp::Mul, ab.cast("f64", ab.var("hi")), ab.floatLit(4294967296.0))))),
                      ab.ret(ab.bin(BinaryOp::Or, ab.bin(BinaryOp::Shl, ab.cast("u64", ab.var("hi")), ab.intLit(32)), ab.cast("u64", ab.var("lo"))))))));
    addFn("__i64_from_f64", "i64", {{"v", "f64"}}, ab.block(
        ab.ifStmt(ab.bin(BinaryOp::Ge, ab.var("v"), ab.floatLit(0.0)),
                  ab.ret(ab.cast("i64", ab.call("__u64_from_f64", ab.var("v")))),
                  ab.ret(ab.bin(BinaryOp::Sub, ab.intLit(0), ab.cast("i64", ab.call("__u64_from_f64", ab.bin(BinaryOp::Sub, ab.floatLit(0.0), ab.var("v")))))))));
}

// ==================== 建表 ====================
void Sema::buildTables() {
    // struct / component 布局先做（其他类型依赖）
    for (auto& d : prog_->decls) {
        if (d->kind == DeclKind::Struct) {
            structs[d->name] = d.get();
            structSize(d.get());
        } else if (d->kind == DeclKind::Component) {
            structs[d->name] = d.get();
            structSize(d.get());
            compIndex[d->name] = static_cast<int>(components.size());
            components.push_back(d.get());
        }
    }
    // 组件 world 布局
    layoutComponents();

    for (auto& d : prog_->decls) {
        switch (d->kind) {
            case DeclKind::Protocol:
                protocols[d->name] = d.get();
                break;
            case DeclKind::Impl:
                impls[d->protoName + "::" + d->typeName] = d.get();
                break;
            case DeclKind::System: {
                systems[d->name] = d.get();
                // system update -> 函数符号
                FuncSym sym;
                sym.name = "sys_" + d->name + "_update";
                sym.decl = d.get();
                sym.implProto = d->name;
                FuncSym* fs = &functions[sym.name];
                *fs = sym;
                break;
            }
            case DeclKind::Extern: {
                FuncSym sym;
                sym.name = d->name;
                sym.decl = d.get();
                sym.isExtern = true;
                sym.externIndex = static_cast<int>(externs.size());
                externs.push_back(d.get());
                functions[d->name] = sym;
                break;
            }            case DeclKind::Function: {
                FuncSym sym;
                sym.name = d->name;
                sym.decl = d.get();
                sym.isGeneric = !d->genericParams.empty();
                sym.genericParams = d->genericParams;
                sym.constraints = d->constraints;
                functions[d->name] = sym;
                break;
            }
            default: break;
        }
    }

    // impl 方法 -> 函数符号：impl_P_T_method
    for (auto& d : prog_->decls) {
        if (d->kind != DeclKind::Impl) continue;
        Decl* proto = protocols[d->protoName];
        if (!proto) { Diag::error(d->loc, "impl references unknown protocol '" + d->protoName + "'"); continue; }
        if (!structs.count(d->typeName) && !components.empty() &&
            !std::any_of(components.begin(), components.end(), [&](Decl* c){ return c->name == d->typeName; })) {
            // 允许组件作为 impl 类型
        }
        for (size_t i = 0; i < proto->methods.size(); ++i) {
            ProtoMethod& pm = proto->methods[i];
            std::string fname = "impl_" + d->protoName + "_" + d->typeName + "_" + pm.name;
            FuncSym sym;
            sym.name = fname;
            sym.decl = d.get();
            sym.implProto = d->protoName;
            sym.implType = d->typeName;
            sym.implMethod = pm.name;
            sym.protoMethodIndex = static_cast<int>(i);
            functions[fname] = sym;
        }
    }

    // 编译器自动注册的外部函数（new/free/memcpy 用）
    auto addAutoExtern = [&](const std::string& name, const std::string& ret,
                             const std::vector<std::string>& argTypes) {
        auto d = std::make_unique<Decl>(DeclKind::Extern, SourceLoc{0, 0});
        d->name = name;
        d->externRetType = ret;
        d->externTypes = argTypes;
        FuncSym sym;
        sym.name = name;
        sym.decl = d.get();
        sym.isExtern = true;
        sym.externIndex = static_cast<int>(externs.size());
        externs.push_back(d.get());
        functions[name] = sym;
        prog_->decls.push_back(std::move(d));
    };
    if (!functions.count("malloc")) addAutoExtern("malloc", "ptr", {"u64"});
    if (!functions.count("free")) addAutoExtern("free", "void", {"ptr"});
    if (!functions.count("memcpy")) addAutoExtern("memcpy", "void", {"ptr", "ptr", "u64"});
    if (!functions.count("dlopen")) addAutoExtern("dlopen", "ptr", {"ptr", "i32"});
    if (!functions.count("dlsym")) addAutoExtern("dlsym", "ptr", {"ptr", "ptr"});
    // 宿主 extern（j8run 内置，非 libc）：多线程 SPMD 支持 —— tid() 当前线程号、
    // shared_buf() 共享内存基址。j8run 在 manifest 注册前占位 externFn[0]/[1]，
    // 清单里的这两行 dlsym 找不到 → 跳过，恰好保留宿主占位。
    if (!functions.count("tid")) addAutoExtern("tid", "u32", {});
    if (!functions.count("shared_buf")) addAutoExtern("shared_buf", "ptr", {});

    // helper 函数（buildHelpers 已把声明加入 decls，统一注册）
    for (auto& d : prog_->decls) {
        if (d->kind != DeclKind::Function) continue;
        if (functions.count(d->name)) continue;
        FuncSym sym;
        sym.name = d->name;
        sym.decl = d.get();
        functions[d->name] = sym;
    }
}

void Sema::layoutComponents() {
    int off = worldHeader;
    for (Decl* c : components) {
        CompLayout cl;
        cl.size = c->structSize;
        cl.bitsetOff = off;
        off += gOpts.ecsCapacity;              // 每实体 1 字节 presence
        cl.dataOff = off;
        off += gOpts.ecsCapacity * c->structSize;
        compLayout[c->name] = cl;
    }
    worldBytes = off;
}

// ==================== 协议一致性 ====================
bool Sema::checkConformance() {
    bool ok = true;
    for (auto& d : prog_->decls) {
        if (d->kind != DeclKind::Impl) continue;
        Decl* proto = protocols[d->protoName];
        if (!proto) { Diag::error(d->loc, "unknown protocol '" + d->protoName + "' in impl"); ok = false; continue; }
        if (!structs.count(d->typeName)) {
            Diag::error(d->loc, "impl for unknown type '" + d->typeName + "'");
            ok = false;
            continue;
        }
        // 每个协议方法：要么 impl 提供，要么协议有默认实现
        for (auto& pm : proto->methods) {
            bool has = false;
            for (auto& im : d->implMethods) {
                if (im.first == pm.name) { has = true; break; }
            }
            if (!has && !pm.body) {
                Diag::error(d->loc, "type '" + d->typeName + "' does not implement protocol method '" +
                                    d->protoName + "." + pm.name + "'");
                ok = false;
            }
        }
        // 检查 impl 方法名是否都在协议里
        for (auto& im : d->implMethods) {
            bool found = false;
            for (auto& pm : proto->methods) {
                if (pm.name == im.first) { found = true; break; }
            }
            if (!found) {
                Diag::warn(d->loc, "impl provides method '" + im.first + "' not declared in protocol '" + d->protoName + "'");
            }
        }
    }
    return ok;
}

// ==================== 实例构建 ====================
FuncInstance* Sema::getInstance(const std::string& fnName, const std::map<std::string, Type*>& env) {
    FuncSym* sym = findFunc(fnName);
    if (!sym) return nullptr;
    // 实例键
    std::string key = fnName;
    if (!env.empty()) {
        key += "<";
        bool first = true;
        for (auto& [k, v] : env) {
            if (!first) key += ",";
            key += v->str();
            first = false;
        }
        key += ">";
    }
    auto it = sym->instances.find(key);
    if (it != sym->instances.end()) return it->second;

    // 生成新实例
    if (!buildInstance(sym, env, key)) return nullptr;
    return sym->instances[key];
}

// 取 impl 方法（或默认实现）的函数体
static Stmt* implMethodBody(Decl* impl, const std::string& method, Decl* proto) {
    for (auto& im : impl->implMethods) {
        if (im.first == method) return im.second.get();
    }
    for (auto& m : proto->methods) {
        if (m.name == method) return m.body.get();
    }
    return nullptr;
}

bool Sema::buildInstance(FuncSym* sym, const std::map<std::string, Type*>& env, const std::string& key) {
    Decl* d = sym->decl;
    if (building_.count(key)) {
        Diag::error(d->loc, "recursive generic instantiation of '" + key + "'");
        return false;
    }
    building_.insert(key);
    auto* inst = new FuncInstance;
    inst->fn = d;
    inst->key = key;
    inst->typeEnv = env;

    if (!sym->isGeneric && !env.empty()) {
        Diag::error(d->loc, "function '" + d->name + "' is not generic but called with type args");
        delete inst;
        building_.erase(key);
        return false;
    }

    if (d->kind == DeclKind::System) {
        // system update：ret void，params = [e: ent]
        inst->retType = Type::make(TypeKind::Void);
        inst->isSystemUpdate = true;
        inst->sysName = d->name;
        Type* entT = Type::make(TypeKind::Ent);
        inst->paramTypes.push_back(entT);
        inst->paramNames.push_back(d->sysUpdateParamName.empty() ? "e" : d->sysUpdateParamName);
    } else {
        if (!sym->implProto.empty()) {
            // impl 方法：返回类型取自协议方法声明（impl 声明不存返回类型）
            Decl* proto = protocols[sym->implProto];
            if (proto) {
                for (auto& m : proto->methods) {
                    if (m.name == sym->implMethod) {
                        inst->retType = resolveType(m.retTypeName, &env);
                        break;
                    }
                }
            }
        }
        if (!inst->retType) inst->retType = resolveType(d->retTypeName, &env);
        if (inst->retType->kind == TypeKind::Err) { delete inst; building_.erase(key); return false; }
        if (!sym->implProto.empty()) {
            // impl 方法：this(T*) + 协议方法参数
            Type* thisT = Type::makePtr(resolveType(sym->implType));
            inst->paramTypes.push_back(thisT);
            inst->paramNames.push_back("this");
            Decl* proto = protocols[sym->implProto];
            if (proto) {
                for (auto& m : proto->methods) {
                    if (m.name != sym->implMethod) continue;
                    for (auto& p : m.params) {
                        if (p.name == "this") continue;
                        inst->paramTypes.push_back(resolveType(p.typeName, &env));
                        inst->paramNames.push_back(p.name);
                    }
                }
            }
        } else {
            for (auto& p : d->params) {
                inst->paramTypes.push_back(resolveType(p.typeName, &env));
                inst->paramNames.push_back(p.name);
            }
        }
    }
    inst->retBytes = sizeOf(inst->retType);
    inst->isMain = (d->name == "main");

    // 参数布局 + 注册
    int off = 0;
    for (size_t i = 0; i < inst->paramTypes.size(); ++i) {
        // 结构体参数按地址传递（槽=8B 指针）
        int w = inst->paramTypes[i]->isStruct() ? 8 : sizeOf(inst->paramTypes[i]);
        if (w <= 0) w = 8;
        VarSlot vs;
        vs.offset = inst->retBytes + off;
        vs.width = w;
        vs.isParam = true;
        vs.type = inst->paramTypes[i];
        vs.name = inst->paramNames[i];
        int id = nextVarId_++;
        inst->slots[id] = vs;
        inst->paramVarIds[inst->paramNames[i]] = id;
        off += w;
    }
    inst->paramBytes = off;

    // 函数体（泛型/默认实现体被多实例共享 → 克隆，避免类型注解冲突）
    Stmt* body = nullptr;
    bool needClone = false;
    if (d->kind == DeclKind::Function) {
        body = d->body.get();
        needClone = sym->isGeneric;
    } else if (d->kind == DeclKind::Impl) {
        body = implMethodBody(d, sym->implMethod, protocols[sym->implProto]);
        // 若 impl 未定义该方法（用了协议默认体）→ 克隆
        bool defined = false;
        for (auto& im : d->implMethods) if (im.first == sym->implMethod) defined = true;
        needClone = !defined;
    } else if (d->kind == DeclKind::System) {
        for (auto& im : d->implMethods) if (im.first == "update") body = im.second.get();
    }

    // 先注册再检查函数体：直接/间接递归时，函数体内的调用能取回本实例
    // （此前在 checkBody 之后才注册，递归调用会命中 building_ 守卫而误报）
    sym->instances[key] = inst;
    instances.push_back(inst);

    if (body) {
        Stmt* toCheck = needClone ? cloneStmt(body) : body;
        inst->compileBody = toCheck;
        if (!checkBody(inst, toCheck)) {
            sym->instances.erase(key);
            instances.pop_back();
            delete inst;
            building_.erase(key);
            return false;
        }
    } else {
        inst->compileBody = nullptr;
    }

    inst->bodyChecked = true;
    building_.erase(key);
    return true;
}

// ==================== 帧布局 ====================
void Sema::layoutLocals(FuncInstance* inst) {
    // main：前 8 字节留给世界指针槽（编译器保留）
    int baseLocal = inst->isMain ? 8 : inst->retBytes + inst->paramBytes + 8;
    int cum = 0;
    // 按 varId（声明顺序）布局，跳过参数
    for (auto& [id, vs] : inst->slots) {
        if (vs.isParam) continue;
        vs.offset = baseLocal + cum;
        cum += vs.width;
    }
    inst->userLocalBytes = cum;
    // 8 个 8 字节 temp 槽（编译器用）
    inst->tempBase = baseLocal + cum;
    inst->localBytes = cum + 64;
}

VarSlot* Sema::findVar(FuncInstance* inst, const std::string& name) {
    auto it = inst->paramVarIds.find(name);
    if (it != inst->paramVarIds.end()) {
        auto it2 = inst->slots.find(it->second);
        if (it2 != inst->slots.end()) return &it2->second;
    }
    auto it3 = inst->localVarIds.find(name);
    if (it3 != inst->localVarIds.end()) {
        auto it4 = inst->slots.find(it3->second);
        if (it4 != inst->slots.end()) return &it4->second;
    }
    return nullptr;
}

// ==================== 类型转换 ====================
Type* Sema::unsignedOf(Type* t) {
    switch (t->kind) {
        case TypeKind::I8: return Type::make(TypeKind::U8);
        case TypeKind::I16: return Type::make(TypeKind::U16);
        case TypeKind::I32: return Type::make(TypeKind::U32);
        case TypeKind::I64: return Type::make(TypeKind::U64);
        default: return t;
    }
}

Type* Sema::promoteBinary(Type* a, Type* b, const SourceLoc& loc, bool& ok) {
    ok = true;
    auto norm = [](Type* t) -> Type* {
        if (!t) return Type::err();
        if (t->kind == TypeKind::Bool || t->kind == TypeKind::Char) return Type::make(TypeKind::U8);
        if (t->kind == TypeKind::Ent) return Type::make(TypeKind::U32);
        return t;
    };
    Type* na = norm(a);
    Type* nb = norm(b);
    if (na->kind == TypeKind::Err || nb->kind == TypeKind::Err) { ok = false; return Type::err(); }
    if (na->isFloat() || nb->isFloat()) return Type::make(TypeKind::F64);
    if (na->isInt() && nb->isInt()) {
        int wa = typeWidth(na), wb = typeWidth(nb);
        if (wa == wb) {
            if (na->isSigned() == nb->isSigned()) return na;
            return na->isSigned() ? nb : na;   // 有符号 + 无符号 → 无符号
        }
        return wa > wb ? na : nb;              // 宽者胜
    }
    ok = false;
    Diag::error(loc, "cannot mix operand types '" + na->str() + "' and '" + nb->str() + "'");
    return Type::err();
}

bool Sema::assignable(Type* dst, Type* src, const SourceLoc& loc) {
    if (!dst || !src) return false;
    if (dst->kind == TypeKind::Err || src->kind == TypeKind::Err) return false;
    if (dst->equals(src)) return true;
    // 数值隐式转换
    if (dst->isNumeric() && src->isNumeric()) return true;
    if (dst->kind == TypeKind::Bool && src->isNumeric()) return true;
    if (dst->isNumeric() && src->kind == TypeKind::Bool) return true;
    // 指针
    if (dst->kind == TypeKind::Ptr && src->kind == TypeKind::Ptr) {
        if (dst->pointee->kind == TypeKind::Void) return true;
        if (src->pointee->kind == TypeKind::Void) return false;
        return dst->pointee->equals(src->pointee);
    }
    if (dst->kind == TypeKind::Ptr && src->kind == TypeKind::Ent) return true; // ent -> void*
    if (dst->kind == TypeKind::Ptr && src->kind == TypeKind::U32) return true;  // 整数（0/地址）→ 指针
    // ent 视为 u32
    if (dst->kind == TypeKind::Ent && src->kind == TypeKind::U32) return true;
    if (dst->kind == TypeKind::U32 && src->kind == TypeKind::Ent) return true;
    // 具体类型 -> 存在类型（boxing）
    if (dst->kind == TypeKind::Exist) {
        if (src->kind == TypeKind::Struct) return isConforming(src->name, dst->name);
        if (src->kind == TypeKind::Ptr && src->pointee && src->pointee->kind == TypeKind::Struct)
            return isConforming(src->pointee->name, dst->name);
        if (src->kind == TypeKind::Exist) return dst->name == src->name;
    }
    // 泛型（同键）
    if (dst->kind == TypeKind::GenericParam && src->kind == TypeKind::GenericParam)
        return dst->name == src->name;
    return false;
}

// ==================== 语句检查 ====================
bool Sema::checkBody(FuncInstance* inst, Stmt* body) {
    cur_ = inst;
    bool ok = true;
    if (body) ok = checkStmt(body, inst, inst->retType) && ok;
    layoutLocals(inst);
    cur_ = nullptr;
    return ok;
}

bool Sema::checkStmt(Stmt* s, FuncInstance* inst, Type* retType) {
    if (!s) return true;
    switch (s->kind) {
        case StmtKind::Block:
            for (auto& st : s->body) checkStmt(st.get(), inst, retType);
            break;
        case StmtKind::ExprStmt:
            if (s->expr) checkExpr(s->expr.get(), inst);
            break;
        case StmtKind::VarDecl: {
            VarDeclStmt* vd = s->varDecl.get();
            Type* t = resolveType(vd->typeName, &inst->typeEnv);
            if (t->kind == TypeKind::Err) return false;
            if (findVar(inst, vd->name)) {
                Diag::error(s->loc, "redeclaration of variable '" + vd->name + "'");
                return false;
            }
            if (vd->arraySize > 0) {
                t = Type::makePtr(t);   // 数组 → 指针
            }
            vd->type = t;
            VarSlot vs;
            vs.type = t;
            vs.width = sizeOf(t);
            vs.isArray = vd->arraySize > 0;
            if (t->kind == TypeKind::Exist) vs.width = 16;
            if (t->kind == TypeKind::Ptr && vd->arraySize > 0) vs.width = vd->arraySize * sizeOf(t->pointee);
            if (vs.width <= 0) vs.width = 8;
            vs.name = vd->name;
            int id = nextVarId_++;
            inst->slots[id] = vs;
            inst->localVarIds[vd->name] = id;
            vd->varId = id;
            if (!vd->fieldInits.empty()) {
                if (!t->isStruct()) {
                    Diag::error(s->loc, "field initializer list requires struct type '" + t->str() + "'");
                    return false;
                }
                Decl* sd = structs[t->name];
                if (vd->fieldInits.size() > sd->fields.size()) {
                    Diag::error(s->loc, "too many initializers for struct '" + t->name + "'");
                    return false;
                }
                for (auto& fi : vd->fieldInits) checkExpr(fi.get(), inst);
            } else if (vd->init) {
                Type* it = checkExpr(vd->init.get(), inst);
                if (!checkAssignable(t, it, s->loc)) {
                    Diag::error(s->loc, "cannot initialize '" + vd->name + "' (" + t->str() +
                                        ") with value of type '" + it->str() + "'");
                }
            }
            break;
        }
        case StmtKind::If: {
            Type* ct = checkExpr(s->cond.get(), inst);
            if (ct && !ct->isNumeric() && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Ptr) {
                Diag::error(s->loc, "if condition must be scalar, got '" + ct->str() + "'");
            }
            checkStmt(s->single.get(), inst, retType);
            for (auto& st : s->body) checkStmt(st.get(), inst, retType);
            break;
        }
        case StmtKind::While: case StmtKind::DoWhile: {
            checkExpr(s->cond.get(), inst);
            checkStmt(s->single.get(), inst, retType);
            break;
        }
        case StmtKind::For: {
            if (s->forVar) {
                // 复用 VarDecl 逻辑
                VarDeclStmt* vd = s->forVar.get();
                Type* t = resolveType(vd->typeName, &inst->typeEnv);
                vd->type = t;
                VarSlot vs;
                vs.type = t;
                vs.width = sizeOf(t);
                if (vs.width <= 0) vs.width = 8;
                vs.name = vd->name;
                int id = nextVarId_++;
                inst->slots[id] = vs;
                inst->localVarIds[vd->name] = id;
                vd->varId = id;
                if (vd->init) {
                    Type* it = checkExpr(vd->init.get(), inst);
                    if (!checkAssignable(t, it, s->loc)) {
                        Diag::error(s->loc, "cannot initialize loop variable '" + vd->name + "'");
                    }
                }
            } else if (s->init) {
                checkExpr(s->init.get(), inst);
            }
            if (s->cond) checkExpr(s->cond.get(), inst);
            if (s->step) checkExpr(s->step.get(), inst);
            checkStmt(s->single.get(), inst, retType);
            break;
        }
        case StmtKind::Return: {
            if (s->expr) {
                Type* et = checkExpr(s->expr.get(), inst);
                if (retType->kind == TypeKind::Void) {
                    Diag::error(s->loc, "void function cannot return a value");
                } else if (!checkAssignable(retType, et, s->loc)) {
                    Diag::error(s->loc, "return type mismatch: expected '" + retType->str() +
                                        "', got '" + et->str() + "'");
                }
            } else if (retType->kind != TypeKind::Void) {
                Diag::error(s->loc, "non-void function must return a value");
            }
            break;
        }
        default: break;
    }
    return true;
}

// ==================== witness 表 ====================
void Sema::ensureWitness(const std::string& proto, const std::string& type) {
    std::string key = proto + "::" + type;
    if (witnessIndex.count(key)) return;
    Decl* p = protocols[proto];
    if (!p) return;
    WitnessInfo w;
    w.proto = proto;
    w.type = type;
    for (auto& m : p->methods) {
        FuncInstance* fi = getInstance("impl_" + proto + "_" + type + "_" + m.name, {});
        w.methodEntries.push_back(fi);
        if (!fi) {
            Diag::error(p->loc, "cannot build witness for " + proto + "::" + type + " method '" + m.name + "'");
        }
    }
    witnessIndex[key] = static_cast<int>(witnesses.size());
    witnesses.push_back(std::move(w));
}

bool Sema::checkAssignable(Type* dst, Type* src, const SourceLoc& loc) {
    bool ok = assignable(dst, src, loc);
    // boxing 钩子：具体类型 -> 存在类型时登记 witness 表
    if (dst && dst->kind == TypeKind::Exist && src && ok) {
        if (src->kind == TypeKind::Struct) {
            ensureWitness(dst->name, src->name);
        } else if (src->kind == TypeKind::Ptr && src->pointee &&
                   src->pointee->kind == TypeKind::Struct) {
            ensureWitness(dst->name, src->pointee->name);
        }
    }
    return ok;
}

// ==================== 表达式检查 ====================
Type* Sema::checkExpr(Expr* e, FuncInstance* inst) {
    if (!e) return Type::err();
    switch (e->kind) {
        case ExprKind::IntLit: {
            if (e->name == "neg") {
                e->type = (e->intVal <= 0x7FFFFFFFULL) ? Type::make(TypeKind::I32)
                                                       : Type::make(TypeKind::I64);
            } else {
                e->type = (e->intVal <= 0xFFFFFFFFULL) ? Type::make(TypeKind::U32)
                                                       : Type::make(TypeKind::U64);
            }
            break;
        }
        case ExprKind::FloatLit: e->type = Type::make(TypeKind::F64); break;
        case ExprKind::StrLit: e->type = Type::makePtr(Type::make(TypeKind::U8)); break;
        case ExprKind::BoolLit: e->type = Type::make(TypeKind::Bool); break;
        case ExprKind::CharLit: e->type = Type::make(TypeKind::Char); break;
        case ExprKind::NullLit: e->type = Type::makePtr(Type::make(TypeKind::Void)); break;
        case ExprKind::VarRef: {
            VarSlot* v = findVar(inst, e->name);
            if (!v) {
                Diag::error(e->loc, "unknown identifier '" + e->name + "'");
                e->type = Type::err();
                break;
            }
            e->type = v->type;
            e->lvalue = true;
            break;
        }
        case ExprKind::Unary: {
            Type* t = checkExpr(e->lhs.get(), inst);
            switch (e->unOp) {
                case UnaryOp::Neg:
                    if (!t->isNumeric() && t->kind != TypeKind::Ent) {
                        Diag::error(e->loc, "cannot negate '" + t->str() + "'");
                        e->type = Type::err();
                    } else e->type = t;
                    break;
                case UnaryOp::Pos: e->type = t; break;
                case UnaryOp::Not: e->type = Type::make(TypeKind::Bool); break;
                case UnaryOp::BitNot:
                    if (!t->isInt()) {
                        Diag::error(e->loc, "bitwise not requires integer operand");
                        e->type = Type::err();
                    } else e->type = t;
                    break;
                case UnaryOp::Deref:
                    if (t->kind == TypeKind::Ptr) {
                        e->type = t->pointee;
                        e->lvalue = true;
                    } else {
                        Diag::error(e->loc, "cannot dereference non-pointer '" + t->str() + "'");
                        e->type = Type::err();
                    }
                    break;
                case UnaryOp::AddrOf:
                    if (!e->lhs->lvalue) {
                        Diag::error(e->loc, "cannot take address of non-lvalue");
                        e->type = Type::err();
                    } else {
                        e->type = Type::makePtr(t);
                        e->lvalue = false;
                    }
                    break;
                case UnaryOp::PreInc: case UnaryOp::PreDec:
                case UnaryOp::PostInc: case UnaryOp::PostDec:
                    if (!e->lhs->lvalue) {
                        Diag::error(e->loc, "inc/dec target is not assignable");
                        e->type = Type::err();
                    } else if (!t->isNumeric() && t->kind != TypeKind::Ptr && t->kind != TypeKind::Ent) {
                        Diag::error(e->loc, "cannot inc/dec '" + t->str() + "'");
                        e->type = Type::err();
                    } else e->type = t;
                    break;
            }
            break;
        }
        case ExprKind::Binary: {
            Type* lt = checkExpr(e->lhs.get(), inst);
            Type* rt = checkExpr(e->rhs.get(), inst);
            switch (e->binOp) {
                case BinaryOp::LogAnd: case BinaryOp::LogOr:
                    e->type = Type::make(TypeKind::Bool);
                    break;
                case BinaryOp::Lt: case BinaryOp::Le: case BinaryOp::Gt: case BinaryOp::Ge:
                case BinaryOp::Eq: case BinaryOp::Ne: {
                    bool num = lt->isNumeric() && rt->isNumeric();
                    bool ptr = lt->kind == TypeKind::Ptr && rt->kind == TypeKind::Ptr;
                    if (num || ptr) e->type = Type::make(TypeKind::Bool);
                    else {
                        Diag::error(e->loc, "cannot compare '" + lt->str() + "' and '" + rt->str() + "'");
                        e->type = Type::err();
                    }
                    break;
                }
                case BinaryOp::Add: case BinaryOp::Sub: {
                    if (lt->isNumeric() && rt->isNumeric()) {
                        bool ok; e->type = promoteBinary(lt, rt, e->loc, ok);
                    } else if (lt->kind == TypeKind::Ptr && rt->isInt()) {
                        e->type = lt;
                    } else if (lt->isInt() && rt->kind == TypeKind::Ptr) {
                        e->type = rt;
                    } else if (e->binOp == BinaryOp::Sub && lt->kind == TypeKind::Ptr &&
                               rt->kind == TypeKind::Ptr) {
                        e->type = Type::make(TypeKind::U64);
                    } else {
                        Diag::error(e->loc, "invalid operands to +/-: '" + lt->str() + "' and '" + rt->str() + "'");
                        e->type = Type::err();
                    }
                    break;
                }
                case BinaryOp::Mul: case BinaryOp::Div: {
                    if (lt->isNumeric() && rt->isNumeric()) {
                        bool ok; e->type = promoteBinary(lt, rt, e->loc, ok);
                    } else {
                        Diag::error(e->loc, "invalid operands to * or /");
                        e->type = Type::err();
                    }
                    break;
                }
                case BinaryOp::Mod: {
                    if (lt->isInt() && rt->isInt()) {
                        bool ok; e->type = promoteBinary(lt, rt, e->loc, ok);
                    } else {
                        Diag::error(e->loc, "% requires integer operands");
                        e->type = Type::err();
                    }
                    break;
                }
                case BinaryOp::Shl: case BinaryOp::Shr:
                case BinaryOp::And: case BinaryOp::Or: case BinaryOp::Xor: {
                    if (lt->isInt() && rt->isInt()) {
                        bool ok; e->type = promoteBinary(lt, rt, e->loc, ok);
                    } else {
                        Diag::error(e->loc, "bitwise operator requires integer operands");
                        e->type = Type::err();
                    }
                    break;
                }
            }
            break;
        }
        case ExprKind::Ternary: {
            Type* ct = checkExpr(e->cond.get(), inst);
            Type* at = checkExpr(e->thenExpr.get(), inst);
            Type* bt = checkExpr(e->elseExpr.get(), inst);
            if (ct && !ct->isNumeric() && ct->kind != TypeKind::Bool && ct->kind != TypeKind::Ptr) {
                Diag::error(e->loc, "ternary condition must be scalar");
            }
            if (at->isNumeric() && bt->isNumeric()) {
                bool ok; e->type = promoteBinary(at, bt, e->loc, ok);
            } else if (at->equals(bt)) {
                e->type = at;
            } else {
                Diag::error(e->loc, "ternary branches have incompatible types");
                e->type = Type::err();
            }
            break;
        }
        case ExprKind::Index: {
            Type* at = checkExpr(e->lhs.get(), inst);
            checkExpr(e->rhs.get(), inst);
            if (at->kind != TypeKind::Ptr) {
                Diag::error(e->loc, "cannot index non-pointer '" + at->str() + "'");
                e->type = Type::err();
            } else {
                e->type = at->pointee;
                e->lvalue = true;
            }
            break;
        }
        case ExprKind::Member: {
            Type* ot = checkExpr(e->lhs.get(), inst);
            Type* base = ot;
            if (base->kind == TypeKind::Ptr && base->pointee &&
                base->pointee->kind == TypeKind::Struct) {
                base = base->pointee;
            }
            if (base->kind == TypeKind::Struct) {
                Decl* sd = structs[base->name];
                bool found = false;
                if (sd) {
                    for (auto& f : sd->fields) {
                        if (f.name == e->name) {
                            e->type = resolveType(f.typeName);
                            e->lvalue = true;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    Diag::error(e->loc, "no field '" + e->name + "' in struct '" + base->name + "'");
                    e->type = Type::err();
                }
            } else if (base->kind == TypeKind::Exist) {
                Diag::error(e->loc, "protocol member '" + e->name + "' must be called");
                e->type = Type::err();
            } else if (base->kind == TypeKind::Ent) {
                // ECS sugar：当前 system 的组件字段
                if (inst->sysName.empty()) {
                    Diag::error(e->loc, "entity field access only allowed inside a system update");
                    e->type = Type::err();
                    break;
                }
                Decl* sys = systems[inst->sysName];
                bool found = false;
                if (sys) {
                    for (auto& cn : sys->componentNames) {
                        Decl* cd = structs[cn];
                        if (!cd) continue;
                        for (auto& f : cd->fields) {
                            if (f.name == e->name) {
                                if (found) {
                                    Diag::error(e->loc, "ambiguous component field '" + e->name + "'");
                                }
                                e->compIdx = compIndex[cn];
                                e->type = resolveType(f.typeName);
                                e->lvalue = true;
                                found = true;
                            }
                        }
                    }
                }
                if (!found) {
                    Diag::error(e->loc, "no component field '" + e->name + "' in system '" +
                                        inst->sysName + "'");
                    e->type = Type::err();
                }
            } else {
                Diag::error(e->loc, "cannot access member of type '" + base->str() + "'");
                e->type = Type::err();
            }
            break;
        }
        case ExprKind::Call: {
            e->type = checkCall(e, inst);
            break;
        }
        case ExprKind::MethodCall: {
            e->type = checkMethodCall(e, inst);
            break;
        }
        case ExprKind::GenericCall: {
            const std::string& name = e->tmplName;
            if (name == "get" || name == "has" || name == "add" || name == "remove" || name == "run") {
                e->type = checkEcsCall(e, inst, name, e->name);
                break;
            }
            FuncSym* sym = findFunc(name);
            if (!sym || !sym->isGeneric) {
                Diag::error(e->loc, "unknown generic function '" + name + "'");
                e->type = Type::err();
                break;
            }
            if (sym->genericParams.size() != 1) {
                Diag::error(e->loc, "this version supports single generic parameter");
                e->type = Type::err();
                break;
            }
            Type* targ = resolveType(e->name, &inst->typeEnv);
            if (targ->kind == TypeKind::Err) { e->type = Type::err(); break; }
            for (auto& [p, proto] : sym->constraints) {
                if (p == sym->genericParams[0]) {
                    if (targ->kind != TypeKind::Struct || !isConforming(targ->name, proto)) {
                        Diag::error(e->loc, "type '" + targ->str() + "' does not conform to protocol '" +
                                            proto + "'");
                    }
                }
            }
            std::map<std::string, Type*> env;
            env[sym->genericParams[0]] = targ;
            FuncInstance* callee = getInstance(name, env);
            if (!callee) { e->type = Type::err(); break; }
            if (e->args.size() != callee->paramTypes.size()) {
                Diag::error(e->loc, "wrong number of arguments to '" + name + "'");
                e->type = Type::err();
                break;
            }
            for (size_t i = 0; i < e->args.size(); ++i) {
                Type* at = checkExpr(e->args[i].get(), inst);
                if (!checkAssignable(callee->paramTypes[i], at, e->loc)) {
                    Diag::error(e->loc, "argument " + std::to_string(i) + " type mismatch for '" + name + "'");
                }
            }
            e->resolved = new ResolvedCall;
            e->resolved->callee = callee;
            e->type = callee->retType;
            break;
        }
        case ExprKind::Cast: {
            Type* target = resolveType(e->name, &inst->typeEnv);
            Type* st = checkExpr(e->lhs.get(), inst);
            bool ok = false;
            if (target->isNumeric() && st->isNumeric()) ok = true;
            else if (target->kind == TypeKind::Ptr && st->kind == TypeKind::Ptr) ok = true;
            else if (target->kind == TypeKind::Ptr && st->kind == TypeKind::U64) ok = true;
            else if (target->kind == TypeKind::U64 && st->kind == TypeKind::Ptr) ok = true;
            else if (target->kind == TypeKind::Ptr && st->kind == TypeKind::Ent) ok = true;
            else if (target->kind == TypeKind::Bool && st->isNumeric()) ok = true;
            else if (target->kind == TypeKind::Ent && st->kind == TypeKind::U32) ok = true;
            else if (target->kind == TypeKind::Exist && st->kind == TypeKind::Struct) {
                if (isConforming(st->name, target->name)) { ok = true; ensureWitness(target->name, st->name); }
            } else if (target->kind == TypeKind::Exist && st->kind == TypeKind::Ptr &&
                       st->pointee && st->pointee->kind == TypeKind::Struct) {
                if (isConforming(st->pointee->name, target->name)) {
                    ok = true;
                    ensureWitness(target->name, st->pointee->name);
                }
            }
            if (!ok) {
                Diag::error(e->loc, "cannot cast '" + st->str() + "' to '" + target->str() + "'");
            }
            e->type = target;
            break;
        }
        case ExprKind::Sizeof: {
            int w = 0;
            if (!e->name.empty()) {
                w = sizeOf(resolveType(e->name, &inst->typeEnv));
            } else {
                Type* t = checkExpr(e->lhs.get(), inst);
                w = sizeOf(t);
            }
            e->intVal = static_cast<uint64_t>(w);
            e->type = Type::make(TypeKind::U32);
            break;
        }
        case ExprKind::New: {
            Type* elem = resolveType(e->name, &inst->typeEnv);
            checkExpr(e->rhs.get(), inst);
            e->type = Type::makePtr(elem);
            break;
        }
        case ExprKind::Assign: {
            Type* lt = checkExpr(e->lhs.get(), inst);
            Type* rt = checkExpr(e->rhs.get(), inst);
            if (!e->lhs->lvalue) {
                Diag::error(e->loc, "assignment target is not assignable");
            }
            if (!checkAssignable(lt, rt, e->loc)) {
                Diag::error(e->loc, "cannot assign '" + rt->str() + "' to '" + lt->str() + "'");
            }
            e->type = lt;
            break;
        }
        case ExprKind::CompoundAssign: {
            Type* lt = checkExpr(e->lhs.get(), inst);
            Type* rt = checkExpr(e->rhs.get(), inst);
            if (!e->lhs->lvalue) {
                Diag::error(e->loc, "compound assignment target is not assignable");
            }
            if (!lt->isNumeric() && lt->kind != TypeKind::Ptr) {
                Diag::error(e->loc, "compound assignment requires numeric target");
            }
            e->type = lt;
            (void)rt;
            break;
        }
        case ExprKind::IncDec: {
            Type* lt = checkExpr(e->lhs.get(), inst);
            if (!e->lhs->lvalue) {
                Diag::error(e->loc, "inc/dec target is not assignable");
            }
            e->type = lt;
            break;
        }
        default:
            e->type = Type::err();
            break;
    }
    e->constVal = evalConst(e);
    return e->type;
}

// ==================== 调用解析 ====================
Type* Sema::checkCall(Expr* e, FuncInstance* inst) {
    const std::string& name = e->callee;
    // 内建
    Type* b = checkBuiltin(e, inst, name);
    if (b) return b;
    // ECS 无模板调用
    if (name == "spawn" || name == "despawn" || name == "alive" || name == "entity_count") {
        return checkEcsCall(e, inst, name, "");
    }
    FuncSym* sym = findFunc(name);
    if (!sym) {
        Diag::error(e->loc, "call to unknown function '" + name + "'");
        return Type::err();
    }
    if (sym->isGeneric) {
        Diag::error(e->loc, "generic function '" + name + "' must be called with type args: " + name + "<T>(...)");
        return Type::err();
    }
    if (sym->isExtern) {
        Decl* d = sym->decl;
        if (e->args.size() != d->externTypes.size()) {
            Diag::error(e->loc, "wrong number of arguments to extern '" + name + "'");
        }
        for (size_t i = 0; i < e->args.size() && i < d->externTypes.size(); ++i) {
            Type* at = checkExpr(e->args[i].get(), inst);
            Type* pt = resolveType(d->externTypes[i]);
            if (!checkAssignable(pt, at, e->loc)) {
                Diag::error(e->loc, "argument " + std::to_string(i) + " type mismatch for extern '" + name + "'");
            }
        }
        e->resolved = new ResolvedCall;
        e->resolved->isExtern = true;
        e->resolved->externIndex = sym->externIndex;
        return resolveType(d->externRetType);
    }
    FuncInstance* callee = getInstance(name, {});
    if (!callee) return Type::err();
    if (e->args.size() != callee->paramTypes.size()) {
        Diag::error(e->loc, "wrong number of arguments to '" + name + "': expected " +
                            std::to_string(callee->paramTypes.size()));
        return Type::err();
    }
    for (size_t i = 0; i < e->args.size(); ++i) {
        Type* at = checkExpr(e->args[i].get(), inst);
        if (!checkAssignable(callee->paramTypes[i], at, e->loc)) {
            Diag::error(e->loc, "argument " + std::to_string(i) + " type mismatch for '" + name + "'");
        }
    }
    e->resolved = new ResolvedCall;
    e->resolved->callee = callee;
    return callee->retType;
}

Type* Sema::checkMethodCall(Expr* e, FuncInstance* inst) {
    Type* ot = checkExpr(e->lhs.get(), inst);
    for (auto& a : e->args) checkExpr(a.get(), inst);
    Type* base = ot;
    if (base->kind == TypeKind::Ptr && base->pointee &&
        base->pointee->kind == TypeKind::Struct) {
        base = base->pointee;
    }
    if (base->kind == TypeKind::Exist) {
        Decl* proto = protocols[base->name];
        ProtoMethod* pm = nullptr;
        if (proto) {
            for (auto& m : proto->methods) if (m.name == e->name) { pm = &m; break; }
        }
        if (!pm) {
            Diag::error(e->loc, "protocol '" + base->name + "' has no method '" + e->name + "'");
            return Type::err();
        }
        size_t expectArgs = pm->params.size() - 1; // 去掉 this
        if (e->args.size() != expectArgs) {
            Diag::error(e->loc, "wrong number of arguments to '" + e->name + "'");
        }
        for (size_t i = 0; i < e->args.size() && i + 1 < pm->params.size(); ++i) {
            Type* pt = resolveType(pm->params[i + 1].typeName);
            if (!checkAssignable(pt, e->args[i]->type, e->loc)) {
                Diag::error(e->loc, "argument " + std::to_string(i) + " type mismatch");
            }
        }
        e->resolved = new ResolvedCall;
        e->resolved->isDynamicDispatch = true;
        e->resolved->witnessMethodIndex = pm->index;
        e->resolved->protoName = base->name;
        return resolveType(pm->retTypeName);
    }
    if (base->kind == TypeKind::Struct) {
        // 静态派发：遍历该类型的所有 impl
        for (auto& [key, implD] : impls) {
            if (implD->typeName != base->name) continue;
            Decl* proto = protocols[implD->protoName];
            ProtoMethod* pm = nullptr;
            if (proto) {
                for (auto& m : proto->methods) if (m.name == e->name) { pm = &m; break; }
            }
            if (!pm) continue;
            FuncInstance* callee = getInstance("impl_" + implD->protoName + "_" + base->name + "_" + e->name, {});
            if (!callee) return Type::err();
            if (e->args.size() != callee->paramTypes.size() - 1) {
                Diag::error(e->loc, "wrong number of arguments to '" + e->name + "'");
            }
            for (size_t i = 0; i < e->args.size() && i + 1 < callee->paramTypes.size(); ++i) {
                if (!checkAssignable(callee->paramTypes[i + 1], e->args[i]->type, e->loc)) {
                    Diag::error(e->loc, "argument " + std::to_string(i) + " type mismatch");
                }
            }
            e->resolved = new ResolvedCall;
            e->resolved->callee = callee;
            return callee->retType;
        }
        Diag::error(e->loc, "type '" + base->name + "' has no protocol method '" + e->name + "'");
        return Type::err();
    }
    if (base->kind == TypeKind::GenericParam) {
        // 泛型约束方法调用：T: Proto -> impl_Proto_<concrete>_method
        const std::string& tp = base->name;
        std::string proto;
        for (auto& [p, pr] : inst->fn->constraints) {
            if (p == tp) { proto = pr; break; }
        }
        if (proto.empty()) {
            Diag::error(e->loc, "generic parameter '" + tp + "' has no protocol constraint");
            return Type::err();
        }
        auto envIt = inst->typeEnv.find(tp);
        if (envIt == inst->typeEnv.end() || envIt->second->kind != TypeKind::Struct) {
            Diag::error(e->loc, "generic parameter '" + tp + "' not resolved to a concrete type");
            return Type::err();
        }
        const std::string& concreteName = envIt->second->name;
        FuncInstance* callee = getInstance("impl_" + proto + "_" + concreteName + "_" + e->name, {});
        if (!callee) {
            Diag::error(e->loc, "type '" + concreteName + "' does not provide '" + e->name + "'");
            return Type::err();
        }
        if (e->args.size() != callee->paramTypes.size() - 1) {
            Diag::error(e->loc, "wrong number of arguments to '" + e->name + "'");
        }
        for (size_t i = 0; i < e->args.size() && i + 1 < callee->paramTypes.size(); ++i) {
            if (!checkAssignable(callee->paramTypes[i + 1], e->args[i]->type, e->loc)) {
                Diag::error(e->loc, "argument " + std::to_string(i) + " type mismatch");
            }
        }
        e->resolved = new ResolvedCall;
        e->resolved->callee = callee;
        return callee->retType;
    }
    Diag::error(e->loc, "cannot call method on type '" + base->str() + "'");
    return Type::err();
}

Type* Sema::checkBuiltin(Expr* e, FuncInstance* inst, const std::string& name) {
    auto arg = [&](size_t i) -> Type* {
        if (i >= e->args.size()) {
            Diag::error(e->loc, "missing argument to '" + name + "'");
            return Type::err();
        }
        return checkExpr(e->args[i].get(), inst);
    };
    if (name == "print") {
        for (size_t i = 0; i < e->args.size(); ++i) {
            Type* t = checkExpr(e->args[i].get(), inst);
            if (!t->isNumeric() && t->kind != TypeKind::Bool && t->kind != TypeKind::Char &&
                t->kind != TypeKind::Ptr && t->kind != TypeKind::Ent && t->kind != TypeKind::Struct) {
                Diag::error(e->loc, "cannot print type '" + t->str() + "'");
            }
        }
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "printc") {
        Type* t = arg(0);
        if (!t->isNumeric() && t->kind != TypeKind::Char && t->kind != TypeKind::Bool) {
            Diag::error(e->loc, "printc expects a char (u8)");
        }
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "sqrt" || name == "log") {
        Type* t = arg(0);
        if (!t->isNumeric()) Diag::error(e->loc, name + " expects a numeric argument");
        e->type = Type::make(TypeKind::F64);
        return e->type;
    }
    if (name == "getsystem") {
        // GET_SYSTEM：返回 u64（低16位=平台，次16位=架构），无参数
        if (e->args.size() != 0) Diag::error(e->loc, "getsystem takes no arguments");
        e->type = Type::make(TypeKind::U64);
        return e->type;
    }
    if (name == "dl_reg") {
        // dl_reg(fnPtr, sigStr) → u32：运行时注册 C 函数（签名串 "rettype(argtypes)"）
        if (e->args.size() != 2) Diag::error(e->loc, "dl_reg expects (fnPtr, signature)");
        arg(0); arg(1); // 给参数定型（字符串字面量参数也需要）
        e->type = Type::make(TypeKind::U32);
        return e->type;
    }
    if (name == "dl_call") {
        // dl_call(idx, sigLiteral, args...) → 返回类型由签名串字面量决定（void/无参函数允许仅 (idx, sig)）
        if (e->args.size() < 2) { Diag::error(e->loc, "dl_call expects (idx, signature[, args...])"); e->type = Type::make(TypeKind::U64); return e->type; }
        arg(0); arg(1); // idx 与签名串定型
        for (size_t ai = 2; ai < e->args.size(); ++ai) arg(ai); // 值参数定型
        Expr* sig = e->args[1].get();
        if (sig->kind != ExprKind::StrLit) { Diag::error(e->loc, "dl_call signature must be a string literal"); e->type = Type::make(TypeKind::U64); return e->type; }
        std::string s = sig->strVal;
        size_t lp = s.find('(');
        TypeKind k = TypeKind::U64;
        if (lp != std::string::npos) {
            std::string rn = s.substr(0, lp);
            if (rn == "u8" || rn == "i8") k = TypeKind::U8;
            else if (rn == "u16" || rn == "i16") k = TypeKind::U16;
            else if (rn == "u32") k = TypeKind::U32;
            else if (rn == "i32") k = TypeKind::I32;
            else if (rn == "u64") k = TypeKind::U64;
            else if (rn == "i64") k = TypeKind::I64;
            else if (rn == "f64") k = TypeKind::F64;
            else if (rn == "ptr") { e->type = Type::makePtr(Type::make(TypeKind::Void)); return e->type; } // 可赋给 ptr 变量
            else if (rn == "void") k = TypeKind::Void;
        }
        e->type = Type::make(k);
        return e->type;
    }
    if (name == "dload") {
        // dload("x.so") → dlopen("lib/x.so", RTLD_NOW)：动态链接默认根目录 = lib/
        if (e->args.size() != 1) { Diag::error(e->loc, "dload expects (filename)"); e->type = Type::make(TypeKind::Ptr); return e->type; }
        Expr* nm = e->args[0].get();
        if (nm->kind != ExprKind::StrLit) {
            Diag::error(e->loc, "dload filename must be a string literal");
        } else {
            nm->strVal = "lib/" + nm->strVal; // 编译期拼接默认根目录
        }
        e->type = Type::makePtr(Type::make(TypeKind::Void)); // ptr（带 pointee，避免 coerceOnStack 崩溃）
        return e->type;
    }
    if (name == "memcpy") {
        Type* d = arg(0);
        Type* s = arg(1);
        arg(2);
        if (d->kind != TypeKind::Ptr || s->kind != TypeKind::Ptr) {
            Diag::error(e->loc, "memcpy expects pointer arguments");
        }
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "free") {
        Type* p = arg(0);
        if (p->kind != TypeKind::Ptr) Diag::error(e->loc, "free expects a pointer");
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "bytecode_base" || name == "manager_addr") {
        e->type = Type::make(TypeKind::U64);
        return e->type;
    }
    if (name.rfind("atomic_", 0) == 0) {
        bool is64 = name.find("u64") != std::string::npos;
        if (name.find("atomic_load") == 0) {
            arg(0);
            e->type = Type::make(is64 ? TypeKind::U64 : TypeKind::U32);
            return e->type;
        }
        if (name.find("atomic_store") == 0) {
            arg(0); arg(1);
            e->type = Type::make(TypeKind::Void);
            return e->type;
        }
        if (name.find("atomic_add") == 0 || name.find("atomic_xchg") == 0) {
            arg(0); arg(1);
            e->type = Type::make(is64 ? TypeKind::U64 : TypeKind::U32);
            return e->type;
        }
        if (name.find("atomic_cas") == 0) {
            arg(0); arg(1); arg(2);
            e->type = Type::make(TypeKind::Bool);
            return e->type;
        }
    }
    return nullptr; // 非内建
}

Type* Sema::checkEcsCall(Expr* e, FuncInstance* inst, const std::string& name, const std::string& tmpl) {
    auto arg = [&](size_t i) -> Type* {
        if (i >= e->args.size()) {
            Diag::error(e->loc, "missing argument to '" + name + "'");
            return Type::err();
        }
        return checkExpr(e->args[i].get(), inst);
    };
    if (name == "spawn") {
        e->type = Type::make(TypeKind::Ent);
        return e->type;
    }
    if (name == "despawn") {
        Type* t = arg(0);
        if (t->kind != TypeKind::Ent) Diag::error(e->loc, "despawn expects an entity");
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "alive") {
        Type* t = arg(0);
        if (t->kind != TypeKind::Ent) Diag::error(e->loc, "alive expects an entity");
        e->type = Type::make(TypeKind::Bool);
        return e->type;
    }
    if (name == "entity_count") {
        e->type = Type::make(TypeKind::U32);
        return e->type;
    }
    if (name == "get") {
        if (!structs.count(tmpl)) {
            Diag::error(e->loc, "unknown component '" + tmpl + "'");
            return Type::err();
        }
        Type* t = arg(0);
        if (t->kind != TypeKind::Ent) Diag::error(e->loc, "get<C> expects an entity");
        e->type = Type::makePtr(Type::makeStruct(tmpl));
        return e->type;
    }
    if (name == "has") {
        if (!structs.count(tmpl)) {
            Diag::error(e->loc, "unknown component '" + tmpl + "'");
            return Type::err();
        }
        Type* t = arg(0);
        if (t->kind != TypeKind::Ent) Diag::error(e->loc, "has<C> expects an entity");
        e->type = Type::make(TypeKind::Bool);
        return e->type;
    }
    if (name == "remove") {
        if (!structs.count(tmpl)) {
            Diag::error(e->loc, "unknown component '" + tmpl + "'");
            return Type::err();
        }
        Type* t = arg(0);
        if (t->kind != TypeKind::Ent) Diag::error(e->loc, "remove<C> expects an entity");
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "add") {
        if (!structs.count(tmpl)) {
            Diag::error(e->loc, "unknown component '" + tmpl + "'");
            return Type::err();
        }
        Decl* cd = structs[tmpl];
        if (e->args.size() != 1 + cd->fields.size()) {
            Diag::error(e->loc, "add<" + tmpl + "> expects an entity + " +
                                std::to_string(cd->fields.size()) + " field values");
        }
        Type* t = arg(0);
        if (t->kind != TypeKind::Ent) Diag::error(e->loc, "add<C> first argument must be an entity");
        for (size_t i = 0; i < cd->fields.size() && i + 1 < e->args.size(); ++i) {
            Type* at = checkExpr(e->args[i + 1].get(), inst);
            Type* ft = resolveType(cd->fields[i].typeName);
            if (!checkAssignable(ft, at, e->loc)) {
                Diag::error(e->loc, "field '" + cd->fields[i].name + "' type mismatch in add<" + tmpl + ">");
            }
        }
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    if (name == "run") {
        if (!systems.count(tmpl)) {
            Diag::error(e->loc, "unknown system '" + tmpl + "'");
            return Type::err();
        }
        // 预实例化 system 的 update（codegen 内联调用）
        getInstance("sys_" + tmpl + "_update", {});
        e->type = Type::make(TypeKind::Void);
        return e->type;
    }
    return Type::err();
}

// ==================== 主入口 ====================
bool Sema::run() {
    buildHelpers();      // 先注入 print/转换 helper
    buildTables();
    if (Diag::errorCount) return false;
    if (!checkConformance()) return false;
    if (Diag::errorCount) return false;

    // 预实例化 helper（codegen 调用它们时不再新建实例）
    static const char* helperNames[] = {
        "__print_u32", "__print_u64", "__print_i64", "__print_f64",
        "__print_str", "__print_bool",
        "__f64_from_u64", "__f64_from_i64", "__u64_from_f64", "__i64_from_f64",
    };
    for (const char* hn : helperNames) getInstance(hn, {});

    FuncSym* mainSym = findFunc("main");
    if (!mainSym) {
        Diag::error({0, 0}, "missing 'main' function");
        return false;
    }
    FuncInstance* mi = getInstance("main", {});
    if (!mi) return false;
    mi->isMain = true;
    return Diag::errorCount == 0;
}

} // namespace j8
