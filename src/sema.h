// sema.h — J8 语义分析：符号表、类型检查、协议一致性、泛型实例化、帧布局
#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "ast.h"

namespace j8 {

// 变量槽（帧内）
struct VarSlot {
    int offset = 0;      // 相对函数帧基址（mode0 偏移）
    int width = 0;
    bool isParam = false;
    bool isConst = false;
    bool isArray = false;    // 局部数组（VarRef 取值 = 地址）
    Type* type = nullptr;
    std::string name;
};

// 函数实例（含泛型实例化）：一个可编译/调用的具体函数
struct FuncInstance {
    Decl* fn = nullptr;                          // 源声明
    std::string key;                             // 去重键
    std::map<std::string, Type*> typeEnv;        // 泛型参数 -> 具体类型
    std::vector<Type*> paramTypes;
    std::vector<std::string> paramNames;
    Type* retType = nullptr;
    int paramBytes = 0;
    int localBytes = 0;
    int userLocalBytes = 0;                      // 不含 64 字节 temp 区
    int tempBase = 0;                            // temp 槽起始（相对帧基址）
    int retBytes = 0;
    int maxExprDepth = 0;                        // 表达式最大栈深（字节），codegen 填
    int codeOffset = -1;                         // 字节码入口偏移，codegen 填
    bool isMain = false;
    bool isSystemUpdate = false;                 // system 的 update（this=ent 在 params[0]）
    std::string sysName;                         // 所属 system
    bool isDefaultImpl = false;                  // 协议默认实现（params=[data,vt]，this=盒）
    std::string protoName;                       // 默认实现所属协议
    std::map<int, VarSlot> slots;                // varId -> 槽
    std::map<std::string, int> paramVarIds;      // 参数名 -> varId
    std::map<std::string, int> localVarIds;      // 局部变量名 -> varId
    bool bodyChecked = false;
    Stmt* compileBody = nullptr;                 // 实际编译的函数体（泛型/默认为克隆）
};

// 泛型函数符号
struct FuncSym {
    std::string name;
    Decl* decl = nullptr;
    bool isExtern = false;
    int externIndex = -1;
    bool isGeneric = false;
    std::vector<std::string> genericParams;
    std::vector<std::pair<std::string, std::string>> constraints; // param->protocol
    std::map<std::string, FuncInstance*> instances; // key -> 实例
    FuncInstance* baseInstance = nullptr;  // 非泛型
    // impl 方法：protoName / typeName / methodName / 协议方法序号
    std::string implProto, implType, implMethod;
    int protoMethodIndex = -1;
    bool isDefaultImpl = false;            // 协议默认实现函数
};

// 调用解析结果
struct ResolvedCall {
    FuncInstance* callee = nullptr;   // 普通/泛型/impl 方法/helper
    bool isExtern = false;
    int externIndex = -1;
    // 协议动态派发：methodIndex 为协议内方法序号（witness 表下标）
    bool isDynamicDispatch = false;
    int witnessMethodIndex = -1;
    std::string protoName;
};

class Sema {
public:
    explicit Sema(Program* prog) : prog_(prog) {}

    // 全流程：建表 -> 一致性检查 -> 布局/类型检查（含泛型实例化）-> ECS 布局
    bool run();

    // ---- 结果表（供 optimize/codegen 使用）----
    std::map<std::string, Decl*> structs;       // struct + component（名字去重）
    std::map<std::string, Decl*> protocols;     // 协议
    std::map<std::string, Decl*> impls;         // key = proto::type
    std::map<std::string, Decl*> systems;
    std::map<std::string, FuncSym> functions;   // 函数符号（含 extern）
    std::vector<FuncInstance*> instances;       // 所有待编译实例
    std::vector<Decl*> externs;                 // 按 EXTERN_CALL 索引顺序
    std::vector<Decl*> components;              // 组件（声明顺序）
    std::map<std::string, int> compIndex;

    // ECS world 布局
    int worldBytes = 0;                         // 总堆字节数
    int worldHeader = 8;                        // [count:u32][cap:u32]
    struct CompLayout { int bitsetOff = 0; int dataOff = 0; int size = 0; };
    std::map<std::string, CompLayout> compLayout;

    // 存在类型 witness 表：key = proto::type -> 数据段偏移（codegen 填）
    struct WitnessInfo {
        std::string proto, type;
        std::vector<FuncInstance*> methodEntries;  // 各方法实例（codegen 读 codeOffset）
        int dataOff = -1;                // codegen 填
    };
    std::vector<WitnessInfo> witnesses;
    std::map<std::string, int> witnessIndex;   // "proto::type" -> index
    void ensureWitness(const std::string& proto, const std::string& type);

    // 查找
    Type* resolveType(const std::string& typeName, const std::map<std::string, Type*>* env = nullptr);
    FuncInstance* getInstance(const std::string& fnName, const std::map<std::string, Type*>& env);
    FuncSym* findFunc(const std::string& name);
    Decl* findImpl(const std::string& proto, const std::string& type);
    int sizeOf(Type* t);
    int structSize(Decl* d);

private:
    Program* prog_;
    std::set<std::string> building_;   // 防止递归实例化死循环

    void buildTables();
    bool checkConformance();
    void buildHelpers();
    bool buildInstance(FuncSym* sym, const std::map<std::string, Type*>& env, const std::string& key);
    bool checkBody(FuncInstance* inst, Stmt* body);

    // 表达式/语句类型检查（填充 type、varId、解析调用）
    Type* checkExpr(Expr* e, FuncInstance* inst);
    bool checkStmt(Stmt* s, FuncInstance* inst, Type* retType);

    Type* checkCall(Expr* e, FuncInstance* inst);
    Type* checkMethodCall(Expr* e, FuncInstance* inst);
    Type* checkBuiltin(Expr* e, FuncInstance* inst, const std::string& name);
    Type* checkEcsCall(Expr* e, FuncInstance* inst, const std::string& name, const std::string& tmpl);

    // 帧布局
    void layoutLocals(FuncInstance* inst);
    VarSlot* findVar(FuncInstance* inst, const std::string& name);
    FuncInstance* cur_ = nullptr;
    int nextVarId_ = 1;

    // 转换辅助
    Type* promoteBinary(Type* a, Type* b, const SourceLoc& loc, bool& ok);
    Type* unsignedOf(Type* t);
    bool assignable(Type* dst, Type* src, const SourceLoc& loc);
    bool checkAssignable(Type* dst, Type* src, const SourceLoc& loc);
    bool isConforming(const std::string& type, const std::string& proto);

    // 组件布局
    void layoutComponents();
};

// 常量求值（optimize 也使用）
ConstVal evalConst(const Expr* e);

} // namespace j8
