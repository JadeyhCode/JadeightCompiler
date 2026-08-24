// j8trace.cpp — 迷你解释器（带追踪），支持 j8c 输出的指令子集
// 寻址模型简化：LEA mode0 压入裸偏移 off；mode1 先解引用槽值；JMP_IND 目标=绝对偏移
#include "jadeight_asm.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <map>
#include <string>
#include <vector>
using namespace jadeight;

static constexpr size_t MEMSZ = 64u * 1024u * 1024u;
static std::vector<uint8_t> mem(MEMSZ, 0);

static uint64_t rd(size_t off, int w) {
    if (off + w > MEMSZ) { printf("MEM OOB read @%zu\n", off); return 0; }
    uint64_t v = 0;
    for (int i = 0; i < w; ++i) v |= static_cast<uint64_t>(mem[off + i]) << (8 * i);
    return v;
}
static void wr(size_t off, int w, uint64_t v) {
    if (off + w > MEMSZ) { printf("MEM OOB write @%zu\n", off); return; }
    for (int i = 0; i < w; ++i) mem[off + i] = static_cast<uint8_t>(v >> (8 * i));
}
static uint64_t rdBE(const uint8_t* p, int w) {
    uint64_t v = 0;
    for (int i = 0; i < w; ++i) v = (v << 8) | p[i];
    return v;
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    uint32_t hdr[3];
    fread(hdr, 4, 3, f);
    std::vector<uint8_t> bc;
    int c;
    while ((c = fgetc(f)) != EOF) bc.push_back(static_cast<uint8_t>(c));
    printf("argSize=%u retSize=%u entry=%u size=%zu\n", hdr[0], hdr[1], hdr[2], bc.size());

    uint32_t sp = 0;
    uint64_t regs[16] = {};
    size_t count = hdr[2];
    const uint64_t bcBase = 0;   // 伪基址：JMP_IND 目标 = 绝对偏移
    size_t heapCur = 0x800000;
    std::vector<uint64_t> scopeStack(256, 0);
    uint32_t scopeDepth = 1;
    auto scopeBase = [&]() -> uint64_t { return scopeStack[scopeDepth - 1]; };

    std::map<std::string, uint8_t> names;
    buildOpNameMapFull(names);

    int steps = 0;
    while (count < bc.size() && steps++ < 500000) {
        uint8_t op = bc[count];
        if (getenv("J8_DBGSLOT") && count == atoi(getenv("J8_DBGSLOT"))) {
            printf("AT %zu: base=%llu slot76=%u slot80=%u slot0=%u\n", count,
                   (unsigned long long)scopeBase(), (unsigned)rd(scopeBase()+76, 4),
                   (unsigned)rd(scopeBase()+80, 4), (unsigned)rd(scopeBase()+0, 4));
        }
        if (getenv("J8_TRACE")) {
            const char* name = "?";
            for (auto& [n, o] : names) if (o == op) { name = n.c_str(); break; }
            printf("%5zu: %-20s sp=%u\n", count, name, sp);
        }
        switch (op) {
            case OP_END: printf("END ok\n"); return 0;
            case OP_STACK_INIT: count += 9; continue;
            case OP_JMP: count = rdBE(&bc[count + 1], 4); continue;
            case OP_STACK_PTR_MOVE: sp -= static_cast<uint32_t>(rdBE(&bc[count + 1], 4)); break;
            case OP_SCOPE_PUSH: scopeStack[scopeDepth++] = sp; break;
            case OP_SCOPE_POP: sp = static_cast<uint32_t>(scopeStack[--scopeDepth]); break;
            case OP_NEW_STACK: sp += static_cast<uint32_t>(rdBE(&bc[count + 1], 8)); break;
            case OP_IF_GOTO: {
                if (bc[count + 1] == 0) { count = rdBE(&bc[count + 2], 4); continue; }
                break;
            }
            case OP_LEA: {
                uint64_t off = rdBE(&bc[count + 2], 8);
                uint64_t v = (bc[count + 1] == 0) ? scopeBase() + off : rd(scopeBase() + off, 8);
                wr(sp, 8, v); sp += 8;
                break;
            }
            case OP_MOVI_U32: wr(sp, 4, rdBE(&bc[count + 1], 4)); sp += 4; break;
            case OP_GET_ADDRS:
                wr(sp, 8, bcBase); sp += 8;
                wr(sp, 8, bcBase); sp += 8;
                wr(sp, 8, 0); sp += 8;
                wr(sp, 8, 0); sp += 8;
                break;
            case OP_JMP_IND: {
                sp -= 8;
                uint64_t addr = rd(sp, 8);
                if (count == 887 || getenv("J8_DBGJ")) {
                    printf("JMP_IND sp=%u addr=%llx regs: R0=%llx R1=%llx R12=%llx stack[-16..]: ", sp,
                           (unsigned long long)addr, (unsigned long long)regs[0],
                           (unsigned long long)regs[1], (unsigned long long)regs[12]);
                    for (int k = 0; k < 16; ++k) printf("%02x", mem[sp + k]);
                    printf("\n");
                }
                count = static_cast<size_t>(addr) - bcBase;
                if (count >= bc.size()) { printf("JMP_IND OUT OF RANGE -> %zu\n", count); return 2; }
                if (getenv("J8_TRACE")) printf("  -> jump to %zu\n", count);
                continue;
            }
            case OP_NEW_ARRAY: {
                uint64_t size = rdBE(&bc[count + 1], 8);
                uint32_t slotOff = rdBE(&bc[count + 9], 4);
                wr(scopeBase() + slotOff, 8, heapCur);
                heapCur += size;
                break;
            }
            case OP_FREE_ARRAY: break;
            case OP_MEMCPY: {
                uint64_t dOff = rdBE(&bc[count + 2], 8), sOff = rdBE(&bc[count + 11], 8);
                uint64_t sz = rdBE(&bc[count + 19], 8);
                uint64_t d = (bc[count + 1] == 0) ? dOff : rd(dOff, 8);
                uint64_t s = (bc[count + 10] == 0) ? sOff : rd(sOff, 8);
                for (uint64_t i = 0; i < sz; ++i) mem[d + i] = mem[s + i];
                break;
            }
            case OP_EXTERN_CALL: {
                uint8_t idx = bc[count + 1];
                uint32_t argOff = rdBE(&bc[count + 2], 4);
                uint32_t retOff = rdBE(&bc[count + 6], 4);
                uint64_t abase = scopeBase() + argOff;
                uint64_t rbase = scopeBase() + retOff;
                if (idx == 0) {  // malloc(n) → ret = ptr
                    uint64_t n = rd(abase, 8);
                    wr(rbase, 8, heapCur);
                    heapCur += n;
                } else if (idx == 1) {  // free
                } else if (idx == 2) {  // memcpy(dst, src, n)
                    uint64_t d = rd(abase, 8), s2 = rd(abase + 8, 8), n = rd(abase + 16, 8);
                    for (uint64_t i = 0; i < n; ++i) mem[d + i] = mem[s2 + i];
                } else {
                    printf("UNSUPPORTED extern %u\n", idx);
                    return 2;
                }
                break;
            }
#define BINOP(OP, W, F) { sp -= 2 * (W); uint64_t a = rd(sp, W), b = rd(sp + W, W); wr(sp, W, (uint64_t)(F)); sp += W; break; }
#define BINOPF(OP, F) { sp -= 16; double a, b; memcpy(&a, &mem[sp], 8); memcpy(&b, &mem[sp + 8], 8); double r = (F); memcpy(&mem[sp], &r, 8); sp += 8; break; }
            case OP_ADD_U32: BINOP(0, 4, a + b)
            case OP_SUB_U32: BINOP(0, 4, a - b)
            case OP_MUL_U32: BINOP(0, 4, a * b)
            case OP_DIV_U32: BINOP(0, 4, b ? a / b : 0)
            case OP_ADD_U64: BINOP(0, 8, a + b)
            case OP_SUB_U64: BINOP(0, 8, a - b)
            case OP_MUL_U64: BINOP(0, 8, a * b)
            case OP_DIV_U64: BINOP(0, 8, b ? a / b : 0)
            case OP_ADD_PTR: BINOP(0, 8, a + b)
            case OP_SUB_PTR: BINOP(0, 8, a - b)
            case OP_SHL_U32: BINOP(0, 4, a << (b & 31))
            case OP_SHR_U32: BINOP(0, 4, a >> (b & 31))
            case OP_SHR_I32: BINOP(0, 4, (int32_t)a >> (b & 31))
            case OP_AND_U8: BINOP(0, 1, a & b)
            case OP_OR_U8: BINOP(0, 1, a | b)
            case OP_AND_U32: BINOP(0, 4, a & b)
            case OP_OR_U32: BINOP(0, 4, a | b)
            case OP_NOT_U32: { sp -= 4; wr(sp, 4, ~rd(sp, 4)); sp += 4; break; }
            case OP_SHL_U64: BINOP(0, 8, a << (b & 63))
            case OP_SHR_U64: BINOP(0, 8, a >> (b & 63))
            case OP_SHR_I64: BINOP(0, 8, (int64_t)a >> (b & 63))
            case OP_AND_U64: BINOP(0, 8, a & b)
            case OP_OR_U64: BINOP(0, 8, a | b)
            case OP_NOT_U64: { sp -= 8; wr(sp, 8, ~rd(sp, 8)); sp += 8; break; }
            case OP_ADD_F64: BINOPF(0, a + b)
            case OP_SUB_F64: BINOPF(0, a - b)
            case OP_MUL_F64: BINOPF(0, a * b)
            case OP_DIV_F64: BINOPF(0, b != 0 ? a / b : 0)
            case OP_CMP_EQ_U8: { sp -= 2; uint8_t b = rd(sp, 1), a = rd(sp + 1, 1); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_U8: { sp -= 2; uint8_t b = rd(sp, 1), a = rd(sp + 1, 1); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_U8: { sp -= 2; uint8_t b = rd(sp, 1), a = rd(sp + 1, 1); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_U16: { sp -= 4; uint16_t b = rd(sp, 2), a = rd(sp + 2, 2); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_U16: { sp -= 4; uint16_t b = rd(sp, 2), a = rd(sp + 2, 2); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_U16: { sp -= 4; uint16_t b = rd(sp, 2), a = rd(sp + 2, 2); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_U32: { sp -= 8; uint32_t b = rd(sp, 4), a = rd(sp + 4, 4); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_U32: { sp -= 8; uint32_t b = rd(sp, 4), a = rd(sp + 4, 4); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_U32: { sp -= 8; uint32_t b = rd(sp, 4), a = rd(sp + 4, 4); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_U64: { sp -= 16; uint64_t b = rd(sp, 8), a = rd(sp + 8, 8); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_U64: { sp -= 16; uint64_t b = rd(sp, 8), a = rd(sp + 8, 8); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_U64: { sp -= 16; uint64_t b = rd(sp, 8), a = rd(sp + 8, 8); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_I32: { sp -= 8; int32_t b = (int32_t)rd(sp, 4), a = (int32_t)rd(sp + 4, 4); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_I32: { sp -= 8; int32_t b = (int32_t)rd(sp, 4), a = (int32_t)rd(sp + 4, 4); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_I32: { sp -= 8; int32_t b = (int32_t)rd(sp, 4), a = (int32_t)rd(sp + 4, 4); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_I64: { sp -= 16; int64_t b = (int64_t)rd(sp, 8), a = (int64_t)rd(sp + 8, 8); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_I64: { sp -= 16; int64_t b = (int64_t)rd(sp, 8), a = (int64_t)rd(sp + 8, 8); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_I64: { sp -= 16; int64_t b = (int64_t)rd(sp, 8), a = (int64_t)rd(sp + 8, 8); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_F64: { sp -= 16; double b, a; memcpy(&b, &mem[sp], 8); memcpy(&a, &mem[sp + 8], 8); wr(sp, 1, a == b); sp += 1; break; }
            case OP_CMP_LT_F64: { sp -= 16; double b, a; memcpy(&b, &mem[sp], 8); memcpy(&a, &mem[sp + 8], 8); wr(sp, 1, a < b); sp += 1; break; }
            case OP_CMP_GT_F64: { sp -= 16; double b, a; memcpy(&b, &mem[sp], 8); memcpy(&a, &mem[sp + 8], 8); wr(sp, 1, a > b); sp += 1; break; }
            case OP_CMP_EQ_PTR: { sp -= 16; uint64_t b = rd(sp, 8), a = rd(sp + 8, 8); wr(sp, 1, a == b); sp += 1; break; }
            case OP_NOT_U8: { sp -= 1; wr(sp, 1, ~rd(sp, 1)); sp += 1; break; }
            case OP_COUT_CHAR8: { sp -= 1; printf("%c\n", (char)rd(sp, 1)); break; }
            case OP_COUT_CHAR16: { sp -= 2; printf("%u\n", (unsigned)rd(sp, 2)); break; }
            case OP_COUT_CHAR32: { sp -= 4; printf("%u\n", (unsigned)rd(sp, 4)); break; }
            case OP_CVT_F64_U32: { sp -= 8; double a; memcpy(&a, &mem[sp], 8); uint32_t r = (uint32_t)a; wr(sp, 4, r); sp += 4; break; }
            case OP_CVT_U32_F64: { sp -= 4; uint32_t a = (uint32_t)rd(sp, 4); double r = (double)a; memcpy(&mem[sp], &r, 8); sp += 8; break; }
            case OP_CVT_I32_F64: { sp -= 4; int32_t a = (int32_t)rd(sp, 4); double r = (double)a; memcpy(&mem[sp], &r, 8); sp += 8; break; }
            case OP_SQRT_F64: { sp -= 8; double a; memcpy(&a, &mem[sp], 8); double r = sqrt(a); memcpy(&mem[sp], &r, 8); sp += 8; break; }
            case OP_REG_MOVI_U8: regs[bc[count+1] & 0xF] = bc[count+2]; break;
            case OP_REG_MOVI_U16: regs[bc[count+1] & 0xF] = rdBE(&bc[count+2], 2); break;
            case OP_REG_MOVI_U32: regs[bc[count+1] & 0xF] = rdBE(&bc[count+2], 4); break;
            case OP_REG_MOVI_U64: regs[bc[count+1] & 0xF] = rdBE(&bc[count+2], 8); break;
            case OP_REG_MOV: regs[bc[count+1] & 0xF] = regs[bc[count+2] & 0xF]; break;
            case OP_REG_PUSH_U8: wr(sp, 1, regs[bc[count+1] & 0xF]); sp += 1; break;
            case OP_REG_PUSH_U16: wr(sp, 2, regs[bc[count+1] & 0xF]); sp += 2; break;
            case OP_REG_PUSH_U32: wr(sp, 4, regs[bc[count+1] & 0xF]); sp += 4; break;
            case OP_REG_PUSH_U64: wr(sp, 8, regs[bc[count+1] & 0xF]); sp += 8; break;
            case OP_REG_POP_U8: sp -= 1; regs[bc[count+1] & 0xF] = rd(sp, 1); break;
            case OP_REG_POP_U16: sp -= 2; regs[bc[count+1] & 0xF] = rd(sp, 2); break;
            case OP_REG_POP_U32: sp -= 4; regs[bc[count+1] & 0xF] = rd(sp, 4); break;
            case OP_REG_POP_U64: sp -= 8; regs[bc[count+1] & 0xF] = rd(sp, 8); break;
            case OP_REG_LOAD_U8: case OP_REG_LOAD_U16: case OP_REG_LOAD_U32: case OP_REG_LOAD_U64: {
                int w = (op == OP_REG_LOAD_U8) ? 1 : (op == OP_REG_LOAD_U16) ? 2 : (op == OP_REG_LOAD_U32) ? 4 : 8;
                uint8_t mode = bc[count + 2];
                uint64_t off = rdBE(&bc[count + 3], 8);
                uint64_t slotAddr = scopeBase() + off;
                uint64_t addr = (mode == 0) ? slotAddr : rd(slotAddr, 8);
                if (addr >= MEMSZ) { printf("LOAD OOB addr=%llx\n", (unsigned long long)addr); return 2; }
                regs[bc[count+1] & 0xF] = rd(addr, w);
                break;
            }
            case OP_REG_STORE_U8: case OP_REG_STORE_U16: case OP_REG_STORE_U32: case OP_REG_STORE_U64: {
                int w = (op == OP_REG_STORE_U8) ? 1 : (op == OP_REG_STORE_U16) ? 2 : (op == OP_REG_STORE_U32) ? 4 : 8;
                uint8_t mode = bc[count + 2];
                uint64_t off = rdBE(&bc[count + 3], 8);
                uint64_t slotAddr = scopeBase() + off;
                uint64_t addr = (mode == 0) ? slotAddr : rd(slotAddr, 8);
                if (addr >= MEMSZ) { printf("STORE OOB addr=%llx\n", (unsigned long long)addr); return 2; }
                wr(addr, w, regs[bc[count+1] & 0xF]);
                break;
            }
            default:
                printf("UNSUPPORTED op %u at %zu (sp=%u)\n", op, count, sp);
                return 2;
        }
        count += instrLen(op);
    }
    printf("done after %d steps (count=%zu)\n", steps, count);
    return 0;
}
