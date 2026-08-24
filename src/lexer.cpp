// lexer.cpp — J8 词法分析器实现
#include "lexer.h"

#include <cctype>
#include <cstdlib>

namespace j8 {

int Diag::errorCount = 0;
int Diag::warningCount = 0;
CompileOptions gOpts;

void Diag::error(const SourceLoc& loc, const std::string& msg) {
    ++errorCount;
    std::fprintf(stderr, "error:%d:%d: %s\n", loc.line, loc.col, msg.c_str());
}
void Diag::warn(const SourceLoc& loc, const std::string& msg) {
    ++warningCount;
    std::fprintf(stderr, "warning:%d:%d: %s\n", loc.line, loc.col, msg.c_str());
}
void Diag::note(const std::string& msg) {
    std::fprintf(stderr, "note: %s\n", msg.c_str());
}

// ---------------- Type ----------------
Type* Type::make(TypeKind k) { auto* t = new Type; t->kind = k; return t; }
Type* Type::makePtr(Type* p) { auto* t = new Type; t->kind = TypeKind::Ptr; t->pointee = p; return t; }
Type* Type::makeStruct(const std::string& n) { auto* t = new Type; t->kind = TypeKind::Struct; t->name = n; return t; }
Type* Type::makeExist(const std::string& p) { auto* t = new Type; t->kind = TypeKind::Exist; t->name = p; return t; }
Type* Type::makeGeneric(const std::string& n) { auto* t = new Type; t->kind = TypeKind::GenericParam; t->name = n; return t; }
Type* Type::err() { auto* t = new Type; t->kind = TypeKind::Err; return t; }

std::string Type::str() const {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::U8: return "u8";
        case TypeKind::U16: return "u16";
        case TypeKind::U32: return "u32";
        case TypeKind::U64: return "u64";
        case TypeKind::I8: return "i8";
        case TypeKind::I16: return "i16";
        case TypeKind::I32: return "i32";
        case TypeKind::I64: return "i64";
        case TypeKind::F64: return "f64";
        case TypeKind::Bool: return "bool";
        case TypeKind::Char: return "char";
        case TypeKind::Ent: return "ent";
        case TypeKind::Ptr: return pointee ? (pointee->str() + "*") : "void*";
        case TypeKind::Struct: return name;
        case TypeKind::Exist: return name;
        case TypeKind::GenericParam: return name;
        default: return "?";
    }
}

bool Type::equals(const Type* o) const {
    if (!o) return false;
    if (kind != o->kind) return false;
    if (kind == TypeKind::Ptr) return pointee->equals(o->pointee);
    if (kind == TypeKind::Struct || kind == TypeKind::Exist ||
        kind == TypeKind::GenericParam) return name == o->name;
    return true;
}

// ---------------- Lexer ----------------
char Lexer::peek(size_t off) const {
    if (pos_ + off >= src_.size()) return '\0';
    return src_[pos_ + off];
}
char Lexer::advance() {
    if (pos_ >= src_.size()) return '\0';
    char c = src_[pos_++];
    if (c == '\n') { ++line_; col_ = 1; } else { ++col_; }
    return c;
}

void Lexer::skipWsAndComments() {
    for (;;) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { advance(); continue; }
        if (c == '/' && peek(1) == '/') { while (peek() && peek() != '\n') advance(); continue; }
        if (c == '/' && peek(1) == '*') {
            advance(); advance();
            while (peek() && !(peek() == '*' && peek(1) == '/')) advance();
            if (peek()) { advance(); advance(); }
            continue;
        }
        break;
    }
}

Token Lexer::make(Tok k) {
    Token t;
    t.kind = k;
    t.loc = { line_, col_ };
    return t;
}

Token Lexer::lexIdentOrKeyword() {
    Token t = make(Tok::Ident);
    std::string s;
    while (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_') s += advance();
    t.text = s;

    // 关键字表
    struct KW { const char* k; Tok t; };
    static const KW kws[] = {
        {"fn", Tok::KwFn}, {"return", Tok::KwReturn}, {"if", Tok::KwIf}, {"else", Tok::KwElse},
        {"while", Tok::KwWhile}, {"do", Tok::KwDo}, {"for", Tok::KwFor},
        {"break", Tok::KwBreak}, {"continue", Tok::KwContinue},
        {"struct", Tok::KwStruct}, {"protocol", Tok::KwProtocol}, {"impl", Tok::KwImpl},
        {"component", Tok::KwComponent}, {"system", Tok::KwSystem}, {"extern", Tok::KwExtern},
        {"const", Tok::KwConst}, {"new", Tok::KwNew}, {"delete", Tok::KwDelete},
        {"null", Tok::KwNull}, {"true", Tok::KwTrue}, {"false", Tok::KwFalse},
        {"ent", Tok::KwEnt}, {"void", Tok::KwVoid}, {"sizeof", Tok::KwSizeof},
        {"world", Tok::KwWorld},
    };
    for (const auto& kw : kws) {
        if (s == kw.k) { t.kind = kw.t; break; }
    }
    return t;
}

Token Lexer::lexNumber() {
    Token t = make(Tok::Int);
    std::string s;
    bool isFloat = false;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        s += advance(); s += advance();
        while (std::isxdigit(static_cast<unsigned char>(peek()))) s += advance();
        t.intVal = std::strtoull(s.c_str(), nullptr, 16);
    } else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        s += advance(); s += advance();
        while (peek() == '0' || peek() == '1') s += advance();
        t.intVal = std::strtoull(s.c_str(), nullptr, 2);
    } else {
        while (std::isdigit(static_cast<unsigned char>(peek()))) s += advance();
        if (peek() == '.' && std::isdigit(static_cast<unsigned char>(peek(1)))) {
            isFloat = true;
            s += advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) s += advance();
        }
        if (peek() == 'e' || peek() == 'E') {
            isFloat = true;
            s += advance();
            if (peek() == '+' || peek() == '-') s += advance();
            while (std::isdigit(static_cast<unsigned char>(peek()))) s += advance();
        }
        if (isFloat) {
            t.floatVal = std::strtod(s.c_str(), nullptr);
            t.kind = Tok::Float;
        } else {
            t.intVal = std::strtoull(s.c_str(), nullptr, 10);
        }
    }
    t.text = s;
    return t;
}

static uint64_t hexVal(char c) {
    if (c >= '0' && c <= '9') return static_cast<uint64_t>(c - '0');
    if (c >= 'a' && c <= 'f') return static_cast<uint64_t>(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return static_cast<uint64_t>(c - 'A' + 10);
    return 0;
}

Token Lexer::lexString() {
    Token t = make(Tok::Str);
    advance(); // "
    std::string s;
    while (peek() && peek() != '"') {
        char c = advance();
        if (c == '\\') {
            char e = advance();
            switch (e) {
                case 'n': s += '\n'; break;
                case 't': s += '\t'; break;
                case 'r': s += '\r'; break;
                case '0': s += '\0'; break;
                case '\\': s += '\\'; break;
                case '\'': s += '\''; break;
                case '"': s += '"'; break;
                case 'x': {
                    uint64_t v = 0;
                    for (int i = 0; i < 2 && std::isxdigit(static_cast<unsigned char>(peek())); ++i)
                        v = (v << 4) | hexVal(advance());
                    s += static_cast<char>(v);
                    break;
                }
                default: s += e; break;
            }
        } else s += c;
    }
    if (peek() == '"') advance();
    else { ok_ = false; Diag::error(t.loc, "unterminated string literal"); }
    t.text = s;
    return t;
}

Token Lexer::lexChar() {
    Token t = make(Tok::Char);
    advance(); // '
    uint64_t v = 0;
    if (peek() == '\\') {
        advance();
        char e = advance();
        switch (e) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case '0': v = '\0'; break;
            case '\\': v = '\\'; break;
            case '\'': v = '\''; break;
            case 'x': {
                for (int i = 0; i < 2 && std::isxdigit(static_cast<unsigned char>(peek())); ++i)
                    v = (v << 4) | hexVal(advance());
                break;
            }
            default: v = static_cast<uint64_t>(e); break;
        }
    } else if (peek()) v = static_cast<uint64_t>(advance());
    if (peek() == '\'') advance();
    else { ok_ = false; Diag::error(t.loc, "unterminated char literal"); }
    t.intVal = v;
    return t;
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> toks;
    for (;;) {
        skipWsAndComments();
        SourceLoc loc = { line_, col_ };
        char c = peek();
        if (!c) { Token t = make(Tok::End); t.loc = loc; toks.push_back(t); break; }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') { toks.push_back(lexIdentOrKeyword()); continue; }
        if (std::isdigit(static_cast<unsigned char>(c))) { toks.push_back(lexNumber()); continue; }
        if (c == '"') { toks.push_back(lexString()); continue; }
        if (c == '\'') { toks.push_back(lexChar()); continue; }
        if (c == '#') {
            // #pragma unroll N
            Token t = make(Tok::Pragma);
            advance();
            std::string s;
            while (peek() && peek() != '\n') s += advance();
            t.text = s;
            toks.push_back(t);
            continue;
        }
        advance();
        Token t = make(Tok::Err);
        t.loc = loc;
        switch (c) {
            case '(': t.kind = Tok::LParen; break;
            case ')': t.kind = Tok::RParen; break;
            case '{': t.kind = Tok::LBrace; break;
            case '}': t.kind = Tok::RBrace; break;
            case '[': t.kind = Tok::LBracket; break;
            case ']': t.kind = Tok::RBracket; break;
            case ',': t.kind = Tok::Comma; break;
            case ';': t.kind = Tok::Semicolon; break;
            case ':': t.kind = Tok::Colon; break;
            case '.': t.kind = Tok::Dot; break;
            case '?': t.kind = Tok::Question; break;
            case '+':
                if (peek() == '+') { advance(); t.kind = Tok::Inc; }
                else if (peek() == '=') { advance(); t.kind = Tok::PlusEq; }
                else t.kind = Tok::Plus;
                break;
            case '-':
                if (peek() == '>') { advance(); t.kind = Tok::Arrow; }
                else if (peek() == '-') { advance(); t.kind = Tok::Dec; }
                else if (peek() == '=') { advance(); t.kind = Tok::MinusEq; }
                else t.kind = Tok::Minus;
                break;
            case '*':
                if (peek() == '=') { advance(); t.kind = Tok::StarEq; }
                else t.kind = Tok::Star;
                break;
            case '/':
                if (peek() == '=') { advance(); t.kind = Tok::SlashEq; }
                else t.kind = Tok::Slash;
                break;
            case '%':
                if (peek() == '=') { advance(); t.kind = Tok::PercentEq; }
                else t.kind = Tok::Percent;
                break;
            case '&':
                if (peek() == '&') { advance(); t.kind = Tok::AmpAmp; }
                else if (peek() == '=') { advance(); t.kind = Tok::AmpEq; }
                else t.kind = Tok::Amp;
                break;
            case '|':
                if (peek() == '|') { advance(); t.kind = Tok::PipePipe; }
                else if (peek() == '=') { advance(); t.kind = Tok::PipeEq; }
                else t.kind = Tok::Pipe;
                break;
            case '^':
                if (peek() == '=') { advance(); t.kind = Tok::CaretEq; }
                else t.kind = Tok::Caret;
                break;
            case '~': t.kind = Tok::Tilde; break;
            case '!':
                if (peek() == '=') { advance(); t.kind = Tok::BangEq; }
                else t.kind = Tok::Bang;
                break;
            case '<':
                if (peek() == '<') { advance(); if (peek() == '=') { advance(); t.kind = Tok::ShlEq; } else t.kind = Tok::Shl; }
                else if (peek() == '=') { advance(); t.kind = Tok::Le; }
                else t.kind = Tok::Lt;
                break;
            case '>':
                if (peek() == '>') { advance(); if (peek() == '=') { advance(); t.kind = Tok::ShrEq; } else t.kind = Tok::Shr; }
                else if (peek() == '=') { advance(); t.kind = Tok::Ge; }
                else t.kind = Tok::Gt;
                break;
            case '=':
                if (peek() == '=') { advance(); t.kind = Tok::EqEq; }
                else t.kind = Tok::Assign;
                break;
            default:
                Diag::error(t.loc, std::string("unexpected character '") + c + "'");
                ok_ = false;
                break;
        }
        toks.push_back(t);
    }
    return toks;
}

} // namespace j8
