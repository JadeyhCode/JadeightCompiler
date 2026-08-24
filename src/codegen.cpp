// codegen.cpp — J8 代码生成实现
#include "codegen.h"

#include <cstring>
#include <sstream>

namespace j8 {

using namespace jadeight;

std::map<uint8_t, std::string> CodeGen::Emitter::opNames;

std::string CodeGen::Emitter::nameOf(uint8_t op) {
    if (opNames.empty()) {
        std::map<std::string, uint8_t> m;
        buildOpNameMapFull(m);
        for (auto& [n, o] : m) opNames[o] = n;
    }
    auto it = opNames.find(op);
    return it == opNames.end() ? "OP_" + std::to_string(op) : it->second;
}

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
#define OPSW(NAME, U8_, U16_, U32_, U64_) \
    [](VmNum n)->uint8_t { switch(n){case VmNum::U8:case VmNum::I8:return OP_##NAME##_U8;case VmNum::U16:case VmNum::I16:return OP_##NAME##_U16;case VmNum::U32:case VmNum::I32:return OP_##NAME##_U32;case VmNum::U64:case VmNum::I64:return OP_##NAME##_U64;default:return OP_##NAME##_U64;} }
// 加法/减法：VM 只有无符号宽度变体，有符号整型复用同宽无符号 opcode
static const auto kAdd = [](VmNum n) -> uint8_t {
    switch (n) {
        case VmNum::U8: case VmNum::I8: return OP_ADD_U8;
        case VmNum::U16: case VmNum::I16: return OP_ADD_U16;
        case VmNum::U32: case VmNum::I32: return OP_ADD_U32;
        case VmNum::U64: case VmNum::I64: return OP_ADD_U64;
        case VmNum::F64: return OP_ADD_F64;
        default: return OP_ADD_U64;
    }
};
static const auto kSub = [](VmNum n) -> uint8_t {
    switch (n) {
        case VmNum::U8: case VmNum::I8: return OP_SUB_U8;
        case VmNum::U16: case VmNum::I16: return OP_SUB_U16;
        case VmNum::U32: case VmNum::I32: return OP_SUB_U32;
        case VmNum::U64: case VmNum::I64: return OP_SUB_U64;
        case VmNum::F64: return OP_SUB_F64;
        default: return OP_SUB_U64;
    }
};
static const auto kShl  = OPSW(SHL, SHL_U8, SHL_U16, SHL_U32, SHL_U64);
static const auto kShr  = OPSW(SHR, SHR_U8, SHR_U16, SHR_U32, SHR_U64);
static uint8_t opShri(VmNum n) {
    switch (n) {
        case VmNum::I8: return OP_SHR_I8;
        case VmNum::I16: return OP_SHR_I16;
        case VmNum::I32: return OP_SHR_I32;
        case VmNum::I64: return OP_SHR_I64;
        default: return OP_SHR_I32;
    }
}
static const auto kShri = opShri;
static const auto kAnd  = OPSW(AND, AND_U8, AND_U16, AND_U32, AND_U64);
static const auto kOr   = OPSW(OR, OR_U8, OR_U16, OR_U32, OR_U64);
static const auto kNot  = OPSW(NOT, NOT_U8, NOT_U16, NOT_U32, NOT_U64);
static const auto kRegMovi = OPSW(REG_MOVI, REG_MOVI_U8, REG_MOVI_U16, REG_MOVI_U32, REG_MOVI_U64);
static const auto kRegPush = OPSW(REG_PUSH, REG_PUSH_U8, REG_PUSH_U16, REG_PUSH_U32, REG_PUSH_U64);
static const auto kRegPop  = OPSW(REG_POP, REG_POP_U8, REG_POP_U16, REG_POP_U32, REG_POP_U64);
static const auto kRegLoad = OPSW(REG_LOAD, REG_LOAD_U8, REG_LOAD_U16, REG_LOAD_U32, REG_LOAD_U64);
static const auto kRegStore= OPSW(REG_STORE, REG_STORE_U8, REG_STORE_U16, REG_STORE_U32, REG_STORE_U64);

static uint8_t opMul(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_MUL_U8; case VmNum::I8: return OP_MUL_I8;
        case VmNum::U16: return OP_MUL_U16; case VmNum::I16: return OP_MUL_I16;
        case VmNum::U32: return OP_MUL_U32; case VmNum::I32: return OP_MUL_I32;
        case VmNum::U64: return OP_MUL_U64; case VmNum::I64: return OP_MUL_I64;
        case VmNum::F64: return OP_MUL_F64;
        default: return OP_MUL_U64;
    }
}
static uint8_t opDiv(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_DIV_U8; case VmNum::I8: return OP_DIV_I8;
        case VmNum::U16: return OP_DIV_U16; case VmNum::I16: return OP_DIV_I16;
        case VmNum::U32: return OP_DIV_U32; case VmNum::I32: return OP_DIV_I32;
        case VmNum::U64: return OP_DIV_U64; case VmNum::I64: return OP_DIV_I64;
        case VmNum::F64: return OP_DIV_F64;
        default: return OP_DIV_U64;
    }
}
static uint8_t opCmpLT(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_CMP_LT_U8; case VmNum::I8: return OP_CMP_LT_I8;
        case VmNum::U16: return OP_CMP_LT_U16; case VmNum::I16: return OP_CMP_LT_I16;
        case VmNum::U32: return OP_CMP_LT_U32; case VmNum::I32: return OP_CMP_LT_I32;
        case VmNum::U64: return OP_CMP_LT_U64; case VmNum::I64: return OP_CMP_LT_I64;
        case VmNum::F64: return OP_CMP_LT_F64; case VmNum::Ptr: return OP_CMP_LT_PTR;
        default: return OP_CMP_LT_U64;
    }
}
static uint8_t opCmpEQ(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_CMP_EQ_U8; case VmNum::I8: return OP_CMP_EQ_I8;
        case VmNum::U16: return OP_CMP_EQ_U16; case VmNum::I16: return OP_CMP_EQ_I16;
        case VmNum::U32: return OP_CMP_EQ_U32; case VmNum::I32: return OP_CMP_EQ_I32;
        case VmNum::U64: return OP_CMP_EQ_U64; case VmNum::I64: return OP_CMP_EQ_I64;
        case VmNum::F64: return OP_CMP_EQ_F64; case VmNum::Ptr: return OP_CMP_EQ_PTR;
        default: return OP_CMP_EQ_U64;
    }
}
static uint8_t opCmpGT(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_CMP_GT_U8; case VmNum::I8: return OP_CMP_GT_I8;
        case VmNum::U16: return OP_CMP_GT_U16; case VmNum::I16: return OP_CMP_GT_I16;
        case VmNum::U32: return OP_CMP_GT_U32; case VmNum::I32: return OP_CMP_GT_I32;
        case VmNum::U64: return OP_CMP_GT_U64; case VmNum::I64: return OP_CMP_GT_I64;
        case VmNum::F64: return OP_CMP_GT_F64; case VmNum::Ptr: return OP_CMP_GT_PTR;
        default: return OP_CMP_GT_U64;
    }
}
static uint8_t opSqrt(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_SQRT_U8; case VmNum::I8: return OP_SQRT_I8;
        case VmNum::U16: return OP_SQRT_U16; case VmNum::I16: return OP_SQRT_I16;
        case VmNum::U32: return OP_SQRT_U32; case VmNum::I32: return OP_SQRT_I32;
        case VmNum::U64: return OP_SQRT_U64; case VmNum::I64: return OP_SQRT_I64;
        case VmNum::F64: return OP_SQRT_F64;
        default: return OP_SQRT_F64;
    }
}
static uint8_t opLog(VmNum n) {
    switch (n) {
        case VmNum::U8: return OP_LOG_U8; case VmNum::I8: return OP_LOG_I8;
        case VmNum::U16: return OP_LOG_U16; case VmNum::I16: return OP_LOG_I16;
        case VmNum::U32: return OP_LOG_U32; case VmNum::I32: return OP_LOG_I32;
        case VmNum::U64: return OP_LOG_U64; case VmNum::I64: return OP_LOG_I64;
        case VmNum::F64: return OP_LOG_F64;
        default: return OP_LOG_F64;
    }
}

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
        case 1: ins(kRegMovi(VmNum::U8), "R0, " + std::to_string(value), comment); ins(kRegPush(VmNum::U8), "R0"); break;
        case 2: ins(kRegMovi(VmNum::U16), "R0, " + std::to_string(value)); ins(kRegPush(VmNum::U16), "R0"); break;
        case 4: ins(kRegMovi(VmNum::U32), "R0, " + std::to_string(value)); ins(kRegPush(VmNum::U32), "R0"); break;
        case 8: ins(kRegMovi(VmNum::U64), "R0, " + std::to_string(value)); ins(kRegPush(VmNum::U64), "R0"); break;
        default: break;
    }
    em_.pushD(width);
}

void CodeGen::pushReg(int reg, int width) {
    switch (width) {
        case 1: ins(kRegPush(VmNum::U8), "R" + std::to_string(reg)); break;
        case 2: ins(kRegPush(VmNum::U16), "R" + std::to_string(reg)); break;
        case 4: ins(kRegPush(VmNum::U32), "R" + std::to_string(reg)); break;
        case 8: ins(kRegPush(VmNum::U64), "R" + std::to_string(reg)); break;
        default: break;
    }
    em_.pushD(width);
}

void CodeGen::popReg(int reg, int width) {
    switch (width) {
        case 1: ins(kRegPop(VmNum::U8), "R" + std::to_string(reg)); break;
        case 2: ins(kRegPop(VmNum::U16), "R" + std::to_string(reg)); break;
        case 4: ins(kRegPop(VmNum::U32), "R" + std::to_string(reg)); break;
        case 8: ins(kRegPop(VmNum::U64), "R" + std::to_string(reg)); break;
        default: break;
    }
    em_.popD(width);
}

void CodeGen::loadSlot(int reg, int width, int off) {
    switch (width) {
        case 1: ins(kRegLoad(VmNum::U8), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 2: ins(kRegLoad(VmNum::U16), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 4: ins(kRegLoad(VmNum::U32), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 8: ins(kRegLoad(VmNum::U64), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        default: break;
    }
}

void CodeGen::storeSlot(int reg, int width, int off) {
    switch (width) {
        case 1: ins(kRegStore(VmNum::U8), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 2: ins(kRegStore(VmNum::U16), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 4: ins(kRegStore(VmNum::U32), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        case 8: ins(kRegStore(VmNum::U64), "R" + std::to_string(reg) + ", 0, " + std::to_string(off)); break;
        default: break;
    }
}

void CodeGen::loadPtr(int reg, int width, int slot) {
    switch (width) {
        case 1: ins(kRegLoad(VmNum::U8), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 2: ins(kRegLoad(VmNum::U16), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 4: ins(kRegLoad(VmNum::U32), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 8: ins(kRegLoad(VmNum::U64), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        default: break;
    }
}

void CodeGen::storePtr(int reg, int width, int slot) {
    switch (width) {
        case 1: ins(kRegStore(VmNum::U8), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 2: ins(kRegStore(VmNum::U16), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 4: ins(kRegStore(VmNum::U32), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        case 8: ins(kRegStore(VmNum::U64), "R" + std::to_string(reg) + ", 1, " + std::to_string(slot)); break;
        default: break;
    }
}

// 分支：flag(u8 在栈顶) != 0 → lTrue；== 0 → lFalse
void CodeGen::branchOnFlag(const std::string& lTrue, const std::string& lFalse) {
    int base = em_.counting ? 0 : codeOff_[curFuncIdx_];
    int t = em_.counting ? 0 : base + labelPos_[curFuncIdx_][lTrue];
    int f = em_.counting ? 0 : base + labelPos_[curFuncIdx_][lFalse];
    if (!em_.counting && getenv("J8_DEBUG_BRANCH")) {
        fprintf(stderr, "  branch inst=%d %s->%d %s->%d\n", curFuncIdx_, lTrue.c_str(), t, lFalse.c_str(), f);
    }
    if (!em_.counting && getenv("J8_DEBUG_BRANCH")) {
        fprintf(stderr, "[branch inst=%d] %s=%d %s=%d (map size %zu)\n", curFuncIdx_,
                lTrue.c_str(), t, lFalse.c_str(), f, labelPos_[curFuncIdx_].size());
    }
    popReg(0, 1);                       // flag → R0（零扩展）
    pushReg(0, 8);                      // flag 上栈（u64）
    ins(kRegMovi(VmNum::U64), "R1, " + std::to_string(static_cast<uint64_t>(t - f)));
    pushReg(1, 8);                      // diff
    ins(OP_MUL_U64);
    ins(kRegMovi(VmNum::U64), "R1, " + std::to_string(static_cast<uint64_t>(f)));
    pushReg(1, 8);
    ins(OP_ADD_U64);
    pushReg(kBcBase, 8);
    ins(OP_ADD_U64);
    ins(OP_JMP_IND);
}

void CodeGen::jumpTo(const std::string& target) {
    int base = em_.counting ? 0 : codeOff_[curFuncIdx_];
    int t = em_.counting ? 0 : base + labelPos_[curFuncIdx_][target];
    ins(OP_JMP, std::to_string(t));
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
        stackSize_ = opts_.recursionStackSize;
        scopeSize_ = 64;
    } else {
        int64_t need = maxSp + 1024;
        stackSize_ = static_cast<uint32_t>(std::max<int64_t>(need, 16384));
        scopeSize_ = static_cast<uint32_t>(std::max(maxDepth + 8, 8));
    }
}

void CodeGen::emitDataSection(std::string& asmText) {
    em_.reset(false, &asmText, nullptr);
    em_.comment("=== 数据段 ===");
    for (const auto& s : strings_) {
        if (!stringRelOff_.count(s)) continue;   // 已去重
        em_.comment("string \"" + s + "\"");
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
    asmText += ".ARGS 0\n.RETS 0\n.ENTRY " + std::to_string(codeOff_[mainIndex_]) + "\n";
    for (size_t i = 0; i < sema_->instances.size(); ++i) {
        em_.reset(false, &asmText, &labelPosEmit_[i]);
        emitFunction(sema_->instances[i], static_cast<int>(i), false);
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
    emitDataSection(asmText);

    // 汇编（纯字节码，无头部）
    Assembler asmblr;
    asmblr.pureBinary = true;
    uint32_t as_, rs, en;
    if (!asmblr.assemble(asmText, outBytes, as_, rs, en)) {
        Diag::error({0, 0}, "internal assembler failed");
        return false;
    }
    // 校验尺寸与预测一致（偏移正确性的强保证）
    int expect = dataStart_ + dataCursor_;
    if (static_cast<int>(outBytes.size()) != expect) {
        Diag::error({0, 0}, "internal: assembled size " + std::to_string(outBytes.size()) +
                            " != predicted " + std::to_string(expect));
        return false;
    }

    argSize = 0;
    retSize = 0;
    entry = static_cast<uint32_t>(codeOff_[mainIndex_]);
    return true;
}

// ==================== 函数序言/尾声 ====================
void CodeGen::emitPrologue(FuncInstance* inst) {
    if (inst->isMain) {
        ins(OP_STACK_INIT, std::to_string(stackSize_) + " " + std::to_string(scopeSize_),
            "main: stack/scope init");
        ins(OP_GET_ADDRS);
        ins(kRegPop(VmNum::U64), "R11", "mgr");
        ins(kRegPop(VmNum::U64), "R10", "ds");
        ins(kRegPop(VmNum::U64), "R9", "sizeAddr");
        ins(kRegPop(VmNum::U64), "R12", "bcBase -> R12");
        em_.comment("初始化 ECS world");
        ins(OP_NEW_ARRAY, std::to_string(sema_->worldBytes) + " 0", "world heap");
        ins(kRegLoad(VmNum::U64), "R13, 0, 0", "worldPtr -> R13");
        // VM 的 NEW_ARRAY 不保证清零：把整个 world 堆零初始化
        // （count/cap/bitset 必须为 0；数据区一并清零无害）
        if (sema_->worldBytes > 0) {
            std::string wzTop = newLabel("wz_top");
            std::string wzBody = newLabel("wz_body");
            std::string wzEnd = newLabel("wz_end");
            // i 存 R2（branch/压栈会覆盖 R0/R1 与低地址栈槽）
            ins(kRegMovi(VmNum::U32), "R2, 0", "zero loop i = 0");
            label(wzTop);
            // i < worldBytes ?
            pushReg(2, 4);
            pushConst(4, static_cast<uint64_t>(sema_->worldBytes));
            ins(OP_CMP_LT_U32);
            branchOnFlag(wzBody, wzEnd);
            label(wzBody);
            // *(u8*)(world + i) = 0
            pushReg(13, 8);
            pushReg(2, 8);
            ins(OP_ADD_PTR);
            popReg(1, 8);
            storeSlot(1, 8, tempOffset(inst, 1));
            ins(kRegMovi(VmNum::U8), "R3, 0");
            storePtr(3, 1, tempOffset(inst, 1));
            // i++
            pushReg(2, 4);
            pushConst(4, 1);
            ins(OP_ADD_U32);
            popReg(2, 4);
            jumpTo(wzTop);
            label(wzEnd);
        }
    }
    ins(OP_NEW_STACK, std::to_string(inst->localBytes), "frame locals");
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
        ins(OP_STACK_PTR_MOVE, std::to_string(inst->localBytes), "pop frame");
        ins(OP_JMP_IND, "", "return");
    } else {
        ins(OP_END, "", "program end");
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
                    ins(OP_STACK_PTR_MOVE, std::to_string(w), "drop expr value");
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
                if (w > 0) { ins(OP_STACK_PTR_MOVE, std::to_string(w)); em_.popD(w); }
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
                if (w > 0) { ins(OP_STACK_PTR_MOVE, std::to_string(w)); em_.popD(w); }
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
            if (s->expr) emitExpr(s->expr.get(), inst);
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
