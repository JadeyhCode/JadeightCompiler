// ast.h — J8 语言 AST 定义
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common.h"

namespace j8 {

struct Expr;
struct Stmt;
struct Decl;
struct ResolvedCall;   // sema.h 中定义，sema 填充

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;

// ---------------- 表达式 ----------------
enum class ExprKind {
    IntLit, FloatLit, StrLit, BoolLit, CharLit, NullLit,
    VarRef,        // 名字（变量/参数/组件字段等）
    Unary,         // ! ~ - + & * ++ -- (前/后缀)
    Binary,        // 算术/位/比较/逻辑
    Ternary,       // c ? a : b
    Call,          // 普通/泛型/方法调用（经语义分析解析）
    MethodCall,    // obj.method(args) 或 obj->method(args)
    Index,         // a[i]
    Member,        // a.b 或 a->b
    Cast,          // (T)expr
    Sizeof,        // sizeof(T) / sizeof(expr)
    New,           // new T[n]
    Assign,        // =
    CompoundAssign,// += 等
    IncDec,        // ++/--
    EcsCall,       // spawn()/get<C>(e)/add<C>(e,...)/run<S>()/has<C>/remove<C>/despawn/alive/entity_count
    BuiltinCall,   // print/sqrt/log/memcpy/free/atomic_* 等（语义分析后解析）
    GenericCall,   // f<T>(args) —— 泛型实例化请求
};

enum class UnaryOp { Neg, Pos, Not, BitNot, Deref, AddrOf, PreInc, PreDec, PostInc, PostDec };
enum class BinaryOp {
    Add, Sub, Mul, Div, Mod,      // 算术
    Shl, Shr, And, Or, Xor,       // 位
    Lt, Le, Gt, Ge, Eq, Ne,       // 比较
    LogAnd, LogOr,                // 逻辑
};
enum class AssignOp { Assign, Add, Sub, Mul, Div, Mod, Shl, Shr, And, Or, Xor };

struct Expr {
    ExprKind kind;
    SourceLoc loc;
    Type* type = nullptr;          // 语义分析后填充
    bool lvalue = false;

    // IntLit / CharLit / BoolLit
    uint64_t intVal = 0;
    // FloatLit
    double floatVal = 0.0;
    // StrLit
    std::string strVal;
    // VarRef / Member / MethodCall 的名字
    std::string name;
    // VarRef 解析后：变量符号 id（由语义分析设置）
    int varId = -1;
    // Unary / Binary / Cast / Index / Member / Assign / CompoundAssign
    UnaryOp unOp = UnaryOp::Neg;
    BinaryOp binOp = BinaryOp::Add;
    AssignOp asOp = AssignOp::Assign;
    ExprPtr lhs, rhs;              // 二元/赋值：左/右；一元：lhs=操作数
    ExprPtr cond, thenExpr, elseExpr; // Ternary
    std::vector<ExprPtr> args;     // Call / MethodCall / GenericCall / EcsCall
    // Call：被调函数名；MethodCall：obj 在 lhs
    std::string callee;
    // GenericCall / EcsCall：模板/组件/系统名
    std::string tmplName;
    // Index：lhs = 数组，rhs = 下标
    // Cast：lhs = 表达式，name = 目标类型名
    // Member：lhs = 对象，name = 字段
    // IncDec：lhs = 操作数
    bool postfix = false;

    // 语义分析结果：
    ResolvedCall* resolved = nullptr;  // Call/MethodCall 的解析结果
    int compIdx = -1;                  // Member 在 ent 上时：组件下标（ECS）
    bool isThisBox = false;            // 协议默认方法里的 this（存在类型盒）

    // 优化器用：若可求值，记录常量
    std::optional<ConstVal> constVal;

    Expr(ExprKind k, const SourceLoc& l) : kind(k), loc(l) {}
};

// ---------------- 语句 ----------------
enum class StmtKind {
    Block, ExprStmt, If, While, DoWhile, For, Break, Continue, Return, VarDecl, Empty,
};

struct VarDeclStmt {
    std::string name;
    std::string typeName;      // 原始类型名（语义分析后填 type）
    Type* type = nullptr;
    ExprPtr init;              // 可为空
    bool isConst = false;
    int varId = -1;
    int arraySize = 0;         // >0: T name[N] 数组局部
    // 结构体初始化 {a, b, ...}
    std::vector<ExprPtr> fieldInits;
};

struct Stmt {
    StmtKind kind;
    SourceLoc loc;
    std::vector<StmtPtr> body;          // Block / If(子语句用 single)
    StmtPtr single;                     // If/While/DoWhile/For 的单语句体
    ExprPtr cond;                       // If/While/For
    ExprPtr init;                       // For 的初始化表达式（或 VarDecl 存 varDecl）
    std::unique_ptr<VarDeclStmt> forVar; // For 的声明式初始化
    ExprPtr step;                       // For 的步进
    ExprPtr expr;                       // ExprStmt / Return
    std::unique_ptr<VarDeclStmt> varDecl; // VarDecl
    // #pragma unroll N（附着在 For 上）
    bool hasUnrollPragma = false;
    int unrollFactor = 0;
    // 循环标签（优化/代码生成用）
    std::string loopId;

    Stmt(StmtKind k, const SourceLoc& l) : kind(k), loc(l) {}
};

// ---------------- 声明 ----------------
enum class DeclKind {
    Function, Extern, Struct, Protocol, Impl, Component, System, Var,
};

struct Param {
    std::string name;
    std::string typeName;
    Type* type = nullptr;
    int offset = 0;            // 帧内参数偏移（语义/布局阶段）
};

struct Field {
    std::string name;
    std::string typeName;
    Type* type = nullptr;
    int offset = 0;            // 结构体/组件内偏移
};

// 协议方法：`ret name(this, args) [; | { body }]`
struct ProtoMethod {
    std::string name;
    Type* retType = nullptr;
    std::string retTypeName;
    std::vector<Param> params;   // 不含 this
    StmtPtr body;                // 默认实现（可为空）
    int index = 0;               // 协议内方法序号
};

struct Decl {
    DeclKind kind;
    SourceLoc loc;
    std::string name;

    // Function
    std::string retTypeName;
    Type* retType = nullptr;
    std::vector<Param> params;
    StmtPtr body;
    std::vector<std::string> genericParams;  // 泛型函数
    // 泛型约束：paramName -> protocolName（如 T -> Shape）
    std::vector<std::pair<std::string, std::string>> constraints;
    // 布局结果
    int paramBytes = 0;
    int localBytes = 0;
    int retBytes = 0;
    int maxExprDepth = 0;      // 表达式的最大栈深（字节）
    int codeOffset = -1;       // 函数入口字节码偏移
    bool isMain = false;
    bool isSystemUpdate = false; // system 的 update 函数（隐藏 worldPtr 参数）
    int ecsWorldSlot = -1;

    // Extern
    std::vector<std::string> externTypes;  // 参数类型名
    std::string externRetType;
    int externIndex = -1;

    // Struct / Component
    std::vector<Field> fields;
    int structSize = 0;

    // Protocol
    std::vector<ProtoMethod> methods;
    std::vector<std::string> inherited;    // 协议继承（v1 不做，预留）

    // Impl
    std::string protoName;      // 实现的协议
    std::string typeName;       // 实现类型
    std::vector<std::pair<std::string, StmtPtr>> implMethods; // name -> body
    // 系统
    std::vector<std::string> componentNames;
    std::string sysUpdateParamName = "e";   // system update 的参数名

    // Var（全局：v1 仅编译器内部使用）
    Type* type = nullptr;
    ExprPtr init;

    Decl(DeclKind k, const SourceLoc& l) : kind(k), loc(l) {}
};

struct Program {
    std::vector<DeclPtr> decls;
};

// AST 深拷贝（optimize.cpp 实现）
Stmt* cloneStmt(const Stmt* s);
Expr* cloneExpr(const Expr* e);

} // namespace j8
