// parser.cpp — J8 语法分析器实现
#include "parser.h"

#include <cstring>

namespace j8 {

const Token& Parser::peek(size_t off) const {
    static Token endTok = [] { Token t; t.kind = Tok::End; return t; }();
    if (pos_ + off >= toks_.size()) return endTok;
    return toks_[pos_ + off];
}
const Token& Parser::next() { return toks_[pos_ < toks_.size() ? pos_++ : pos_]; }
bool Parser::at(Tok k, size_t off) const { return peek(off).kind == k; }
bool Parser::accept(Tok k) { if (at(k)) { next(); return true; } return false; }
bool Parser::expect(Tok k, const char* what) {
    if (at(k)) { next(); return true; }
    Diag::error(peek().loc, std::string("expected ") + what);
    ok_ = false;
    return false;
}

bool Parser::isTypeToken(const Token& t) const {
    switch (t.kind) {
        case Tok::KwVoid: case Tok::KwEnt:
            return true;
        case Tok::Ident: {
            const std::string& s = t.text;
            return s == "u8" || s == "u16" || s == "u32" || s == "u64" ||
                   s == "i8" || s == "i16" || s == "i32" || s == "i64" ||
                   s == "f32" || s == "f64" || s == "bool" || s == "char";
        }
        default: return false;
    }
}

// 解析类型名（带指针后缀），返回名字字符串与 resolved 指针（可为空，语义分析再解析）
bool Parser::parseTypeName(std::string& out, Type*& resolved) {
    resolved = nullptr;
    const Token& t = peek();
    std::string base;
    if (t.kind == Tok::KwVoid) { base = "void"; next(); }
    else if (t.kind == Tok::KwEnt) { base = "ent"; next(); }
    else if (t.kind == Tok::Ident) { base = t.text; next(); }
    else return false;

    while (at(Tok::Star)) {
        next();
        base += "*";
    }
    out = base;
    return true;
}

bool Parser::parseFieldList(std::vector<Field>& out) {
    while (!at(Tok::RBrace) && !at(Tok::End)) {
        std::string tn; Type* t = nullptr;
        if (!parseTypeName(tn, t)) {
            Diag::error(peek().loc, "expected field type");
            ok_ = false; return false;
        }
        if (!at(Tok::Ident)) {
            Diag::error(peek().loc, "expected field name");
            ok_ = false; return false;
        }
        Field f;
        f.typeName = tn;
        f.name = next().text;
        while (accept(Tok::Semicolon)) {}
        out.push_back(f);
    }
    return true;
}

bool Parser::parseParamList(std::vector<Param>& out, bool& hasThis) {
    hasThis = false;
    if (!expect(Tok::LParen, "'('")) return false;
    if (accept(Tok::RParen)) return true;

    for (;;) {
        // 可能带 this 参数：`ret name(this, ...)`
        if (at(Tok::Ident) && peek().text == "this") {
            next();
            Param p;
            p.name = "this";
            p.typeName = "this";
            hasThis = true;
            out.push_back(p);
        } else {
            // C 风格: Type name  或 Swift 风格: name: Type
            std::string tn; Type* t = nullptr;
            Param p;
            if (isTypeToken(peek()) && (isTypeToken(peek(1)) || at(Tok::KwEnt, 1) ||
                                        (peek(1).kind == Tok::Ident) || at(Tok::Star, 1))) {
                if (!parseTypeName(tn, t)) return false;
                if (at(Tok::Star)) return false;
                p.typeName = tn;
                if (at(Tok::Ident)) p.name = next().text;
                else { Diag::error(peek().loc, "expected parameter name"); ok_ = false; return false; }
            } else if (at(Tok::Ident) && peek(1).kind == Tok::Ident) {
                // C 风格泛型/结构体参数: T a  或  Shape s（类型名是标识符）
                p.typeName = next().text;
                p.name = next().text;
            } else if (at(Tok::Ident) && at(Tok::Colon, 1)) {
                p.name = next().text;
                next(); // ':'
                if (!parseTypeName(p.typeName, t)) return false;
            } else if (at(Tok::KwEnt) && peek(1).kind == Tok::Ident) {
                p.typeName = "ent";
                next();
                p.name = next().text;
            } else {
                Diag::error(peek().loc, "expected parameter");
                ok_ = false; return false;
            }
            out.push_back(p);
        }
        if (!accept(Tok::Comma)) break;
    }
    return expect(Tok::RParen, "')'");
}

// ---------------- 声明 ----------------
std::unique_ptr<Program> Parser::parseProgram() {
    auto prog = std::make_unique<Program>();
    while (!at(Tok::End)) {
        if (at(Tok::Pragma)) {
            const Token& t = next();
            // #pragma unroll N
            std::string txt = t.text;
            size_t u = txt.find("unroll");
            if (u != std::string::npos) {
                pendingUnroll_ = true;
                pendingUnrollFactor_ = 0;
                size_t d = txt.find_first_of("0123456789", u + 6);
                if (d != std::string::npos) pendingUnrollFactor_ = std::atoi(txt.c_str() + d);
            }
            continue;
        }
        DeclPtr d = parseFunctionOrDecl();
        if (!d) { ok_ = false; break; }
        prog->decls.push_back(std::move(d));
        if (!ok_) break;
    }
    return prog;
}

DeclPtr Parser::parseFunctionOrDecl() {
    const Token& t = peek();
    if (t.kind == Tok::KwStruct) return parseStruct();
    if (t.kind == Tok::KwProtocol) return parseProtocol();
    if (t.kind == Tok::KwImpl) return parseImpl();
    if (t.kind == Tok::KwComponent) return parseComponent();
    if (t.kind == Tok::KwSystem) return parseSystem();
    if (t.kind == Tok::KwExtern) return parseExtern();

    // 函数：retType name(params) { ... }   或带泛型 fn name<T: Proto>(...)
    std::string retTn; Type* rt = nullptr;
    if (!parseTypeName(retTn, rt)) {
        Diag::error(t.loc, "expected declaration");
        ok_ = false;
        return nullptr;
    }
    if (!at(Tok::Ident)) {
        Diag::error(peek().loc, "expected function name");
        ok_ = false;
        return nullptr;
    }
    auto fn = std::make_unique<Decl>(DeclKind::Function, peek().loc);
    fn->retTypeName = retTn;
    fn->name = next().text;

    // 泛型参数: name<T: Proto, U>
    if (accept(Tok::Lt)) {
        for (;;) {
            std::string g;
            if (at(Tok::Ident)) g = next().text;
            else { Diag::error(peek().loc, "expected generic parameter"); ok_ = false; return nullptr; }
            if (accept(Tok::Colon)) {
                if (at(Tok::Ident)) fn->constraints.emplace_back(g, next().text);
                else { Diag::error(peek().loc, "expected protocol name"); ok_ = false; return nullptr; }
            }
            fn->genericParams.push_back(g);
            if (!accept(Tok::Comma)) break;
        }
        if (!expect(Tok::Gt, "'>'")) return nullptr;
    }

    bool hasThis = false;
    if (!parseParamList(fn->params, hasThis)) return nullptr;

    if (accept(Tok::Semicolon)) {
        // 函数前置声明（非 extern）：暂不支持，报错
        Diag::error(fn->loc, "function declaration without body (use 'extern' for external functions)");
        ok_ = false;
        return nullptr;
    }
    if (!at(Tok::LBrace)) {
        Diag::error(peek().loc, "expected '{' for function body");
        ok_ = false;
        return nullptr;
    }
    fn->body = parseBlock();
    return fn;
}

DeclPtr Parser::parseStruct() {
    auto d = std::make_unique<Decl>(DeclKind::Struct, next().loc); // struct
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected struct name"); ok_ = false; return nullptr; }
    d->name = next().text;
    if (!expect(Tok::LBrace, "'{'")) return nullptr;
    if (!parseFieldList(d->fields)) return nullptr;
    if (!expect(Tok::RBrace, "'}'")) return nullptr;
    accept(Tok::Semicolon);
    typeNames_.insert(d->name);
    return d;
}

DeclPtr Parser::parseProtocol() {
    auto d = std::make_unique<Decl>(DeclKind::Protocol, next().loc); // protocol
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected protocol name"); ok_ = false; return nullptr; }
    d->name = next().text;
    if (!expect(Tok::LBrace, "'{'")) return nullptr;

    while (!at(Tok::RBrace) && !at(Tok::End)) {
        std::string retTn; Type* rt = nullptr;
        if (!parseTypeName(retTn, rt)) {
            Diag::error(peek().loc, "expected return type in protocol method");
            ok_ = false; return nullptr;
        }
        if (!at(Tok::Ident)) {
            Diag::error(peek().loc, "expected method name");
            ok_ = false; return nullptr;
        }
        ProtoMethod m;
        m.retTypeName = retTn;
        m.name = next().text;
        m.index = static_cast<int>(d->methods.size());
        bool hasThis = false;
        if (!parseParamList(m.params, hasThis)) return nullptr;
        if (!hasThis) {
            Diag::error(m.name.empty() ? peek().loc : d->loc,
                        "protocol method '" + m.name + "' must take 'this' as first parameter");
            ok_ = false; return nullptr;
        }
        if (accept(Tok::Semicolon)) {
            // 无默认实现
        } else if (at(Tok::LBrace)) {
            m.body = parseBlock();
        } else {
            Diag::error(peek().loc, "expected ';' or '{' after protocol method");
            ok_ = false; return nullptr;
        }
        d->methods.push_back(std::move(m));
    }
    expect(Tok::RBrace, "'}'");
    accept(Tok::Semicolon);
    typeNames_.insert(d->name);
    return d;
}

DeclPtr Parser::parseImpl() {
    auto d = std::make_unique<Decl>(DeclKind::Impl, next().loc); // impl
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected protocol name"); ok_ = false; return nullptr; }
    d->protoName = next().text;
    if (!expect(Tok::KwFor, "'for'")) return nullptr;
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected type name"); ok_ = false; return nullptr; }
    d->typeName = next().text;
    if (!expect(Tok::LBrace, "'{'")) return nullptr;

    while (!at(Tok::RBrace) && !at(Tok::End)) {
        std::string retTn; Type* rt = nullptr;
        if (!parseTypeName(retTn, rt)) {
            Diag::error(peek().loc, "expected return type in impl method");
            ok_ = false; return nullptr;
        }
        if (!at(Tok::Ident)) {
            Diag::error(peek().loc, "expected method name");
            ok_ = false; return nullptr;
        }
        std::string mname = next().text;
        std::vector<Param> params;
        bool hasThis = false;
        if (!parseParamList(params, hasThis)) return nullptr;
        if (!hasThis) {
            Diag::error(d->loc, "impl method '" + mname + "' must take 'this'");
            ok_ = false; return nullptr;
        }
        if (!at(Tok::LBrace)) {
            Diag::error(peek().loc, "expected '{' in impl method body");
            ok_ = false; return nullptr;
        }
        StmtPtr body = parseBlock();
        d->implMethods.emplace_back(mname, std::move(body));
    }
    expect(Tok::RBrace, "'}'");
    accept(Tok::Semicolon);
    return d;
}

DeclPtr Parser::parseComponent() {
    auto d = std::make_unique<Decl>(DeclKind::Component, next().loc); // component
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected component name"); ok_ = false; return nullptr; }
    d->name = next().text;
    if (!expect(Tok::LBrace, "'{'")) return nullptr;
    if (!parseFieldList(d->fields)) return nullptr;
    if (!expect(Tok::RBrace, "'}'")) return nullptr;
    accept(Tok::Semicolon);
    typeNames_.insert(d->name);
    return d;
}

DeclPtr Parser::parseSystem() {
    auto d = std::make_unique<Decl>(DeclKind::System, next().loc); // system
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected system name"); ok_ = false; return nullptr; }
    d->name = next().text;
    if (accept(Tok::Colon)) {
        for (;;) {
            if (at(Tok::Ident)) d->componentNames.push_back(next().text);
            else { Diag::error(peek().loc, "expected component name"); ok_ = false; return nullptr; }
            if (!accept(Tok::Comma)) break;
        }
    }
    if (!expect(Tok::LBrace, "'{'")) return nullptr;

    while (!at(Tok::RBrace) && !at(Tok::End)) {
        // fn update(e: ent) { ... }
        if (accept(Tok::KwFn)) {
            if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected method name"); ok_ = false; return nullptr; }
            std::string mname = next().text;
            std::vector<Param> params;
            bool hasThis = false;
            if (!parseParamList(params, hasThis)) return nullptr;
            if (mname != "update") {
                Diag::error(d->loc, "system only supports 'fn update(ent e)' in this version");
                ok_ = false; return nullptr;
            }
            if (!at(Tok::LBrace)) { Diag::error(peek().loc, "expected '{'"); ok_ = false; return nullptr; }
            auto body = parseBlock();
            d->implMethods.emplace_back(mname, std::move(body));
            // 记录 update 的参数（应为一个 ent）
            if (!params.empty()) {
                d->retTypeName = params[0].typeName; // 借用字段存 update 的 e 参数类型
                d->sysUpdateParamName = params[0].name;
            }
        } else {
            Diag::error(peek().loc, "expected 'fn update(...)' in system");
            ok_ = false; return nullptr;
        }
    }
    expect(Tok::RBrace, "'}'");
    accept(Tok::Semicolon);
    return d;
}

DeclPtr Parser::parseExtern() {
    auto d = std::make_unique<Decl>(DeclKind::Extern, next().loc); // extern
    if (!parseTypeName(d->externRetType, d->retType)) {
        Diag::error(peek().loc, "expected return type");
        ok_ = false; return nullptr;
    }
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected extern function name"); ok_ = false; return nullptr; }
    d->name = next().text;
    if (!expect(Tok::LParen, "'('")) return nullptr;
    while (!at(Tok::RParen) && !at(Tok::End)) {
        std::string tn; Type* t = nullptr;
        if (!parseTypeName(tn, t)) { ok_ = false; return nullptr; }
        d->externTypes.push_back(tn);
        // 跳过参数名（可选）
        if (at(Tok::Ident)) next();
        if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RParen, "')'");
    expect(Tok::Semicolon, "';'");
    return d;
}

// ---------------- 语句 ----------------
StmtPtr Parser::parseStmt() {
    const Token& t = peek();
    if (t.kind == Tok::LBrace) return parseBlock();
    if (t.kind == Tok::KwIf) return parseIf();
    if (t.kind == Tok::KwWhile) return parseWhile();
    if (t.kind == Tok::KwDo) return parseDoWhile();
    if (t.kind == Tok::KwFor) return parseFor();
    if (t.kind == Tok::KwBreak) {
        auto s = std::make_unique<Stmt>(StmtKind::Break, next().loc);
        expect(Tok::Semicolon, "';'");
        return s;
    }
    if (t.kind == Tok::KwContinue) {
        auto s = std::make_unique<Stmt>(StmtKind::Continue, next().loc);
        expect(Tok::Semicolon, "';'");
        return s;
    }
    if (t.kind == Tok::KwReturn) return parseReturn();
    if (t.kind == Tok::KwConst) return parseVarDecl(true);
    if (isTypeToken(t)) {
        // 变量声明 vs 表达式语句（类型名开头一定是声明，因为表达式不能以类型名开头）
        // 需要前瞻：Type Ident (;|=|,)
        const Token& t2 = peek(1);
        if (t2.kind == Tok::Ident) return parseVarDecl(false);
        if (t2.kind == Tok::KwEnt && peek(2).kind == Tok::Ident) { next(); return parseVarDecl(false); }
    }
    if (t.kind == Tok::KwEnt && peek(1).kind == Tok::Ident) return parseVarDecl(false);
    // 结构体/协议/泛型类型名的 C 风格声明: Shape s = {...};  Shape *p = &c;
    // （歧义：`x * y;` 乘法语句会被当作声明，属已知取舍，与 C 的 typedef 消歧同理）
    if (t.kind == Tok::Ident && peek(1).kind == Tok::Ident) return parseVarDecl(false);
    if (t.kind == Tok::Ident && peek(1).kind == Tok::Star && peek(2).kind == Tok::Ident)
        return parseVarDecl(false);
    return parseExprStmt();
}

StmtPtr Parser::parseBlock() {
    auto s = std::make_unique<Stmt>(StmtKind::Block, next().loc); // {
    while (!at(Tok::RBrace) && !at(Tok::End)) {
        if (at(Tok::Pragma)) {
            // #pragma unroll N（函数体内也允许）
            const Token& t = next();
            std::string txt = t.text;
            size_t u = txt.find("unroll");
            if (u != std::string::npos) {
                pendingUnroll_ = true;
                pendingUnrollFactor_ = 0;
                size_t d = txt.find_first_of("0123456789", u + 6);
                if (d != std::string::npos) pendingUnrollFactor_ = std::atoi(txt.c_str() + d);
            }
            continue;
        }
        StmtPtr st = parseStmt();
        if (!st) { ok_ = false; break; }
        s->body.push_back(std::move(st));
    }
    expect(Tok::RBrace, "'}'");
    return s;
}

StmtPtr Parser::parseIf() {
    auto s = std::make_unique<Stmt>(StmtKind::If, next().loc); // if
    if (!expect(Tok::LParen, "'('")) return nullptr;
    s->cond = parseExpr();
    if (!expect(Tok::RParen, "')'")) return nullptr;
    s->single = parseStmt();
    if (accept(Tok::KwElse)) {
        StmtPtr elseStmt = parseStmt();
        if (elseStmt) {
            // 转为 if 的 else 分支：用 block 包一层
            auto blk = std::make_unique<Stmt>(StmtKind::Block, elseStmt->loc);
            blk->body.push_back(std::move(elseStmt));
            s->body.push_back(std::move(blk));
        }
    }
    return s;
}

StmtPtr Parser::parseWhile() {
    auto s = std::make_unique<Stmt>(StmtKind::While, next().loc); // while
    if (!expect(Tok::LParen, "'('")) return nullptr;
    s->cond = parseExpr();
    if (!expect(Tok::RParen, "')'")) return nullptr;
    s->single = parseStmt();
    return s;
}

StmtPtr Parser::parseDoWhile() {
    auto s = std::make_unique<Stmt>(StmtKind::DoWhile, next().loc); // do
    s->single = parseStmt();
    if (!expect(Tok::KwWhile, "'while'")) return nullptr;
    if (!expect(Tok::LParen, "'('")) return nullptr;
    s->cond = parseExpr();
    if (!expect(Tok::RParen, "')'")) return nullptr;
    expect(Tok::Semicolon, "';'");
    return s;
}

StmtPtr Parser::parseFor() {
    auto s = std::make_unique<Stmt>(StmtKind::For, next().loc); // for
    if (pendingUnroll_) {
        s->hasUnrollPragma = true;
        s->unrollFactor = pendingUnrollFactor_;
        pendingUnroll_ = false;
        pendingUnrollFactor_ = 0;
    }
    if (!expect(Tok::LParen, "'('")) return nullptr;
    if (accept(Tok::Semicolon)) {
        // 无初始化
    } else if (isTypeToken(peek()) || (peek().kind == Tok::KwEnt && peek(1).kind == Tok::Ident)) {
        s->forVar = std::make_unique<VarDeclStmt>();
        if (!parseTypeName(s->forVar->typeName, s->forVar->type)) return nullptr;
        if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected variable name"); ok_ = false; return nullptr; }
        s->forVar->name = next().text;
        if (accept(Tok::Assign)) s->forVar->init = parseExpr();
    } else {
        s->init = parseExpr();
    }
    if (!expect(Tok::Semicolon, "';'")) return nullptr;
    if (!at(Tok::Semicolon)) s->cond = parseExpr();
    if (!expect(Tok::Semicolon, "';'")) return nullptr;
    if (!at(Tok::RParen)) s->step = parseExpr();
    if (!expect(Tok::RParen, "')'")) return nullptr;
    s->single = parseStmt();
    return s;
}

StmtPtr Parser::parseVarDecl(bool isConst) {
    auto s = std::make_unique<Stmt>(StmtKind::VarDecl, peek().loc);
    s->varDecl = std::make_unique<VarDeclStmt>();
    s->varDecl->isConst = isConst;
    if (isConst) next(); // const
    if (!parseTypeName(s->varDecl->typeName, s->varDecl->type)) {
        ok_ = false;
        return nullptr;
    }
    if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected variable name"); ok_ = false; return nullptr; }
    s->varDecl->name = next().text;

    // 数组声明: T name[N]
    if (accept(Tok::LBracket)) {
        if (at(Tok::Int)) {
            s->varDecl->arraySize = static_cast<int>(next().intVal);
        } else {
            Diag::error(peek().loc, "array size must be a constant integer");
            ok_ = false;
        }
        expect(Tok::RBracket, "']'");
    }

    if (accept(Tok::Assign)) {
        if (at(Tok::LBrace)) {
            // 结构体初始化 {a, b, ...}
            next();
            while (!at(Tok::RBrace) && !at(Tok::End)) {
                s->varDecl->fieldInits.push_back(parseExpr());
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RBrace, "'}'");
        } else {
            s->varDecl->init = parseExpr();
        }
    }
    expect(Tok::Semicolon, "';'");
    return s;
}

StmtPtr Parser::parseReturn() {
    auto s = std::make_unique<Stmt>(StmtKind::Return, next().loc); // return
    if (!at(Tok::Semicolon)) s->expr = parseExpr();
    expect(Tok::Semicolon, "';'");
    return s;
}

StmtPtr Parser::parseExprStmt() {
    auto s = std::make_unique<Stmt>(StmtKind::ExprStmt, peek().loc);
    s->expr = parseExpr();
    expect(Tok::Semicolon, "';'");
    return s;
}

// ---------------- 表达式 ----------------
ExprPtr Parser::binary(ExprPtr lhs, Tok op, ExprPtr rhs, const SourceLoc& loc) {
    BinaryOp b;
    switch (op) {
        case Tok::Plus: b = BinaryOp::Add; break;
        case Tok::Minus: b = BinaryOp::Sub; break;
        case Tok::Star: b = BinaryOp::Mul; break;
        case Tok::Slash: b = BinaryOp::Div; break;
        case Tok::Percent: b = BinaryOp::Mod; break;
        case Tok::Shl: b = BinaryOp::Shl; break;
        case Tok::Shr: b = BinaryOp::Shr; break;
        case Tok::Amp: b = BinaryOp::And; break;
        case Tok::Pipe: b = BinaryOp::Or; break;
        case Tok::Caret: b = BinaryOp::Xor; break;
        case Tok::Lt: b = BinaryOp::Lt; break;
        case Tok::Le: b = BinaryOp::Le; break;
        case Tok::Gt: b = BinaryOp::Gt; break;
        case Tok::Ge: b = BinaryOp::Ge; break;
        case Tok::EqEq: b = BinaryOp::Eq; break;
        case Tok::BangEq: b = BinaryOp::Ne; break;
        case Tok::AmpAmp: b = BinaryOp::LogAnd; break;
        case Tok::PipePipe: b = BinaryOp::LogOr; break;
        default: return lhs;
    }
    auto e = std::make_unique<Expr>(ExprKind::Binary, loc);
    e->binOp = b;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

ExprPtr Parser::parseExpr() { return parseAssign(); }

ExprPtr Parser::parseAssign() {
    ExprPtr lhs = parseTernary();
    const Token& t = peek();
    AssignOp op;
    switch (t.kind) {
        case Tok::Assign: op = AssignOp::Assign; break;
        case Tok::PlusEq: op = AssignOp::Add; break;
        case Tok::MinusEq: op = AssignOp::Sub; break;
        case Tok::StarEq: op = AssignOp::Mul; break;
        case Tok::SlashEq: op = AssignOp::Div; break;
        case Tok::PercentEq: op = AssignOp::Mod; break;
        case Tok::ShlEq: op = AssignOp::Shl; break;
        case Tok::ShrEq: op = AssignOp::Shr; break;
        case Tok::AmpEq: op = AssignOp::And; break;
        case Tok::PipeEq: op = AssignOp::Or; break;
        case Tok::CaretEq: op = AssignOp::Xor; break;
        default: return lhs;
    }
    next();
    ExprPtr rhs = parseAssign();
    if (op == AssignOp::Assign) {
        auto e = std::make_unique<Expr>(ExprKind::Assign, t.loc);
        e->lhs = std::move(lhs);
        e->rhs = std::move(rhs);
        return e;
    }
    auto e = std::make_unique<Expr>(ExprKind::CompoundAssign, t.loc);
    e->asOp = op;
    e->lhs = std::move(lhs);
    e->rhs = std::move(rhs);
    return e;
}

ExprPtr Parser::parseTernary() {
    ExprPtr c = parseLogOr();
    if (at(Tok::Question)) {
        SourceLoc loc = next().loc;
        ExprPtr a = parseExpr();
        if (!expect(Tok::Colon, "':'")) return c;
        ExprPtr b = parseAssign();
        auto e = std::make_unique<Expr>(ExprKind::Ternary, loc);
        e->cond = std::move(c);
        e->thenExpr = std::move(a);
        e->elseExpr = std::move(b);
        return e;
    }
    return c;
}

ExprPtr Parser::parseLogOr() {
    ExprPtr lhs = parseLogAnd();
    while (at(Tok::PipePipe)) {
        SourceLoc loc = next().loc;
        lhs = binary(std::move(lhs), Tok::PipePipe, parseLogAnd(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseLogAnd() {
    ExprPtr lhs = parseBitOr();
    while (at(Tok::AmpAmp)) {
        SourceLoc loc = next().loc;
        lhs = binary(std::move(lhs), Tok::AmpAmp, parseBitOr(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseBitOr() {
    ExprPtr lhs = parseBitXor();
    while (at(Tok::Pipe)) {
        SourceLoc loc = next().loc;
        lhs = binary(std::move(lhs), Tok::Pipe, parseBitXor(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseBitXor() {
    ExprPtr lhs = parseBitAnd();
    while (at(Tok::Caret)) {
        SourceLoc loc = next().loc;
        lhs = binary(std::move(lhs), Tok::Caret, parseBitAnd(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseBitAnd() {
    ExprPtr lhs = parseEquality();
    while (at(Tok::Amp)) {
        SourceLoc loc = next().loc;
        lhs = binary(std::move(lhs), Tok::Amp, parseEquality(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseEquality() {
    ExprPtr lhs = parseRelational();
    while (at(Tok::EqEq) || at(Tok::BangEq)) {
        SourceLoc loc = peek().loc;
        Tok op = next().kind;
        lhs = binary(std::move(lhs), op, parseRelational(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseRelational() {
    ExprPtr lhs = parseShift();
    while (at(Tok::Lt) || at(Tok::Le) || at(Tok::Gt) || at(Tok::Ge)) {
        SourceLoc loc = peek().loc;
        Tok op = next().kind;
        lhs = binary(std::move(lhs), op, parseShift(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseShift() {
    ExprPtr lhs = parseAdditive();
    while (at(Tok::Shl) || at(Tok::Shr)) {
        SourceLoc loc = peek().loc;
        Tok op = next().kind;
        lhs = binary(std::move(lhs), op, parseAdditive(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseAdditive() {
    ExprPtr lhs = parseMultiplicative();
    while (at(Tok::Plus) || at(Tok::Minus)) {
        SourceLoc loc = peek().loc;
        Tok op = next().kind;
        lhs = binary(std::move(lhs), op, parseMultiplicative(), loc);
    }
    return lhs;
}
ExprPtr Parser::parseMultiplicative() {
    ExprPtr lhs = parseUnary();
    while (at(Tok::Star) || at(Tok::Slash) || at(Tok::Percent)) {
        SourceLoc loc = peek().loc;
        Tok op = next().kind;
        lhs = binary(std::move(lhs), op, parseUnary(), loc);
    }
    return lhs;
}

ExprPtr Parser::parseUnary() {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::Bang: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnaryOp::Not;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::Tilde: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnaryOp::BitNot;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::Minus: {
            next();
            // 负数直接字面量：-123
            const Token& n = peek();
            if (n.kind == Tok::Int) {
                next();
                auto lit = std::make_unique<Expr>(ExprKind::IntLit, n.loc);
                lit->intVal = n.intVal;
                lit->name = "neg"; // 标记：负数
                auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
                e->unOp = UnaryOp::Neg;
                e->lhs = std::move(lit);
                return e;
            }
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnaryOp::Neg;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::Plus: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnaryOp::Pos;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::Amp: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnaryOp::AddrOf;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::Star: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::Unary, t.loc);
            e->unOp = UnaryOp::Deref;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::Inc: case Tok::Dec: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::IncDec, t.loc);
            e->unOp = (t.kind == Tok::Inc) ? UnaryOp::PreInc : UnaryOp::PreDec;
            e->postfix = false;
            e->lhs = parseUnary();
            return e;
        }
        case Tok::KwNew: return parseNew();
        case Tok::KwSizeof: return parseSizeof();
        case Tok::LParen: {
            // 可能是类型转换 (T)expr
            const Token& n1 = peek(1);
            // Ident 转型仅当该名字是已声明的类型（struct/protocol/component/泛型名除外），
            // 避免把 (var).x 误判为转型
            bool identIsType = n1.kind == Tok::Ident &&
                               (typeNames_.count(n1.text) > 0);
            if ((isTypeToken(n1)) || (n1.kind == Tok::KwVoid) || (n1.kind == Tok::KwEnt) ||
                (identIsType && (at(Tok::Star, 2) || at(Tok::RParen, 2)))) {
                // 前瞻：Type ')' 后跟表达式
                size_t save = pos_;
                next(); // (
                std::string tn; Type* t2 = nullptr;
                if (parseTypeName(tn, t2)) {
                    if (at(Tok::RParen)) {
                        next();
                        ExprPtr inner = parseUnary();
                        auto e = std::make_unique<Expr>(ExprKind::Cast, t.loc);
                        e->name = tn;
                        e->lhs = std::move(inner);
                        return applyPostfix(std::move(e));
                    }
                }
                pos_ = save;
            }
            // 普通括号：解析后继续处理后缀（(var).x 等）
            next();
            ExprPtr inner = parseExpr();
            if (!expect(Tok::RParen, "')'")) return nullptr;
            return applyPostfix(std::move(inner));
        }
        default: return parsePostfix();
    }
}

ExprPtr Parser::parsePostfix() {
    return applyPostfix(parsePrimary());
}

// 对已有表达式继续处理后缀（.x / ->x / [i] / (args) / ++ / -- / 泛型调用）
ExprPtr Parser::applyPostfix(ExprPtr e) {
    for (;;) {
        // 泛型调用消歧：ident < Ident > ( args )（当前 token 就是 <，偏移从 0 起）
        if (e && e->kind == ExprKind::VarRef && at(Tok::Lt, 0) &&
            peek(1).kind == Tok::Ident && at(Tok::Gt, 2) && at(Tok::LParen, 3)) {
            auto gc = std::make_unique<Expr>(ExprKind::GenericCall, e->loc);
            gc->tmplName = e->name;
            next(); // <
            gc->name = next().text;   // 泛型参数类型名
            next(); // >
            next(); // (
            while (!at(Tok::RParen) && !at(Tok::End)) {
                gc->args.push_back(parseExpr());
                if (!accept(Tok::Comma)) break;
            }
            expect(Tok::RParen, "')'");
            e = std::move(gc);
            continue;
        }
        if (at(Tok::LParen)) {
            e = parseCallArgs(std::move(e));
        } else if (at(Tok::LBracket)) {
            SourceLoc loc = next().loc;
            ExprPtr idx = parseExpr();
            expect(Tok::RBracket, "']'");
            auto i = std::make_unique<Expr>(ExprKind::Index, loc);
            i->lhs = std::move(e);
            i->rhs = std::move(idx);
            e = std::move(i);
        } else if (at(Tok::Dot) || at(Tok::Arrow)) {
            SourceLoc loc = peek().loc;
            bool isArrow = at(Tok::Arrow);
            next();
            if (!at(Tok::Ident)) { Diag::error(peek().loc, "expected member name"); ok_ = false; return nullptr; }
            std::string mname = next().text;
            auto m = std::make_unique<Expr>(ExprKind::Member, loc);
            m->name = mname;
            m->lhs = std::move(e);
            m->intVal = isArrow ? 1 : 0; // 复用 intVal 标记 -> 还是 .
            e = std::move(m);
        } else if (at(Tok::Inc) || at(Tok::Dec)) {
            SourceLoc loc = peek().loc;
            Tok incTok = peek().kind;
            next();
            auto inc = std::make_unique<Expr>(ExprKind::IncDec, loc);
            inc->unOp = (incTok == Tok::Inc) ? UnaryOp::PostInc : UnaryOp::PostDec;
            inc->postfix = true;
            inc->lhs = std::move(e);
            e = std::move(inc);
        } else {
            break;
        }
    }
    return e;
}

ExprPtr Parser::parseGenericCall(const Token& identTok) {
    // ident < Type > ( args )
    next(); // <
    std::string tn; Type* t = nullptr;
    if (!parseTypeName(tn, t)) { ok_ = false; return nullptr; }
    if (!expect(Tok::Gt, "'>'")) return nullptr;
    auto e = std::make_unique<Expr>(ExprKind::GenericCall, identTok.loc);
    e->tmplName = identTok.text;
    e->name = tn; // 泛型参数类型名
    if (accept(Tok::LParen)) {
        while (!at(Tok::RParen) && !at(Tok::End)) {
            e->args.push_back(parseExpr());
            if (!accept(Tok::Comma)) break;
        }
        expect(Tok::RParen, "')'");
    }
    return e;
}

ExprPtr Parser::parseCallArgs(ExprPtr callee) {
    // callee 是 VarRef（名字）或 Member（obj.method）
    SourceLoc loc = peek().loc;
    auto call = std::make_unique<Expr>(ExprKind::Call, loc);
    if (callee && callee->kind == ExprKind::Member) {
        call->kind = ExprKind::MethodCall;
        call->lhs = std::move(callee->lhs);
        call->name = callee->name;
        call->intVal = callee->intVal; // 标记 ->/.
    } else if (callee && callee->kind == ExprKind::VarRef) {
        call->callee = callee->name;
        call->loc = callee->loc;
    } else {
        Diag::error(loc, "cannot call this expression");
        ok_ = false;
        // 尽量恢复
        while (!at(Tok::RParen) && !at(Tok::End)) parseExpr();
        if (at(Tok::RParen)) next();
        return nullptr;
    }
    next(); // (
    while (!at(Tok::RParen) && !at(Tok::End)) {
        call->args.push_back(parseExpr());
        if (!accept(Tok::Comma)) break;
    }
    expect(Tok::RParen, "')'");
    return call;
}

ExprPtr Parser::parseNew() {
    auto e = std::make_unique<Expr>(ExprKind::New, next().loc); // new
    std::string tn; Type* t = nullptr;
    if (!parseTypeName(tn, t)) { ok_ = false; return nullptr; }
    e->name = tn;
    if (at(Tok::LBracket)) {
        next();
        e->rhs = parseExpr(); // 长度
        expect(Tok::RBracket, "']'");
    } else {
        // new T —— 单个对象
        auto one = std::make_unique<Expr>(ExprKind::IntLit, e->loc);
        one->intVal = 1;
        e->rhs = std::move(one);
    }
    return e;
}

ExprPtr Parser::parseSizeof() {
    auto e = std::make_unique<Expr>(ExprKind::Sizeof, next().loc); // sizeof
    if (!expect(Tok::LParen, "'('")) return nullptr;
    // 尝试类型
    const Token& t = peek();
    if (isTypeToken(t) || t.kind == Tok::KwVoid || t.kind == Tok::KwEnt) {
        std::string tn; Type* tt = nullptr;
        if (parseTypeName(tn, tt)) {
            if (accept(Tok::RParen)) {
                e->name = tn;
                return e;
            }
        }
    }
    e->lhs = parseExpr();
    expect(Tok::RParen, "')'");
    return e;
}

ExprPtr Parser::parsePrimary() {
    const Token& t = peek();
    switch (t.kind) {
        case Tok::Int: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::IntLit, t.loc);
            e->intVal = t.intVal;
            return e;
        }
        case Tok::Float: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::FloatLit, t.loc);
            e->floatVal = t.floatVal;
            return e;
        }
        case Tok::Str: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::StrLit, t.loc);
            e->strVal = t.text;
            return e;
        }
        case Tok::Char: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::CharLit, t.loc);
            e->intVal = t.intVal;
            return e;
        }
        case Tok::KwTrue: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::BoolLit, t.loc);
            e->intVal = 1;
            return e;
        }
        case Tok::KwFalse: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::BoolLit, t.loc);
            e->intVal = 0;
            return e;
        }
        case Tok::KwNull: {
            next();
            return std::make_unique<Expr>(ExprKind::NullLit, t.loc);
        }
        case Tok::Ident: {
            next();
            auto e = std::make_unique<Expr>(ExprKind::VarRef, t.loc);
            e->name = t.text;
            return e;
        }
        case Tok::LParen: {
            next();
            ExprPtr inner = parseExpr();
            expect(Tok::RParen, "')'");
            return inner;
        }
        default:
            Diag::error(t.loc, "unexpected token in expression");
            ok_ = false;
            return nullptr;
    }
}

} // namespace j8
