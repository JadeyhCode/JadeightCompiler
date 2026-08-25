// common.h — J8 编译器公共定义（诊断、类型、工具）
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace j8 {

// ---------------- 诊断 ----------------
struct SourceLoc {
    int line = 0;
    int col = 0;
};

struct Diag {
    static int errorCount;
    static int warningCount;
    static void error(const SourceLoc& loc, const std::string& msg);
    static void warn(const SourceLoc& loc, const std::string& msg);
    static void note(const std::string& msg);
};

// ---------------- 类型 ----------------
enum class TypeKind : uint8_t {
    Void, U8, U16, U32, U64, I8, I16, I32, I64, F64,
    Bool,   // = u8
    Char,   // = u8
    Ptr,    // T*
    Struct, // named struct
    Ent,    // ECS entity handle (u32)
    Exist,  // protocol existential box {data*, vt*}
    GenericParam,
    Err,
};

struct Type {
    TypeKind kind = TypeKind::Void;
    std::string name;              // Struct/Exist/GenericParam 的名字
    Type* pointee = nullptr;       // Ptr
    std::shared_ptr<Type> pointeeShared; // 生命周期管理（简单起见用 shared_ptr 存根）

    static Type* make(TypeKind k);
    static Type* makePtr(Type* pointee);
    static Type* makeStruct(const std::string& n);
    static Type* makeExist(const std::string& protocolName);
    static Type* makeGeneric(const std::string& n);
    static Type* err();

    bool isInt() const { return kind >= TypeKind::U8 && kind <= TypeKind::I64; }
    bool isSigned() const { return kind == TypeKind::I8 || kind == TypeKind::I16 ||
                                   kind == TypeKind::I32 || kind == TypeKind::I64; }
    bool isFloat() const { return kind == TypeKind::F64; }
    bool isNumeric() const { return isInt() || isFloat(); }
    bool isPointer() const { return kind == TypeKind::Ptr; }
    bool isStruct() const { return kind == TypeKind::Struct; }
    std::string str() const;
    bool equals(const Type* o) const;
};

// 类型宽度（字节）
inline int typeWidth(const Type* t) {
    switch (t->kind) {
        case TypeKind::Void: return 0;
        case TypeKind::U8: case TypeKind::I8: case TypeKind::Bool: case TypeKind::Char: return 1;
        case TypeKind::U16: case TypeKind::I16: return 2;
        case TypeKind::U32: case TypeKind::I32: case TypeKind::Ent: return 4;
        case TypeKind::U64: case TypeKind::I64: case TypeKind::F64:
        case TypeKind::Ptr: case TypeKind::GenericParam: return 8;
        case TypeKind::Struct: return -1; // 由结构体表决定
        case TypeKind::Exist: return 16;
        default: return 0;
    }
}

// VM 数值类型标签（用于 opcode 后缀）
enum class VmNum : uint8_t { U8, U16, U32, U64, I8, I16, I32, I64, F64, Ptr };

// ---------------- 常量值（优化器用） ----------------
struct ConstVal {
    bool isFloat = false;
    uint64_t i = 0;   // 整型位模式
    double f = 0.0;
    bool valid = false;
    static ConstVal Int(uint64_t v) { ConstVal c; c.i = v; c.valid = true; return c; }
    static ConstVal Float(double v) { ConstVal c; c.isFloat = true; c.f = v; c.valid = true; return c; }
    static ConstVal Invalid() { return ConstVal(); }
};

// ---------------- 全局选项 ----------------
struct CompileOptions {
    int optLevel = 2;
    bool emitAsm = false;        // -S: 保留 .jasm
    bool dumpAst = false;
    bool verbose = false;
    bool noUnroll = false;
    bool noInline = false;
    int unrollLimit = 8;         // 全展开的迭代数上限
    int ecsCapacity = 4096;
    uint32_t stackSize = 0;      // 0 = 自动计算
    uint32_t recursionStackSize = 1048576; // 递归默认栈 1MB（约 1.5 万层深递归）
    std::string externsPath;     // -emit-externs
};

extern CompileOptions gOpts;

// 字符串转小写
inline std::string toLower(std::string s) {
    for (char& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}

} // namespace j8
