// codegen.h — J8 代码生成：AST → Jadeight 汇编 → .bc
#pragma once

#include <map>
#include <string>
#include <vector>

#include "jadeight_asm.hpp"
#include "optimize.h"
#include "sema.h"

namespace j8 {

class CodeGen {
public:
    CodeGen(Sema* sema, CompileOptions& opts) : sema_(sema), opts_(opts) {}

    // 生成汇编文本 + 汇编出字节码（LE 头，FunctionSave 兼容）
    bool generate(std::string& asmText, std::vector<uint8_t>& outBytes,
                  uint32_t& argSize, uint32_t& retSize, uint32_t& entry);
    uint32_t stackSizeForDisplay() const { return stackSize_; }

    // v3 模块：汇编产出的函数目录 + 入口函数下标（driver 直接写 .bc 用）
    std::vector<jadeight::ModuleFunc> moduleFuncs;
    uint32_t entryFuncIndex = 0;

private:
    Sema* sema_;
    CompileOptions& opts_;

    // ---------------- 发射器 ----------------
    struct Emitter {
        bool counting = false;
        std::string* out = nullptr;
        std::map<std::string, int>* labels = nullptr;
        int pc = 0;
        int depth = 0;
        int maxDepth = 0;

        void reset(bool c, std::string* o, std::map<std::string, int>* lb) {
            counting = c; out = o; labels = lb; pc = 0; depth = 0; maxDepth = 0;
        }
        void ins(uint8_t op, const std::string& args = "", const std::string& comment = "") {
            pc += static_cast<int>(jadeight::opLenFixed(op));
            if (!counting) {
                *out += nameOf(op) + (args.empty() ? "" : " " + args) +
                        (comment.empty() ? "" : " ; " + comment) + "\n";
            }
        }
        // ISA v3：类型是操作数（TypeDesc）。MOVI/PUSH_IMM 的长度还要加立即数宽度。
        void insT(uint8_t op, uint8_t td, const std::string& args = "", const std::string& comment = "") {
            int len = static_cast<int>(jadeight::opLenFixed(op));
            if (op == jadeight::OP_MOVI || op == jadeight::OP_PUSH_IMM)
                len += jadeight::tdBytes(td);
            pc += len;
            if (!counting) {
                *out += nameOf(op) + " " + std::string(jadeight::tdName(td)) +
                        (args.empty() ? "" : " " + args) +
                        (comment.empty() ? "" : " ; " + comment) + "\n";
            }
        }
        // ISA v3 的 CVT 同时带源/目标类型
        void insCvt(uint8_t src, uint8_t dst, const std::string& comment = "") {
            pc += 3;
            if (!counting) {
                *out += "CVT " + std::string(jadeight::tdName(src)) + ", " +
                        std::string(jadeight::tdName(dst)) +
                        (comment.empty() ? "" : " ; " + comment) + "\n";
            }
        }
        // ISA v3 的 OP_CMP 带一个子操作（LT/LE/EQ/NE/GT/GE）
        void insCmp(uint8_t sub, uint8_t td, const std::string& comment = "") {
            static const char* names[] = {"LT", "LE", "EQ", "NE", "GT", "GE"};
            pc += 3;
            if (!counting) {
                *out += "CMP " + std::string(names[sub & 7]) + " " + std::string(jadeight::tdName(td)) +
                        (comment.empty() ? "" : " ; " + comment) + "\n";
            }
        }
        void label(const std::string& name) {
            if (counting) (*labels)[name] = pc;
            else *out += name + ":\n";   // v3 用标签跳转：标签要真的写进汇编文本
        }
        void dataByte(uint64_t v) {
            if (counting) pc += 1;
            else *out += ".BYTE " + std::to_string(v) + "\n";
        }
        void comment(const std::string& c) {
            if (!counting) *out += "; " + c + "\n";
        }
        void pushD(int w) { depth += w; if (depth > maxDepth) maxDepth = depth; }
        void popD(int w) { depth -= w; }
        void beginStmt() { depth = 0; }

        static std::string nameOf(uint8_t op);
    };

    Emitter em_;
    int curFuncIdx_ = 0;

    // ---------------- 实例偏移/尺寸 ----------------
    std::vector<int> codeSizes_;
    std::vector<int> codeOff_;
    std::vector<std::map<std::string, int>> labelPos_;
    std::vector<std::map<std::string, int>> labelPosEmit_;
    int mainIndex_ = -1;

    // ---------------- 数据段 ----------------
    std::vector<std::string> strings_;
    std::map<std::string, int> stringRelOff_;
    int dataStart_ = 0;
    int dataCursor_ = 0;

    uint32_t stackSize_ = 16384;
    uint32_t scopeSize_ = 16;

    // ---------------- 寄存器约定 ----------------
    static constexpr int kBcBase = 12;
    static constexpr int kWorld = 13;

    // ---------------- 内部 ----------------
    void measureAll();
    void layoutData();
    void assignOffsets();
    void computeStackSizes();
    void emitAll(std::string& asmText);
    void emitDataSection(std::string& asmText);

    void emitFunction(FuncInstance* inst, int idx, bool counting);
    void emitStmt(Stmt* s, FuncInstance* inst);
    void emitStmts(std::vector<StmtPtr>& body, FuncInstance* inst);
    void emitPrologue(FuncInstance* inst);
    void emitReturn(FuncInstance* inst);

    void ins(uint8_t op, const std::string& args = "", const std::string& comment = "") { em_.ins(op, args, comment); }
    void insT(uint8_t op, uint8_t td, const std::string& args = "", const std::string& comment = "") { em_.insT(op, td, args, comment); }
    void insCmp(uint8_t sub, uint8_t td, const std::string& comment = "") { em_.insCmp(sub, td, comment); }
    void insCvt(uint8_t src, uint8_t dst, const std::string& comment = "") { em_.insCvt(src, dst, comment); }
    void label(const std::string& n) { em_.label(n); }
    void pushConst(int width, uint64_t value, const std::string& comment = "");
    void pushReg(int reg, int width);
    void popReg(int reg, int width);
    void loadSlot(int reg, int width, int off);
    void storeSlot(int reg, int width, int off);
    void loadPtr(int reg, int width, int slot);
    void storePtr(int reg, int width, int slot);
    int tempOffset(FuncInstance* inst, int k);
    std::string newLabel(const std::string& prefix);
    int labelCount_ = 0;

    // 表达式
    void emitExpr(Expr* e, FuncInstance* inst);
    void emitAddress(Expr* e, FuncInstance* inst);
    void emitBoolFlag(Expr* e, FuncInstance* inst);
    void emitBinaryOp(BinaryOp op, Type* t, FuncInstance* inst);
    void emitCompare(BinaryOp op, Type* t, FuncInstance* inst);
    void emitBoolNot();   // NOT_U8 + AND 1 → 规范化 0/1
    void coerceOnStack(Type* from, Type* to, FuncInstance* inst);
    void loadThroughAddress(Type* t, FuncInstance* inst);
    void emitAssign(Expr* e, FuncInstance* inst);
    void emitCompoundAssign(Expr* e, FuncInstance* inst);
    void emitIncDec(Expr* e, FuncInstance* inst);
    void emitNew(Expr* e, FuncInstance* inst);
    void emitCallExpr(Expr* e, FuncInstance* inst);
    void emitStaticCall(FuncInstance* callee, std::vector<ExprPtr>& args, FuncInstance* inst);
    void emitMethodCall(FuncInstance* callee, Expr* obj, std::vector<ExprPtr>& args, FuncInstance* inst);
    void emitObjectAddress(Expr* obj, FuncInstance* inst);
    void emitDynamicCall(Expr* e, FuncInstance* inst);
    void emitExternCall(Expr* e, FuncInstance* inst);
    void externMemcpy(Expr* e, FuncInstance* inst);
    void externFree(Expr* e, FuncInstance* inst);
    void callHelper(const std::string& name, int argWidth, FuncInstance* inst);
    FuncInstance* helper(const std::string& name);
    int instanceIndex(FuncInstance* fi);
    void branchOnFlag(const std::string& lTrue, const std::string& lFalse);
    void jumpTo(const std::string& target);

    // 内建 / ECS
    void emitPrint(Expr* e, FuncInstance* inst);
    void emitEcs(Expr* e, FuncInstance* inst, const std::string& name, const std::string& tmpl);
    void worldAddr(uint64_t off);

    std::vector<std::pair<std::string, std::string>> loopStack_;  // break, continue
};

} // namespace j8
