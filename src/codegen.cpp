// codegen.cpp — J8 代码生成实现
#include "codegen.h"

#include <cstring>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace j8 {

using namespace jadeight;

std::string CodeGen::Emitter::nameOf(uint8_t op) { return std::string(jadeight::opName(op)); }

// ==================== opcode 选择 ====================
// VmNum 定义于 common.h

static VmNum vmNum(Type* t) {
    if (!t) return VmNum::U64;
    switch (t->kind) {
        case TypeKind::U8: case TypeKind::Bool: case TypeKind::Char: return VmNum::U8;
        case TypeKind::U16: return VmNum::U16;
        case TypeKind::U32: case TypeKind::Ent: return VmNum::U32;
        case TypeKind::U64: return VmNum::U64;
        case TypeKind::I8: return VmNum::I8;
        case TypeKind::I16: return VmNum::I16;
        case TypeKind::I32: return VmNum::I32;
        case TypeKind::I64: return VmNum::I64;
        case TypeKind::F64: return VmNum::F64;
        case TypeKind::Ptr: return VmNum::Ptr;
        default: return VmNum::U64;
    }
}
static int vmW(VmNum n) {
    switch (n) { case VmNum::U8: case VmNum::I8: return 1;
                 case VmNum::U16: case VmNum::I16: return 2;
                 case VmNum::U32: case VmNum::I32: return 4;
                 default: return 8; }
}

// 有符号整型复用同宽无符号 opcode（VM 的按位/移位/寄存器指令没有符号变体）
// ISA v3 把「宽度 + 符号 + 浮点 + 指针」合并成一个参数字节（TypeDesc），
// 所以 v2 里 kAdd/kMul/opCmpLT/... 这些"按类型选 opcode"的表全部塌缩成 tdOf()。
// 旧的 helper 名字保留为别名，调用点只多传一个 opcode：insT(OP_ADD, kAdd(n))。
static uint8_t tdOf(VmNum n) {
    switch (n) {
        case VmNum::U8: return TD_U8;
        case VmNum::I8: return TD_I8;
        case VmNum::U16: return TD_U16;
        case VmNum::I16: return TD_I16;
        case VmNum::U32: return TD_U32;
        case VmNum::I32: return TD_I32;
        case VmNum::U64: return TD_U64;
        case VmNum::I64: return TD_I64;
        case VmNum::F64: return TD_F64;
        case VmNum::Ptr: return TD_PTR;
        default: return TD_U64;
    }
}
static const auto kAdd = tdOf, kSub = tdOf, kShl = tdOf, kShr = tdOf, kShri = tdOf;
static const auto kAnd = tdOf, kOr = tdOf, kNot = tdOf;
static const auto kRegMovi = tdOf, kRegPush = tdOf, kRegPop = tdOf, kRegLoad = tdOf, kRegStore = tdOf;
static const auto opMul = tdOf, opDiv = tdOf, opSqrt = tdOf, opLog = tdOf;
static const auto opCmpLT = tdOf, opCmpEQ = tdOf, opCmpGT = tdOf;

// ==================== 内部工具 ====================
std::string CodeGen::newLabel(const std::string& prefix) {
    return "L" + prefix + "_" + std::to_string(labelCount_++);
}

FuncInstance* CodeGen::helper(const std::string& name) {
    auto it = sema_->functions.find(name);
    if (it == sema_->functions.end()) return nullptr;
    auto it2 = it->second.instances.find(name);
    if (it2 != it->second.instances.end()) return it2->second;
    return sema_->getInstance(name, {});
}

void CodeGen::pushConst(int width, uint64_t value, const std::string& comment) {
    switch (width) {
        case 1: insT(OP_MOVI, kRegMovi(VmNum::U8), "R0, " + std::to_string(value), comment); insT(OP_PUSH_REG, kRegPush(VmNum::U8), "R0"); break;
        case 2: insT(OP_MOVI, kRegMovi(VmNum::U16), "R0, " + std::to_string(value)); insT(OP_PUSH_REG, kRegPush(VmNum::U16), "R0"); break;
        case 4: insT(OP_MOVI, kRegMovi(VmNum::U32), "R0, " + std::to_string(value)); insT(OP_PUSH_REG, kRegPush(VmNum::U32), "R0"); break;
        case 8: insT(OP_MOVI, kRegMovi(VmNum::U64), "R0, " + std::to_string(value)); insT(OP_PUSH_REG, kRegPush(VmNum::U64), "R0"); break;
        default: break;
    }
    em_.pushD(width);
}

void CodeGen::pushReg(int reg, int width) {
    switch (width) {
        case 1: insT(OP_PUSH_REG, kRegPush(VmNum::U8), "R" + std::to_string(reg)); break;
        case 2: insT(OP_PUSH_REG, kRegPush(VmNum::U16), "R" + std::to_string(reg)); break;
        case 4: insT(OP_PUSH_REG, kRegPush(VmNum::U32), "R" + std::to_string(reg)); break;
        case 8: insT(OP_PUSH_REG, kRegPush(VmNum::U64), "R" + std::to_string(reg)); break;
        default: break;
    }
    em_.pushD(width);
}

void CodeGen::popReg(int reg, int width) {
    switch (width) {
        case 1: insT(OP_POP_REG, kRegPop(VmNum::U8), "R" + std::to_string(reg)); break;
        case 2: insT(OP_POP_REG, kRegPop(VmNum::U16), "R" + std::to_string(reg)); break;
        case 4: insT(OP_POP_REG, kRegPop(VmNum::U32), "R" + std::to_string(reg)); break;
        case 8: insT(OP_POP_REG, kRegPop(VmNum::U64), "R" + std::to_string(reg)); break;
        default: break;
    }
    em_.popD(width);
}

void CodeGen::loadSlot(int reg, int width, int off) {
    switch (width) {
        case 1: insT(OP_LOAD, kRegLoad(VmNum::U8), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 2: insT(OP_LOAD, kRegLoad(VmNum::U16), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 4: insT(OP_LOAD, kRegLoad(VmNum::U32), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 8: insT(OP_LOAD, kRegLoad(VmNum::U64), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        default: break;
    }
}

void CodeGen::storeSlot(int reg, int width, int off) {
    switch (width) {
        case 1: insT(OP_STORE, kRegStore(VmNum::U8), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 2: insT(OP_STORE, kRegStore(VmNum::U16), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 4: insT(OP_STORE, kRegStore(VmNum::U32), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 8: insT(OP_STORE, kRegStore(VmNum::U64), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        default: break;
    }
}

void CodeGen::loadPtr(int reg, int width, int slot) {
    switch (width) {
        case 1: insT(OP_LOAD, kRegLoad(VmNum::U8), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 2: insT(OP_LOAD, kRegLoad(VmNum::U16), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 4: insT(OP_LOAD, kRegLoad(VmNum::U32), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 8: insT(OP_LOAD, kRegLoad(VmNum::U64), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        default: break;
    }
}

void CodeGen::storePtr(int reg, int width, int slot) {
    switch (width) {
        case 1: insT(OP_STORE, kRegStore(VmNum::U8), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 2: insT(OP_STORE, kRegStore(VmNum::U16), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 4: insT(OP_STORE, kRegStore(VmNum::U32), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 8: insT(OP_STORE, kRegStore(VmNum::U64), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        default: break;
    }
}

// 分支：flag(u8 在栈顶) != 0 → lTrue；== 0 → lFalse
void CodeGen::branchOnFlag(const std::string& lTrue, const std::string& lFalse) {
    // v3 的 BRANCH 自己弹 u8 条件：非 0 → lTrue，否则落空走 JMP lFalse。
    ins(OP_BRANCH, lTrue, "cond != 0");
    em_.popD(1);
    jumpTo(lFalse);
}

void CodeGen::jumpTo(const std::string& target) {
    // v3 只有标签跳转：标签由汇编器按「函数相对偏移」解析，编译期不再算绝对地址
    ins(OP_JMP, target);
}

// ==================== 生成流程 ====================
void CodeGen::measureAll() {
    codeSizes_.clear();
    labelPos_.clear();
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        FuncInstance* inst = sema_->instances[i];
        labelPos_.emplace_back();
        em_.reset(true, nullptr, &labelPos_.back());
        emitFunction(inst, static_cast<int>(i), true);
        codeSizes_.push_back(static_cast<int>(em_.pc));
        inst->maxExprDepth = em_.maxDepth;
    }
}

void CodeGen::layoutData() {
    dataCursor_ = 0;
    stringRelOff_.clear();
    // 字符串
    for (const std::string& s : strings_) {
        if (!stringRelOff_.count(s)) {
            stringRelOff_[s] = dataCursor_;
            dataCursor_ += static_cast<int>(s.size()) + 1;   // NUL 结尾
        }
    }
    // witness 表（每条目 4 字节）
    for (auto& w : sema_->witnesses) {
        w.dataOff = dataCursor_;
        dataCursor_ += static_cast<int>(w.methodEntries.size()) * 4;
    }
}

void CodeGen::assignOffsets() {
    int cur = 0;
    codeOff_.clear();
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        codeOff_.push_back(cur);
        cur += codeSizes_[i];
    }
    dataStart_ = cur;
}

void CodeGen::computeStackSizes() {
    // 调用图 DFS
    struct Frame { int64_t spCost; };
    std::vector<int64_t> spCost(sema_->instances.size(), 0);
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        FuncInstance* inst = sema_->instances[i];
        spCost[i] = inst->retBytes + inst->paramBytes + (inst->isMain ? 0 : 8) +
                    inst->localBytes + inst->maxExprDepth;
    }
    // 边：inst 内 resolved 调用 + witness 入口（保守：所有函数都可能触发任意 witness）
    std::vector<std::vector<int>> edges(sema_->instances.size());
    std::function<void(Expr*)> scanE = [&](Expr* e) {
        if (!e) return;
        if (e->resolved && e->resolved->callee) {
            auto it = std::find(sema_->instances.begin(), sema_->instances.end(), e->resolved->callee);
            if (it != sema_->instances.end()) {
                int ci = static_cast<int>(it - sema_->instances.begin());
                edges[ci].push_back(0); // 占位（下面处理方向）
            }
        }
        scanE(e->lhs.get()); scanE(e->rhs.get()); scanE(e->cond.get());
        scanE(e->thenExpr.get()); scanE(e->elseExpr.get());
        for (auto& a : e->args) scanE(a.get());
    };
    // 重建方向正确的边（caller -> callee）
    edges.assign(sema_->instances.size(), {});
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        FuncInstance* inst = sema_->instances[i];
        std::function<void(Expr*)> walk = [&](Expr* e) {
            if (!e) return;
            if (e->resolved && e->resolved->callee) {
                auto it = std::find(sema_->instances.begin(), sema_->instances.end(), e->resolved->callee);
                if (it != sema_->instances.end()) {
                    edges[i].push_back(static_cast<int>(it - sema_->instances.begin()));
                }
            }
            walk(e->lhs.get()); walk(e->rhs.get()); walk(e->cond.get());
            walk(e->thenExpr.get()); walk(e->elseExpr.get());
            for (auto& a : e->args) walk(a.get());
        };
        if (inst->compileBody) {
            std::function<void(Stmt*)> walkS = [&](Stmt* s) {
                if (!s) return;
                walk(s->cond.get()); walk(s->init.get()); walk(s->step.get()); walk(s->expr.get());
                if (s->varDecl) walk(s->varDecl->init.get());
                if (s->forVar) walk(s->forVar->init.get());
                for (auto& b : s->body) walkS(b.get());
                if (s->single) walkS(s->single.get());
            };
            walkS(inst->compileBody);
        }
        // 保守：每个函数都可能触发所有 witness 派发
        for (auto& w : sema_->witnesses) {
            for (auto* fi : w.methodEntries) {
                auto it = std::find(sema_->instances.begin(), sema_->instances.end(), fi);
                if (it != sema_->instances.end()) {
                    edges[i].push_back(static_cast<int>(it - sema_->instances.begin()));
                }
            }
        }
    }

    bool recursion = false;
    int64_t maxSp = 0;
    int maxDepth = 0;
    std::vector<bool> visited(sema_->instances.size(), false);
    std::vector<bool> inStack(sema_->instances.size(), false);
    std::function<void(int, int64_t, int)> dfs = [&](int i, int64_t acc, int depth) {
        if (inStack[i]) { recursion = true; return; }
        visited[i] = true;
        inStack[i] = true;
        acc += spCost[i];
        maxSp = std::max(maxSp, acc);
        maxDepth = std::max(maxDepth, depth + 1);
        for (int c : edges[i]) dfs(c, acc, depth + 1);
        inStack[i] = false;
    };
    if (mainIndex_ >= 0) dfs(mainIndex_, 0, 0);
    // 也检查未被 main 触达的实例链（如 helper）
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        if (!visited[i]) dfs(static_cast<int>(i), 0, 0);
    }

    if (recursion) {
        // --stack 显式指定优先；否则用递归默认栈
        stackSize_ = opts_.stackSize > 0 ? opts_.stackSize : opts_.recursionStackSize;
        // 递归深度上限 ≈ stackSize/帧大小；作用域容量按栈大小放大，避免作用域栈越界
        scopeSize_ = static_cast<uint32_t>(std::max<int64_t>(64, stackSize_ / 8));
    } else {
        int64_t need = maxSp + 1024;
        stackSize_ = static_cast<uint32_t>(opts_.stackSize > 0 ? opts_.stackSize : std::max<int64_t>(need, 16384));
        scopeSize_ = static_cast<uint32_t>(std::max<int64_t>(std::max(maxDepth + 8, 8), stackSize_ / 8));
    }
}

void CodeGen::emitDataSection(std::string& asmText) {
    em_.reset(false, &asmText, nullptr);
    em_.comment("=== 数据段 ===");
    std::set<std::string> emitted; // 去重：与 layoutData 的偏移分配一致（按首次出现顺序）
    for (const auto& s : strings_) {
        if (emitted.count(s)) continue; // 重复字面量只发射一次
        emitted.insert(s);
        // 注释里不能出现换行（汇编器按行解析），把不可打印字符转义
        std::string disp;
        for (char c : s) {
            if (c == '\n') disp += "\\n";
            else if (c == '\t') disp += "\\t";
            else if (c == '\r') disp += "\\r";
            else disp += c;
        }
        em_.comment("string \"" + disp + "\"");
        for (unsigned char c : s) em_.dataByte(c);
        em_.dataByte(0);
    }
    for (auto& w : sema_->witnesses) {
        em_.comment("witness " + w.proto + "::" + w.type);
        for (auto* fi : w.methodEntries) {
            auto it = std::find(sema_->instances.begin(), sema_->instances.end(), fi);
            int off = (it != sema_->instances.end()) ? codeOff_[it - sema_->instances.begin()] : 0;
            em_.dataByte(static_cast<uint8_t>(off & 0xFF));
            em_.dataByte(static_cast<uint8_t>((off >> 8) & 0xFF));
            em_.dataByte(static_cast<uint8_t>((off >> 16) & 0xFF));
            em_.dataByte(static_cast<uint8_t>((off >> 24) & 0xFF));
        }
    }
}

bool CodeGen::generate(std::string& asmText, std::vector<uint8_t>& outBytes,
                       uint32_t& argSize, uint32_t& retSize, uint32_t& entry) {
    // 收集字符串字面量（先遍历一次，保证字符串先于 witness 且按需去重）
    std::function<void(Expr*)> collectS = [&](Expr* e) {
        if (!e) return;
        if (e->kind == ExprKind::StrLit) strings_.push_back(e->strVal);
        collectS(e->lhs.get()); collectS(e->rhs.get()); collectS(e->cond.get());
        collectS(e->thenExpr.get()); collectS(e->elseExpr.get());
        for (auto& a : e->args) collectS(a.get());
    };
    std::function<void(Stmt*)> collectSt = [&](Stmt* s) {
        if (!s) return;
        collectS(s->cond.get()); collectS(s->init.get()); collectS(s->step.get()); collectS(s->expr.get());
        if (s->varDecl) collectS(s->varDecl->init.get());
        if (s->forVar) collectS(s->forVar->init.get());
        for (auto& b : s->body) collectSt(b.get());
        if (s->single) collectSt(s->single.get());
    };
    for (auto* inst : sema_->instances) {
        if (inst->compileBody) collectSt(inst->compileBody);
    }

    // 找到 main
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        if (sema_->instances[i]->isMain) { mainIndex_ = static_cast<int>(i); break; }
    }
    if (mainIndex_ < 0) {
        Diag::error({0, 0}, "missing main function");
        return false;
    }

    measureAll();
    if (getenv("J8_DEBUG_LABELS")) {
        for (size_t i = 0; i < labelPos_.size(); ++i) {
            fprintf(stderr, "[labels %zu] %zu entries, first=%s->%d\n", i, labelPos_[i].size(),
                    labelPos_[i].empty() ? "-" : labelPos_[i].begin()->first.c_str(),
                    labelPos_[i].empty() ? -1 : labelPos_[i].begin()->second);
        }
    }
    layoutData();
    assignOffsets();
    computeStackSizes();

    // emit 阶段（标签计数器重置，保证与 measure 趟的名字一致）
    labelCount_ = 0;
    labelPosEmit_.clear();
    for (size_t i = 0; i < sema_->instances.size(); ++i) labelPosEmit_.emplace_back();
    asmText.clear();
    // ISA v3：每个函数用 .FUNC 划出来（汇编器据此产出函数目录），
    // .ENTRY 用绝对偏移，汇编器会换算成入口函数下标。
    asmText += ".ENTRY " + std::to_string(codeOff_[mainIndex_]) + "\n";
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        FuncInstance* fi2 = sema_->instances[i];
        asmText += ".FUNC " + fi2->key + " " + std::to_string(fi2->paramBytes) + " " +
                   std::to_string(fi2->retBytes) + "\n";
        em_.reset(false, &asmText, &labelPosEmit_[i]);
        emitFunction(fi2, static_cast<int>(i), false);
        if (getenv("J8_DEBUG_LABELS")) {
            fprintf(stderr, "  inst=%zu emit-labels=%zu\n", i, labelPosEmit_[i].size());
            for (auto& [n2, p2] : labelPos_[i]) fprintf(stderr, "    %s=%d\n", n2.c_str(), p2);
            int mism = 0;
            for (auto& [n, p] : labelPos_[i]) {
                auto it = labelPosEmit_[i].find(n);
                int pe = (it != labelPosEmit_[i].end()) ? it->second : -1;
                if (pe != p) {
                    if (mism < 5) fprintf(stderr, "  LABEL MISMATCH inst=%zu %s measure=%d emit=%d\n", i, n.c_str(), p, pe);
                    ++mism;
                }
            }
            if (mism) fprintf(stderr, "  inst=%zu: %d mismatches\n", i, mism);
        }
    }
    // 数据段单独划一个"函数"，免得落进最后一个代码函数里（永不调用）
    asmText += ".FUNC __data 0 0\n";
    emitDataSection(asmText);

    // 汇编（纯字节码，无头部；函数目录从 asmblr.funcs 取）
    Assembler asmblr;
    asmblr.pureBinary = true;
    uint32_t as_, rs, en;
    if (!asmblr.assemble(asmText, outBytes, as_, rs, en)) {
        if (getenv("J8_DUMP_ASM")) {
            std::istringstream dbg(asmText);
            std::string ln;
            int k = 0;
            while (std::getline(dbg, ln) && k < 4000) { std::fprintf(stderr, "%5d| %s\n", ++k, ln.c_str()); }
        }
        Diag::error({0, 0}, "internal assembler failed: " + asmblr.error);
        return false;
    }
    // 校验尺寸与预测一致（偏移正确性的强保证）
    int expect = dataStart_ + dataCursor_;
    if (static_cast<int>(outBytes.size()) != expect) {
        Diag::error({0, 0}, "internal: assembled size " + std::to_string(outBytes.size()) +
                            " != predicted " + std::to_string(expect));
        return false;
    }

    moduleFuncs = asmblr.funcs;
    // 数据段必须落在编译期算出的 dataStart_ 上（字符串/见证表地址全靠它）
    if (moduleFuncs.size() >= 2 && static_cast<int>(moduleFuncs.back().offset) != dataStart_) {
        Diag::error({0, 0}, "internal: data section offset " +
                            std::to_string(moduleFuncs.back().offset) + " != predicted " +
                            std::to_string(dataStart_));
        return false;
    }
    // 入口函数下标：entry 落在哪个函数的区间里
    entryFuncIndex = 0;
    for (size_t i = 0; i < moduleFuncs.size(); ++i)
        if (en >= moduleFuncs[i].offset && en < moduleFuncs[i].offset + moduleFuncs[i].size) { entryFuncIndex = static_cast<uint32_t>(i); break; }
    argSize = 0;
    retSize = 0;
    entry = en;
    return true;
}

// ==================== 函数序言/尾声 ====================
void CodeGen::emitPrologue(FuncInstance* inst) {
    if (inst->isMain) {
        ins(OP_STACK_INIT, std::to_string(stackSize_) + " " + std::to_string(scopeSize_),
            "main: stack/scope init");
        ins(OP_GET_ADDRS);
        insT(OP_POP_REG, kRegPop(VmNum::U64), "R11", "mgr");
        insT(OP_POP_REG, kRegPop(VmNum::U64), "R10", "ds");
        insT(OP_POP_REG, kRegPop(VmNum::U64), "R9", "sizeAddr");
        insT(OP_POP_REG, kRegPop(VmNum::U64), "R12", "bcBase -> R12");
        em_.comment("初始化 ECS world");
        ins(OP_NEW_ARRAY, std::to_string(sema_->worldBytes) + " 0", "world heap");
        insT(OP_LOAD, kRegLoad(VmNum::U64), "R13, 0, 0", "worldPtr -> R13");
        // VM 的 NEW_ARRAY 不保证清零：把整个 world 堆零初始化
        // （count/cap/bitset 必须为 0；数据区一并清零无害）
        if (sema_->worldBytes > 0) {
            std::string wzTop = newLabel("wz_top");
            std::string wzBody = newLabel("wz_body");
            std::string wzEnd = newLabel("wz_end");
            // i 存 R2（branch/压栈会覆盖 R0/R1 与低地址栈槽）
            insT(OP_MOVI, kRegMovi(VmNum::U32), "R2, 0", "zero loop i = 0");
            label(wzTop);
            // i < worldBytes ?
            pushReg(2, 4);
            pushConst(4, static_cast<uint64_t>(sema_->worldBytes));
            insCmp(CMP_LT, TD_U32);
            branchOnFlag(wzBody, wzEnd);
            label(wzBody);
            // *(u8*)(world + i) = 0
            pushReg(13, 8);
            pushReg(2, 8);
            insT(OP_ADD, TD_PTR);
            popReg(1, 8);
            storeSlot(1, 8, tempOffset(inst, 1));
            insT(OP_MOVI, kRegMovi(VmNum::U8), "R3, 0");
            storePtr(3, 1, tempOffset(inst, 1));
            // i++
            pushReg(2, 4);
            pushConst(4, 1);
            insT(OP_ADD, TD_U32);
            popReg(2, 4);
            jumpTo(wzTop);
            label(wzEnd);
        }
    }
    // v3：CALL 把被调函数的帧基址设在「调用者 CALL 时的 sp」上，所以入口 sp = 帧基址 X。
    // 一帧要盖住 [ret 区][参数][本地]，总长 = retBytes + paramBytes + localBytes。
    ins(OP_STACK_ALLOC, std::to_string(inst->retBytes + inst->paramBytes + inst->localBytes),
        "frame: ret+params+locals");
}

void CodeGen::emitReturn(FuncInstance* inst) {
    // 返回值已在栈上（若非 void）
    if (inst->retBytes > 0) {
        if (inst->retType->isStruct()) {
            // 值 = 地址（8B）
            popReg(0, 8);
            storeSlot(0, 8, 0);
        } else if (inst->retType->kind == TypeKind::Exist) {
            // 盒 [data][vt]：vt 在上
            popReg(1, 8);
            storeSlot(1, 8, 8);
            popReg(0, 8);
            storeSlot(0, 8, 0);
        } else {
            int w = typeWidth(inst->retType);
            if (w <= 0) w = 8;
            popReg(0, w);
            storeSlot(0, w, 0);
        }
    }
    if (!inst->isMain) {
        ins(OP_RET, "", "return（帧栈还原 sp/scope）");
    } else {
        ins(OP_HALT, "", "program end");
    }
}

// ==================== 语句 ====================
void CodeGen::emitStmts(std::vector<StmtPtr>& body, FuncInstance* inst) {
    for (auto& s : body) emitStmt(s.get(), inst);
}

void CodeGen::emitStmt(Stmt* s, FuncInstance* inst) {
    if (!s) return;
    em_.beginStmt();
    switch (s->kind) {
        case StmtKind::Block:
            emitStmts(s->body, inst);
            break;
        case StmtKind::Empty:
            break;
        case StmtKind::ExprStmt: {
            if (!s->expr) break;
            emitExpr(s->expr.get(), inst);
            // 丢弃结果值
            Type* t = s->expr->type;
            if (t) {
                int w = t->isStruct() ? 8 : (t->kind == TypeKind::Exist ? 16 : typeWidth(t));
                if (w > 0) {
                    ins(OP_STACK_MOVE, std::to_string(w), "drop expr value");
                    em_.popD(w);
                }
            }
            break;
        }
        case StmtKind::VarDecl: {
            VarDeclStmt* vd = s->varDecl.get();
            VarSlot* vs = nullptr;
            for (auto& [id, slot] : inst->slots) {
                if (slot.name == vd->name) { vs = &slot; break; }
            }
            if (!vs) break;
            int off = vs->offset;
            Type* t = vd->type;
            if (!vd->fieldInits.empty()) {
                Decl* sd = sema_->structs[t->name];
                for (size_t i = 0; i < vd->fieldInits.size() && i < sd->fields.size(); ++i) {
                    emitExpr(vd->fieldInits[i].get(), inst);
                    coerceOnStack(vd->fieldInits[i]->type, sema_->resolveType(sd->fields[i].typeName), inst);
                    popReg(0, typeWidth(sema_->resolveType(sd->fields[i].typeName)));
                    storeSlot(0, typeWidth(sema_->resolveType(sd->fields[i].typeName)), off + sd->fields[i].offset);
                }
            } else if (vd->init) {
                emitExpr(vd->init.get(), inst);
                coerceOnStack(vd->init->type, t, inst);
                if (t->isStruct()) {
                    // 结构体拷贝：init 值为地址 → MEMCPY
                    popReg(1, 8);                       // src 地址
                    ins(OP_LEA, "0, " + std::to_string(off), "&dst");
                    popReg(0, 8);
                    storeSlot(0, 8, tempOffset(inst, 0));
                    storeSlot(1, 8, tempOffset(inst, 1));
                    ins(OP_MEMCPY, "1 " + std::to_string(tempOffset(inst, 0)) +
                                   " 1 " + std::to_string(tempOffset(inst, 1)) +
                                   " " + std::to_string(sema_->sizeOf(t)));
                } else if (t->kind == TypeKind::Exist) {
                    popReg(1, 8);  // vt
                    storeSlot(1, 8, off + 8);
                    popReg(0, 8);  // data
                    storeSlot(0, 8, off);
                } else {
                    int w = typeWidth(t);
                    if (w <= 0) w = 8;
                    popReg(0, w);
                    storeSlot(0, w, off);
                }
            }
            break;
        }
        case StmtKind::If: {
            std::string lThen = newLabel("then");
            std::string lElse = newLabel("else");
            std::string lEnd = newLabel("endif");
            emitBoolFlag(s->cond.get(), inst);
            if (!s->body.empty()) {
                branchOnFlag(lThen, lElse);
                label(lThen);
                if (s->single) emitStmt(s->single.get(), inst);
                jumpTo(lEnd);
                label(lElse);
                for (auto& b : s->body) emitStmt(b.get(), inst);
            } else {
                branchOnFlag(lThen, lEnd);
                label(lThen);
                if (s->single) emitStmt(s->single.get(), inst);
            }
            label(lEnd);
            break;
        }
        case StmtKind::While: {
            std::string lTop = newLabel("while");
            std::string lBody = newLabel("wbody");
            std::string lEnd = newLabel("wend");
            loopStack_.emplace_back(lEnd, lTop);
            label(lTop);
            emitBoolFlag(s->cond.get(), inst);
            branchOnFlag(lBody, lEnd);
            label(lBody);
            if (s->single) emitStmt(s->single.get(), inst);
            jumpTo(lTop);
            label(lEnd);
            loopStack_.pop_back();
            break;
        }
        case StmtKind::DoWhile: {
            std::string lTop = newLabel("do");
            std::string lEnd = newLabel("dend");
            loopStack_.emplace_back(lEnd, lTop);
            label(lTop);
            if (s->single) emitStmt(s->single.get(), inst);
            emitBoolFlag(s->cond.get(), inst);
            branchOnFlag(lTop, lEnd);
            label(lEnd);
            loopStack_.pop_back();
            break;
        }
        case StmtKind::For: {
            if (s->forVar) {
                // 把 forVar 当作变量声明处理
                VarDeclStmt* vd = s->forVar.get();
                if (vd->init) {
                    emitExpr(vd->init.get(), inst);
                    coerceOnStack(vd->init->type, vd->type, inst);
                    VarSlot* vs = nullptr;
                    for (auto& [id, slot] : inst->slots) if (slot.name == vd->name) { vs = &slot; break; }
                    if (vs) {
                        int w = typeWidth(vd->type);
                        if (w <= 0) w = 8;
                        popReg(0, w);
                        storeSlot(0, w, vs->offset);
                    }
                }
            } else if (s->init) {
                emitExpr(s->init.get(), inst);
                int w = typeWidth(s->init->type);
                if (w > 0) { ins(OP_STACK_MOVE, std::to_string(w)); em_.popD(w); }
            }
            std::string lTop = newLabel("for");
            std::string lBody = newLabel("fbody");
            std::string lCont = newLabel("fcont");
            std::string lEnd = newLabel("fend");
            loopStack_.emplace_back(lEnd, lCont);
            label(lTop);
            if (s->cond) {
                emitBoolFlag(s->cond.get(), inst);
                branchOnFlag(lBody, lEnd);
            }
            label(lBody);
            if (s->single) emitStmt(s->single.get(), inst);
            label(lCont);
            if (s->step) {
                emitExpr(s->step.get(), inst);
                int w = typeWidth(s->step->type);
                if (w > 0) { ins(OP_STACK_MOVE, std::to_string(w)); em_.popD(w); }
            }
            jumpTo(lTop);
            label(lEnd);
            loopStack_.pop_back();
            break;
        }
        case StmtKind::Break: {
            if (!loopStack_.empty()) jumpTo(loopStack_.back().first);
            break;
        }
        case StmtKind::Continue: {
            if (!loopStack_.empty()) jumpTo(loopStack_.back().second);
            break;
        }
        case StmtKind::Return: {
            if (s->expr) {
                emitExpr(s->expr.get(), inst);
                // 按函数返回类型 coercion：否则字面量（如 return 0 的 u32）宽度与
                // emitReturn 的弹出宽度不符，栈不平衡会把返回地址弹错、打断调用方
                coerceOnStack(s->expr->type, inst->retType, inst);
            }
            emitReturn(inst);
            break;
        }
        default: break;
    }
}

// ==================== 表达式 ====================
// 占位：完整实现在 codegen2.cpp（见文件尾部的 include）
#include "codegen2.inc"

} // namespace j8
