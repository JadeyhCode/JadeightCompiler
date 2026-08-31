//没错，这本来是一个动画软件，后面我发现这个Jadeight虚拟机太NB了就单独提了出来
//动画软件版本是0.03，结果回头一看虚拟机已经是2.10了
//====================  v2.00：类型全展开  ====================
// 1. 指令性能：memcpy 全部移除
//    - 字节码取数用模板 rdBE/wrBE（大端序手工拼字节，编译期展开）
//    - 栈/堆内存读写用 loadRaw/storeRaw（按宽度逐字节，对齐安全）
// 2. 类型指令全部展开（编译期硬编码宽度）：
//    - 每个 (运算,类型) 组合是独立 opcode，如 ADD_U32 / ADD_F64 / SQRT_F64
//    - 运行期不读任何类型字节，处理函数直接按 sizeof(T) 读写，最快路径
//    - CHAR8/16/32 与 U8/U16/U32 位模式一致（v2.03 起只有 COUT 保留 char，按字符输出）
// 3. ptr 只能寻址：只有 ADD_PTR / SUB_PTR（指针加减偏移），
//    禁止 ptr 的乘除/开方/log；LEA 负责取地址
// 4. 两种寻址方式：mode0 直接读栈内[基址+off]；mode1 从栈顶弹指针再读写
// 5. 分配器不关心类型：NEW_ARRAY(size:u64,slotOff:u32) 按 uint8_t[] 分配，
//    指针写到指定栈槽——只在乎 How size? Where?
// 6. REG 寄存器指令族（v2.03 新增）：16 个 64 位寄存器 R0..R15，
//    子指令族：MOVI（立即数→寄存器）、MOV（寄存器间拷贝）、
//            PUSH/POP（寄存器↔栈）、LOAD/STORE（内存↔寄存器，mode 与 LEA 一致）
// 7. 系统地址与间接跳转（v2.03 新增）：GET_ADDRS 一条指令压入关键地址
//    （bytecode 缓冲区 / size 字段 / DataSave 对象 / 管理器）；JMP_IND 弹栈绝对地址跳转
// 8. 多线程（v2.03 新增）：executoringHarness 每个线程一份 DataSave/executoring，
//    共享同一个 Manager（共有指针）；原子指令 ATOMIC_*（共 10 条，不超过 10 条）
//    实现跨线程同步。REG 指令族 opcode 最小 + 处理器代码最靠前，利于缓存命中。
// 9. 外部调用（v2.03 新增）：EXTERN_CALL 指令 —— libffi 原样暴露（薄透传，零封装）。
//    宿主用 ffi_prep_cif 预生成 ffi_cif（存全局/栈）连同函数指针登记进 externFn 表，
//    客人程序即可调用任意 C 函数（含 libffi 自身：ffi_prep_cif/ffi_call/ffi_raw_*…）。
// 10. VM 函数（v2.03 新增）：FunctionSave —— save 类对象（save 一行未改），可动态加载
//    （从文件/内存装载函数字节码）；FUNC_CALL 指令调用它。正常执行路径零开销：
//    只多一条 opcode 和一次函数调用时的状态重置，热循环不碰任何新字段。
// 11. MEMCPY 指令（v2.03 新增）：一条指令完成任意两块内存间的 bulk 拷贝
//    （mode0 栈槽 / mode1 指针解引用，与 REG_LOAD/STORE 一致）。
// 12. 枚举动态链接库的函数（v2.03 新增）：collect_externs —— 遍历 dl_iterate_phdr
//    所有已加载对象，只挑"刚刚 dlopen 过"的库（按加载基址匹配），解析 .dynsym
//    把所有函数符号的 {名字指针, 地址} 逐条写入客人缓冲区，返回条数。
// 13. LLVM JIT（v2.12 新增）：JadeightJIT::submit(save*) → 返回 LLVM JIT 编译后的
//    原生函数指针（O2 优化 + MCJIT；同 save 地址缓存复用，编译一次）。
//    调用约定与 FUNC_CALL 一致：fn(argPtr, retPtr, manager)（R15/R14 语义）；
//    全指令集逐条翻译成 LLVM IR，EXTERN_CALL/FUNC_CALL/原子/COUT 走
//    addGlobalMapping 绑定的运行期辅助，行为与解释器逐字节一致。
// 14. 字节码只读冻结（v2.12 新增）：save 的写能力仅限构造阶段（文件/内存加载时），
//    构造完成后 mprotect 冻结为只读 —— 解释器只能读字节码，运行时任何写入
//    （含客人程序经 GET_ADDRS 拿到的字节码指针）都会触发段错误而非悄悄改坏。
// 15. JIT_SUBMIT 指令（v2.12 新增）：参数为 save* 所在栈槽 fnPtrOff 与返回槽 retOff，
//    运行时把该 save 交给 JadeightJIT::submit 即时编译（同地址缓存复用），
//    返回的函数指针写回 retOff（8 字节）——客人程序可自行把任意 save 变成原生函数。
//==================== 指令表（opcode） =====================
//  REG 寄存器指令族放最前：寄存器指令是热点，opcode 值最小（1..21），
//  处理器代码也放在解释器最前面，尽量让指令缓存命中：
//   1..4     REG_MOVI_U8,U16,U32,U64(reg,imm)        —— 立即数写寄存器（零扩展）
//   5        REG_MOV(dst,src)                        —— 寄存器间拷贝（64 位）
//   6..9     REG_PUSH_U8,U16,U32,U64(reg)            —— 寄存器低 W 位压栈
//   10..13   REG_POP_U8,U16,U32,U64(reg)             —— 弹栈 W 字节写寄存器（零扩展）
//   14..17   REG_LOAD_U8,U16,U32,U64(reg,mode,off)   —— 内存→寄存器（mode 同 LEA）
//   18..21   REG_STORE_U8,U16,U32,U64(reg,mode,off)  —— 寄存器→内存（mode 同 LEA）
//  基础指令：
//   22=END  23=STACK_INIT(stackSize:u32,scopeSize:u32)
//   24=JMP(addr:u32)  25=SHORT_JMP(rel:i8)  26=SCOPE_PUSH  27=SCOPE_POP
//   28=STACK_PTR_MOVE(dec:u32)  29=NEW_STACK(bytes:u64)
//   30=NEW_HEAP(slotOff:u32,size:u64)  31=DEL_HEAP(slotOff:u32)
//   32=IF_GOTO(cond:u8,addr:u32)
//  立即数压栈 / 寻址：
//   33       LEA(mode:u8,off:u64) —— mode0压入栈内地址 / mode1压入栈槽内的指针
//   34       MOVI_U32(imm:u32) —— 压入 4 字节立即数（大端序）
//  四则运算（ADD/SUB 只 uint+float；MUL/DIV 全 10 种数值类型）：
//   35..40   ADD_U8,U16,U32,U64,F32,F64
//   41..46   SUB_U8,U16,U32,U64,F32,F64
//   47..56   MUL_U8,I8,U16,I16,U32,I32,U64,I64,F32,F64
//   57..66   DIV_U8,I8,U16,I16,U32,I32,U64,I64,F32,F64
//   67=ADD_PTR  68=SUB_PTR          —— ptr 只能寻址（加减偏移）
//   69..78   SQRT_<类型>  79..88 LOG_<类型> —— 按 double 计算，结果一律 f64 压栈
//  输出（只保留 char；CHAR16/CHAR32 无 ostream 重载，按数值打印）：
//   89=COUT_CHAR8  90=COUT_CHAR16  91=COUT_CHAR32
//  大小比较（< = >，除 char 外全部类型；结果统一压 U8 的 0/1）：
//   92..102  CMP_LT_U8,I8,U16,I16,U32,I32,U64,I64,F32,F64,PTR
//   103..113 CMP_EQ_U8,I8,U16,I16,U32,I32,U64,I64,F32,F64,PTR
//   114..124 CMP_GT_U8,I8,U16,I16,U32,I32,U64,I64,F32,F64,PTR
//  位运算（<< & | ~ 只给 uint；>> 给 uint 和 int 各一套）：
//   125..128 SHL_U8,U16,U32,U64       129..132 AND_U8,U16,U32,U64
//   133..136 OR_U8,U16,U32,U64        137..140 NOT_U8,U16,U32,U64
//   141..144 SHR_U8,U16,U32,U64（逻辑右移）  145..148 SHR_I8,I16,I32,I64（算术右移）
//  类型转换（浮点↔整数，只 32 位）：
//   149=CVT_F32_U32  150=CVT_F32_I32  151=CVT_F64_U32  152=CVT_F64_I32
//   153=CVT_U32_F32  154=CVT_U32_F64  155=CVT_I32_F32  156=CVT_I32_F64
//  分配器（无类型，只认 size 和 where）：
//   157      NEW_ARRAY(size:u64,slotOff:u32) —— 按 uint8_t[] 分配（不关心类型）
//   158      FREE_ARRAY(slotOff:u32)
//  系统地址 / 间接跳转 / 原子变量（多线程）：
//   159      GET_ADDRS —— 一条指令依次压入：bytecode 缓冲区地址、size 字段地址、
//                         DataSave 对象地址、管理器地址（各 8 字节，管理器在栈顶）
//   160      JMP_IND —— 间接跳转：弹栈 u64 绝对地址，换算成字节码偏移后跳转
//   161..170 ATOMIC_LOAD/STORE/XCHG/CAS/ADD_U32,U64(reg,mode,off) —— 原子变量操作
//            （mode 同 LEA：mode0 栈槽 / mode1 指针解引用；目标需 4/8 字节自然对齐）
//            LOAD: reg=原子读     STORE: 原子写 reg 低 W 位
//            XCHG: reg↔内存交换，reg=旧值
//            CAS:  弹栈 expected，相等则内存=reg；reg=实际旧值，压 U8 成功标志
//            ADD:  fetch_add，reg=旧值（加数取 reg 原值）
//  外部函数调用（libffi 原样暴露）：
//   171      EXTERN_CALL(idx:u8, argBaseOff:u32, retOff:u32) —— 从全局 externFn 表
//            取宿主用 ffi_prep_cif 预生成的 {ffi_cif, 函数指针}，把 Stack[base+argBaseOff]
//            起按 arg_types 顺序拆成 avalue[]（先 8 位再 16 位…），原样调用 ffi_call，
//            返回值写入 Stack[base+retOff]；对 libffi 接口不做任何修改/封装。
//  VM 函数调用（FunctionSave，save 类对象，可动态加载）：
//   172      FUNC_CALL(fnPtrOff:u32, argBaseOff:u32, retOff:u32) —— 从栈槽 fnPtrOff
//            读 FunctionSave*（真的指向该 save 类对象的指针），以 argBaseOff 为参数
//            基址、retOff 为返回值地址调用它。调用约定：函数被调用时 R15=参数基址
//            指针、R14=返回值地址指针（函数字节码以 STACK_INIT 开头、END 收尾，
//            END 的"正常退出"打印由 silent 标志抑制）。
//  LLVM JIT（v2.12 新增）：save 即时编译成原生函数指针
//   173      JIT_SUBMIT(fnPtrOff:u32, retOff:u32) —— 从栈槽 fnPtrOff 读 save*，
//            JadeightJIT::submit 编译（同地址缓存复用，编译一次），返回的函数指针
//            （调用约定 fn(argPtr,retPtr,manager)）写回栈槽 retOff（8 字节）。
//  内存 bulk 拷贝：
//   174      MEMCPY(dstMode:u8, dstOff:u64, srcMode:u8, srcOff:u64, size:u64)
//            —— 从 src 拷 size 字节到 dst（语义同 C memcpy，重叠行为未定义）。
//            mode 与 REG_LOAD/STORE 一致：mode0 = Stack[base+off] 本身，
//            mode1 = 先解引用栈槽里的指针再操作（可拷堆/管理器等任意内存）。
// 安全策略：bytecode 末尾追加 3 个 OP_ERR_END 哨兵字节，指令计数越界读到哨兵
//   即跳 ErrorEnd 安全退出；解释器内【不做任何越界检查】，追求极致速度。
//   字节码只读冻结（v2.12 新增）：save 构造完成后 mprotect 置只读，写能力仅限构造阶段，
//   解释器/JIT/客人程序（GET_ADDRS 拿到的字节码指针）只能读，运行时写入触发段错误。
//==================== 类型哲学 =====================
// 类型只在编译期（汇编器）确定：宽度与解释全部硬编码进 opcode，
// 运行期解释器不知道"类型"概念，只按每条指令自己的 sizeof(T) 读写字节。

#define _GNU_SOURCE // dlinfo(RTLD_DI_LINKMAP) / dl_iterate_phdr 需要
// 分配器（NEW_ARRAY/FREE_ARRAY）完全不关心类型：
//   How size?  —— 多大（字节数）
//   Where?    —— 在哪（存放/返回指针的栈槽 slotOff）
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib> // std::abort（字节码分配失败） // FunctionSave 动态加载（文件读写）
#include <cstring>
#include <dlfcn.h>   // 动态链接演示：dlopen/dlsym/dlclose（原样暴露）
#include <elf.h>     // 枚举已链接库的函数符号（ELF 解析）
#include <iomanip>
#include <iostream>
#include <limits>
#include <link.h>    // dl_iterate_phdr：遍历已加载库
#include <memory>
#include <string>
#include <sys/mman.h> // rwx 内存属性演示：mprotect（原样暴露）；save 字节码冻结也用 mprotect
#include <thread>
#include <type_traits>
#include <unistd.h>    // sysconf(_SC_PAGESIZE) / fork（字节码冻结验证）
#include <sys/wait.h>  // waitpid / WIFSIGNALED（字节码冻结验证）
#include <csignal>     // SIGSEGV（字节码冻结验证）
#include <vector>
#include <ffi.h> // EXTERN_CALL：原样暴露 libffi 原始 API（ffi_call/ffi_prep_cif 等）

//==================== LLVM JIT（v2.10 新增）：需要 LLVM 开发包（CMake 检测后定义宏） =====================
#ifdef JADEIGHT_HAS_LLVM
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/Support/Host.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <mutex>
#include <unordered_map>
#endif

//==================== opcode 定义 =====================
// REG 寄存器指令族放最前（opcode 最小 1..21）：寄存器指令是热点，
// 处理器代码同样放在解释器最前面，尽量让指令缓存命中
enum : uint8_t {
    OP_ERR_END = 0, // 哨兵：bytecode 末尾 3 个，越界读到即安全退出
    // REG 寄存器指令族（16 个 64 位寄存器 R0..R15，寄存器下标低 4 位有效）
    OP_REG_MOVI_U8, OP_REG_MOVI_U16, OP_REG_MOVI_U32, OP_REG_MOVI_U64,
    OP_REG_MOV,
    OP_REG_PUSH_U8, OP_REG_PUSH_U16, OP_REG_PUSH_U32, OP_REG_PUSH_U64,
    OP_REG_POP_U8, OP_REG_POP_U16, OP_REG_POP_U32, OP_REG_POP_U64,
    OP_REG_LOAD_U8, OP_REG_LOAD_U16, OP_REG_LOAD_U32, OP_REG_LOAD_U64,
    OP_REG_STORE_U8, OP_REG_STORE_U16, OP_REG_STORE_U32, OP_REG_STORE_U64,
    // 基础指令
    OP_END, OP_STACK_INIT, OP_JMP, OP_SHORT_JMP,
    OP_SCOPE_PUSH, OP_SCOPE_POP, OP_STACK_PTR_MOVE, OP_NEW_STACK,
    OP_NEW_HEAP, OP_DEL_HEAP, OP_IF_GOTO,
    OP_LEA,
    // 立即数压栈（目前只 U32）
    OP_MOVI_U32,
    // 四则运算（ADD/SUB 只保留 uint+float；MUL/DIV 全 10 种数值类型）
    OP_ADD_U8, OP_ADD_U16, OP_ADD_U32, OP_ADD_U64, OP_ADD_F32, OP_ADD_F64,
    OP_SUB_U8, OP_SUB_U16, OP_SUB_U32, OP_SUB_U64, OP_SUB_F32, OP_SUB_F64,
    OP_MUL_U8, OP_MUL_I8, OP_MUL_U16, OP_MUL_I16, OP_MUL_U32, OP_MUL_I32,
    OP_MUL_U64, OP_MUL_I64, OP_MUL_F32, OP_MUL_F64,
    OP_DIV_U8, OP_DIV_I8, OP_DIV_U16, OP_DIV_I16, OP_DIV_U32, OP_DIV_I32,
    OP_DIV_U64, OP_DIV_I64, OP_DIV_F32, OP_DIV_F64,
    // ptr 只能寻址（加减偏移）
    OP_ADD_PTR, OP_SUB_PTR,
    // 开方 / log（按 double 计算，结果 f64）
    OP_SQRT_U8, OP_SQRT_I8, OP_SQRT_U16, OP_SQRT_I16, OP_SQRT_U32, OP_SQRT_I32,
    OP_SQRT_U64, OP_SQRT_I64, OP_SQRT_F32, OP_SQRT_F64,
    OP_LOG_U8, OP_LOG_I8, OP_LOG_U16, OP_LOG_I16, OP_LOG_U32, OP_LOG_I32,
    OP_LOG_U64, OP_LOG_I64, OP_LOG_F32, OP_LOG_F64,
    // COUT（只保留 char）
    OP_COUT_CHAR8, OP_COUT_CHAR16, OP_COUT_CHAR32,
    // 大小比较（< = >，除 char 外全部类型；结果统一压 U8 的 0/1）
    OP_CMP_LT_U8, OP_CMP_LT_I8, OP_CMP_LT_U16, OP_CMP_LT_I16, OP_CMP_LT_U32, OP_CMP_LT_I32,
    OP_CMP_LT_U64, OP_CMP_LT_I64, OP_CMP_LT_F32, OP_CMP_LT_F64, OP_CMP_LT_PTR,
    OP_CMP_EQ_U8, OP_CMP_EQ_I8, OP_CMP_EQ_U16, OP_CMP_EQ_I16, OP_CMP_EQ_U32, OP_CMP_EQ_I32,
    OP_CMP_EQ_U64, OP_CMP_EQ_I64, OP_CMP_EQ_F32, OP_CMP_EQ_F64, OP_CMP_EQ_PTR,
    OP_CMP_GT_U8, OP_CMP_GT_I8, OP_CMP_GT_U16, OP_CMP_GT_I16, OP_CMP_GT_U32, OP_CMP_GT_I32,
    OP_CMP_GT_U64, OP_CMP_GT_I64, OP_CMP_GT_F32, OP_CMP_GT_F64, OP_CMP_GT_PTR,
    // 位运算（<< & | ~ 只给 uint；>> 给 uint 和 int 各一套）
    OP_SHL_U8, OP_SHL_U16, OP_SHL_U32, OP_SHL_U64,
    OP_AND_U8, OP_AND_U16, OP_AND_U32, OP_AND_U64,
    OP_OR_U8, OP_OR_U16, OP_OR_U32, OP_OR_U64,
    OP_NOT_U8, OP_NOT_U16, OP_NOT_U32, OP_NOT_U64,
    OP_SHR_U8, OP_SHR_U16, OP_SHR_U32, OP_SHR_U64,
    OP_SHR_I8, OP_SHR_I16, OP_SHR_I32, OP_SHR_I64,
    // 类型转换（浮点↔整数，只 32 位）
    OP_CVT_F32_U32, OP_CVT_F32_I32, OP_CVT_F64_U32, OP_CVT_F64_I32,
    OP_CVT_U32_F32, OP_CVT_U32_F64, OP_CVT_I32_F32, OP_CVT_I32_F64,
    // 分配器（无类型，只认 size 和 where）
    OP_NEW_ARRAY, OP_FREE_ARRAY,
    // 系统地址获取 / 间接跳转
    OP_GET_ADDRS, // 依次压入 bytecode 缓冲区地址、size 字段地址、DataSave 地址、管理器地址（各 u64）
    OP_JMP_IND,   // 间接跳转：弹栈 u64 绝对地址，换算成字节码偏移后跳转
    // 原子变量（多线程；共 10 条，不超过 10 条的上限）
    OP_ATOMIC_LOAD_U32, OP_ATOMIC_LOAD_U64,
    OP_ATOMIC_STORE_U32, OP_ATOMIC_STORE_U64,
    OP_ATOMIC_XCHG_U32, OP_ATOMIC_XCHG_U64,
    OP_ATOMIC_CAS_U32, OP_ATOMIC_CAS_U64,
    OP_ATOMIC_ADD_U32, OP_ATOMIC_ADD_U64,
    // 外部函数调用（libffi 原样暴露）
    OP_EXTERN_CALL, // EXTERN_CALL(idx:u8, argBaseOff:u32, retOff:u32) —— 调 externFn 表里的 C 函数
    // VM 函数调用（FunctionSave：save 类对象，可动态加载）
    OP_FUNC_CALL,   // FUNC_CALL(fnPtrOff:u32, argBaseOff:u32, retOff:u32) —— 调栈槽里的 FunctionSave*
    // LLVM JIT（v2.12 新增）：把 save 即时编译成原生函数指针
    OP_JIT_SUBMIT,  // JIT_SUBMIT(fnPtrOff:u32, retOff:u32) —— 从栈槽 fnPtrOff 读 save*，
                    // JadeightJIT::submit 编译（同地址缓存复用），函数指针写栈槽 retOff（8字节）
    // 内存 bulk 拷贝
    OP_MEMCPY,      // MEMCPY(dstMode:u8,dstOff:u64,srcMode:u8,srcOff:u64,size:u64)
};

//==================== 无 memcpy 的快速字节工具 =====================
// 从字节码读取大端序整数（编译器展开成直接字节加载，零函数调用）
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
[[gnu::always_inline]] static inline T rdBE(const uint8_t* p) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < sizeof(T); ++i) v = (v << 8) | p[i];
    return static_cast<T>(v);
}
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
[[gnu::always_inline]] static inline void wrBE(uint8_t* p, T v) {
    for (uint32_t i = 0; i < sizeof(T); ++i) {
        p[sizeof(T) - 1 - i] = static_cast<uint8_t>(v & 0xFF);
        v = static_cast<T>(static_cast<uint64_t>(v) >> 8);
    }
}
// 从任意内存读 width 字节拼成 uint64（对齐安全，代替 memcpy）
[[gnu::always_inline]] static inline uint64_t loadRaw(const uint8_t* src, uint8_t width) {
    uint64_t r = 0;
    for (uint8_t i = 0; i < width; ++i) r |= static_cast<uint64_t>(src[i]) << (8 * i);
    return r;
}
[[gnu::always_inline]] static inline void storeRaw(uint8_t* dst, uint64_t raw, uint8_t width) {
    for (uint8_t i = 0; i < width; ++i) dst[i] = static_cast<uint8_t>(raw >> (8 * i));
}

//==================== 编译期类型化栈/内存工具 =====================
// 整型：直接按 make_unsigned 位模式存取（C++20 补码，符号自然正确）
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
[[gnu::always_inline]] static inline void pushT(uint8_t* Stack, uint32_t& sp, T val) {
    storeRaw(Stack + sp, static_cast<uint64_t>(static_cast<std::make_unsigned_t<T>>(val)), sizeof(T));
    sp += sizeof(T);
}
template <class T, std::enable_if_t<std::is_integral_v<T>, int> = 0>
[[gnu::always_inline]] static inline T popT(const uint8_t* Stack, uint32_t& sp) {
    sp -= sizeof(T);
    return static_cast<T>(static_cast<std::make_unsigned_t<T>>(loadRaw(Stack + sp, sizeof(T))));
}
// 浮点：按 32/64 位位模式存取
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
[[gnu::always_inline]] static inline void pushT(uint8_t* Stack, uint32_t& sp, T val) {
    using U = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    storeRaw(Stack + sp, static_cast<uint64_t>(std::bit_cast<U>(val)), sizeof(T));
    sp += sizeof(T);
}
template <class T, std::enable_if_t<std::is_floating_point_v<T>, int> = 0>
[[gnu::always_inline]] static inline T popT(const uint8_t* Stack, uint32_t& sp) {
    using U = std::conditional_t<sizeof(T) == 4, uint32_t, uint64_t>;
    sp -= sizeof(T);
    return std::bit_cast<T>(static_cast<U>(loadRaw(Stack + sp, sizeof(T))));
}
// 按类型打印（COUT 专用，只保留 char：CHAR8 按字符打印；
// CHAR16/CHAR32 没有 ostream 字符重载，只能按数值打印）
[[gnu::always_inline]] static inline void coutVal(uint8_t  v) { std::cout << static_cast<char>(v) << '\n'; }
[[gnu::always_inline]] static inline void coutVal(uint16_t v) { std::cout << v << '\n'; }
[[gnu::always_inline]] static inline void coutVal(uint32_t v) { std::cout << v << '\n'; }

//==================== 字节码容器 =====================
// 只读冻结设计（v2.12 新增）：字节码的写能力仅限于构造阶段 —— 构造时从输入
// （文件/内存加载）拷入字节码并追加 3 个哨兵，构造完成后立即 mprotect 冻结为
// 只读。此后解释器、JIT 以及客人程序（经 GET_ADDRS 拿到的字节码指针）都只能读；
// 任何运行时写入都会触发 SIGSEGV，而不是悄悄改坏字节码。
struct save{
    // 页映射释放器：mmap 按页对齐（mprotect 冻结的前置条件），munmap 释放
    struct PageFree {
        size_t len = 0;
        void operator()(uint8_t* p) const noexcept { if (p) munmap(p, len); }
    };
    static size_t pageSize() { // 系统页大小（mprotect 要求整页）
        static const size_t p = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        return p;
    }
    static size_t roundUp(size_t n) { const size_t pg = pageSize(); return (n + pg - 1) & ~(pg - 1); }
    static uint8_t* allocPages(size_t n) { // 页对齐的可写映射（构造期专用）
        void* p = mmap(nullptr, roundUp(n), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { // 本 TU 编译带 -fno-exceptions（LLVM 头要求），不可抛异常
            std::perror("[save] 字节码分配(mmap)失败");
            std::abort();
        }
        return static_cast<uint8_t*>(p);
    }

    std::unique_ptr<uint8_t[], PageFree> byteCode;
    size_t size;

    // 构造：拷贝 + 哨兵 + 冻结（写能力仅限本阶段）。
    // 注意：byteCode 必须在成员初始化列表里直接构造 —— 若先默认构造再赋值，
    // 会实例化 unique_ptr 的默认构造（GCC 11 对"嵌套删除器+默认成员初始化器+
    // 作为成员"组合会把 is_default_constructible 误判为 false）。
    save(const uint8_t *inputBC, size_t n)
        : byteCode(allocPages(n + 3), PageFree{roundUp(n + 3)}), size(n + 3) {
        if (inputBC != nullptr) {
            std::copy(inputBC, inputBC + n, byteCode.get());
            byteCode[n] = OP_ERR_END; byteCode[n+1] = OP_ERR_END; byteCode[n+2] = OP_ERR_END; // 3个哨兵：越界读→ErrorEnd
        }
        freeze(); // 构造完成 → 冻结内存（只读）
    }
    [[nodiscard]] size_t getSize() const { return size; }
    [[nodiscard]] uint8_t getByte(size_t digit) const { return byteCode[digit]; }
    [[nodiscard]] bool getBit(size_t digit) const {
        return (byteCode[digit / 8] >> (digit % 8)) & 1;
    }
    // 拷贝也是构造：拷入新的可写映射后同样冻结
    save(const save &other)
        : byteCode(allocPages(other.size), PageFree{roundUp(other.size)}), size(other.size) {
        std::copy(other.byteCode.get(), other.byteCode.get() + other.size, byteCode.get());
        freeze();
    }
    save(save &&other) noexcept : byteCode(std::move(other.byteCode)), size(other.size) {
        other.size = 0; // 移动不触碰冻结的映射，原样交接
    }
    save &operator=(save &&other) noexcept {
        if (this != &other) {
            byteCode.reset(); size = 0;
            this->size = other.size;
            this->byteCode = std::move(other.byteCode);
            other.size = 0;
        }
        return *this;
    }
    save &operator=(const save &other) {
        if (this != &other) {
            save temp(other);
            std::swap(byteCode, temp.byteCode);
            std::swap(size, temp.size);
        }
        return *this;
    }
private:
    void freeze() { // 冻结：整个映射置只读（构造期写完哨兵后调用）
        if (mprotect(byteCode.get(), roundUp(size), PROT_READ) != 0)
            std::perror("[save] 字节码冻结(mprotect)失败");
    }
};

//==================== 管理器（多线程共享） =====================
// 宿主可自由扩展；counter 放在偏移 0，多线程演示直接用管理器指针原子自增它。
// hostFn 放在偏移 16：FUNC_CALL 演示里宿主把 FunctionSave* 经管理器交给客人程序。
// rwxBuf 放在偏移 24：mprotect 演示里宿主给客人程序一块页对齐缓冲（改 rwx 用）。
struct Manager {
    std::atomic<uint64_t> counter{0}; // 原子计数器（偏移 0）
    uint64_t magic = 0x4D414E47;      // 'MANG' 标识（偏移 8）
    void* hostFn = nullptr;           // 宿主提供的 VM 函数指针 FunctionSave*（偏移 16）
    void* rwxBuf = nullptr;           // 页对齐缓冲（偏移 24）：客人程序 mprotect 改 rwx 用
    void* jitSave = nullptr;          // JIT_SUBMIT 演示：宿主提供的 save*（偏移 32）
    void* jitFn = nullptr;            // JIT_SUBMIT 演示：客人程序写回的 JIT 函数指针（偏移 40）
};

//==================== 虚拟机状态 =====================
struct DataSave {
    uint32_t stackSize = 0;
    uint8_t* Stack = nullptr;
    uint32_t stackPtr = 0;
    uint64_t Regs[16] = {}; // REG 寄存器文件 R0..R15（64 位，初始 0）
    std::shared_ptr<Manager> manager; // 管理器共有指针（多线程共享同一实例）
    uint8_t end = 0; //0=运行中 1=正常退出 2=越界/非法退出
    uint8_t silent = 0; //1=函数执行模式（FUNC_CALL）：END/出错不打印退出信息；正常路径零开销
    uint8_t commandCode = 0;
    uint32_t zuoYongYv = 1;
    unsigned long long* zuoYongYvStackPtr = nullptr;
    unsigned long long count = 0;
    DataSave() = default;
    DataSave(const DataSave&) = delete;
    DataSave& operator=(const DataSave&) = delete;
    DataSave(DataSave&&) noexcept = default;
    DataSave& operator=(DataSave&&) noexcept = default;
};

//==================== EXTERN_CALL 外部函数表（libffi 原样暴露） =====================
// externFn[idx] = { ffi_cif*（宿主用 ffi_prep_cif 预生成，存全局/栈）, 函数指针 }
// EXTERN_CALL 只按 arg_types 顺序拆分参数指针后调用【原始 ffi_call】——零封装。
// 必须声明在解释器之前（解释器里的 ITP_EXTERN_CALL 直接读它）。
struct ExternFn { const ffi_cif* cif; void* fn; };
ExternFn externFn[256] = {};

// FUNC_CALL 前置声明（完整定义在 FunctionSave 之后，解释器处理器只调这个辅助函数）：
// 调用约定：函数被调用时 R15=参数基址指针、R14=返回值地址指针
class FunctionSave;
static void callFunctionSave(FunctionSave* fn, uint8_t* argPtr, uint8_t* retPtr);
// JIT_SUBMIT 前置声明（完整定义在 JadeightJIT 之后；无 LLVM 时 submit 返回 nullptr）：
// 输入 save* → 返回 JIT 编译后的函数指针（调用约定 fn(argPtr,retPtr,manager)）
static void* jitSubmitSave(const save& s);

//==================== 解释器（computed goto 线程化分发，类型全展开） =====================
struct executoring {
    DataSave *ptr;
    void F8BFLRead(const save &MainBY) {
        // 跳转表：256项全部指向 ERR_END，再覆盖已定义指令
        const void* interpretedPTR[256];
        for (size_t i = 0; i < 256; ++i) interpretedPTR[i] = &&ITP_ERR_END;
        interpretedPTR[OP_ERR_END] = &&ITP_ERR_END;
        interpretedPTR[OP_END] = &&ITP_END;
        interpretedPTR[OP_STACK_INIT] = &&ITP_STACK_INIT;
        interpretedPTR[OP_JMP] = &&ITP_JMP;
        interpretedPTR[OP_SHORT_JMP] = &&ITP_SHORT_JMP;
        interpretedPTR[OP_SCOPE_PUSH] = &&ITP_SCOPE_PUSH;
        interpretedPTR[OP_SCOPE_POP] = &&ITP_SCOPE_POP;
        interpretedPTR[OP_STACK_PTR_MOVE] = &&ITP_STACK_PTR_MOVE;
        interpretedPTR[OP_NEW_STACK] = &&ITP_NEW_STACK;
        interpretedPTR[OP_NEW_HEAP] = &&ITP_NEW_HEAP;
        interpretedPTR[OP_DEL_HEAP] = &&ITP_DEL_HEAP;
        interpretedPTR[OP_IF_GOTO] = &&ITP_IF_GOTO;
        interpretedPTR[OP_LEA] = &&ITP_LEA;
        interpretedPTR[OP_ADD_PTR] = &&ITP_ADD_PTR;
        interpretedPTR[OP_SUB_PTR] = &&ITP_SUB_PTR;
        interpretedPTR[OP_NEW_ARRAY] = &&ITP_NEW_ARRAY;
        interpretedPTR[OP_FREE_ARRAY] = &&ITP_FREE_ARRAY;
        interpretedPTR[OP_GET_ADDRS] = &&ITP_GET_ADDRS;
        interpretedPTR[OP_JMP_IND] = &&ITP_JMP_IND;
        interpretedPTR[OP_EXTERN_CALL] = &&ITP_EXTERN_CALL;
        interpretedPTR[OP_FUNC_CALL] = &&ITP_FUNC_CALL;
        interpretedPTR[OP_JIT_SUBMIT] = &&ITP_JIT_SUBMIT;
        interpretedPTR[OP_MEMCPY] = &&ITP_MEMCPY;
        #define SET_ATOMIC_LOAD(NAME)  interpretedPTR[OP_ATOMIC_LOAD_##NAME]  = &&ITP_ATOMIC_LOAD_##NAME;
        #define SET_ATOMIC_STORE(NAME) interpretedPTR[OP_ATOMIC_STORE_##NAME] = &&ITP_ATOMIC_STORE_##NAME;
        #define SET_ATOMIC_XCHG(NAME)  interpretedPTR[OP_ATOMIC_XCHG_##NAME]  = &&ITP_ATOMIC_XCHG_##NAME;
        #define SET_ATOMIC_CAS(NAME)   interpretedPTR[OP_ATOMIC_CAS_##NAME]   = &&ITP_ATOMIC_CAS_##NAME;
        #define SET_ATOMIC_ADD(NAME)   interpretedPTR[OP_ATOMIC_ADD_##NAME]   = &&ITP_ATOMIC_ADD_##NAME;
        // 原子变量（多线程）
        SET_ATOMIC_LOAD(U32) SET_ATOMIC_LOAD(U64)
        SET_ATOMIC_STORE(U32) SET_ATOMIC_STORE(U64)
        SET_ATOMIC_XCHG(U32) SET_ATOMIC_XCHG(U64)
        SET_ATOMIC_CAS(U32) SET_ATOMIC_CAS(U64)
        SET_ATOMIC_ADD(U32) SET_ATOMIC_ADD(U64)
        // 类型化指令
        #define SET_ADD(NAME)  interpretedPTR[OP_ADD_##NAME]  = &&ITP_ADD_##NAME;
        #define SET_SUB(NAME)  interpretedPTR[OP_SUB_##NAME]  = &&ITP_SUB_##NAME;
        #define SET_MUL(NAME)  interpretedPTR[OP_MUL_##NAME]  = &&ITP_MUL_##NAME;
        #define SET_DIV(NAME)  interpretedPTR[OP_DIV_##NAME]  = &&ITP_DIV_##NAME;
        #define SET_SQRT(NAME) interpretedPTR[OP_SQRT_##NAME] = &&ITP_SQRT_##NAME;
        #define SET_LOG(NAME)  interpretedPTR[OP_LOG_##NAME]  = &&ITP_LOG_##NAME;
        #define SET_COUT(NAME) interpretedPTR[OP_COUT_##NAME] = &&ITP_COUT_##NAME;
        #define SET_CMP(NAME)  interpretedPTR[OP_CMP_##NAME]  = &&ITP_CMP_##NAME;
        #define SET_SHL(NAME)  interpretedPTR[OP_SHL_##NAME]  = &&ITP_SHL_##NAME;
        #define SET_AND(NAME)  interpretedPTR[OP_AND_##NAME]  = &&ITP_AND_##NAME;
        #define SET_OR(NAME)   interpretedPTR[OP_OR_##NAME]   = &&ITP_OR_##NAME;
        #define SET_NOT(NAME)  interpretedPTR[OP_NOT_##NAME]  = &&ITP_NOT_##NAME;
        #define SET_SHR(NAME)  interpretedPTR[OP_SHR_##NAME]  = &&ITP_SHR_##NAME;
        #define SET_CVT(NAME)  interpretedPTR[OP_CVT_##NAME]  = &&ITP_CVT_##NAME;
        #define SET_MOVI(NAME) interpretedPTR[OP_MOVI_##NAME] = &&ITP_MOVI_##NAME;
        #define SET_REG_MOVI(NAME) interpretedPTR[OP_REG_MOVI_##NAME] = &&ITP_REG_MOVI_##NAME;
        #define SET_REG_MOV interpretedPTR[OP_REG_MOV] = &&ITP_REG_MOV;
        #define SET_REG_PUSH(NAME) interpretedPTR[OP_REG_PUSH_##NAME] = &&ITP_REG_PUSH_##NAME;
        #define SET_REG_POP(NAME) interpretedPTR[OP_REG_POP_##NAME] = &&ITP_REG_POP_##NAME;
        #define SET_REG_LOAD(NAME) interpretedPTR[OP_REG_LOAD_##NAME] = &&ITP_REG_LOAD_##NAME;
        #define SET_REG_STORE(NAME) interpretedPTR[OP_REG_STORE_##NAME] = &&ITP_REG_STORE_##NAME;
        // 立即数压栈
        SET_MOVI(U32)
        // REG 寄存器指令族
        SET_REG_MOVI(U8) SET_REG_MOVI(U16) SET_REG_MOVI(U32) SET_REG_MOVI(U64)
        SET_REG_MOV
        SET_REG_PUSH(U8) SET_REG_PUSH(U16) SET_REG_PUSH(U32) SET_REG_PUSH(U64)
        SET_REG_POP(U8) SET_REG_POP(U16) SET_REG_POP(U32) SET_REG_POP(U64)
        SET_REG_LOAD(U8) SET_REG_LOAD(U16) SET_REG_LOAD(U32) SET_REG_LOAD(U64)
        SET_REG_STORE(U8) SET_REG_STORE(U16) SET_REG_STORE(U32) SET_REG_STORE(U64)
        // ADD/SUB 只保留 uint + float
        SET_ADD(U8) SET_ADD(U16) SET_ADD(U32) SET_ADD(U64) SET_ADD(F32) SET_ADD(F64)
        SET_SUB(U8) SET_SUB(U16) SET_SUB(U32) SET_SUB(U64) SET_SUB(F32) SET_SUB(F64)
        // MUL/DIV 全类型
        SET_MUL(U8) SET_MUL(I8) SET_MUL(U16) SET_MUL(I16) SET_MUL(U32) SET_MUL(I32)
        SET_MUL(U64) SET_MUL(I64) SET_MUL(F32) SET_MUL(F64)
        SET_DIV(U8) SET_DIV(I8) SET_DIV(U16) SET_DIV(I16) SET_DIV(U32) SET_DIV(I32)
        SET_DIV(U64) SET_DIV(I64) SET_DIV(F32) SET_DIV(F64)
        // SQRT / LOG 全类型
        SET_SQRT(U8) SET_SQRT(I8) SET_SQRT(U16) SET_SQRT(I16) SET_SQRT(U32) SET_SQRT(I32)
        SET_SQRT(U64) SET_SQRT(I64) SET_SQRT(F32) SET_SQRT(F64)
        SET_LOG(U8) SET_LOG(I8) SET_LOG(U16) SET_LOG(I16) SET_LOG(U32) SET_LOG(I32)
        SET_LOG(U64) SET_LOG(I64) SET_LOG(F32) SET_LOG(F64)
        // COUT 只保留 char
        SET_COUT(CHAR8) SET_COUT(CHAR16) SET_COUT(CHAR32)
        // 大小比较（< = >，除 char 外全部类型）
        SET_CMP(LT_U8) SET_CMP(LT_I8) SET_CMP(LT_U16) SET_CMP(LT_I16) SET_CMP(LT_U32) SET_CMP(LT_I32)
        SET_CMP(LT_U64) SET_CMP(LT_I64) SET_CMP(LT_F32) SET_CMP(LT_F64) SET_CMP(LT_PTR)
        SET_CMP(EQ_U8) SET_CMP(EQ_I8) SET_CMP(EQ_U16) SET_CMP(EQ_I16) SET_CMP(EQ_U32) SET_CMP(EQ_I32)
        SET_CMP(EQ_U64) SET_CMP(EQ_I64) SET_CMP(EQ_F32) SET_CMP(EQ_F64) SET_CMP(EQ_PTR)
        SET_CMP(GT_U8) SET_CMP(GT_I8) SET_CMP(GT_U16) SET_CMP(GT_I16) SET_CMP(GT_U32) SET_CMP(GT_I32)
        SET_CMP(GT_U64) SET_CMP(GT_I64) SET_CMP(GT_F32) SET_CMP(GT_F64) SET_CMP(GT_PTR)
        // 位运算（<< & | ~ 只给 uint；>> 给 uint 和 int 各一套）
        SET_SHL(U8) SET_SHL(U16) SET_SHL(U32) SET_SHL(U64)
        SET_AND(U8) SET_AND(U16) SET_AND(U32) SET_AND(U64)
        SET_OR(U8)  SET_OR(U16)  SET_OR(U32)  SET_OR(U64)
        SET_NOT(U8) SET_NOT(U16) SET_NOT(U32) SET_NOT(U64)
        SET_SHR(U8) SET_SHR(U16) SET_SHR(U32) SET_SHR(U64)
        SET_SHR(I8) SET_SHR(I16) SET_SHR(I32) SET_SHR(I64)
        // 类型转换（浮点 ↔ 整数，只 32 位）
        SET_CVT(F32_U32) SET_CVT(F32_I32) SET_CVT(F64_U32) SET_CVT(F64_I32)
        SET_CVT(U32_F32) SET_CVT(U32_F64) SET_CVT(I32_F32) SET_CVT(I32_F64)

        uint64_t u64;
        uint32_t u32;
        uint8_t mode;
        uint8_t* tptr;
        #define INTERPRETED do{goto *interpretedPTR[ptr->commandCode];}while(0)
        #define SCOPE_BASE (ptr->zuoYongYvStackPtr[ptr->zuoYongYv - 1])
        #define NEXT(countAdd) do{ptr->count += (countAdd); ptr->commandCode = MainBY.byteCode[ptr->count]; INTERPRETED;}while(0)

        end:
        // ===== 调试追踪钩子 =====
        {
            static uint64_t trcPos = 0;
            static uint64_t trcBuf[512];
            static uint64_t trcOp[512];
            trcBuf[trcPos % 512] = ptr->count;
            trcOp[trcPos % 512] = MainBY.byteCode[ptr->count];
            ++trcPos;
            if (getenv("J8_VMTRACE")) fprintf(stderr, "T %llu op=%u\n", (unsigned long long)ptr->count, (unsigned)MainBY.byteCode[ptr->count]);
        }
        if(!ptr->end){
        ptr->commandCode = MainBY.byteCode[ptr->count];
        INTERPRETED;
        }
        // silent=1（FUNC_CALL 函数执行模式）：不打印退出信息
        if(ptr->end==0 && !ptr->silent){ std::cout<<"意外退出"<<std::endl; }
        if(ptr->end==1 && !ptr->silent){ std::cout<<"正常退出"<<std::endl; }
        if(ptr->end==2 && !ptr->silent){ std::cout<<"越界退出"<<std::endl; }
        return;

        ITP_ERR_END:
        delete[] ptr->Stack;
        delete[] ptr->zuoYongYvStackPtr;
        ptr->Stack = nullptr; ptr->zuoYongYvStackPtr = nullptr;
        ptr->end = 2; // 越界/非法指令 → 越界退出
        goto end;
        ITP_END:
        delete[] ptr->Stack;
        delete[] ptr->zuoYongYvStackPtr;
        ptr->Stack = nullptr; ptr->zuoYongYvStackPtr = nullptr;
        ptr->end = 1; // 正常退出
        goto end;

        ITP_STACK_INIT:
        ptr->Stack = new uint8_t[rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1])];
        ptr->stackSize = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
        ptr->zuoYongYvStackPtr = new unsigned long long[rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 5])];
        ptr->zuoYongYvStackPtr[0] = 0; // 作用域0基址=0
        NEXT(9);
        ITP_JMP:
        ptr->count = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
        ptr->commandCode = MainBY.byteCode[ptr->count];
        INTERPRETED;
        ITP_SHORT_JMP:
        if(MainBY.byteCode[ptr->count + 1] > 0b01111111){
            ptr->count -= (MainBY.byteCode[ptr->count + 1] - 129);
        } else {
            ptr->count += (MainBY.byteCode[ptr->count + 1] + 2);
        }
        ptr->commandCode = MainBY.byteCode[ptr->count];
        INTERPRETED;
        ITP_STACK_PTR_MOVE:
        ptr->stackPtr -= rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
        NEXT(5);
        ITP_SCOPE_PUSH:
        if (getenv("J8_VMTRACE")) fprintf(stderr, "SCOPE_PUSH sp=%u -> level=%d\n", (unsigned)ptr->stackPtr, (int)ptr->zuoYongYv);
        ptr->zuoYongYvStackPtr[ptr->zuoYongYv] = ptr->stackPtr;
        ++ptr->zuoYongYv;
        NEXT(1);
        ITP_SCOPE_POP: // 无越界检查：作用域栈下溢由调用方保证
        if (getenv("J8_VMTRACE")) fprintf(stderr, "SCOPE_POP sp=%u level=%d->%d base=%llu\n", (unsigned)ptr->stackPtr, (int)ptr->zuoYongYv, (int)(ptr->zuoYongYv - 1), (unsigned long long)ptr->zuoYongYvStackPtr[ptr->zuoYongYv - 1]);
        --ptr->zuoYongYv;
        ptr->stackPtr = ptr->zuoYongYvStackPtr[ptr->zuoYongYv];
        NEXT(1);
        ITP_NEW_STACK:
        if (getenv("J8_VMTRACE")) fprintf(stderr, "NEW_STACK %llu sp=%u -> %u\n", (unsigned long long)rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 1]), (unsigned)ptr->stackPtr, (unsigned)(ptr->stackPtr + rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 1])));
        ptr->stackPtr += rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 1]);
        NEXT(9);
        ITP_NEW_HEAP:
        u32 = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
        u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 5]);
        tptr = new uint8_t[u64];
        storeRaw(ptr->Stack + (SCOPE_BASE + u32), reinterpret_cast<uint64_t>(tptr), 8);
        NEXT(13);
        ITP_DEL_HEAP:
        u32 = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
        tptr = reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u32), 8));
        delete[] tptr;
        NEXT(5);
        ITP_IF_GOTO:
        if(MainBY.byteCode[ptr->count + 1]){
            NEXT(6);
        } else {
            ptr->count = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 2]);
            ptr->commandCode = MainBY.byteCode[ptr->count];
            INTERPRETED;
        }



        //============== 类型全展开的指令（宽度 = sizeof(T)，编译期硬编码） ==============
        // REG 寄存器指令族放最前：寄存器指令是热点，处理器代码靠前，利于指令缓存命中
        // 寄存器下标 1 字节，低 4 位有效（R0..R15），无越界检查
        #define DEF_REG_MOVI(NAME, T) \
            ITP_REG_MOVI_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                ptr->Regs[u32 & 0x0F] = rdBE<T>(&MainBY.byteCode[ptr->count + 2]); \
                NEXT(2 + sizeof(T)); /* 1 opcode + 1 reg + imm */ \
            }
        #define DEF_REG_MOV \
            ITP_REG_MOV: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                const uint32_t src = MainBY.byteCode[ptr->count + 2]; \
                ptr->Regs[u32 & 0x0F] = ptr->Regs[src & 0x0F]; \
                NEXT(3); \
            }
        #define DEF_REG_PUSH(NAME, T) \
            ITP_REG_PUSH_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                if (getenv("J8_VMTRACE")) fprintf(stderr, "PUSH_%s R%u = %llu (sp=%u)\n", #NAME, u32 & 0x0F, (unsigned long long)ptr->Regs[u32 & 0x0F], (unsigned)ptr->stackPtr); \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(ptr->Regs[u32 & 0x0F])); \
                NEXT(2); \
            }
        #define DEF_REG_POP(NAME, T) \
            ITP_REG_POP_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                if (getenv("J8_VMTRACE")) fprintf(stderr, "POP_%s R%u (sp=%u)\n", #NAME, u32 & 0x0F, (unsigned)ptr->stackPtr); \
                ptr->Regs[u32 & 0x0F] = popT<T>(ptr->Stack, ptr->stackPtr); \
                NEXT(2); \
            }
        #define DEF_REG_LOAD(NAME, T) \
            ITP_REG_LOAD_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                ptr->Regs[u32 & 0x0F] = loadRaw(tptr, sizeof(T)); \
                NEXT(11); /* 1 opcode + 1 reg + 1 mode + 8 off */ \
            }
        #define DEF_REG_STORE(NAME, T) \
            ITP_REG_STORE_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                storeRaw(tptr, ptr->Regs[u32 & 0x0F], sizeof(T)); \
                NEXT(11); \
            }
        // 立即数压栈：MOVI_U32(imm:u32) —— 大端序读 4 字节立即数压栈
        #define DEF_MOVI(NAME, T) \
            ITP_MOVI_##NAME: { \
                pushT<T>(ptr->Stack, ptr->stackPtr, rdBE<T>(&MainBY.byteCode[ptr->count + 1])); \
                NEXT(5); /* 1 字节 opcode + 4 字节立即数 */ \
            }
        #define DEF_ARITH_INT(NAME, T, OP) \
            ITP_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                using U = std::make_unsigned_t<T>; \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(static_cast<U>(a) OP static_cast<U>(b))); \
                NEXT(1); \
            }
        #define DEF_DIV_INT(NAME, T) \
            ITP_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                T r; \
                if (b == 0) { \
                    r = 0; \
                } else if constexpr (std::is_signed_v<T>) { \
                    if (a == std::numeric_limits<T>::min() && b == static_cast<T>(-1)) r = a; /* INT_MIN/-1 回绕 */ \
                    else r = static_cast<T>(a / b); \
                } else { \
                    r = static_cast<T>(a / b); \
                } \
                pushT<T>(ptr->Stack, ptr->stackPtr, r); \
                NEXT(1); \
            }
        #define DEF_ARITH_FLOAT(NAME, T, OP) \
            ITP_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(a OP b)); \
                NEXT(1); \
            }
        #define DEF_DIV_FLOAT(NAME, T) \
            ITP_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(a / b)); \
                NEXT(1); \
            }
        #define DEF_SQRT(NAME, T) \
            ITP_SQRT_##NAME: { \
                T v = popT<T>(ptr->Stack, ptr->stackPtr); \
                pushT<double>(ptr->Stack, ptr->stackPtr, std::sqrt(static_cast<double>(v))); \
                NEXT(1); \
            }
        #define DEF_LOG(NAME, T) \
            ITP_LOG_##NAME: { \
                T v = popT<T>(ptr->Stack, ptr->stackPtr); \
                pushT<double>(ptr->Stack, ptr->stackPtr, std::log(static_cast<double>(v))); \
                NEXT(1); \
            }
        #define DEF_COUT(NAME, T) \
            ITP_COUT_##NAME: { \
                coutVal(popT<T>(ptr->Stack, ptr->stackPtr)); /* 重载按编译期类型 T 决议 */ \
                NEXT(1); \
            }
        // 大小比较：弹 b、a，压 U8 的 0/1（<=、>=、!= 由 & | ~ 与 < = > 组合）
        #define DEF_CMP(NAME, T, OP) \
            ITP_CMP_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                pushT<uint8_t>(ptr->Stack, ptr->stackPtr, (a OP b) ? 1 : 0); \
                NEXT(1); \
            }
        // 左移（只 uint）：移位量按宽度取模，宽类型运算后截断，无 UB
        #define DEF_SHL(NAME, T) \
            ITP_SHL_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                const uint32_t bits = static_cast<uint32_t>(b) & (8 * sizeof(T) - 1); \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(static_cast<uint64_t>(a) << bits)); \
                NEXT(1); \
            }
        // 右移：uint 逻辑右移 / int 算术右移（C++20 起有符号右移即算术移位）
        #define DEF_SHR_U(NAME, T) \
            ITP_SHR_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                const uint32_t bits = static_cast<uint32_t>(b) & (8 * sizeof(T) - 1); \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(static_cast<uint64_t>(a) >> bits)); \
                NEXT(1); \
            }
        #define DEF_SHR_I(NAME, T) \
            ITP_SHR_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                const uint32_t bits = static_cast<uint32_t>(b) & (8 * sizeof(T) - 1); \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(static_cast<int64_t>(a) >> bits)); \
                NEXT(1); \
            }
        // 按位与 / 或（只 uint）
        #define DEF_BITWISE(NAME, T, OP) \
            ITP_##NAME: { \
                T b = popT<T>(ptr->Stack, ptr->stackPtr); \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                using U = std::make_unsigned_t<T>; \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(static_cast<uint64_t>(static_cast<U>(a)) OP static_cast<uint64_t>(static_cast<U>(b)))); \
                NEXT(1); \
            }
        // 按位取反（只 uint）
        #define DEF_NOT(NAME, T) \
            ITP_NOT_##NAME: { \
                T a = popT<T>(ptr->Stack, ptr->stackPtr); \
                using U = std::make_unsigned_t<T>; \
                pushT<T>(ptr->Stack, ptr->stackPtr, static_cast<T>(~static_cast<uint64_t>(static_cast<U>(a)))); \
                NEXT(1); \
            }
        // 类型转换：浮点 ↔ 整数（只 32 位；越界/负数转整数的行为由平台转换指令决定）
        #define DEF_CVT_F2I(NAME, FT, IT) \
            ITP_CVT_##NAME: { \
                pushT<IT>(ptr->Stack, ptr->stackPtr, static_cast<IT>(popT<FT>(ptr->Stack, ptr->stackPtr))); \
                NEXT(1); \
            }
        #define DEF_CVT_I2F(NAME, IT, FT) \
            ITP_CVT_##NAME: { \
                pushT<FT>(ptr->Stack, ptr->stackPtr, static_cast<FT>(popT<IT>(ptr->Stack, ptr->stackPtr))); \
                NEXT(1); \
            }

        // REG 寄存器指令族（全宽度展开，处理器代码最靠前）
        DEF_REG_MOVI(U8, uint8_t) DEF_REG_MOVI(U16, uint16_t)
        DEF_REG_MOVI(U32, uint32_t) DEF_REG_MOVI(U64, uint64_t)
        DEF_REG_MOV
        DEF_REG_PUSH(U8, uint8_t) DEF_REG_PUSH(U16, uint16_t)
        DEF_REG_PUSH(U32, uint32_t) DEF_REG_PUSH(U64, uint64_t)
        DEF_REG_POP(U8, uint8_t) DEF_REG_POP(U16, uint16_t)
        DEF_REG_POP(U32, uint32_t) DEF_REG_POP(U64, uint64_t)
        DEF_REG_LOAD(U8, uint8_t) DEF_REG_LOAD(U16, uint16_t)
        DEF_REG_LOAD(U32, uint32_t)
        ITP_REG_LOAD_U64: {
            u32 = MainBY.byteCode[ptr->count + 1];
            mode = MainBY.byteCode[ptr->count + 2];
            u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]);
            tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64))
                               : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8));
            if (getenv("J8_VMTRACE")) {
                int64_t d = tptr - ptr->Stack;
                fprintf(stderr, "LOAD64 mode=%d rel=%lld val=%llx sp=%u\n", (int)mode, (long long)d,
                        (unsigned long long)loadRaw(tptr, 8), (unsigned)ptr->stackPtr);
            }
            ptr->Regs[u32 & 0x0F] = loadRaw(tptr, 8);
            NEXT(11);
        }
        DEF_REG_STORE(U8, uint8_t) DEF_REG_STORE(U16, uint16_t)
        DEF_REG_STORE(U32, uint32_t)
        ITP_REG_STORE_U64: {
            u32 = MainBY.byteCode[ptr->count + 1];
            mode = MainBY.byteCode[ptr->count + 2];
            u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]);
            tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64))
                               : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8));
            if (getenv("J8_VMTRACE")) {
                int64_t d = tptr - ptr->Stack;
                fprintf(stderr, "STORE64 mode=%d rel=%lld val=%llx\n", (int)mode, (long long)d,
                        (unsigned long long)ptr->Regs[u32 & 0x0F]);
            }
            storeRaw(tptr, ptr->Regs[u32 & 0x0F], 8);
            NEXT(11);
        }
        // 立即数压栈
        DEF_MOVI(U32, uint32_t)
        // ADD/SUB 只保留 uint + float
        DEF_ARITH_INT(ADD_U8, uint8_t, +) DEF_ARITH_INT(ADD_U16, uint16_t, +)
        ITP_ADD_U32: {
            uint32_t b = popT<uint32_t>(ptr->Stack, ptr->stackPtr);
            uint32_t a = popT<uint32_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "ADD32 a=%u b=%u\n", a, b);
            pushT<uint32_t>(ptr->Stack, ptr->stackPtr, a + b);
            NEXT(1);
        }
        ITP_ADD_U64: {
            uint64_t b = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
            uint64_t a = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "ADD64 a=%llu b=%llu\n", (unsigned long long)a, (unsigned long long)b);
            pushT<uint64_t>(ptr->Stack, ptr->stackPtr, a + b);
            NEXT(1);
        }
        DEF_ARITH_INT(SUB_U8, uint8_t, -) DEF_ARITH_INT(SUB_U16, uint16_t, -)
        DEF_ARITH_INT(SUB_U32, uint32_t, -)
        ITP_SUB_U64: {
            uint64_t b = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
            uint64_t a = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "SUB64 a=%llu b=%llu\n", (unsigned long long)a, (unsigned long long)b);
            pushT<uint64_t>(ptr->Stack, ptr->stackPtr, a - b);
            NEXT(1);
        }
        DEF_ARITH_FLOAT(ADD_F32, float, +) DEF_ARITH_FLOAT(ADD_F64, double, +)
        DEF_ARITH_FLOAT(SUB_F32, float, -)
        ITP_SUB_F64: {
            double b = popT<double>(ptr->Stack, ptr->stackPtr);
            double a = popT<double>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "SUB_F64 a=%g b=%g -> %g\n", a, b, a - b);
            pushT<double>(ptr->Stack, ptr->stackPtr, a - b);
            NEXT(1);
        }
        // MUL/DIV 全类型
        DEF_ARITH_INT(MUL_U8, uint8_t, *) DEF_ARITH_INT(MUL_I8, int8_t, *)
        DEF_ARITH_INT(MUL_U16, uint16_t, *) DEF_ARITH_INT(MUL_I16, int16_t, *)
        DEF_ARITH_INT(MUL_U32, uint32_t, *) DEF_ARITH_INT(MUL_I32, int32_t, *)
        DEF_ARITH_INT(MUL_U64, uint64_t, *) DEF_ARITH_INT(MUL_I64, int64_t, *)
        DEF_ARITH_FLOAT(MUL_F32, float, *)
        ITP_MUL_F64: {
            double b = popT<double>(ptr->Stack, ptr->stackPtr);
            double a = popT<double>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "MUL_F64 a=%g b=%g -> %g\n", a, b, a * b);
            pushT<double>(ptr->Stack, ptr->stackPtr, a * b);
            NEXT(1);
        }
        DEF_DIV_INT(DIV_U8, uint8_t) DEF_DIV_INT(DIV_I8, int8_t)
        DEF_DIV_INT(DIV_U16, uint16_t) DEF_DIV_INT(DIV_I16, int16_t)
        ITP_DIV_U32: {
            uint32_t b = popT<uint32_t>(ptr->Stack, ptr->stackPtr);
            uint32_t a = popT<uint32_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "DIV32 a=%u b=%u\n", a, b);
            pushT<uint32_t>(ptr->Stack, ptr->stackPtr, b == 0 ? 0 : a / b);
            NEXT(1);
        }
        DEF_DIV_INT(DIV_I32, int32_t)
        ITP_DIV_U64: {
            uint64_t b = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
            uint64_t a = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "DIV64 a=%llu b=%llu\n", (unsigned long long)a, (unsigned long long)b);
            pushT<uint64_t>(ptr->Stack, ptr->stackPtr, b == 0 ? 0 : a / b);
            NEXT(1);
        }
        DEF_DIV_INT(DIV_I64, int64_t)
        DEF_DIV_FLOAT(DIV_F32, float) DEF_DIV_FLOAT(DIV_F64, double)
        // SQRT / LOG 全类型
        DEF_SQRT(U8, uint8_t) DEF_SQRT(I8, int8_t)
        DEF_SQRT(U16, uint16_t) DEF_SQRT(I16, int16_t)
        DEF_SQRT(U32, uint32_t) DEF_SQRT(I32, int32_t)
        DEF_SQRT(U64, uint64_t) DEF_SQRT(I64, int64_t)
        DEF_SQRT(F32, float) DEF_SQRT(F64, double)
        DEF_LOG(U8, uint8_t) DEF_LOG(I8, int8_t)
        DEF_LOG(U16, uint16_t) DEF_LOG(I16, int16_t)
        DEF_LOG(U32, uint32_t) DEF_LOG(I32, int32_t)
        DEF_LOG(U64, uint64_t) DEF_LOG(I64, int64_t)
        DEF_LOG(F32, float) DEF_LOG(F64, double)
        // COUT 只保留 char
        ITP_COUT_CHAR8: {
            uint8_t v = popT<uint8_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "COUT8 val=%u ch=%c\n", (unsigned)v, v >= 32 && v < 127 ? (char)v : '.');
            coutVal(v);
            NEXT(1);
        }
        ITP_COUT_CHAR16: {
            uint16_t v = popT<uint16_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "COUT16 val=%u\n", (unsigned)v);
            coutVal(v);
            NEXT(1);
        }
        ITP_COUT_CHAR32: {
            uint32_t v = popT<uint32_t>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "COUT32 val=%u\n", (unsigned)v);
            coutVal(v);
            NEXT(1);
        }
        // 大小比较（< = >，除 char 外全部类型）
        DEF_CMP(LT_U8, uint8_t, <) DEF_CMP(LT_I8, int8_t, <)
        DEF_CMP(LT_U16, uint16_t, <) DEF_CMP(LT_I16, int16_t, <)
        DEF_CMP(LT_U32, uint32_t, <) DEF_CMP(LT_I32, int32_t, <)
        DEF_CMP(LT_U64, uint64_t, <) DEF_CMP(LT_I64, int64_t, <)
        DEF_CMP(LT_F32, float, <) DEF_CMP(LT_F64, double, <) DEF_CMP(LT_PTR, uint64_t, <)
        DEF_CMP(EQ_U8, uint8_t, ==) DEF_CMP(EQ_I8, int8_t, ==)
        DEF_CMP(EQ_U16, uint16_t, ==) DEF_CMP(EQ_I16, int16_t, ==)
        DEF_CMP(EQ_U32, uint32_t, ==) DEF_CMP(EQ_I32, int32_t, ==)
        DEF_CMP(EQ_U64, uint64_t, ==) DEF_CMP(EQ_I64, int64_t, ==)
        DEF_CMP(EQ_F32, float, ==) DEF_CMP(EQ_F64, double, ==) DEF_CMP(EQ_PTR, uint64_t, ==)
        DEF_CMP(GT_U8, uint8_t, >) DEF_CMP(GT_I8, int8_t, >)
        DEF_CMP(GT_U16, uint16_t, >) DEF_CMP(GT_I16, int16_t, >)
        DEF_CMP(GT_U32, uint32_t, >) DEF_CMP(GT_I32, int32_t, >)
        DEF_CMP(GT_U64, uint64_t, >) DEF_CMP(GT_I64, int64_t, >)
        DEF_CMP(GT_F32, float, >) DEF_CMP(GT_F64, double, >) DEF_CMP(GT_PTR, uint64_t, >)
        // 位运算（<< & | ~ 只给 uint；>> 给 uint 和 int 各一套）
        DEF_SHL(U8, uint8_t) DEF_SHL(U16, uint16_t) DEF_SHL(U32, uint32_t) DEF_SHL(U64, uint64_t)
        DEF_BITWISE(AND_U8, uint8_t, &) DEF_BITWISE(AND_U16, uint16_t, &)
        DEF_BITWISE(AND_U32, uint32_t, &) DEF_BITWISE(AND_U64, uint64_t, &)
        DEF_BITWISE(OR_U8, uint8_t, |) DEF_BITWISE(OR_U16, uint16_t, |)
        DEF_BITWISE(OR_U32, uint32_t, |) DEF_BITWISE(OR_U64, uint64_t, |)
        DEF_NOT(U8, uint8_t) DEF_NOT(U16, uint16_t) DEF_NOT(U32, uint32_t) DEF_NOT(U64, uint64_t)
        DEF_SHR_U(U8, uint8_t) DEF_SHR_U(U16, uint16_t) DEF_SHR_U(U32, uint32_t) DEF_SHR_U(U64, uint64_t)
        DEF_SHR_I(I8, int8_t) DEF_SHR_I(I16, int16_t) DEF_SHR_I(I32, int32_t) DEF_SHR_I(I64, int64_t)
        // 类型转换（浮点 ↔ 整数，只 32 位）
        DEF_CVT_F2I(F32_U32, float, uint32_t) DEF_CVT_F2I(F32_I32, float, int32_t)
        ITP_CVT_F64_U32: {
            double a = popT<double>(ptr->Stack, ptr->stackPtr);
            if (getenv("J8_VMTRACE")) fprintf(stderr, "CVT_F64_U32 in=%g out=%u\n", a, (unsigned)(uint32_t)a);
            pushT<uint32_t>(ptr->Stack, ptr->stackPtr, static_cast<uint32_t>(a));
            NEXT(1);
        }
        DEF_CVT_F2I(F64_I32, double, int32_t)
        DEF_CVT_I2F(U32_F32, uint32_t, float) DEF_CVT_I2F(U32_F64, uint32_t, double)
        DEF_CVT_I2F(I32_F32, int32_t, float) DEF_CVT_I2F(I32_F64, int32_t, double)

        #undef DEF_ARITH_INT
        #undef DEF_DIV_INT
        #undef DEF_ARITH_FLOAT
        #undef DEF_DIV_FLOAT
        #undef DEF_SQRT
        #undef DEF_LOG
        #undef DEF_COUT
        #undef DEF_CMP
        #undef DEF_SHL
        #undef DEF_SHR_U
        #undef DEF_SHR_I
        #undef DEF_BITWISE
        #undef DEF_NOT
        #undef DEF_CVT_F2I
        #undef DEF_CVT_I2F
        #undef DEF_MOVI
        #undef DEF_REG_MOVI
        #undef DEF_REG_MOV
        #undef DEF_REG_PUSH
        #undef DEF_REG_POP
        #undef DEF_REG_LOAD
        #undef DEF_REG_STORE
        #undef SET_ADD
        #undef SET_SUB
        #undef SET_MUL
        #undef SET_DIV
        #undef SET_SQRT
        #undef SET_LOG
        #undef SET_COUT
        #undef SET_CMP
        #undef SET_SHL
        #undef SET_AND
        #undef SET_OR
        #undef SET_NOT
        #undef SET_SHR
        #undef SET_CVT
        #undef SET_MOVI
        #undef SET_REG_MOVI
        #undef SET_REG_MOV
        #undef SET_REG_PUSH
        #undef SET_REG_POP
        #undef SET_REG_LOAD
        #undef SET_REG_STORE
        #undef SET_ATOMIC_LOAD
        #undef SET_ATOMIC_STORE
        #undef SET_ATOMIC_XCHG
        #undef SET_ATOMIC_CAS
        #undef SET_ATOMIC_ADD

        //============== 寻址 / 分配（无类型） ==============
        ITP_LEA: // 寻址：mode0 压入栈内地址；mode1 压入栈槽里存的指针
        mode = MainBY.byteCode[ptr->count + 1];
        u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 2]);
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr,
            (mode == 0) ? reinterpret_cast<uint64_t>(ptr->Stack + (SCOPE_BASE + u64))
                        : loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8));
        NEXT(10);
        ITP_ADD_PTR: // ptr 只能寻址：指针加偏移
        u64 = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
        u64 += popT<uint64_t>(ptr->Stack, ptr->stackPtr);
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr, u64);
        NEXT(1);
        ITP_SUB_PTR:
        u64 = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
        u64 = popT<uint64_t>(ptr->Stack, ptr->stackPtr) - u64;
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr, u64);
        NEXT(1);

        //============== uint8_t[] 堆分配（只认 size 和 where，无类型） ==============
        ITP_NEW_ARRAY: // NEW_ARRAY(size:u64, slotOff:u32)：指针写到 Stack[base+slotOff]
        u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 1]);
        u32 = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 9]);
        storeRaw(ptr->Stack + (SCOPE_BASE + u32), reinterpret_cast<uint64_t>(new uint8_t[u64]), 8);
        NEXT(13);
        ITP_FREE_ARRAY: // FREE_ARRAY(slotOff:u32)：从 Stack[base+slotOff] 读指针并释放
        u32 = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
        delete[] reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u32), 8));
        NEXT(5);

        //============== 系统地址获取 / 间接跳转 / 原子变量（多线程） ==============
        ITP_GET_ADDRS: // 一条指令压入四个地址：bytecode 缓冲区、size 字段、DataSave、管理器
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr, reinterpret_cast<uint64_t>(MainBY.byteCode.get()));
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr, reinterpret_cast<uint64_t>(&MainBY.size));
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr, reinterpret_cast<uint64_t>(ptr));
        pushT<uint64_t>(ptr->Stack, ptr->stackPtr, reinterpret_cast<uint64_t>(ptr->manager.get()));
        NEXT(1);
        ITP_JMP_IND: // 间接跳转：弹栈 u64 绝对地址 → 换算字节码偏移 → 跳转
        u64 = popT<uint64_t>(ptr->Stack, ptr->stackPtr);
        if (getenv("J8_VMTRACE")) fprintf(stderr, "JMP_IND addr=%llx -> off=%zu\n", (unsigned long long)u64,
            (size_t)(static_cast<size_t>(u64) - reinterpret_cast<size_t>(MainBY.byteCode.get())));
        ptr->count = static_cast<size_t>(u64) - reinterpret_cast<size_t>(MainBY.byteCode.get());
        ptr->commandCode = MainBY.byteCode[ptr->count];
        INTERPRETED;

        //============== 原子变量（多线程；目标地址需 4/8 字节自然对齐） ==============
        // 编码统一：opcode + reg(1) + mode(1) + off:u64(8) = 11 字节
        // mode0：原子操作 Stack[base+off]；mode1：先解引用栈槽里的指针再原子操作
        // 语义：LOAD reg=原子读；STORE 原子写 reg 低 W 位；XCHG reg↔内存交换，reg=旧值；
        //       CAS 弹栈 expected，相等则内存=reg；reg=实际旧值，压 U8 成功标志；
        //       ADD fetch_add，reg=旧值（加数取 reg 原值）
        #define DEF_ATOMIC_LOAD(NAME, T) \
            ITP_ATOMIC_LOAD_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                ptr->Regs[u32 & 0x0F] = std::atomic_ref<T>(*reinterpret_cast<T*>(tptr)).load(std::memory_order_seq_cst); \
                NEXT(11); \
            }
        #define DEF_ATOMIC_STORE(NAME, T) \
            ITP_ATOMIC_STORE_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                std::atomic_ref<T>(*reinterpret_cast<T*>(tptr)).store(static_cast<T>(ptr->Regs[u32 & 0x0F]), std::memory_order_seq_cst); \
                NEXT(11); \
            }
        #define DEF_ATOMIC_XCHG(NAME, T) \
            ITP_ATOMIC_XCHG_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                ptr->Regs[u32 & 0x0F] = std::atomic_ref<T>(*reinterpret_cast<T*>(tptr)).exchange(static_cast<T>(ptr->Regs[u32 & 0x0F]), std::memory_order_seq_cst); \
                NEXT(11); \
            }
        #define DEF_ATOMIC_CAS(NAME, T) \
            ITP_ATOMIC_CAS_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                T expected = popT<T>(ptr->Stack, ptr->stackPtr); \
                const T desired = static_cast<T>(ptr->Regs[u32 & 0x0F]); \
                auto ref = std::atomic_ref<T>(*reinterpret_cast<T*>(tptr)); \
                const bool ok = ref.compare_exchange_strong(expected, desired, std::memory_order_seq_cst, std::memory_order_seq_cst); \
                ptr->Regs[u32 & 0x0F] = expected; /* 实际旧值（失败时=当前值） */ \
                pushT<uint8_t>(ptr->Stack, ptr->stackPtr, ok ? 1 : 0); \
                NEXT(11); \
            }
        #define DEF_ATOMIC_ADD(NAME, T) \
            ITP_ATOMIC_ADD_##NAME: { \
                u32 = MainBY.byteCode[ptr->count + 1]; \
                mode = MainBY.byteCode[ptr->count + 2]; \
                u64 = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 3]); \
                tptr = (mode == 0) ? (ptr->Stack + (SCOPE_BASE + u64)) \
                                   : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + u64), 8)); \
                ptr->Regs[u32 & 0x0F] = std::atomic_ref<T>(*reinterpret_cast<T*>(tptr)).fetch_add(static_cast<T>(ptr->Regs[u32 & 0x0F]), std::memory_order_seq_cst); \
                NEXT(11); \
            }
        // 原子变量实例化（U32/U64，共 10 条）
        DEF_ATOMIC_LOAD(U32, uint32_t) DEF_ATOMIC_LOAD(U64, uint64_t)
        DEF_ATOMIC_STORE(U32, uint32_t) DEF_ATOMIC_STORE(U64, uint64_t)
        DEF_ATOMIC_XCHG(U32, uint32_t) DEF_ATOMIC_XCHG(U64, uint64_t)
        DEF_ATOMIC_CAS(U32, uint32_t) DEF_ATOMIC_CAS(U64, uint64_t)
        DEF_ATOMIC_ADD(U32, uint32_t) DEF_ATOMIC_ADD(U64, uint64_t)
        #undef DEF_ATOMIC_LOAD
        #undef DEF_ATOMIC_STORE
        #undef DEF_ATOMIC_XCHG
        #undef DEF_ATOMIC_CAS
        #undef DEF_ATOMIC_ADD

        //============== 外部函数调用（libffi 原样暴露，薄透传） ==============
        // EXTERN_CALL(idx:u8, argBaseOff:u32, retOff:u32)：从全局 externFn 表取
        // 宿主用 ffi_prep_cif 预生成的 {ffi_cif, 函数指针}，把 Stack[base+argBaseOff]
        // 起的参数缓冲按 arg_types 顺序拆成 avalue[]（先 8 位再 16 位…），
        // 原样调用 ffi_call（libffi 原始 API，一字未改），返回值写 Stack[base+retOff]。
        // 这里只维护一张"已注册函数"的表，不做任何封装/重实现。
        ITP_EXTERN_CALL:
        u32 = MainBY.byteCode[ptr->count + 1]; // 表下标（1 字节，类似 commandCode）
        if (externFn[u32 & 0xFF].cif == nullptr) goto ITP_ERR_END; // 未注册 → 越界退出
        {
            const uint32_t argOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 2]);
            const uint32_t retOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 6]);
            const ffi_cif* cif = externFn[u32 & 0xFF].cif;
            void* avalue[64]; // 参数指针表（上限 64，信任用户预生成的 cif）
            uint8_t* ap = ptr->Stack + (SCOPE_BASE + argOff);
            for (unsigned i = 0; i < cif->nargs; ++i) {
                avalue[i] = ap; // 按顺序：先 8 位、再 16 位…
                ap += cif->arg_types[i]->size;
            }
            ffi_call(const_cast<ffi_cif*>(cif),
                     reinterpret_cast<void (*)(void)>(externFn[u32 & 0xFF].fn),
                     ptr->Stack + (SCOPE_BASE + retOff), avalue);
        }
        NEXT(10);

        //============== VM 函数调用（FunctionSave：save 类对象，可动态加载） ==============
        // FUNC_CALL(fnPtrOff:u32, argBaseOff:u32, retOff:u32)：从栈槽 fnPtrOff 读
        // FunctionSave*（真的指向 save 类对象的指针），以 Stack[base+argBaseOff] 为参数
        // 基址、Stack[base+retOff] 为返回值地址调用。函数用自己的 DataSave 执行，
        // 栈每次调用全新分配/释放（函数字节码以 STACK_INIT 开头、END 收尾），
        // silent=1 抑制"正常退出"打印。正常路径零开销：此处理器只在 OP_FUNC_CALL 命中。
        ITP_FUNC_CALL:
        {
            const uint32_t fnOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
            const uint32_t argOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 5]);
            const uint32_t retOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 9]);
            FunctionSave* fn = reinterpret_cast<FunctionSave*>(loadRaw(ptr->Stack + (SCOPE_BASE + fnOff), 8));
            if (fn == nullptr) goto ITP_ERR_END; // 空函数指针 → 越界退出
            callFunctionSave(fn, ptr->Stack + (SCOPE_BASE + argOff), ptr->Stack + (SCOPE_BASE + retOff));
        }
        NEXT(13);

        //============== LLVM JIT（JIT_SUBMIT）：save 指针 → 原生函数指针 ==============
        // JIT_SUBMIT(fnPtrOff:u32, retOff:u32) = 9 字节：从栈槽 fnPtrOff 读 save*，
        // 交给 JadeightJIT::submit 即时编译（同地址缓存复用，编译一次），返回的函数
        // 指针（调用约定 fn(argPtr,retPtr,manager)）写回栈槽 retOff（8 字节）。
        // 无 LLVM 时 submit 返回 nullptr，客人程序可据此判断。空 save* → 越界退出。
        ITP_JIT_SUBMIT:
        {
            const uint32_t fnOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 1]);
            const uint32_t retOff = rdBE<uint32_t>(&MainBY.byteCode[ptr->count + 5]);
            save* s = reinterpret_cast<save*>(loadRaw(ptr->Stack + (SCOPE_BASE + fnOff), 8));
            if (s == nullptr) goto ITP_ERR_END; // 空 save 指针 → 越界退出
            storeRaw(ptr->Stack + (SCOPE_BASE + retOff),
                     reinterpret_cast<uint64_t>(jitSubmitSave(*s)), 8);
        }
        NEXT(9);

        //============== 内存 bulk 拷贝（MEMCPY） ==============
        // MEMCPY(dstMode:u8, dstOff:u64, srcMode:u8, srcOff:u64, size:u64) = 27 字节
        // mode 与 REG_LOAD/STORE 一致：mode0 = Stack[base+off] 本身；
        // mode1 = 先解引用栈槽里的指针再操作（可拷堆/管理器等任意内存）。
        // 语义同 C memcpy（重叠未定义）；bulk 拷贝用 std::memcpy 一次完成最快。
        ITP_MEMCPY:
        {
            const uint8_t dm = MainBY.byteCode[ptr->count + 1];
            const uint64_t dOff = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 2]);
            const uint8_t sm = MainBY.byteCode[ptr->count + 10];
            const uint64_t sOff = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 11]);
            const uint64_t sz = rdBE<uint64_t>(&MainBY.byteCode[ptr->count + 19]);
            uint8_t* dst = (dm == 0) ? (ptr->Stack + (SCOPE_BASE + dOff))
                                     : reinterpret_cast<uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + dOff), 8));
            const uint8_t* src = (sm == 0) ? (ptr->Stack + (SCOPE_BASE + sOff))
                                           : reinterpret_cast<const uint8_t*>(loadRaw(ptr->Stack + (SCOPE_BASE + sOff), 8));
            std::memcpy(dst, src, static_cast<size_t>(sz));
        }
        NEXT(27);

        #undef INTERPRETED
        #undef SCOPE_BASE
        #undef NEXT
    }
};

//==================== 多线程执行器（harness） =====================
// 每个线程一份独立的 DataSave/executoring，跑同一份只读字节码；
// 所有线程共享同一个 Manager（共有指针），靠 GET_ADDRS（拿管理器地址）
// + ATOMIC_*（原子变量）跨线程同步。runThreads 后必须 join() 再销毁本对象。
class executoringHarness {
public:
    std::shared_ptr<Manager> manager; // 管理器共有指针：所有线程共享同一实例
    std::vector<DataSave> states;     // 每线程一份 VM 状态（各自独立的 Stack/寄存器）
    std::vector<executoring> engines; // 每线程一份解释器
    std::vector<std::thread> threads; // 线程句柄

    explicit executoringHarness(std::shared_ptr<Manager> m = nullptr)
        : manager(m ? std::move(m) : std::make_shared<Manager>()) {}

    // 启动 n 个线程跑同一份字节码（字节码必须比所有线程活得久）
    void runThreads(const save& prog, size_t n) {
        states.resize(n);
        engines.resize(n);
        for (size_t i = 0; i < n; ++i) {
            states[i].manager = manager;            // 共享同一个管理器
            engines[i].ptr = &states[i];
            threads.emplace_back([this, &prog, i]() { engines[i].F8BFLRead(prog); });
        }
    }

    void join() {
        for (auto& t : threads) if (t.joinable()) t.join();
        threads.clear();
    }
};

//==================== FunctionSave：save 类函数对象（save 一行未改） =====================
// 一个可被 FUNC_CALL 调用的"VM 函数"：内部持有一个 save（字节码，原样复用）+ 独立的
// DataSave/executoring（自己的 Stack/寄存器），入口、参数总字节、返回字节由加载时给定。
// 动态加载：loadFromFile 从文件装载（文件头 [argSize:u32][retSize:u32][entry:u32] LE +
// 函数字节码）；也可从内存构造。save 类代码零改动，架构零改动。
// 调用约定（FUNC_CALL 预置）：R15=参数基址指针，R14=返回值地址指针；
// 函数字节码以 STACK_INIT 开头（自建栈）、END 收尾（silent=1 抑制退出打印），
// 每次调用栈全新分配/释放，无泄漏、无残留状态。
class FunctionSave {
public:
    save bytecode;      // 函数字节码（save 类原样复用）
    DataSave state;     // 函数执行状态（独立 Stack/寄存器）
    executoring engine; // 解释器（ptr 恒指向自己的 state）
    uint32_t entry = 0;   // 入口偏移
    uint32_t argSize = 0; // 参数缓冲总字节数（约定，加载时给定）
    uint32_t retSize = 0; // 返回值字节数（约定，加载时给定）

    // 默认构造 = 空函数（仅 END），保证未加载的 FunctionSave 也可安全执行
    FunctionSave() : FunctionSave(kEndOnly, 1, 0, 0, 0) {}
    // 从内存动态加载
    FunctionSave(const uint8_t* bc, size_t size, uint32_t entry_, uint32_t argSize_, uint32_t retSize_)
        : bytecode(bc, size), entry(entry_), argSize(argSize_), retSize(retSize_) {
        engine.ptr = &state;
    }
    FunctionSave(const FunctionSave&) = delete;
    FunctionSave& operator=(const FunctionSave&) = delete;
    FunctionSave(FunctionSave&& o) noexcept
        : bytecode(std::move(o.bytecode)), state(std::move(o.state)), engine(o.engine),
          entry(o.entry), argSize(o.argSize), retSize(o.retSize) {
        engine.ptr = &state; // 移动后重新指向自己的 state
    }
    FunctionSave& operator=(FunctionSave&& o) noexcept {
        if (this != &o) {
            bytecode = std::move(o.bytecode);
            state = std::move(o.state);
            engine = o.engine;
            entry = o.entry; argSize = o.argSize; retSize = o.retSize;
            engine.ptr = &state;
        }
        return *this;
    }

    // 从文件动态加载：文件头 [argSize:u32][retSize:u32][entry:u32]（小端）+ 函数字节码
    static FunctionSave loadFromFile(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) return FunctionSave();
        uint32_t hdr[3] = {0, 0, 0};
        if (fread(hdr, 4, 3, f) != 3) { fclose(f); return FunctionSave(); }
        fseek(f, 0, SEEK_END);
        const long sz = ftell(f) - 12;
        std::vector<uint8_t> buf(sz > 0 ? static_cast<size_t>(sz) : 0);
        fseek(f, 12, SEEK_SET);
        if (sz > 0) fread(buf.data(), 1, static_cast<size_t>(sz), f);
        fclose(f);
        return FunctionSave(buf.data(), buf.size(), hdr[2], hdr[0], hdr[1]);
    }
    // 保存为文件（配合动态加载演示）
    void saveToFile(const char* path) const {
        FILE* f = fopen(path, "wb");
        if (!f) return;
        const uint32_t hdr[3] = { argSize, retSize, entry };
        fwrite(hdr, 4, 3, f);
        fwrite(bytecode.byteCode.get(), 1, bytecode.size, f);
        fclose(f);
    }

private:
    static const uint8_t kEndOnly[1];
};
const uint8_t FunctionSave::kEndOnly[1] = { OP_END };

// FUNC_CALL 的处理器实现（FunctionSave 完整类型在此可见）：
// 预置 R15=参数指针、R14=返回指针，重置函数状态后跑函数自己的字节码。
// 只被 ITP_FUNC_CALL 调用 —— 正常执行路径不经过这里，零开销。
static void callFunctionSave(FunctionSave* fn, uint8_t* argPtr, uint8_t* retPtr) {
    DataSave& fs = fn->state;
    fs.Regs[15] = reinterpret_cast<uint64_t>(argPtr); // 参数基址
    fs.Regs[14] = reinterpret_cast<uint64_t>(retPtr); // 返回值地址
    fs.stackPtr = 0;
    fs.zuoYongYv = 1;
    fs.count = fn->entry;
    fs.end = 0;
    fs.silent = 1; // 函数执行：END/出错不打印退出信息
    fn->engine.F8BFLRead(fn->bytecode);
}

#ifdef JADEIGHT_HAS_LLVM
//==================== LLVM JIT（v2.12 新增）：save 指针 → 原生函数指针 =====================
// 提交一个 save（字节码）→ 返回 LLVM JIT 编译后的原生函数指针（同 save 地址缓存复用）。
// 调用约定与解释器 FUNC_CALL 完全一致：
//     void fn(uint8_t* argPtr, uint8_t* retPtr, void* manager)
//   argPtr = 参数缓冲基址（等价 R15）、retPtr = 返回值缓冲（等价 R14）、
//   manager = 共享管理器（字节码含 GET_ADDRS 时由宿主传入，否则可传 nullptr）。
// 逐条 opcode 翻译成 LLVM IR（PassBuilder O2 优化 + MCJIT 落原生码）；EXTERN_CALL /
// FUNC_CALL / 原子变量 / COUT / 堆分配等走 addGlobalMapping 绑定的运行期辅助，
// 行为与解释器逐字节一致。约束：返回的函数指针只在该 save 存活期间有效
// （IR 里捕获了字节码基址）。

//===== 运行期辅助（经 ExecutionEngine::addGlobalMapping 按名绑定，无需导出符号） =====
// EXTERN_CALL：语义与 ITP_EXTERN_CALL 逐字节一致（argPtr 已含作用域基址）
static int jitExternCall(uint8_t idx, uint8_t* argPtr, uint8_t* retPtr) {
    if (externFn[idx & 0xFF].cif == nullptr) return 1; // 未注册 → 越界退出
    const ffi_cif* cif = externFn[idx & 0xFF].cif;
    void* avalue[64];
    uint8_t* ap = argPtr;
    for (unsigned i = 0; i < cif->nargs; ++i) { avalue[i] = ap; ap += cif->arg_types[i]->size; }
    ffi_call(const_cast<ffi_cif*>(cif),
             reinterpret_cast<void (*)(void)>(externFn[idx & 0xFF].fn), retPtr, avalue);
    return 0;
}
static void jitCoutU8(uint8_t v)   { coutVal(v); }
static void jitCoutU16(uint16_t v) { coutVal(v); }
static void jitCoutU32(uint32_t v) { coutVal(v); }
static void jitMemcpy(uint8_t* d, const uint8_t* s, uint64_t n) { std::memcpy(d, s, static_cast<size_t>(n)); }
static double jitSqrt(double v) { return std::sqrt(v); }
static double jitLog(double v)  { return std::log(v); }
// 原子变量：语义与解释器 DEF_ATOMIC_* 完全一致（std::atomic_ref + seq_cst）
static uint32_t jitAtomicLoadU32(uint8_t* p) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p)).load(std::memory_order_seq_cst); }
static uint64_t jitAtomicLoadU64(uint8_t* p) { return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(p)).load(std::memory_order_seq_cst); }
static void jitAtomicStoreU32(uint8_t* p, uint32_t v) { std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p)).store(v, std::memory_order_seq_cst); }
static void jitAtomicStoreU64(uint8_t* p, uint64_t v) { std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(p)).store(v, std::memory_order_seq_cst); }
static uint32_t jitAtomicXchgU32(uint8_t* p, uint32_t v) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p)).exchange(v, std::memory_order_seq_cst); }
static uint64_t jitAtomicXchgU64(uint8_t* p, uint64_t v) { return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(p)).exchange(v, std::memory_order_seq_cst); }
static void jitAtomicCasU32(uint8_t* p, uint32_t expected, uint32_t desired, uint64_t* oldOut, uint8_t* flagOut) {
    auto ref = std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p));
    const bool ok = ref.compare_exchange_strong(expected, desired, std::memory_order_seq_cst, std::memory_order_seq_cst);
    *oldOut = expected; *flagOut = ok ? 1 : 0; // 实际旧值 + 成功标志
}
static void jitAtomicCasU64(uint8_t* p, uint64_t expected, uint64_t desired, uint64_t* oldOut, uint8_t* flagOut) {
    auto ref = std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(p));
    const bool ok = ref.compare_exchange_strong(expected, desired, std::memory_order_seq_cst, std::memory_order_seq_cst);
    *oldOut = expected; *flagOut = ok ? 1 : 0;
}
static uint32_t jitAtomicAddU32(uint8_t* p, uint32_t v) { return std::atomic_ref<uint32_t>(*reinterpret_cast<uint32_t*>(p)).fetch_add(v, std::memory_order_seq_cst); }
static uint64_t jitAtomicAddU64(uint8_t* p, uint64_t v) { return std::atomic_ref<uint64_t>(*reinterpret_cast<uint64_t*>(p)).fetch_add(v, std::memory_order_seq_cst); }
// GET_ADDRS 的 DataSave 地址：每次调用现场建一个（仅当字节码含 GET_ADDRS），结束即毁
static DataSave* jitMakeDS(uint8_t* stack, uint32_t stackSize, unsigned long long* scopeStack, uint32_t sp, void* mgr) {
    auto* ds = new DataSave();
    ds->Stack = stack; ds->stackSize = stackSize; ds->stackPtr = sp;
    ds->zuoYongYv = 1; ds->zuoYongYvStackPtr = scopeStack;
    ds->end = 0; ds->silent = 1;
    if (mgr) ds->manager = std::shared_ptr<Manager>(static_cast<Manager*>(mgr), [](Manager*) {});
    return ds;
}
static void jitFreeDS(DataSave* ds) { delete ds; }

class JadeightJIT {
public:
    using Fn = void (*)(uint8_t* argPtr, uint8_t* retPtr, void* manager);

    // 提交 save → 返回 JIT 编译后的函数指针（同地址缓存，编译一次）
    static Fn submit(const save& prog) {
        std::lock_guard<std::mutex> lk(mu());
        Compiled& slot = cache()[&prog];
        if (slot.fn) return slot.fn;                 // 已编译过
        if (!initTargets()) return nullptr;          // 本机目标初始化失败
        llvm::ExecutionEngine* ee = compile(prog);
        if (!ee) return nullptr;
        slot.ee.reset(ee);
        slot.fn = reinterpret_cast<Fn>(ee->getFunctionAddress("jit_entry"));
        return slot.fn;
    }
    static void release(const save& prog) { std::lock_guard<std::mutex> lk(mu()); cache().erase(&prog); }
    static size_t cacheSize() { std::lock_guard<std::mutex> lk(mu()); return cache().size(); }

private:
    struct Compiled { std::unique_ptr<llvm::ExecutionEngine> ee; Fn fn = nullptr; };
    static std::mutex& mu() { static std::mutex m; return m; }
    static std::unordered_map<const void*, Compiled>& cache() {
        // 故意泄漏注册表：进程退出时若先析构静态 LLVMContext 再析构 EE，
        // MCJIT 的模块析构会访问已销毁的上下文（静态析构顺序未定义）。
        static auto* c = new std::unordered_map<const void*, Compiled>;
        return *c;
    }
    static bool initTargets() {
        static const bool ok = [] {
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            return true;
        }();
        return ok;
    }
    // LLVMContext 必须比 MCJIT 引擎活得久（引擎持有的模块引用它）：进程级单例
    static llvm::LLVMContext& context() {
        static llvm::LLVMContext C;
        return C;
    }

    //============== 字节码 → LLVM IR 翻译器 ==============
    struct X {
        llvm::LLVMContext& C;
        std::unique_ptr<llvm::Module> M;
        llvm::IRBuilder<> B;
        const save& prog;
        // 帧（每次调用在原生栈上 alloca）
        llvm::Value* stack = nullptr;   // i8*（VM 数据栈，容量 = 各 STACK_INIT 最大值）
        llvm::Value* scope = nullptr;   // i64*（作用域基址栈）
        llvm::Value* regs = nullptr;    // [16 x i64]*（寄存器文件）
        llvm::AllocaInst* spA = nullptr;  // i32*（stackPtr）
        llvm::AllocaInst* depA = nullptr; // i32*（zuoYongYv）
        llvm::AllocaInst* dsA = nullptr;  // i8**（GET_ADDRS 的 DataSave*，仅需要时）
        llvm::Value* argPtr = nullptr;
        llvm::Value* retPtr = nullptr;
        llvm::Value* mgrArg = nullptr;
        llvm::Function* F = nullptr;
        uint32_t stackMax = 0, scopeMax = 0;
        uint64_t bcBase = 0, sizeAddr = 0;
        bool needDS = false;
        std::vector<size_t> starts;
        std::unordered_map<size_t, llvm::BasicBlock*> blocks;
        llvm::BasicBlock* exitOK = nullptr;
        llvm::BasicBlock* exitErr = nullptr;
        // 运行期辅助函数声明
        struct H {
            llvm::FunctionCallee extern_call, call_fn, cout_u8, cout_u16, cout_u32,
                               memcpy, sqrt, log, make_ds, free_ds, jit_submit,
                               a_load_u32, a_load_u64, a_store_u32, a_store_u64,
                               a_xchg_u32, a_xchg_u64, a_cas_u32, a_cas_u64,
                               a_add_u32, a_add_u64, malloc, free;
        } h;

        explicit X(const save& p, llvm::LLVMContext& c)
            : C(c), M(std::make_unique<llvm::Module>("jit_module", C)), B(C), prog(p) {}

        // 指令长度（解码用；未列出的都是 1 字节）
        static size_t lenOf(uint8_t op) {
            switch (op) {
            case OP_REG_MOVI_U8: return 3;
            case OP_REG_MOVI_U16: return 4;
            case OP_REG_MOVI_U32: return 6;
            case OP_REG_MOVI_U64: return 10;
            case OP_REG_MOV: return 3;
            case OP_REG_PUSH_U8: case OP_REG_PUSH_U16: case OP_REG_PUSH_U32: case OP_REG_PUSH_U64: return 2;
            case OP_REG_POP_U8: case OP_REG_POP_U16: case OP_REG_POP_U32: case OP_REG_POP_U64: return 2;
            case OP_REG_LOAD_U8: case OP_REG_LOAD_U16: case OP_REG_LOAD_U32: case OP_REG_LOAD_U64: return 11;
            case OP_REG_STORE_U8: case OP_REG_STORE_U16: case OP_REG_STORE_U32: case OP_REG_STORE_U64: return 11;
            case OP_STACK_INIT: return 9;
            case OP_JMP: return 5;
            case OP_SHORT_JMP: return 2;
            case OP_STACK_PTR_MOVE: return 5;
            case OP_NEW_STACK: return 9;
            case OP_NEW_HEAP: return 13;
            case OP_DEL_HEAP: return 5;
            case OP_IF_GOTO: return 6;
            case OP_LEA: return 10;
            case OP_MOVI_U32: return 5;
            case OP_NEW_ARRAY: return 13;
            case OP_FREE_ARRAY: return 5;
            case OP_ATOMIC_LOAD_U32: case OP_ATOMIC_LOAD_U64:
            case OP_ATOMIC_STORE_U32: case OP_ATOMIC_STORE_U64:
            case OP_ATOMIC_XCHG_U32: case OP_ATOMIC_XCHG_U64:
            case OP_ATOMIC_CAS_U32: case OP_ATOMIC_CAS_U64:
            case OP_ATOMIC_ADD_U32: case OP_ATOMIC_ADD_U64: return 11;
            case OP_EXTERN_CALL: return 10;
            case OP_FUNC_CALL: return 13;
            case OP_JIT_SUBMIT: return 9;
            case OP_MEMCPY: return 27;
            default: return 1;
            }
        }

        llvm::Type* i8()  { return llvm::Type::getInt8Ty(C); }
        llvm::Type* i16() { return llvm::Type::getInt16Ty(C); }
        llvm::Type* i32() { return llvm::Type::getInt32Ty(C); }
        llvm::Type* i64() { return llvm::Type::getInt64Ty(C); }
        llvm::Type* f32() { return llvm::Type::getFloatTy(C); }
        llvm::Type* f64() { return llvm::Type::getDoubleTy(C); }
        llvm::Type* intTy(unsigned w) {
            switch (w) { case 1: return i8(); case 2: return i16(); case 4: return i32(); default: return i64(); }
        }

        void declareHelpers() {
            auto* i8p  = llvm::Type::getInt8PtrTy(C);
            auto* i64p = llvm::Type::getInt64PtrTy(C);
            auto fn = [&](const char* n, llvm::Type* rt, std::initializer_list<llvm::Type*> args) {
                return M->getOrInsertFunction(
                    n, llvm::FunctionType::get(rt, llvm::ArrayRef<llvm::Type*>(args.begin(), args.size()), false));
            };
            h.extern_call = fn("jit_extern_call", i32(), {i8(), i8p, i8p});
            h.call_fn     = fn("jit_call_fn", B.getVoidTy(), {i8p, i8p, i8p});
            h.cout_u8     = fn("jit_cout_u8", B.getVoidTy(), {i8()});
            h.cout_u16    = fn("jit_cout_u16", B.getVoidTy(), {i16()});
            h.cout_u32    = fn("jit_cout_u32", B.getVoidTy(), {i32()});
            h.memcpy      = fn("jit_memcpy", B.getVoidTy(), {i8p, i8p, i64()});
            h.sqrt        = fn("jit_sqrt", f64(), {f64()});
            h.log         = fn("jit_log", f64(), {f64()});
            h.make_ds     = fn("jit_make_ds", i8p, {i8p, i32(), i64p, i32(), i8p});
            h.free_ds     = fn("jit_free_ds", B.getVoidTy(), {i8p});
            h.jit_submit  = fn("jit_submit", i8p, {i8p});
            h.a_load_u32  = fn("jit_atomic_load_u32", i32(), {i8p});
            h.a_load_u64  = fn("jit_atomic_load_u64", i64(), {i8p});
            h.a_store_u32 = fn("jit_atomic_store_u32", B.getVoidTy(), {i8p, i32()});
            h.a_store_u64 = fn("jit_atomic_store_u64", B.getVoidTy(), {i8p, i64()});
            h.a_xchg_u32  = fn("jit_atomic_xchg_u32", i32(), {i8p, i32()});
            h.a_xchg_u64  = fn("jit_atomic_xchg_u64", i64(), {i8p, i64()});
            h.a_cas_u32   = fn("jit_atomic_cas_u32", B.getVoidTy(), {i8p, i32(), i32(), i64p, i8p});
            h.a_cas_u64   = fn("jit_atomic_cas_u64", B.getVoidTy(), {i8p, i64(), i64(), i64p, i8p});
            h.a_add_u32   = fn("jit_atomic_add_u32", i32(), {i8p, i32()});
            h.a_add_u64   = fn("jit_atomic_add_u64", i64(), {i8p, i64()});
            h.malloc      = fn("malloc", i8p, {i64()});
            h.free        = fn("free", B.getVoidTy(), {i8p});
        }

        //============== 帧 / 内存辅助 ==============
        llvm::Value* baseAddr() { // SCOPE_BASE = scope[depth-1]
            llvm::Value* d = B.CreateLoad(i32(), depA);
            llvm::Value* idx = B.CreateZExt(B.CreateSub(d, B.getInt32(1)), i64());
            return B.CreateLoad(i64(), B.CreateInBoundsGEP(i64(), scope, idx));
        }
        llvm::Value* loadSp()   { return B.CreateLoad(i32(), spA); }
        void storeSp(llvm::Value* v) { B.CreateStore(v, spA); }
        llvm::Value* loadSp64() { return B.CreateZExt(loadSp(), i64()); }
        void storeSp64(llvm::Value* v) { B.CreateStore(B.CreateTrunc(v, i32()), spA); }
        void addSp32(int32_t d) { storeSp(B.CreateAdd(loadSp(), B.getInt32(d))); }
        void addSp64(uint64_t d) { storeSp64(B.CreateAdd(loadSp64(), B.getInt64(d))); }
        llvm::Value* addrAt(llvm::Value* baseI64, uint64_t offv) { // Stack + base + off
            return B.CreateInBoundsGEP(i8(), stack, B.CreateAdd(baseI64, B.getInt64(offv)));
        }
        llvm::Value* loadWAt(llvm::Value* a, unsigned w) {
            return (w == 8) ? B.CreateLoad(i64(), a)
                            : B.CreateZExt(B.CreateLoad(intTy(w), a), i64());
        }
        void storeWAt(llvm::Value* a, llvm::Value* v, unsigned w) {
            if (w == 8) B.CreateStore(v, a);
            else B.CreateStore(B.CreateTrunc(v, intTy(w)), a);
        }
        llvm::Value* load64At(llvm::Value* a) { return B.CreateLoad(i64(), a); }
        void store64At(llvm::Value* a, llvm::Value* v) { B.CreateStore(v, a); }
        void pushW(unsigned w, llvm::Value* v) { // 压 W 字节（v 取低 W 位）
            llvm::Value* sp = loadSp();
            llvm::Value* a = B.CreateInBoundsGEP(i8(), stack, B.CreateZExt(sp, i64()));
            storeWAt(a, v, w);
            B.CreateStore(B.CreateAdd(sp, B.getInt32(w)), spA);
        }
        llvm::Value* popW(unsigned w) {
            llvm::Value* sp = loadSp();
            llvm::Value* sp2 = B.CreateSub(sp, B.getInt32(w));
            B.CreateStore(sp2, spA);
            llvm::Value* a = B.CreateInBoundsGEP(i8(), stack, B.CreateZExt(sp2, i64()));
            return loadWAt(a, w);
        }
        llvm::Value* popFloat(unsigned w) { // 按位模式读 f32/f64
            llvm::Value* bits = popW(w);
            return (w == 8) ? B.CreateBitCast(bits, f64())
                            : B.CreateBitCast(B.CreateTrunc(bits, i32()), f32());
        }
        void pushFloat(unsigned w, llvm::Value* v) {
            pushW(w, (w == 8) ? B.CreateBitCast(v, i64())
                              : B.CreateZExt(B.CreateBitCast(v, i32()), i64()));
        }
        llvm::Value* regPtr(uint8_t r) {
            return B.CreateInBoundsGEP(llvm::ArrayType::get(i64(), 16), regs,
                                       {B.getInt32(0), B.getInt32(r & 0x0F)});
        }
        llvm::Value* regLoad(uint8_t r) { return B.CreateLoad(i64(), regPtr(r)); }
        void regStore(uint8_t r, llvm::Value* v) { B.CreateStore(v, regPtr(r)); }
        llvm::Value* resolve(uint8_t mode, uint64_t offv) { // mode0 栈内 / mode1 解引用栈槽里的指针
            llvm::Value* a = addrAt(baseAddr(), offv);
            return (mode == 0) ? a : B.CreateIntToPtr(load64At(a), B.getInt8PtrTy());
        }

        bool translate() {
            declareHelpers();
            const uint8_t* bc = prog.byteCode.get();
            const size_t size = prog.size;
            bcBase = reinterpret_cast<uint64_t>(bc);
            sizeAddr = reinterpret_cast<uint64_t>(&prog.size);
            // 解码：指令起始、栈容量（各 STACK_INIT 最大值）、是否用 GET_ADDRS
            for (size_t off = 0; off < size; ) {
                starts.push_back(off);
                const uint8_t op = bc[off];
                if (op == OP_STACK_INIT) {
                    stackMax = std::max(stackMax, rdBE<uint32_t>(bc + off + 1));
                    scopeMax = std::max(scopeMax, rdBE<uint32_t>(bc + off + 5));
                } else if (op == OP_GET_ADDRS) {
                    needDS = true;
                }
                off += lenOf(op);
            }
            if (stackMax == 0) stackMax = 4096;
            if (scopeMax == 0) scopeMax = 64;
            // 函数：void jit_entry(i8*, i8*, i8*)
            auto* FT = llvm::FunctionType::get(
                B.getVoidTy(), {B.getInt8PtrTy(), B.getInt8PtrTy(), B.getInt8PtrTy()}, false);
            F = llvm::Function::Create(FT, llvm::Function::ExternalLinkage, "jit_entry", M.get());
            auto ai = F->arg_begin();
            argPtr = ai++; retPtr = ai++; mgrArg = ai;
            // 块（每指令一块，跳转目标即块）
            auto* entry = llvm::BasicBlock::Create(C, "entry", F);
            exitOK = llvm::BasicBlock::Create(C, "exit_ok", F);
            exitErr = llvm::BasicBlock::Create(C, "exit_err", F);
            for (size_t s : starts) blocks[s] = llvm::BasicBlock::Create(C, "b" + std::to_string(s), F);
            // 入口 prologue：帧 alloca + 初始化
            B.SetInsertPoint(entry);
            auto* stackArr = B.CreateAlloca(llvm::ArrayType::get(i8(), stackMax));
            stack = B.CreateInBoundsGEP(llvm::ArrayType::get(i8(), stackMax), stackArr,
                                        {B.getInt32(0), B.getInt32(0)});
            auto* scopeArr = B.CreateAlloca(llvm::ArrayType::get(i64(), scopeMax));
            scope = B.CreateInBoundsGEP(llvm::ArrayType::get(i64(), scopeMax), scopeArr,
                                        {B.getInt32(0), B.getInt32(0)});
            regs = B.CreateAlloca(llvm::ArrayType::get(i64(), 16));
            // 寄存器文件清零
            for (int r = 0; r < 16; ++r)
                B.CreateStore(B.getInt64(0), B.CreateInBoundsGEP(
                    llvm::ArrayType::get(i64(), 16), regs, {B.getInt32(0), B.getInt32(r)}));
            // 调用约定：R15=参数基址、R14=返回地址（与 callFunctionSave 预置一致）
            B.CreateStore(B.CreatePtrToInt(argPtr, i64()), B.CreateInBoundsGEP(
                llvm::ArrayType::get(i64(), 16), regs, {B.getInt32(0), B.getInt32(15)}));
            B.CreateStore(B.CreatePtrToInt(retPtr, i64()), B.CreateInBoundsGEP(
                llvm::ArrayType::get(i64(), 16), regs, {B.getInt32(0), B.getInt32(14)}));
            spA = B.CreateAlloca(i32());  B.CreateStore(B.getInt32(0), spA);
            depA = B.CreateAlloca(i32()); B.CreateStore(B.getInt32(1), depA);
            B.CreateStore(B.getInt64(0), B.CreateInBoundsGEP(i64(), scope, B.getInt64(0))); // scope[0]=0
            if (needDS) { // GET_ADDRS 需要一个 DataSave 地址（现场建，结束即毁）
                dsA = B.CreateAlloca(B.getInt8PtrTy());
                B.CreateStore(B.CreateCall(h.make_ds,
                    {stack, B.getInt32(stackMax), scope, B.getInt32(0), mgrArg}), dsA);
            }
            B.CreateBr(blocks[starts.front()]);
            // 出口（静默：不打印退出信息；帧是 alloca，自动回收）
            B.SetInsertPoint(exitOK);
            finishExit();
            B.SetInsertPoint(exitErr);
            finishExit();
            // 逐条发射
            for (size_t s : starts) emitInstr(s);
            return true;
        }

        void finishExit() {
            if (needDS) B.CreateCall(h.free_ds, B.CreateLoad(B.getInt8PtrTy(), dsA));
            B.CreateRetVoid();
        }

        void emitInstr(size_t off) {
            B.SetInsertPoint(blocks[off]);
            const uint8_t* p = prog.byteCode.get() + off;
            const uint8_t op = p[0];
            // NEXT：顺序落到下一指令（越界/非指令起始 → 越界退出）
            auto NEXT = [&](size_t len) {
                auto it = blocks.find(off + len);
                B.CreateBr(it == blocks.end() ? exitErr : it->second);
            };
            auto BR_TO = [&](size_t t) {
                auto it = blocks.find(t);
                B.CreateBr(it == blocks.end() ? exitErr : it->second);
            };

            // 类型族宏：W=字节宽；SGN=有符号；PRED=LLVM 谓词；INST=IRBuilder 方法名
            #define JIT_CASE_BININT(OPC, W, INST) \
                case OPC: { \
                    llvm::Value* b = popW(W); \
                    llvm::Value* a = popW(W); \
                    pushW(W, B.Create##INST(a, b)); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_BINF(OPC, W, INST) \
                case OPC: { \
                    llvm::Value* b = popFloat(W); \
                    llvm::Value* a = popFloat(W); \
                    pushFloat(W, B.Create##INST(a, b)); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_DIVU(OPC, W) \
                case OPC: { \
                    llvm::Value* b = popW(W); \
                    llvm::Value* a = popW(W); \
                    if (W < 8) { a = B.CreateTrunc(a, intTy(W)); b = B.CreateTrunc(b, intTy(W)); } \
                    llvm::Value* dz = B.CreateICmpEQ(b, B.getIntN(W * 8, 0)); \
                    llvm::Value* div = B.CreateSelect(dz, B.getIntN(W * 8, 1), b); \
                    pushW(W, B.CreateSelect(dz, B.getIntN(W * 8, 0), B.CreateUDiv(a, div))); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_DIVI(OPC, W) \
                case OPC: { \
                    llvm::Value* b = popW(W); \
                    llvm::Value* a = popW(W); \
                    if (W < 8) { a = B.CreateTrunc(a, intTy(W)); b = B.CreateTrunc(b, intTy(W)); } \
                    const uint64_t minV = (uint64_t(1) << (W * 8 - 1)); /* 有符号最小值位模式 */ \
                    llvm::Value* dz  = B.CreateICmpEQ(b, B.getIntN(W * 8, 0)); \
                    llvm::Value* ovf = B.CreateAnd(B.CreateICmpEQ(a, B.getIntN(W * 8, minV)), \
                                                   B.CreateICmpEQ(b, B.getIntN(W * 8, static_cast<uint64_t>(-1)))); \
                    llvm::Value* div = B.CreateSelect(dz, B.getIntN(W * 8, 1), \
                                                      B.CreateSelect(ovf, B.getIntN(W * 8, 1), b)); \
                    llvm::Value* q   = B.CreateSDiv(a, div); \
                    pushW(W, B.CreateSelect(dz, B.getIntN(W * 8, 0), B.CreateSelect(ovf, a, q))); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_MATH(OPC, W, SGN, FLT, FN) \
                case OPC: { \
                    llvm::Value* v = popW(W); \
                    if (FLT) { \
                        v = (W == 8) ? B.CreateBitCast(v, f64()) \
                                     : B.CreateFPExt(B.CreateBitCast(B.CreateTrunc(v, i32()), f32()), f64()); \
                    } else if (SGN) { \
                        v = (W == 8) ? B.CreateSIToFP(v, f64()) \
                                     : B.CreateSIToFP(B.CreateSExt(B.CreateTrunc(v, intTy(W)), i64()), f64()); \
                    } else { \
                        v = (W == 8) ? B.CreateUIToFP(v, f64()) \
                                     : B.CreateUIToFP(B.CreateTrunc(v, intTy(W)), f64()); \
                    } \
                    pushW(8, B.CreateBitCast(B.CreateCall(h.FN, v), i64())); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_CMPI(OPC, W, SGN, PRED) \
                case OPC: { \
                    llvm::Value* b = popW(W); \
                    llvm::Value* a = popW(W); \
                    if (W < 8) { \
                        a = SGN ? B.CreateSExt(B.CreateTrunc(a, intTy(W)), i64()) \
                                : B.CreateTrunc(a, intTy(W)); \
                        b = SGN ? B.CreateSExt(B.CreateTrunc(b, intTy(W)), i64()) \
                                : B.CreateTrunc(b, intTy(W)); \
                    } \
                    pushW(1, B.CreateZExt(B.CreateICmp(llvm::CmpInst::PRED, a, b), i64())); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_CMPF(OPC, W, PRED) \
                case OPC: { \
                    llvm::Value* b = popFloat(W); \
                    llvm::Value* a = popFloat(W); \
                    pushW(1, B.CreateZExt(B.CreateFCmp(llvm::CmpInst::PRED, a, b), i64())); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_SHIFT(OPC, W, INST) \
                case OPC: { \
                    llvm::Value* b = popW(W); \
                    llvm::Value* a = popW(W); \
                    if (W < 8) { a = B.CreateTrunc(a, intTy(W)); b = B.CreateTrunc(b, intTy(W)); } \
                    b = B.CreateAnd(b, B.getIntN(W * 8, (~0ULL >> (64 - W * 8)))); /* 移位量取模 */ \
                    pushW(W, B.Create##INST(a, b)); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_BIT(OPC, W, INST) \
                case OPC: { \
                    llvm::Value* b = popW(W); \
                    llvm::Value* a = popW(W); \
                    if (W < 8) { a = B.CreateTrunc(a, intTy(W)); b = B.CreateTrunc(b, intTy(W)); } \
                    pushW(W, B.Create##INST(a, b)); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_NOT(OPC, W) \
                case OPC: { \
                    llvm::Value* a = popW(W); \
                    if (W < 8) a = B.CreateTrunc(a, intTy(W)); \
                    pushW(W, B.CreateXor(a, B.getIntN(W * 8, ~0ULL))); \
                    NEXT(1); break; \
                }
            #define JIT_CASE_REG_PUSH(OPC, W) \
                case OPC: { pushW(W, regLoad(p[1])); NEXT(2); break; }
            #define JIT_CASE_REG_POP(OPC, W) \
                case OPC: { regStore(p[1], popW(W)); NEXT(2); break; }
            #define JIT_CASE_REG_LOAD(OPC, W) \
                case OPC: { regStore(p[1], loadWAt(resolve(p[2], rdBE<uint64_t>(p + 3)), W)); NEXT(11); break; }
            #define JIT_CASE_REG_STORE(OPC, W) \
                case OPC: { storeWAt(resolve(p[2], rdBE<uint64_t>(p + 3)), regLoad(p[1]), W); NEXT(11); break; }
            #define JIT_CASE_ATOMIC_LOAD(OPC, W, HELPER) \
                case OPC: { \
                    llvm::Value* r = B.CreateCall(h.HELPER, resolve(p[2], rdBE<uint64_t>(p + 3))); \
                    regStore(p[1], (W == 8) ? r : B.CreateZExt(r, i64())); \
                    NEXT(11); break; \
                }
            #define JIT_CASE_ATOMIC_STORE(OPC, W, HELPER) \
                case OPC: { \
                    llvm::Value* a = resolve(p[2], rdBE<uint64_t>(p + 3)); \
                    llvm::Value* v = (W == 8) ? regLoad(p[1]) : B.CreateTrunc(regLoad(p[1]), intTy(W)); \
                    B.CreateCall(h.HELPER, {a, v}); \
                    NEXT(11); break; \
                }
            #define JIT_CASE_ATOMIC_RMW(OPC, W, HELPER) \
                case OPC: { \
                    llvm::Value* a = resolve(p[2], rdBE<uint64_t>(p + 3)); \
                    llvm::Value* v = (W == 8) ? regLoad(p[1]) : B.CreateTrunc(regLoad(p[1]), intTy(W)); \
                    llvm::Value* r = B.CreateCall(h.HELPER, {a, v}); \
                    regStore(p[1], (W == 8) ? r : B.CreateZExt(r, i64())); \
                    NEXT(11); break; \
                }

            switch (op) {
            case OP_ERR_END: B.CreateBr(exitErr); break; // 哨兵 / 非法
            case OP_END: B.CreateBr(exitOK); break;
            case OP_STACK_INIT: { // 帧已按最大值预分配：重置 sp/作用域（语义同重初始化）
                B.CreateStore(B.getInt32(0), spA);
                B.CreateStore(B.getInt32(1), depA);
                B.CreateStore(B.getInt64(0), B.CreateInBoundsGEP(i64(), scope, B.getInt64(0)));
                NEXT(9); break;
            }
            case OP_JMP: BR_TO(rdBE<uint32_t>(p + 1)); break;
            case OP_SHORT_JMP: {
                const uint8_t d = p[1];
                BR_TO((d > 0x7F) ? (off - (d - 129)) : (off + d + 2));
                break;
            }
            case OP_SCOPE_PUSH: {
                llvm::Value* d = B.CreateLoad(i32(), depA);
                B.CreateStore(loadSp64(), B.CreateInBoundsGEP(i64(), scope, B.CreateZExt(d, i64())));
                B.CreateStore(B.CreateAdd(d, B.getInt32(1)), depA);
                NEXT(1); break;
            }
            case OP_SCOPE_POP: {
                llvm::Value* d = B.CreateSub(B.CreateLoad(i32(), depA), B.getInt32(1));
                B.CreateStore(d, depA);
                storeSp64(B.CreateLoad(i64(), B.CreateInBoundsGEP(i64(), scope, B.CreateZExt(d, i64()))));
                NEXT(1); break;
            }
            case OP_STACK_PTR_MOVE: addSp32(-static_cast<int32_t>(rdBE<uint32_t>(p + 1))); NEXT(5); break;
            case OP_NEW_STACK: addSp64(rdBE<uint64_t>(p + 1)); NEXT(9); break;
            case OP_NEW_HEAP: { // 分配 size 字节，指针写栈槽 slotOff
                llvm::Value* a = addrAt(baseAddr(), rdBE<uint32_t>(p + 1));
                llvm::Value* ptr = B.CreateCall(h.malloc, B.getInt64(rdBE<uint64_t>(p + 5)));
                store64At(a, B.CreatePtrToInt(ptr, i64()));
                NEXT(13); break;
            }
            case OP_DEL_HEAP: {
                llvm::Value* a = addrAt(baseAddr(), rdBE<uint32_t>(p + 1));
                B.CreateCall(h.free, B.CreateIntToPtr(load64At(a), B.getInt8PtrTy()));
                NEXT(5); break;
            }
            case OP_IF_GOTO: // cond≠0 → 顺序（+6）；cond==0 → 跳 addr
                if (p[1] != 0) NEXT(6); else BR_TO(rdBE<uint32_t>(p + 2));
                break;
            case OP_LEA: { // mode0 压栈内地址 / mode1 压栈槽里的指针
                const uint64_t offv = rdBE<uint64_t>(p + 2);
                if (p[1] == 0) pushW(8, B.CreatePtrToInt(addrAt(baseAddr(), offv), i64()));
                else           pushW(8, load64At(addrAt(baseAddr(), offv)));
                NEXT(10); break;
            }
            case OP_MOVI_U32: pushW(4, B.getInt64(rdBE<uint32_t>(p + 1))); NEXT(5); break;
            // REG 寄存器族
            case OP_REG_MOVI_U8:  regStore(p[1], B.getInt64(p[2])); NEXT(3); break;
            case OP_REG_MOVI_U16: regStore(p[1], B.getInt64(rdBE<uint16_t>(p + 2))); NEXT(4); break;
            case OP_REG_MOVI_U32: regStore(p[1], B.getInt64(rdBE<uint32_t>(p + 2))); NEXT(6); break;
            case OP_REG_MOVI_U64: regStore(p[1], B.getInt64(rdBE<uint64_t>(p + 2))); NEXT(10); break;
            case OP_REG_MOV: regStore(p[1], regLoad(p[2])); NEXT(3); break;
            JIT_CASE_REG_PUSH(OP_REG_PUSH_U8, 1)  JIT_CASE_REG_PUSH(OP_REG_PUSH_U16, 2)
            JIT_CASE_REG_PUSH(OP_REG_PUSH_U32, 4) JIT_CASE_REG_PUSH(OP_REG_PUSH_U64, 8)
            JIT_CASE_REG_POP(OP_REG_POP_U8, 1)    JIT_CASE_REG_POP(OP_REG_POP_U16, 2)
            JIT_CASE_REG_POP(OP_REG_POP_U32, 4)   JIT_CASE_REG_POP(OP_REG_POP_U64, 8)
            JIT_CASE_REG_LOAD(OP_REG_LOAD_U8, 1)  JIT_CASE_REG_LOAD(OP_REG_LOAD_U16, 2)
            JIT_CASE_REG_LOAD(OP_REG_LOAD_U32, 4) JIT_CASE_REG_LOAD(OP_REG_LOAD_U64, 8)
            JIT_CASE_REG_STORE(OP_REG_STORE_U8, 1)  JIT_CASE_REG_STORE(OP_REG_STORE_U16, 2)
            JIT_CASE_REG_STORE(OP_REG_STORE_U32, 4) JIT_CASE_REG_STORE(OP_REG_STORE_U64, 8)
            // 四则运算（ADD/SUB 只 uint+float；MUL/DIV 全 10 种数值类型）
            JIT_CASE_BININT(OP_ADD_U8, 1, Add)  JIT_CASE_BININT(OP_ADD_U16, 2, Add)
            JIT_CASE_BININT(OP_ADD_U32, 4, Add) JIT_CASE_BININT(OP_ADD_U64, 8, Add)
            JIT_CASE_BININT(OP_SUB_U8, 1, Sub)  JIT_CASE_BININT(OP_SUB_U16, 2, Sub)
            JIT_CASE_BININT(OP_SUB_U32, 4, Sub) JIT_CASE_BININT(OP_SUB_U64, 8, Sub)
            JIT_CASE_BININT(OP_MUL_U8, 1, Mul)  JIT_CASE_BININT(OP_MUL_I8, 1, Mul)
            JIT_CASE_BININT(OP_MUL_U16, 2, Mul) JIT_CASE_BININT(OP_MUL_I16, 2, Mul)
            JIT_CASE_BININT(OP_MUL_U32, 4, Mul) JIT_CASE_BININT(OP_MUL_I32, 4, Mul)
            JIT_CASE_BININT(OP_MUL_U64, 8, Mul) JIT_CASE_BININT(OP_MUL_I64, 8, Mul)
            JIT_CASE_DIVU(OP_DIV_U8, 1)  JIT_CASE_DIVI(OP_DIV_I8, 1)
            JIT_CASE_DIVU(OP_DIV_U16, 2) JIT_CASE_DIVI(OP_DIV_I16, 2)
            JIT_CASE_DIVU(OP_DIV_U32, 4) JIT_CASE_DIVI(OP_DIV_I32, 4)
            JIT_CASE_DIVU(OP_DIV_U64, 8) JIT_CASE_DIVI(OP_DIV_I64, 8)
            JIT_CASE_BINF(OP_ADD_F32, 4, FAdd) JIT_CASE_BINF(OP_ADD_F64, 8, FAdd)
            JIT_CASE_BINF(OP_SUB_F32, 4, FSub) JIT_CASE_BINF(OP_SUB_F64, 8, FSub)
            JIT_CASE_BINF(OP_MUL_F32, 4, FMul) JIT_CASE_BINF(OP_MUL_F64, 8, FMul)
            JIT_CASE_BINF(OP_DIV_F32, 4, FDiv) JIT_CASE_BINF(OP_DIV_F64, 8, FDiv)
            case OP_ADD_PTR: case OP_SUB_PTR: {
                llvm::Value* b = popW(8);
                llvm::Value* a = popW(8);
                pushW(8, (op == OP_ADD_PTR) ? B.CreateAdd(a, b) : B.CreateSub(a, b));
                NEXT(1); break;
            }
            // SQRT / LOG（按 double 计算，结果一律 f64 压栈）
            JIT_CASE_MATH(OP_SQRT_U8, 1, 0, 0, sqrt)  JIT_CASE_MATH(OP_SQRT_I8, 1, 1, 0, sqrt)
            JIT_CASE_MATH(OP_SQRT_U16, 2, 0, 0, sqrt) JIT_CASE_MATH(OP_SQRT_I16, 2, 1, 0, sqrt)
            JIT_CASE_MATH(OP_SQRT_U32, 4, 0, 0, sqrt) JIT_CASE_MATH(OP_SQRT_I32, 4, 1, 0, sqrt)
            JIT_CASE_MATH(OP_SQRT_U64, 8, 0, 0, sqrt) JIT_CASE_MATH(OP_SQRT_I64, 8, 1, 0, sqrt)
            JIT_CASE_MATH(OP_SQRT_F32, 4, 0, 1, sqrt)  JIT_CASE_MATH(OP_SQRT_F64, 8, 0, 1, sqrt)
            JIT_CASE_MATH(OP_LOG_U8, 1, 0, 0, log)  JIT_CASE_MATH(OP_LOG_I8, 1, 1, 0, log)
            JIT_CASE_MATH(OP_LOG_U16, 2, 0, 0, log) JIT_CASE_MATH(OP_LOG_I16, 2, 1, 0, log)
            JIT_CASE_MATH(OP_LOG_U32, 4, 0, 0, log) JIT_CASE_MATH(OP_LOG_I32, 4, 1, 0, log)
            JIT_CASE_MATH(OP_LOG_U64, 8, 0, 0, log) JIT_CASE_MATH(OP_LOG_I64, 8, 1, 0, log)
            JIT_CASE_MATH(OP_LOG_F32, 4, 0, 1, log)  JIT_CASE_MATH(OP_LOG_F64, 8, 0, 1, log)
            // COUT（只保留 char）
            case OP_COUT_CHAR8:  B.CreateCall(h.cout_u8,  B.CreateTrunc(popW(1), i8()));  NEXT(1); break;
            case OP_COUT_CHAR16: B.CreateCall(h.cout_u16, B.CreateTrunc(popW(2), i16())); NEXT(1); break;
            case OP_COUT_CHAR32: B.CreateCall(h.cout_u32, B.CreateTrunc(popW(4), i32())); NEXT(1); break;
            // 大小比较（< = >；结果统一压 U8 的 0/1）
            JIT_CASE_CMPI(OP_CMP_LT_U8, 1, 0, ICMP_ULT)  JIT_CASE_CMPI(OP_CMP_LT_I8, 1, 1, ICMP_SLT)
            JIT_CASE_CMPI(OP_CMP_LT_U16, 2, 0, ICMP_ULT) JIT_CASE_CMPI(OP_CMP_LT_I16, 2, 1, ICMP_SLT)
            JIT_CASE_CMPI(OP_CMP_LT_U32, 4, 0, ICMP_ULT) JIT_CASE_CMPI(OP_CMP_LT_I32, 4, 1, ICMP_SLT)
            JIT_CASE_CMPI(OP_CMP_LT_U64, 8, 0, ICMP_ULT) JIT_CASE_CMPI(OP_CMP_LT_I64, 8, 1, ICMP_SLT)
            JIT_CASE_CMPF(OP_CMP_LT_F32, 4, FCMP_OLT)    JIT_CASE_CMPF(OP_CMP_LT_F64, 8, FCMP_OLT)
            JIT_CASE_CMPI(OP_CMP_LT_PTR, 8, 0, ICMP_ULT)
            JIT_CASE_CMPI(OP_CMP_EQ_U8, 1, 0, ICMP_EQ)   JIT_CASE_CMPI(OP_CMP_EQ_I8, 1, 0, ICMP_EQ)
            JIT_CASE_CMPI(OP_CMP_EQ_U16, 2, 0, ICMP_EQ)  JIT_CASE_CMPI(OP_CMP_EQ_I16, 2, 0, ICMP_EQ)
            JIT_CASE_CMPI(OP_CMP_EQ_U32, 4, 0, ICMP_EQ)  JIT_CASE_CMPI(OP_CMP_EQ_I32, 4, 0, ICMP_EQ)
            JIT_CASE_CMPI(OP_CMP_EQ_U64, 8, 0, ICMP_EQ)  JIT_CASE_CMPI(OP_CMP_EQ_I64, 8, 0, ICMP_EQ)
            JIT_CASE_CMPF(OP_CMP_EQ_F32, 4, FCMP_OEQ)    JIT_CASE_CMPF(OP_CMP_EQ_F64, 8, FCMP_OEQ)
            JIT_CASE_CMPI(OP_CMP_EQ_PTR, 8, 0, ICMP_EQ)
            JIT_CASE_CMPI(OP_CMP_GT_U8, 1, 0, ICMP_UGT)  JIT_CASE_CMPI(OP_CMP_GT_I8, 1, 1, ICMP_SGT)
            JIT_CASE_CMPI(OP_CMP_GT_U16, 2, 0, ICMP_UGT) JIT_CASE_CMPI(OP_CMP_GT_I16, 2, 1, ICMP_SGT)
            JIT_CASE_CMPI(OP_CMP_GT_U32, 4, 0, ICMP_UGT) JIT_CASE_CMPI(OP_CMP_GT_I32, 4, 1, ICMP_SGT)
            JIT_CASE_CMPI(OP_CMP_GT_U64, 8, 0, ICMP_UGT) JIT_CASE_CMPI(OP_CMP_GT_I64, 8, 1, ICMP_SGT)
            JIT_CASE_CMPF(OP_CMP_GT_F32, 4, FCMP_OGT)    JIT_CASE_CMPF(OP_CMP_GT_F64, 8, FCMP_OGT)
            JIT_CASE_CMPI(OP_CMP_GT_PTR, 8, 0, ICMP_UGT)
            // 位运算（<< & | ~ 只给 uint；>> 给 uint 和 int 各一套）
            JIT_CASE_SHIFT(OP_SHL_U8, 1, Shl)  JIT_CASE_SHIFT(OP_SHL_U16, 2, Shl)
            JIT_CASE_SHIFT(OP_SHL_U32, 4, Shl) JIT_CASE_SHIFT(OP_SHL_U64, 8, Shl)
            JIT_CASE_BIT(OP_AND_U8, 1, And)  JIT_CASE_BIT(OP_AND_U16, 2, And)
            JIT_CASE_BIT(OP_AND_U32, 4, And) JIT_CASE_BIT(OP_AND_U64, 8, And)
            JIT_CASE_BIT(OP_OR_U8, 1, Or)  JIT_CASE_BIT(OP_OR_U16, 2, Or)
            JIT_CASE_BIT(OP_OR_U32, 4, Or) JIT_CASE_BIT(OP_OR_U64, 8, Or)
            JIT_CASE_NOT(OP_NOT_U8, 1)  JIT_CASE_NOT(OP_NOT_U16, 2)
            JIT_CASE_NOT(OP_NOT_U32, 4) JIT_CASE_NOT(OP_NOT_U64, 8)
            JIT_CASE_SHIFT(OP_SHR_U8, 1, LShr)  JIT_CASE_SHIFT(OP_SHR_U16, 2, LShr)
            JIT_CASE_SHIFT(OP_SHR_U32, 4, LShr) JIT_CASE_SHIFT(OP_SHR_U64, 8, LShr)
            JIT_CASE_SHIFT(OP_SHR_I8, 1, AShr)  JIT_CASE_SHIFT(OP_SHR_I16, 2, AShr)
            JIT_CASE_SHIFT(OP_SHR_I32, 4, AShr) JIT_CASE_SHIFT(OP_SHR_I64, 8, AShr)
            // 类型转换（浮点 ↔ 整数，只 32 位）
            case OP_CVT_F32_U32: pushW(4, B.CreateZExt(B.CreateFPToUI(popFloat(4), i32()), i64())); NEXT(1); break;
            case OP_CVT_F32_I32: pushW(4, B.CreateZExt(B.CreateFPToSI(popFloat(4), i32()), i64())); NEXT(1); break;
            case OP_CVT_F64_U32: pushW(4, B.CreateZExt(B.CreateFPToUI(popFloat(8), i32()), i64())); NEXT(1); break;
            case OP_CVT_F64_I32: pushW(4, B.CreateZExt(B.CreateFPToSI(popFloat(8), i32()), i64())); NEXT(1); break;
            case OP_CVT_U32_F32: pushFloat(4, B.CreateUIToFP(B.CreateTrunc(popW(4), i32()), f32())); NEXT(1); break;
            case OP_CVT_U32_F64: pushFloat(8, B.CreateUIToFP(B.CreateTrunc(popW(4), i32()), f64())); NEXT(1); break;
            case OP_CVT_I32_F32: pushFloat(4, B.CreateSIToFP(B.CreateTrunc(popW(4), i32()), f32())); NEXT(1); break;
            case OP_CVT_I32_F64: pushFloat(8, B.CreateSIToFP(B.CreateTrunc(popW(4), i32()), f64())); NEXT(1); break;
            // 分配器（无类型，只认 size 和 where）
            case OP_NEW_ARRAY: {
                llvm::Value* a = addrAt(baseAddr(), rdBE<uint32_t>(p + 9));
                llvm::Value* ptr = B.CreateCall(h.malloc, B.getInt64(rdBE<uint64_t>(p + 1)));
                store64At(a, B.CreatePtrToInt(ptr, i64()));
                NEXT(13); break;
            }
            case OP_FREE_ARRAY: {
                llvm::Value* a = addrAt(baseAddr(), rdBE<uint32_t>(p + 1));
                B.CreateCall(h.free, B.CreateIntToPtr(load64At(a), B.getInt8PtrTy()));
                NEXT(5); break;
            }
            case OP_GET_ADDRS: { // 依次压入：bytecode 基址、size 地址、DataSave 地址、管理器地址
                pushW(8, B.getInt64(bcBase));
                pushW(8, B.getInt64(sizeAddr));
                pushW(8, B.CreatePtrToInt(B.CreateLoad(B.getInt8PtrTy(), dsA), i64()));
                pushW(8, B.CreatePtrToInt(mgrArg, i64()));
                NEXT(1); break;
            }
            case OP_JMP_IND: { // 间接跳转：弹栈绝对地址 → 换算字节码偏移 → 链式比较分发
                llvm::Value* pc = B.CreateSub(popW(8), B.getInt64(bcBase));
                for (size_t s : starts) {
                    auto it = blocks.find(s);
                    if (it == blocks.end()) continue;
                    auto* nx = llvm::BasicBlock::Create(C, "ji" + std::to_string(s), F);
                    B.CreateCondBr(B.CreateICmpEQ(pc, B.getInt64(static_cast<int64_t>(s))), it->second, nx);
                    B.SetInsertPoint(nx);
                }
                B.CreateBr(exitErr);
                break;
            }
            // 原子变量（语义与解释器一致：std::atomic_ref + seq_cst）
            JIT_CASE_ATOMIC_LOAD(OP_ATOMIC_LOAD_U32, 4, a_load_u32)
            JIT_CASE_ATOMIC_LOAD(OP_ATOMIC_LOAD_U64, 8, a_load_u64)
            JIT_CASE_ATOMIC_STORE(OP_ATOMIC_STORE_U32, 4, a_store_u32)
            JIT_CASE_ATOMIC_STORE(OP_ATOMIC_STORE_U64, 8, a_store_u64)
            JIT_CASE_ATOMIC_RMW(OP_ATOMIC_XCHG_U32, 4, a_xchg_u32)
            JIT_CASE_ATOMIC_RMW(OP_ATOMIC_XCHG_U64, 8, a_xchg_u64)
            JIT_CASE_ATOMIC_RMW(OP_ATOMIC_ADD_U32, 4, a_add_u32)
            JIT_CASE_ATOMIC_RMW(OP_ATOMIC_ADD_U64, 8, a_add_u64)
            case OP_ATOMIC_CAS_U32: case OP_ATOMIC_CAS_U64: {
                const unsigned W = (op == OP_ATOMIC_CAS_U32) ? 4 : 8;
                llvm::Value* a = resolve(p[2], rdBE<uint64_t>(p + 3));
                // 弹 expected（先减 sp 再读）；CAS 后 reg=实际旧值、压 U8 成功标志
                llvm::Value* sp1 = B.CreateSub(loadSp(), B.getInt32(W));
                B.CreateStore(sp1, spA);
                llvm::Value* expA = B.CreateInBoundsGEP(i8(), stack, B.CreateZExt(sp1, i64()));
                llvm::Value* expected = B.CreateLoad(intTy(W), expA);
                llvm::Value* desired = (W == 8) ? regLoad(p[1]) : B.CreateTrunc(regLoad(p[1]), intTy(W));
                B.CreateCall((W == 4) ? h.a_cas_u32 : h.a_cas_u64,
                             {a, expected, desired, regPtr(p[1]), expA});
                B.CreateStore(B.CreateAdd(sp1, B.getInt32(1)), spA); // flag 已由 helper 写 expA
                NEXT(11); break;
            }
            // EXTERN_CALL：libffi 原样暴露（运行期辅助与解释器逐字节一致）
            case OP_EXTERN_CALL: {
                llvm::Value* base = baseAddr();
                llvm::Value* r = B.CreateCall(h.extern_call,
                    {B.getInt8(p[1]), addrAt(base, rdBE<uint32_t>(p + 2)), addrAt(base, rdBE<uint32_t>(p + 6))});
                auto* nx = llvm::BasicBlock::Create(C, "extcall_next", F);
                B.CreateCondBr(B.CreateICmpNE(r, B.getInt32(0)), exitErr, nx); // 未注册 → 越界退出
                B.SetInsertPoint(nx);
                NEXT(10); break;
            }
            // FUNC_CALL：调栈槽里的 FunctionSave*（走解释器路径，语义一致）
            case OP_FUNC_CALL: {
                llvm::Value* base = baseAddr();
                llvm::Value* fn = load64At(addrAt(base, rdBE<uint32_t>(p + 1)));
                auto* nx = llvm::BasicBlock::Create(C, "funccall_next", F);
                B.CreateCondBr(B.CreateICmpEQ(fn, B.getInt64(0)), exitErr, nx); // 空函数指针 → 越界退出
                B.SetInsertPoint(nx);
                B.CreateCall(h.call_fn,
                    {B.CreateIntToPtr(fn, B.getInt8PtrTy()),
                     addrAt(base, rdBE<uint32_t>(p + 5)), addrAt(base, rdBE<uint32_t>(p + 9))});
                NEXT(13); break;
            }
            // JIT_SUBMIT：栈槽里读 save* → JadeightJIT::submit（辅助函数，含缓存）→ 函数指针写回
            case OP_JIT_SUBMIT: {
                llvm::Value* base = baseAddr();
                llvm::Value* fn = load64At(addrAt(base, rdBE<uint32_t>(p + 1)));
                auto* nx = llvm::BasicBlock::Create(C, "jitsubmit_next", F);
                B.CreateCondBr(B.CreateICmpEQ(fn, B.getInt64(0)), exitErr, nx); // 空 save* → 越界退出
                B.SetInsertPoint(nx);
                llvm::Value* r = B.CreateCall(h.jit_submit, B.CreateIntToPtr(fn, B.getInt8PtrTy()));
                store64At(addrAt(base, rdBE<uint32_t>(p + 5)), r); // 函数指针写 retOff（8 字节）
                NEXT(9); break;
            }
            // MEMCPY：内存 bulk 拷贝（mode0 栈槽 / mode1 指针解引用）
            case OP_MEMCPY: {
                llvm::Value* base = baseAddr();
                const uint64_t dOff = rdBE<uint64_t>(p + 2);
                const uint64_t sOff = rdBE<uint64_t>(p + 11);
                llvm::Value* dst = (p[1] == 0) ? addrAt(base, dOff)
                                               : B.CreateIntToPtr(load64At(addrAt(base, dOff)), B.getInt8PtrTy());
                llvm::Value* src = (p[10] == 0) ? addrAt(base, sOff)
                                                : B.CreateIntToPtr(load64At(addrAt(base, sOff)), B.getInt8PtrTy());
                B.CreateCall(h.memcpy, {dst, src, B.getInt64(rdBE<uint64_t>(p + 19))});
                NEXT(27); break;
            }
            default: B.CreateBr(exitErr); break; // 未知指令 → 越界退出
            }
            #undef JIT_CASE_BININT
            #undef JIT_CASE_BINF
            #undef JIT_CASE_DIVU
            #undef JIT_CASE_DIVI
            #undef JIT_CASE_MATH
            #undef JIT_CASE_CMPI
            #undef JIT_CASE_CMPF
            #undef JIT_CASE_SHIFT
            #undef JIT_CASE_BIT
            #undef JIT_CASE_NOT
            #undef JIT_CASE_REG_PUSH
            #undef JIT_CASE_REG_POP
            #undef JIT_CASE_REG_LOAD
            #undef JIT_CASE_REG_STORE
            #undef JIT_CASE_ATOMIC_LOAD
            #undef JIT_CASE_ATOMIC_STORE
            #undef JIT_CASE_ATOMIC_RMW
        }
    };

    //============== 模块优化 + MCJIT 编译 ==============
    static void optimize(llvm::Module& M, llvm::TargetMachine* TM) {
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;
        llvm::PassBuilder PB(TM);
        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);
        llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
        MPM.run(M, MAM);
    }
    static std::unique_ptr<llvm::TargetMachine> makeTM(std::string& err) {
        const std::string triple = llvm::sys::getProcessTriple();
        const llvm::Target* T = llvm::TargetRegistry::lookupTarget(triple, err);
        if (!T) return nullptr;
        return std::unique_ptr<llvm::TargetMachine>(
            T->createTargetMachine(triple, "generic", "", llvm::TargetOptions(), llvm::Reloc::PIC_));
    }
    static void mapHelpers(llvm::ExecutionEngine& ee) {
        auto M = [&](const char* n, void* p) { ee.addGlobalMapping(n, reinterpret_cast<uint64_t>(p)); };
        M("jit_extern_call", reinterpret_cast<void*>(&jitExternCall));
        M("jit_call_fn", reinterpret_cast<void*>(&callFunctionSave));
        M("jit_cout_u8", reinterpret_cast<void*>(&jitCoutU8));
        M("jit_cout_u16", reinterpret_cast<void*>(&jitCoutU16));
        M("jit_cout_u32", reinterpret_cast<void*>(&jitCoutU32));
        M("jit_memcpy", reinterpret_cast<void*>(&jitMemcpy));
        M("jit_sqrt", reinterpret_cast<void*>(&jitSqrt));
        M("jit_log", reinterpret_cast<void*>(&jitLog));
        M("jit_make_ds", reinterpret_cast<void*>(&jitMakeDS));
        M("jit_free_ds", reinterpret_cast<void*>(&jitFreeDS));
        M("jit_submit", reinterpret_cast<void*>(&jitSubmitSave));
        M("jit_atomic_load_u32", reinterpret_cast<void*>(&jitAtomicLoadU32));
        M("jit_atomic_load_u64", reinterpret_cast<void*>(&jitAtomicLoadU64));
        M("jit_atomic_store_u32", reinterpret_cast<void*>(&jitAtomicStoreU32));
        M("jit_atomic_store_u64", reinterpret_cast<void*>(&jitAtomicStoreU64));
        M("jit_atomic_xchg_u32", reinterpret_cast<void*>(&jitAtomicXchgU32));
        M("jit_atomic_xchg_u64", reinterpret_cast<void*>(&jitAtomicXchgU64));
        M("jit_atomic_cas_u32", reinterpret_cast<void*>(&jitAtomicCasU32));
        M("jit_atomic_cas_u64", reinterpret_cast<void*>(&jitAtomicCasU64));
        M("jit_atomic_add_u32", reinterpret_cast<void*>(&jitAtomicAddU32));
        M("jit_atomic_add_u64", reinterpret_cast<void*>(&jitAtomicAddU64));
        M("malloc", reinterpret_cast<void*>(&malloc));
        M("free", reinterpret_cast<void*>(&free));
    }
    static llvm::ExecutionEngine* compile(const save& prog) {
        X x(prog, context());
        if (!x.translate()) return nullptr;
        std::string err;
        auto TM = makeTM(err);
        if (!TM) { std::cerr << "[JIT] 目标机初始化失败: " << err << "\n"; return nullptr; }
        x.M->setDataLayout(TM->createDataLayout());
        x.M->setTargetTriple(llvm::sys::getProcessTriple());
        optimize(*x.M, TM.get());
        if (llvm::verifyModule(*x.M, &llvm::errs())) {
            std::cerr << "[JIT] IR 校验失败\n";
            return nullptr;
        }
        std::string eerr;
        std::unique_ptr<llvm::ExecutionEngine> ee(
            llvm::EngineBuilder(std::move(x.M)).setErrorStr(&eerr)
                .setEngineKind(llvm::EngineKind::JIT).create());
        if (!ee) { std::cerr << "[JIT] 引擎创建失败: " << eerr << "\n"; return nullptr; }
        mapHelpers(*ee);
        return ee.release();
    }
};
#else // !JADEIGHT_HAS_LLVM
// LLVM 不可用时的空实现：保持 main() 演示代码无条件编译
class JadeightJIT {
public:
    using Fn = void (*)(uint8_t*, uint8_t*, void*);
    static Fn submit(const save&) { return nullptr; }
    static void release(const save&) {}
    static size_t cacheSize() { return 0; }
};
#endif

// JIT_SUBMIT 的处理器实现（完整定义在 JadeightJIT 之后；无 LLVM 时 submit 返回 nullptr）：
// 输入 save* → 返回 JIT 编译后的函数指针（调用约定 fn(argPtr,retPtr,manager)）
static void* jitSubmitSave(const save& s) {
    return reinterpret_cast<void*>(JadeightJIT::submit(s));
}

//==================== EXTERN_CALL 外部函数表（libffi 原样暴露） =====================
// externFn[idx] = { ffi_cif*（宿主用 ffi_prep_cif 预生成，存全局/栈）, 函数指针 }
// EXTERN_CALL 只按 arg_types 顺序拆分参数指针后调用【原始 ffi_call】——零封装。
// 注册辅助：原样调用 libffi 的 ffi_prep_cif 准备 cif，成功（FFI_OK）才登记。
// 不重实现、不包装 libffi 的任何接口。
static void regExtern(unsigned idx, ffi_cif* cif, unsigned nargs, ffi_type* rtype,
                      ffi_type** atypes, void* fn) {
    if (ffi_prep_cif(cif, FFI_DEFAULT_ABI, nargs, rtype, atypes) == FFI_OK)
        externFn[idx] = { cif, fn };
}
// 注册宏：一次性声明静态 cif + atypes 数组并登记（每个条目独立的静态存储）
#define REG_FN(IDX, RTYPE, FUNC, ...) \
    do { \
        static ffi_type* at_[] = { __VA_ARGS__ }; \
        static ffi_cif cif_; \
        regExtern(IDX, &cif_, static_cast<unsigned>(sizeof(at_) / sizeof(at_[0])), \
                  RTYPE, at_, reinterpret_cast<void*>(FUNC)); \
    } while (0)

// 宿主演示函数：混合宽度参数（u8/u16/u32/i64 + float + double），
// 用于验证 EXTERN_CALL 按 arg_types 顺序（先 8 位再 16 位…）读参
extern "C" int64_t host_mix(uint8_t a, uint16_t b, uint32_t c, int64_t d, float e, double f) {
    return static_cast<int64_t>(a) + b + c + d + static_cast<int64_t>(e) + static_cast<int64_t>(f);
}

// 宿主辅助：动态链接的路径以【伪造的 std::string】形式由客人程序提供
// （客户按 libstdc++ SSO 布局在自己内存里伪造 32 字节 string 对象，传 &str）。
// 本函数只负责读出路径并原样调用 dlopen —— 不重实现、不包装动态链接接口。
// 同时记录每次成功 dlopen 的库的加载基址（dlinfo 取 link_map），
// 供 collect_externs 枚举"刚刚动态链接的库"的全部函数符号。
static std::vector<uintptr_t> g_dlopened_bias; // 刚 dlopen 过的库的加载基址
extern "C" void* dlopen_std(const std::string& path, int flags) {
    void* h = dlopen(path.c_str(), flags);
    if (h) {
        struct link_map* lm = nullptr;
        if (dlinfo(h, RTLD_DI_LINKMAP, &lm) == 0 && lm != nullptr)
            g_dlopened_bias.push_back(lm->l_addr);
    }
    return h;
}

// 枚举所有"刚刚动态链接"的库里的全部函数符号指针：
// dl_iterate_phdr 遍历进程已加载对象，只挑 dlopen_std 记录过的库（按加载基址匹配），
// 解析其 .dynsym（DT_SYMTAB/DT_STRTAB/DT_HASH），把每个函数符号写成 16 字节条目
// {名字指针(8) + 运行地址(8)} 存入 out 缓冲区，返回写入条数（超过 cap 截断）。
// 名字指针在库卸载前一直有效 —— 客人程序可直接读名字、直接取地址去调用。
struct CollectCtx { uint8_t* out; uint64_t cap; uint64_t count; };
static int collectCb(struct dl_phdr_info* info, size_t, void* data) {
    CollectCtx* ctx = static_cast<CollectCtx*>(data);
    bool wanted = false;
    for (uintptr_t b : g_dlopened_bias)
        if (b == info->dlpi_addr) { wanted = true; break; }
    if (!wanted) return 0;
    const ElfW(Phdr)* ph = info->dlpi_phdr;
    const ElfW(Dyn)* dyn = nullptr;
    for (ElfW(Half) i = 0; i < info->dlpi_phnum; ++i)
        if (ph[i].p_type == PT_DYNAMIC) { dyn = reinterpret_cast<const ElfW(Dyn)*>(info->dlpi_addr + ph[i].p_vaddr); break; }
    if (!dyn) return 0;
    const ElfW(Sym)* symtab = nullptr;
    const char* strtab = nullptr;
    uint64_t nsyms = 0;
    // 注意：动态段位置用 bias+phdr.p_vaddr 找；但段内 DT_SYMTAB/DT_STRTAB/DT_HASH
    // 的 d_ptr 是 ld.so 装载时已 rebase 过的【绝对运行地址】，不能再加 bias
    // （实测 libc：文件值 0x210a08 + bias 0x7ffff7800000 = 运行值 0x7ffff7a10a08）。
    for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
        if (d->d_tag == DT_SYMTAB)
            symtab = reinterpret_cast<const ElfW(Sym)*>(d->d_un.d_ptr);
        else if (d->d_tag == DT_STRTAB)
            strtab = reinterpret_cast<const char*>(d->d_un.d_ptr);
        else if (d->d_tag == DT_HASH) // [0]=nbucket [1]=nchain=符号总数
            nsyms = *reinterpret_cast<const uint32_t*>(d->d_un.d_ptr + 4);
    }
    if (!symtab || !strtab || nsyms == 0) return 0;
    for (uint64_t i = 0; i < nsyms && ctx->count < ctx->cap; ++i) {
        const ElfW(Sym)& s = symtab[i];
        const unsigned type = ELF64_ST_TYPE(s.st_info);
        if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue; // 未定义/无地址
        if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;  // 只要函数
        const char* name = strtab + s.st_name;
        if (!*name) continue;
        storeRaw(ctx->out + ctx->count * 16 + 0, reinterpret_cast<uint64_t>(name), 8);
        storeRaw(ctx->out + ctx->count * 16 + 8, info->dlpi_addr + s.st_value, 8); // st_value 仍相对
        ++ctx->count;
    }
    return 0;
}
extern "C" uint64_t collect_externs(void* out, uint64_t cap) {
    CollectCtx ctx{ static_cast<uint8_t*>(out), cap, 0 };
    dl_iterate_phdr(collectCb, &ctx);
    return ctx.count;
}

//==================== 演示程序（手写字节码，精简后的指令集） =====================
// 说明：MOVI_U32 压 4 字节立即数，是常量入栈的唯一来源；
//       地址相关的演示仍用 NEW_ARRAY/LEA 从堆上取指针，覆盖寻址/指针运算路径。
//       —— NEW_ARRAY 分配两块内存，指针写入栈槽 1024 / 1032（远离栈顶增长区）
//       —— LEA mode1 把栈槽里的指针压栈，算术/比较/位运算/转换都吃这些值
//       —— 输出只剩 COUT_CHAR8/16/32：CHAR8 按字符打印（弹栈顶字节，
//         所以打印数字字符时先把数值左移到高字节，再加 '0'=0x30）
int main() {
    std::vector<uint8_t> bc;
    auto put   = [&](uint8_t b)              { bc.push_back(b); };
    auto put32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) bc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto put64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) bc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto patch32 = [&](size_t pos, uint32_t v) { for (int i = 3; i >= 0; --i) bc[pos + (3 - i)] = static_cast<uint8_t>(v >> (8 * i)); };
    auto patch64 = [&](size_t pos, uint64_t v) { for (int i = 7; i >= 0; --i) bc[pos + (7 - i)] = static_cast<uint8_t>(v >> (8 * i)); };

    // ===== 外部函数表注册（libffi 自身 API 原样暴露 + 宿主演示函数） =====
    // 注：本机 libffi 3.4.2 没有 ffi_get_version / ffi_get_version_number /
    // ffi_get_default_abi / ffi_get_closure_size（非标准 libffi 符号），故不注册；
    // ffi_java_raw_call 在本平台被 ffi.h 的 !FFI_NATIVE_RAW_API 保护掉，见下方条件编译。
    // 每个 cif 都用 libffi 原始 ffi_prep_cif 预生成（存全局静态），函数指针原样登记。
    // idx 0：宿主演示函数 host_mix（u8,u16,u32,i64,f32,f64 → i64）
    REG_FN(0, &ffi_type_sint64, host_mix,
           &ffi_type_uint8, &ffi_type_uint16, &ffi_type_uint32, &ffi_type_sint64,
           &ffi_type_float, &ffi_type_double);
    // idx 1..：libffi 自身（薄透传，一个字节不改）
    REG_FN(1, &ffi_type_sint, ffi_prep_cif,                                  // ffi_status(ffi_cif*,ffi_abi,uint,ffi_type*,ffi_type**)
           &ffi_type_pointer, &ffi_type_sint, &ffi_type_uint, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(2, &ffi_type_sint, ffi_prep_cif_var,                              // ffi_status(...,uint nfixed,uint ntotal,...)
           &ffi_type_pointer, &ffi_type_sint, &ffi_type_uint, &ffi_type_uint, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(3, &ffi_type_void, ffi_call,                                      // void(ffi_cif*,void(*)(void),void*,void**)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(4, &ffi_type_sint, ffi_get_struct_offsets,                        // ffi_status(ffi_abi,ffi_type*,size_t*)
           &ffi_type_sint, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(5, &ffi_type_void, ffi_raw_call,                                  // void(ffi_cif*,void(*)(void),void*,ffi_raw*)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(6, &ffi_type_void, ffi_ptrarray_to_raw,                           // void(ffi_cif*,void**,ffi_raw*)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(7, &ffi_type_void, ffi_raw_to_ptrarray,                           // void(ffi_cif*,ffi_raw*,void**)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(8, &ffi_type_ulong, ffi_raw_size, &ffi_type_pointer);             // size_t(ffi_cif*)
    REG_FN(9, &ffi_type_void, ffi_java_ptrarray_to_raw,                      // void(ffi_cif*,void**,ffi_java_raw*)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(10, &ffi_type_void, ffi_java_raw_to_ptrarray,                     // void(ffi_cif*,ffi_java_raw*,void**)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(11, &ffi_type_ulong, ffi_java_raw_size, &ffi_type_pointer);       // size_t(ffi_cif*)
#if !FFI_NATIVE_RAW_API // 与 ffi.h 相同条件：本平台(x86-64)原生 raw，此函数未导出
    REG_FN(12, &ffi_type_void, ffi_java_raw_call,                            // void(ffi_cif*,void(*)(void),void*,ffi_java_raw*)
           &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer, &ffi_type_pointer);
#endif
    // idx 13..：内存 rwx 属性 + 动态链接（路径来自客人伪造的 std::string）
    REG_FN(13, &ffi_type_sint, mprotect,                                     // int mprotect(void*, size_t, int)
           &ffi_type_pointer, &ffi_type_ulong, &ffi_type_sint);
    REG_FN(14, &ffi_type_pointer, dlopen_std,                                // void* dlopen_std(const std::string&, int)
           &ffi_type_pointer, &ffi_type_sint);
    REG_FN(15, &ffi_type_pointer, dlsym,                                     // void* dlsym(void*, const char*)
           &ffi_type_pointer, &ffi_type_pointer);
    REG_FN(16, &ffi_type_sint, dlclose, &ffi_type_pointer);                  // int dlclose(void*)
    REG_FN(17, &ffi_type_ulong, collect_externs,                             // uint64_t collect_externs(void*, uint64_t)
           &ffi_type_pointer, &ffi_type_ulong);

    put(OP_STACK_INIT); put32(8192); put32(64); // 栈 8192：dlopen 演示槽位用到 ~4570，4096 会越界

    // —— MOVI_U32 压常量 + 加法：3+4=7，左移到高字节加 '0' → 打印 '7' ——
    put(OP_MOVI_U32); put32(3);
    put(OP_MOVI_U32); put32(4);
    put(OP_ADD_U32);                               // 7
    put(OP_MOVI_U32); put32(24);
    put(OP_SHL_U32);                               // 7 << 24 → 高字节
    put(OP_MOVI_U32); put32(0x30000000);           // '0' 也放高字节
    put(OP_ADD_U32);                               // 0x37000000
    put(OP_COUT_CHAR8);                            // 打印 '7'

    // —— 转换 + 开方：9 → f64 → sqrt → u32（3）→ 加 '0' → 打印 '3' ——
    put(OP_MOVI_U32); put32(9);
    put(OP_CVT_U32_F64); put(OP_SQRT_F64); put(OP_CVT_F64_U32);  // 3
    put(OP_MOVI_U32); put32(24);
    put(OP_SHL_U32);
    put(OP_MOVI_U32); put32(0x30000000);
    put(OP_ADD_U32);                               // 0x33000000
    put(OP_COUT_CHAR8);                            // 打印 '3'

    // —— 移位：1 << 4 = 16，加 '0' → 高字节 0x40 = '@' ——
    put(OP_MOVI_U32); put32(1);
    put(OP_MOVI_U32); put32(4);
    put(OP_SHL_U32);                               // 16
    put(OP_MOVI_U32); put32(24);
    put(OP_SHL_U32);                               // 16 << 24 → 高字节
    put(OP_MOVI_U32); put32(0x30000000);
    put(OP_ADD_U32);                               // 0x40000000
    put(OP_COUT_CHAR8);                            // 打印 '@'

    // —— 比较：5==5 → 压 U8 的 0/1，原样打印（控制字符） ——
    put(OP_MOVI_U32); put32(5);
    put(OP_MOVI_U32); put32(5);
    put(OP_CMP_EQ_U32);
    put(OP_COUT_CHAR8);                            // 打印 \x01
    put(OP_MOVI_U32); put32(5);
    put(OP_MOVI_U32); put32(6);
    put(OP_CMP_LT_U32);
    put(OP_COUT_CHAR8);                            // 打印 \x01（5<6 为真）
    put(OP_MOVI_U32); put32(5);
    put(OP_MOVI_U32); put32(5);
    put(OP_CMP_GT_U32);
    put(OP_COUT_CHAR8);                            // 打印 \x00（5>5 为假）

    // —— 位运算：0x0F0F0F0F & 0x00FF00FF = 0x000F000F，打印高字节 \x00 ——
    put(OP_MOVI_U32); put32(0x0F0F0F0F);
    put(OP_MOVI_U32); put32(0x00FF00FF);
    put(OP_AND_U32); put(OP_COUT_CHAR8);

    // —— 地址部分：下面的数值都从 LEA 取得的堆地址派生 ——
    put(OP_NEW_ARRAY); put64(64); put32(1024);           // 数组 P，指针 → 栈槽1024
    put(OP_NEW_ARRAY); put64(64); put32(1032);           // 数组 Q，指针 → 栈槽1032

    // —— 寻址/指针运算：P+Q、P-Q，打印高位字节（栈顶先弹） ——
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_ADD_PTR); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_SUB_PTR); put(OP_COUT_CHAR8);

    // —— 大小比较：P<Q、P==Q、P>Q，各压 U8 的 0/1 ——
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_CMP_LT_PTR); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_CMP_EQ_PTR); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_CMP_GT_PTR); put(OP_COUT_CHAR8);

    // —— 四则运算（u32）：取 P、Q 的高 32 位做 + - * /，打印高位字节 ——
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_ADD_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_SUB_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_MUL_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1032); put(OP_LEA); put(1); put64(1024);   // Q 高32位 / P 高32位
    put(OP_DIV_U32); put(OP_COUT_CHAR8);

    // —— 浮点链：u32 → f64 → sqrt/log → u32 ——
    put(OP_LEA); put(1); put64(1024);
    put(OP_CVT_U32_F64); put(OP_SQRT_F64); put(OP_CVT_F64_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024);
    put(OP_CVT_U32_F64); put(OP_LOG_F64); put(OP_CVT_F64_I32); put(OP_COUT_CHAR8);
    // 反向：i32 → f64 → i32、u32 → f32 → u32 往返
    put(OP_LEA); put(1); put64(1024);
    put(OP_CVT_I32_F64); put(OP_CVT_F64_I32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024);
    put(OP_CVT_U32_F32); put(OP_CVT_F32_U32); put(OP_COUT_CHAR8);

    // —— 位运算（只 uint）：<< & | ~ 和 >>（uint 逻辑 / int 算术） ——
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);   // 值 << 移位量
    put(OP_SHL_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_AND_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_OR_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024);
    put(OP_NOT_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_SHR_U32); put(OP_COUT_CHAR8);
    put(OP_LEA); put(1); put64(1024); put(OP_LEA); put(1); put64(1032);
    put(OP_SHR_I32); put(OP_COUT_CHAR8);

    // —— 控制流：JMP 跳过一段；IF_GOTO(cond=0 时跳转) 再跳一次 ——
    put(OP_LEA); put(1); put64(1024); put(OP_COUT_CHAR8);   // 打印一次
    size_t jmpPos = bc.size();
    put(OP_JMP); put32(0);                               // 占位：向前跳
    put(OP_LEA); put(1); put64(1032); put(OP_COUT_CHAR8);   // 被跳过的段
    size_t jmpTgt = bc.size();
    patch32(jmpPos + 1, static_cast<uint32_t>(jmpTgt));
    size_t ifPos = bc.size();
    put(OP_IF_GOTO); put(0); put32(0);                   // cond=0 → 跳转
    put(OP_LEA); put(1); put64(1024); put(OP_COUT_CHAR8);   // 被跳过的段
    size_t ifTgt = bc.size();
    patch32(ifPos + 2, static_cast<uint32_t>(ifTgt));

    // —— CHAR16/CHAR32 输出（无 ostream 字符重载，按数值打印栈顶的 16/32 位） ——
    put(OP_LEA); put(1); put64(1024); put(OP_COUT_CHAR16);
    put(OP_LEA); put(1); put64(1024); put(OP_COUT_CHAR32);

    // —— REG 寄存器族：MOVI/MOV/PUSH/POP 路径 ——
    put(OP_REG_MOVI_U8); put(0); put(65);                // R0 = 'A'
    put(OP_REG_PUSH_U8); put(0);                         // 栈：'A'
    put(OP_REG_MOV); put(1); put(0);                     // R1 = R0
    put(OP_REG_PUSH_U8); put(1); put(OP_COUT_CHAR8);     // 打印 'A'
    put(OP_REG_MOVI_U32); put(3); put32(66);             // R3 = 66（'B'）
    put(OP_REG_PUSH_U8); put(3); put(OP_COUT_CHAR8);     // 打印 'B'
    put(OP_REG_POP_U8); put(4);                          // 弹栈 'A' → R4
    put(OP_REG_PUSH_U16); put(4); put(OP_COUT_CHAR16);   // 按 u16 数值打印 65

    // —— REG LOAD/STORE mode0：直接读写 Stack[base+off] ——
    put(OP_REG_STORE_U8); put(0); put(0); put64(2048);   // Stack[2048] = R0 低8位
    put(OP_REG_LOAD_U8); put(2); put(0); put64(2048);    // R2 = Stack[2048]
    put(OP_REG_PUSH_U8); put(2); put(OP_COUT_CHAR8);     // 打印 'A'

    // —— REG LOAD/STORE mode1：栈槽里存指针，解引用读写 ——
    put(OP_LEA); put(0); put64(3000);                    // 压入 Stack+3000 地址
    put(OP_REG_POP_U64); put(5);                         // R5 = 该地址
    put(OP_REG_STORE_U64); put(5); put(0); put64(2040);  // 栈槽2040 = R5（指针）
    put(OP_REG_STORE_U8); put(0); put(1); put64(2040);   // *(栈槽2040)=R0 → Stack[3000]='A'
    put(OP_REG_LOAD_U8); put(6); put(1); put64(2040);    // R6 = *(栈槽2040)
    put(OP_REG_PUSH_U8); put(6); put(OP_COUT_CHAR8);     // 打印 'A'

    // —— GET_ADDRS：一条指令压入 bytecode / size / DataSave / 管理器 四个地址 ——
    put(OP_GET_ADDRS);                                   // 栈：[bcBase, sizeAddr, dsAddr, mgr]
    put(OP_REG_POP_U64); put(12);                        // R12 = 管理器地址（本演示不用）
    put(OP_REG_POP_U64); put(7);                         // R7 = DataSave 对象地址
    put(OP_REG_POP_U64); put(8);                         // R8 = size 字段地址
    put(OP_REG_STORE_U64); put(8); put(0); put64(2064);  // 栈槽2064 = size 地址
    put(OP_REG_LOAD_U32); put(9); put(1); put64(2064);   // R9 = *(size地址) 低32位 = 缓冲区总长
    put(OP_REG_PUSH_U32); put(9); put(OP_COUT_CHAR32);   // 打印缓冲区大小
    put(OP_REG_PUSH_U16); put(7); put(OP_COUT_CHAR16);   // 打印 DataSave 地址低16位

    // —— JMP_IND 间接跳转：bcBase + 目标偏移 = 绝对地址 → 弹栈跳转 ——
    // 注意：ADD_U64 弹 8 字节操作数，偏移必须按 u64 完整压栈（MOVI_U32 只压 4 字节会错位）
    size_t moviPos = bc.size();
    put(OP_REG_MOVI_U64); put(11); put64(0);           // 占位：R11 = 目标偏移
    put(OP_REG_PUSH_U64); put(11);                     // 压入 8 字节偏移
    put(OP_ADD_U64);                                   // bcBase + 偏移 = 绝对地址
    put(OP_JMP_IND);                                   // 间接跳转
    size_t jmpIndTarget = bc.size();                   // 目标 = JMP_IND 之后
    patch64(moviPos + 2, static_cast<uint64_t>(jmpIndTarget));
    put(OP_REG_MOVI_U8); put(10); put(74);             // R10 = 'J'
    put(OP_REG_PUSH_U8); put(10); put(OP_COUT_CHAR8);  // 打印 'J'（证明跳转成功）

    // —— 原子指令冒烟测试（单线程，U32/U64 全覆盖）——
    // 槽2080 作 U32 原子变量（8 字节对齐），槽2088 作 U64 原子变量
    put(OP_REG_MOVI_U32); put(0); put32(0xDEADBEEF);
    put(OP_ATOMIC_STORE_U32); put(0); put(0); put64(2080);    // 原子写初值
    put(OP_ATOMIC_LOAD_U32); put(1); put(0); put64(2080);     // 原子读
    put(OP_REG_PUSH_U16); put(1); put(OP_COUT_CHAR16);        // 0xBEEF = 48879
    put(OP_REG_MOVI_U32); put(2); put32(0x12345678);
    put(OP_ATOMIC_XCHG_U32); put(2); put(0); put64(2080);     // R2=旧值0xDEADBEEF
    put(OP_REG_PUSH_U16); put(2); put(OP_COUT_CHAR16);        // 48879
    put(OP_ATOMIC_LOAD_U32); put(3); put(0); put64(2080);     // 现在 0x12345678
    put(OP_REG_PUSH_U16); put(3); put(OP_COUT_CHAR16);        // 0x5678 = 22136
    // CAS 成功：expected=0x12345678（弹栈），desired=R4=0xCAFEBABE
    put(OP_REG_MOVI_U32); put(4); put32(0xCAFEBABE);
    put(OP_REG_MOVI_U32); put(5); put32(0x12345678);
    put(OP_REG_PUSH_U32); put(5);
    put(OP_ATOMIC_CAS_U32); put(4); put(0); put64(2080);      // 成功：压 flag=1，R4=旧值
    put(OP_COUT_CHAR8);                                       // 打印 \x01
    put(OP_REG_PUSH_U16); put(4); put(OP_COUT_CHAR16);        // 0x5678 = 22136
    // CAS 失败：expected 错 → flag=0，R6=当前值 0xCAFEBABE
    put(OP_REG_MOVI_U32); put(6); put32(0x11111111);
    put(OP_REG_MOVI_U32); put(5); put32(0x99999999);
    put(OP_REG_PUSH_U32); put(5);
    put(OP_ATOMIC_CAS_U32); put(6); put(0); put64(2080);
    put(OP_COUT_CHAR8);                                       // 打印 \x00
    put(OP_REG_PUSH_U16); put(6); put(OP_COUT_CHAR16);        // 0xBABE = 47806
    // fetch_add：R7=5，槽 0xCAFEBABE+5=0xCAFEBAC3
    put(OP_REG_MOVI_U32); put(7); put32(5);
    put(OP_ATOMIC_ADD_U32); put(7); put(0); put64(2080);      // R7=旧值
    put(OP_REG_PUSH_U16); put(7); put(OP_COUT_CHAR16);        // 47806
    put(OP_ATOMIC_LOAD_U32); put(8); put(0); put64(2080);
    put(OP_REG_PUSH_U16); put(8); put(OP_COUT_CHAR16);        // 0xBAC3 = 47811
    // U64 路径：STORE / LOAD / CAS / XCHG
    put(OP_REG_MOVI_U64); put(9); put64(0x1122334455667788);
    put(OP_ATOMIC_STORE_U64); put(9); put(0); put64(2088);
    put(OP_ATOMIC_LOAD_U64); put(10); put(0); put64(2088);
    put(OP_REG_PUSH_U16); put(10); put(OP_COUT_CHAR16);       // 0x7788 = 30600
    put(OP_REG_MOVI_U64); put(11); put64(0x1122334455667788); // expected
    put(OP_REG_PUSH_U64); put(11);
    put(OP_REG_MOVI_U64); put(12); put64(0xAABBCCDDEEFF0011); // desired
    put(OP_ATOMIC_CAS_U64); put(12); put(0); put64(2088);     // 成功：flag=1，R12=旧值
    put(OP_COUT_CHAR8);                                       // 打印 \x01
    put(OP_REG_PUSH_U16); put(12); put(OP_COUT_CHAR16);       // 0x7788 = 30600
    put(OP_REG_MOVI_U64); put(13); put64(0xFFFFFFFFFFFFFFFF);
    put(OP_ATOMIC_XCHG_U64); put(13); put(0); put64(2088);    // R13=旧值 0xAABBCCDDEEFF0011
    put(OP_REG_PUSH_U16); put(13); put(OP_COUT_CHAR16);       // 0x0011 = 17

    // ===== EXTERN_CALL：调用宿主函数 host_mix（libffi 原样暴露） =====
    // 参数缓冲（槽 3000 起）按 EXTERN_CALL 语义【连续紧凑排布】（ap += size，无对齐填充）：
    //   u8=5 @3000(1B) u16=40 @3001(2B) u32=100 @3003(4B) i64=10 @3007(8B)
    //   f32=1.0 @3015(4B) f64=2.0 @3019(8B)  → 5+40+100+10+1+2 = 158（i64 写槽 3100）
    put(OP_REG_MOVI_U32); put(0); put32(5);
    put(OP_REG_STORE_U8);  put(0); put(0); put64(3000);      // buf[3000]   u8=5
    put(OP_REG_MOVI_U32); put(1); put32(40);
    put(OP_REG_STORE_U16); put(1); put(0); put64(3001);      // buf[3001]   u16=40
    put(OP_REG_MOVI_U32); put(2); put32(100);
    put(OP_REG_STORE_U32); put(2); put(0); put64(3003);      // buf[3003]   u32=100
    put(OP_REG_MOVI_U64); put(3); put64(10);
    put(OP_REG_STORE_U64); put(3); put(0); put64(3007);      // buf[3007]   i64=10
    put(OP_REG_MOVI_U32); put(4); put32(0x3F800000);         // 1.0f 位模式
    put(OP_REG_STORE_U32); put(4); put(0); put64(3015);      // buf[3015]   f32=1.0
    put(OP_REG_MOVI_U64); put(5); put64(0x4000000000000000); // 2.0 位模式
    put(OP_REG_STORE_U64); put(5); put(0); put64(3019);      // buf[3019]   f64=2.0
    put(OP_EXTERN_CALL); put(0); put32(3000); put32(3100);   // host_mix(buf, →3100)
    put(OP_REG_LOAD_U32); put(6); put(0); put64(3100);       // 返回 i64 低 32 位
    put(OP_REG_PUSH_U32); put(6); put(OP_COUT_CHAR32);       // 打印 158

    // ===== EXTERN_CALL：客人程序直接调用 libffi 的 ffi_prep_cif（idx=1） =====
    // 客人在自己栈里伪造两个 ffi_type（uint32/sint32，布局见 ffi.h），
    // 拼一个 ffi_cif，用 EXTERN_CALL 调 ffi_prep_cif 原样预处理，打印状态(应=0=FFI_OK)。
    // ffi_type 布局：{size_t size; u16 alignment; u16 type; ffi_type** elements;}（含4字节填充）
    put(OP_REG_MOVI_U64); put(0); put64(4);
    put(OP_REG_STORE_U64); put(0); put(0); put64(3200);      // ft_u32.size = 4
    put(OP_REG_MOVI_U32); put(1); put32(4);
    put(OP_REG_STORE_U16); put(1); put(0); put64(3208);      // ft_u32.alignment = 4
    put(OP_REG_MOVI_U32); put(2); put32(9);                  // FFI_TYPE_UINT32 = 9
    put(OP_REG_STORE_U16); put(2); put(0); put64(3210);      // ft_u32.type = 9
    put(OP_REG_MOVI_U64); put(3); put64(0);
    put(OP_REG_STORE_U64); put(3); put(0); put64(3216);      // ft_u32.elements = 0
    put(OP_REG_MOVI_U64); put(0); put64(4);
    put(OP_REG_STORE_U64); put(0); put(0); put64(3240);      // ft_s32.size = 4
    put(OP_REG_MOVI_U32); put(1); put32(4);
    put(OP_REG_STORE_U16); put(1); put(0); put64(3248);      // ft_s32.alignment = 4
    put(OP_REG_MOVI_U32); put(2); put32(10);                 // FFI_TYPE_SINT32 = 10
    put(OP_REG_STORE_U16); put(2); put(0); put64(3250);      // ft_s32.type = 10
    put(OP_REG_MOVI_U64); put(3); put64(0);
    put(OP_REG_STORE_U64); put(3); put(0); put64(3256);      // ft_s32.elements = 0
    // atypes[]（槽 3280）：[&ft_u32, &ft_s32]
    put(OP_LEA); put(0); put64(3200);
    put(OP_REG_POP_U64); put(7);                             // R7 = &ft_u32
    put(OP_REG_STORE_U64); put(7); put(0); put64(3280);
    put(OP_LEA); put(0); put64(3240);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3288);
    // ffi_cif（槽 3320）：{abi=FFI_DEFAULT_ABI(2), nargs=2, arg_types=&atypes, rtype=&ft_s32, bytes=0, flags=0}
    put(OP_REG_MOVI_U32); put(0); put32(2);                  // FFI_DEFAULT_ABI = FFI_UNIX64 = 2 (x86-64)
    put(OP_REG_STORE_U32); put(0); put(0); put64(3320);      // cif.abi
    put(OP_REG_MOVI_U32); put(1); put32(2);
    put(OP_REG_STORE_U32); put(1); put(0); put64(3324);      // cif.nargs
    put(OP_LEA); put(0); put64(3280);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3328);      // cif.arg_types
    put(OP_LEA); put(0); put64(3240);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3336);      // cif.rtype
    put(OP_REG_MOVI_U32); put(0); put32(0);
    put(OP_REG_STORE_U32); put(0); put(0); put64(3344);      // cif.bytes（libffi 会算）
    put(OP_REG_STORE_U32); put(0); put(0); put64(3348);      // cif.flags
    // ffi_prep_cif 的参数（槽 3400）：[&cif, abi=2, nargs=2, rtype=&ft_s32, atypes=&atypes]
    put(OP_LEA); put(0); put64(3320);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3400);
    put(OP_REG_MOVI_U32); put(0); put32(2);
    put(OP_REG_STORE_U32); put(0); put(0); put64(3408);      // abi
    put(OP_REG_MOVI_U32); put(1); put32(2);
    put(OP_REG_STORE_U32); put(1); put(0); put64(3412);      // nargs
    put(OP_LEA); put(0); put64(3240);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3416);      // rtype
    put(OP_LEA); put(0); put64(3280);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3424);      // atypes
    put(OP_EXTERN_CALL); put(1); put32(3400); put32(3500);   // ffi_prep_cif(→状态写3500)
    put(OP_REG_LOAD_U32); put(6); put(0); put64(3500);
    put(OP_REG_PUSH_U32); put(6); put(OP_COUT_CHAR32);       // 打印 FFI_OK=0

    // ===== EXTERN_CALL：客人再调 ffi_raw_size（idx=8），吃刚预处理好的 cif =====
    // ffi_raw_size(ffi_cif*) → size_t：2×u32 各按 8 字节 raw 对齐 → 16
    put(OP_LEA); put(0); put64(3320);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(3520);      // 参数：[&cif]
    put(OP_EXTERN_CALL); put(8); put32(3520); put32(3536);   // ffi_raw_size(→3536)
    put(OP_REG_LOAD_U32); put(6); put(0); put64(3536);
    put(OP_REG_PUSH_U32); put(6); put(OP_COUT_CHAR32);       // 打印 16

    // ===== FUNC_CALL：调用 VM 函数（FunctionSave，动态加载，两 u32 求和） =====
    // 宿主把 FunctionSave* 存在 Manager.hostFn（偏移16）里，客人程序经 GET_ADDRS
    // 取到管理器指针 → 读出函数指针 → 存进栈槽 3600，然后 FUNC_CALL 调用。
    put(OP_GET_ADDRS);                                   // [bc,size,ds,mgr]
    put(OP_REG_POP_U64); put(12);                        // R12 = 管理器指针
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_POP_U64); put(8);
    put(OP_REG_POP_U64); put(3);                         // R3 = bcBase（本段不用）
    put(OP_REG_MOVI_U64); put(2); put64(16);             // Manager.hostFn 偏移
    put(OP_REG_PUSH_U64); put(12);
    put(OP_REG_PUSH_U64); put(2);
    put(OP_ADD_PTR);                                     // 管理器+16 = &hostFn
    put(OP_REG_POP_U64); put(1);                         // R1 = &manager->hostFn
    put(OP_REG_STORE_U64); put(1); put(0); put64(3584);  // 槽3584 = &hostFn
    put(OP_REG_LOAD_U64); put(0); put(1); put64(3584);   // R0 = hostFn（FunctionSave*）
    put(OP_REG_STORE_U64); put(0); put(0); put64(3600);  // 槽3600 = 函数指针
    // 参数（连续排布）：u32=123 @3620, u32=456 @3624
    put(OP_REG_MOVI_U32); put(1); put32(123);
    put(OP_REG_STORE_U32); put(1); put(0); put64(3620);
    put(OP_REG_MOVI_U32); put(2); put32(456);
    put(OP_REG_STORE_U32); put(2); put(0); put64(3624);
    put(OP_FUNC_CALL); put32(3600); put32(3620); put32(3640); // fnSum(参数, →3640)
    put(OP_REG_LOAD_U32); put(3); put(0); put64(3640);   // 结果 123+456
    put(OP_REG_PUSH_U32); put(3); put(OP_COUT_CHAR32);   // 打印 579

    // ===== EXTERN_CALL：mprotect 修改内存 rwx 属性 =====
    // 页对齐缓冲由宿主 mmap 并经 Manager.rwxBuf（偏移24）交给客人程序；
    // 客人程序原样调用 POSIX mprotect：RW→R→RWX，打印状态（应各为 0）。
    put(OP_GET_ADDRS);                                   // [bc,size,ds,mgr]
    put(OP_REG_POP_U64); put(12);                        // R12 = 管理器
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_POP_U64); put(8);
    put(OP_REG_POP_U64); put(3);
    put(OP_REG_MOVI_U64); put(2); put64(24);             // Manager.rwxBuf 偏移
    put(OP_REG_PUSH_U64); put(12);
    put(OP_REG_PUSH_U64); put(2);
    put(OP_ADD_PTR);                                     // 管理器+24 = &rwxBuf
    put(OP_REG_POP_U64); put(1);
    put(OP_REG_STORE_U64); put(1); put(0); put64(3680);  // 槽3680 = &rwxBuf
    put(OP_REG_LOAD_U64); put(0); put(1); put64(3680);   // R0 = rwxBuf（页对齐）
    put(OP_REG_STORE_U64); put(0); put(0); put64(3700);  // 槽3700 = rwxBuf
    // mprotect(rwxBuf, 4096, PROT_READ=1) → 0
    put(OP_REG_STORE_U64); put(0); put(0); put64(3710);  // args[0] = 地址
    put(OP_REG_MOVI_U64); put(1); put64(4096);
    put(OP_REG_STORE_U64); put(1); put(0); put64(3718);  // args[8] = 长度
    put(OP_REG_MOVI_U32); put(2); put32(1);              // PROT_READ
    put(OP_REG_STORE_U32); put(2); put(0); put64(3726);  // args[16] = prot
    put(OP_EXTERN_CALL); put(13); put32(3710); put32(3750);
    put(OP_REG_LOAD_U32); put(3); put(0); put64(3750);
    put(OP_REG_PUSH_U32); put(3); put(OP_COUT_CHAR32);   // 打印 0
    // mprotect(rwxBuf, 4096, PROT_READ|WRITE|EXEC=7) → 0
    put(OP_REG_MOVI_U32); put(2); put32(7);
    put(OP_REG_STORE_U32); put(2); put(0); put64(3726);
    put(OP_EXTERN_CALL); put(13); put32(3710); put32(3750);
    put(OP_REG_LOAD_U32); put(3); put(0); put64(3750);
    put(OP_REG_PUSH_U32); put(3); put(OP_COUT_CHAR32);   // 打印 0

    // ===== EXTERN_CALL：伪造 std::string 路径 → 动态链接 libc =====
    // 客人在自己栈里按 libstdc++ SSO 布局伪造 32 字节 std::string（槽4000）：
    //   [0]  _M_p = &buf(4016)   [8] length   [16] 本地缓冲 "libc.so.6\0"
    // 然后 dlopen_std(&str, RTLD_NOW) → dlsym("strlen") → 客人自备 ffi_type/cif
    // 调 ffi_prep_cif + ffi_call 真正调用 strlen("hello") → 5
    put(OP_LEA); put(0); put64(4016);
    put(OP_REG_POP_U64); put(0);                        // R0 = &buf
    put(OP_REG_STORE_U64); put(0); put(0); put64(4000); // _M_p = &buf
    put(OP_REG_MOVI_U64); put(1); put64(9);
    put(OP_REG_STORE_U64); put(1); put(0); put64(4008); // length = 9
    { // 本地缓冲："libc.so.6\0"（用 R2 逐字节写）
        const char* libc = "libc.so.6";
        for (size_t i = 0; i <= strlen(libc); ++i) {
            put(OP_REG_MOVI_U32); put(2); put32(static_cast<uint8_t>(libc[i]));
            put(OP_REG_STORE_U8); put(2); put(0); put64(4016 + i);
        }
    }
    // dlopen_std(&str, RTLD_NOW=2) → handle@4150
    put(OP_LEA); put(0); put64(4000);
    put(OP_REG_POP_U64); put(0);
    put(OP_REG_STORE_U64); put(0); put(0); put64(4100); // args[0] = &str
    put(OP_REG_MOVI_U32); put(1); put32(2);             // RTLD_NOW
    put(OP_REG_STORE_U32); put(1); put(0); put64(4108); // args[8] = flags
    put(OP_EXTERN_CALL); put(14); put32(4100); put32(4150); // dlopen_std → handle
    // 符号名 "strlen\0"（槽4120）
    {
        const char* sym = "strlen";
        for (size_t i = 0; i <= strlen(sym); ++i) {
            put(OP_REG_MOVI_U32); put(2); put32(static_cast<uint8_t>(sym[i]));
            put(OP_REG_STORE_U8); put(2); put(0); put64(4120 + i);
        }
    }
    // dlsym(handle, "strlen") → fn@4170
    put(OP_REG_LOAD_U64); put(0); put(0); put64(4150);  // R0 = handle
    put(OP_REG_STORE_U64); put(0); put(0); put64(4160);
    put(OP_LEA); put(0); put64(4120);
    put(OP_REG_POP_U64); put(1);
    put(OP_REG_STORE_U64); put(1); put(0); put64(4168);
    put(OP_EXTERN_CALL); put(15); put32(4160); put32(4170); // dlsym → fn
    // 客人伪造 ffi_type：ft_ptr(4200) {8,8,POINTER=14,0}、ft_u64(4240) {8,8,UINT64=11,0}
    auto putFfiType = [&](size_t off, uint64_t sz, uint32_t align, uint32_t ty) {
        put(OP_REG_MOVI_U64); put(0); put64(sz);
        put(OP_REG_STORE_U64); put(0); put(0); put64(off);
        put(OP_REG_MOVI_U32); put(1); put32(align);
        put(OP_REG_STORE_U16); put(1); put(0); put64(off + 8);
        put(OP_REG_MOVI_U32); put(2); put32(ty);
        put(OP_REG_STORE_U16); put(2); put(0); put64(off + 10);
        put(OP_REG_MOVI_U64); put(0); put64(0);
        put(OP_REG_STORE_U64); put(0); put(0); put64(off + 16);
    };
    putFfiType(4200, 8, 8, 14); // ft_ptr
    putFfiType(4240, 8, 8, 11); // ft_u64
    put(OP_LEA); put(0); put64(4200);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4280); // atypes[0] = &ft_ptr
    // cif（槽4320）：{abi=2, nargs=1, arg_types=&atypes, rtype=&ft_u64, bytes=0, flags=0}
    put(OP_REG_MOVI_U32); put(0); put32(2);
    put(OP_REG_STORE_U32); put(0); put(0); put64(4320);
    put(OP_REG_MOVI_U32); put(1); put32(1);
    put(OP_REG_STORE_U32); put(1); put(0); put64(4324);
    put(OP_LEA); put(0); put64(4280);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4328);
    put(OP_LEA); put(0); put64(4240);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4336);
    put(OP_REG_MOVI_U32); put(0); put32(0);
    put(OP_REG_STORE_U32); put(0); put(0); put64(4344);
    put(OP_REG_STORE_U32); put(0); put(0); put64(4348);
    // 输入串 "hello\0"（槽4360）
    {
        const char* hello = "hello";
        for (size_t i = 0; i <= strlen(hello); ++i) {
            put(OP_REG_MOVI_U32); put(2); put32(static_cast<uint8_t>(hello[i]));
            put(OP_REG_STORE_U8); put(2); put(0); put64(4360 + i);
        }
    }
    // ffi_prep_cif(&cif, 2, 1, &ft_u64, &atypes) → 状态@4440（应 0）
    put(OP_LEA); put(0); put64(4320);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4400);
    put(OP_REG_MOVI_U32); put(0); put32(2);
    put(OP_REG_STORE_U32); put(0); put(0); put64(4408);
    put(OP_REG_MOVI_U32); put(1); put32(1);
    put(OP_REG_STORE_U32); put(1); put(0); put64(4412);
    put(OP_LEA); put(0); put64(4240);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4416);
    put(OP_LEA); put(0); put64(4280);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4424);
    put(OP_EXTERN_CALL); put(1); put32(4400); put32(4440);
    put(OP_REG_LOAD_U32); put(3); put(0); put64(4440);
    put(OP_REG_PUSH_U32); put(3); put(OP_COUT_CHAR32);   // 打印 0（FFI_OK）
    // avalue（槽4480）=[&指针值槽4488]，槽4488=&hello：指针参数 avalue 必须指向
    // 存指针值的位置（8字节），不能直接指向字符串本体
    put(OP_LEA); put(0); put64(4360);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4488);   // 槽4488 = &hello（指针值）
    put(OP_LEA); put(0); put64(4488);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4480);   // avalue[0] = &指针值槽
    put(OP_LEA); put(0); put64(4320);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4520);
    put(OP_REG_LOAD_U64); put(0); put(0); put64(4170);   // R0 = dlsym 出的 strlen
    put(OP_REG_STORE_U64); put(0); put(0); put64(4528);
    put(OP_LEA); put(0); put64(4560);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4536);
    put(OP_LEA); put(0); put64(4480);
    put(OP_REG_POP_U64); put(7);
    put(OP_REG_STORE_U64); put(7); put(0); put64(4544);
    put(OP_EXTERN_CALL); put(3); put32(4520); put32(4560); // ffi_call → rvalue@4560
    put(OP_REG_LOAD_U32); put(3); put(0); put64(4560);
    put(OP_REG_PUSH_U32); put(3); put(OP_COUT_CHAR32);   // 打印 5（strlen("hello")）

    // ===== MEMCPY：内存 bulk 拷贝（mode0 栈槽 / mode1 指针解引用） =====
    // 源数据 "ABCD"（槽4600 起），mode0→mode0 拷 4 字节到 4700，逐字节打印
    {
        const char* abcd = "ABCD";
        for (size_t i = 0; i < 4; ++i) {
            put(OP_REG_MOVI_U32); put(0); put32(static_cast<uint8_t>(abcd[i]));
            put(OP_REG_STORE_U8); put(0); put(0); put64(4600 + i);
        }
    }
    put(OP_MEMCPY); put(0); put64(4700); put(0); put64(4600); put64(4); // 4700 ← 4600
    for (size_t i = 0; i < 4; ++i) {
        put(OP_REG_LOAD_U8); put(1); put(0); put64(4700 + i);
        put(OP_REG_PUSH_U8); put(1); put(OP_COUT_CHAR8);               // 打印 A B C D
    }
    // mode1→mode0：槽4800 = &4600（LEA），从指针拷贝 4 字节到 4720，逐字节打印
    put(OP_LEA); put(0); put64(4600);
    put(OP_REG_POP_U64); put(2);
    put(OP_REG_STORE_U64); put(2); put(0); put64(4800);                // 槽4800 = &源
    put(OP_MEMCPY); put(0); put64(4720); put(1); put64(4800); put64(4); // 4720 ← *(4800)
    for (size_t i = 0; i < 4; ++i) {
        put(OP_REG_LOAD_U8); put(1); put(0); put64(4720 + i);
        put(OP_REG_PUSH_U8); put(1); put(OP_COUT_CHAR8);               // 打印 A B C D
    }

    // ===== collect_externs：枚举刚 dlopen 的库里的全部函数符号指针 =====
    // 前一段已 dlopen("libc.so.6")（dlopen_std 记录了其加载基址）；
    // 这里再调一次确保在记录里，然后 collect_externs 把该库所有 STT_FUNC
    // 符号的 {名字指针, 地址} 写入堆缓冲（NEW_ARRAY 8192 字节 = 512 条 × 16B）。
    put(OP_LEA); put(0); put64(4000);                                  // 复用伪造的 "libc.so.6"
    put(OP_REG_POP_U64); put(0);
    put(OP_REG_STORE_U64); put(0); put(0); put64(4100);                // dlopen 参数 &str
    put(OP_REG_MOVI_U32); put(1); put32(2);                            // RTLD_NOW
    put(OP_REG_STORE_U32); put(1); put(0); put64(4108);
    put(OP_EXTERN_CALL); put(14); put32(4100); put32(4150);            // dlopen_std → handle
    put(OP_NEW_ARRAY); put64(8192); put32(5200);                       // 512 条 × 16B 堆缓冲，指针→槽5200
    put(OP_REG_LOAD_U64); put(0); put(0); put64(5200);                 // R0 = 缓冲地址
    put(OP_REG_STORE_U64); put(0); put(0); put64(5300);                // args[0] = 缓冲
    put(OP_REG_MOVI_U64); put(1); put64(512);
    put(OP_REG_STORE_U64); put(1); put(0); put64(5308);                // args[8] = cap=512
    put(OP_EXTERN_CALL); put(17); put32(5300); put32(5400);            // collect_externs → 条数
    put(OP_REG_LOAD_U32); put(2); put(0); put64(5400);
    put(OP_REG_PUSH_U32); put(2); put(OP_COUT_CHAR32);                 // 打印函数符号条数
    // 第一条：名字指针(条目+0) → 槽5460，取名字首字符打印；地址(条目+8) 低16位打印
    // 条目缓冲在堆上（槽5200 存堆指针），地址字段 = buf+8
    put(OP_REG_LOAD_U64); put(3); put(0); put64(5200);                 // R3 = 缓冲（堆）
    put(OP_REG_MOVI_U64); put(0); put64(8);
    put(OP_REG_PUSH_U64); put(3);
    put(OP_REG_PUSH_U64); put(0);
    put(OP_ADD_PTR);                                                   // buf+8
    put(OP_REG_POP_U64); put(3);
    put(OP_REG_STORE_U64); put(3); put(0); put64(5448);                // 槽5448 = 条目+8
    put(OP_REG_LOAD_U64); put(4); put(1); put64(5448);                 // R4 = 条目[0].地址
    put(OP_REG_PUSH_U16); put(4); put(OP_COUT_CHAR16);                 // 地址低16位
    put(OP_REG_LOAD_U64); put(5); put(0); put64(5200);                 // R5 = 缓冲地址
    put(OP_REG_STORE_U64); put(5); put(0); put64(5460);                // 槽5460 = 缓冲地址
    put(OP_REG_LOAD_U64); put(4); put(1); put64(5460);                 // R4 = 条目[0].名字指针（缓冲首8字节）
    put(OP_REG_STORE_U64); put(4); put(0); put64(5470);                // 槽5470 = 名字指针
    put(OP_REG_LOAD_U8); put(4); put(1); put64(5470);                  // R4 = 名字首字符
    put(OP_REG_PUSH_U8); put(4); put(OP_COUT_CHAR8);                   // 打印名字首字符

    put(OP_FREE_ARRAY); put32(1024);
    put(OP_FREE_ARRAY); put32(1032);
    put(OP_FREE_ARRAY); put32(5200); // collect_externs 的条目缓冲
    put(OP_END);

    DataSave ds;
    ds.manager = std::make_shared<Manager>(); // 单线程演示也给一个管理器（GET_ADDRS 第4项）

    // ===== 动态加载 FunctionSave：写函数字节码到文件 → loadFromFile 装载 =====
    // VM 函数：两个 u32 求和。字节码以 STACK_INIT 开头（自建栈），R15=参数指针、
    // R14=返回指针（FUNC_CALL 预置），END 收尾（silent 抑制打印）。
    // 注意：指针槽要放在高槽位（2000+），因为 PUSH/POP 算术从 sp=0 向上增长，
    // 低槽会被覆盖（这是函数编写约定，不是指令问题）。
    std::vector<uint8_t> fbc;
    auto fput   = [&](uint8_t b)              { fbc.push_back(b); };
    auto fput32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) fbc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto fput64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) fbc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    fput(OP_STACK_INIT); fput32(4096); fput32(16);              // 自建栈
    fput(OP_REG_STORE_U64); fput(15); fput(0); fput64(2000);    // 槽2000 = 参数指针
    fput(OP_REG_STORE_U64); fput(14); fput(0); fput64(2008);    // 槽2008 = 返回指针
    fput(OP_REG_LOAD_U32); fput(0); fput(1); fput64(2000);      // R0 = arg0（mode1：解引用）
    fput(OP_REG_LOAD_U64); fput(1); fput(0); fput64(2000);      // R1 = argPtr（mode0：槽里的指针值）
    fput(OP_REG_MOVI_U64); fput(2); fput64(4);                  // R2 = 4
    fput(OP_REG_PUSH_U64); fput(1);
    fput(OP_REG_PUSH_U64); fput(2);
    fput(OP_ADD_PTR);                                           // argPtr+4
    fput(OP_REG_POP_U64); fput(1);
    fput(OP_REG_STORE_U64); fput(1); fput(0); fput64(2016);     // 槽2016 = argPtr+4
    fput(OP_REG_LOAD_U32); fput(3); fput(1); fput64(2016);      // R3 = arg1
    fput(OP_REG_PUSH_U32); fput(0);
    fput(OP_REG_PUSH_U32); fput(3);
    fput(OP_ADD_U32);                                           // arg0+arg1
    fput(OP_REG_POP_U32); fput(4);
    fput(OP_REG_STORE_U32); fput(4); fput(1); fput64(2008);     // *返回指针 = 和
    fput(OP_END);
    FunctionSave fnTmp(fbc.data(), fbc.size(), 0, 8, 4);        // argSize=8, retSize=4
    fnTmp.saveToFile("fn_sum.bc");                              // 写文件（动态加载源）
    FunctionSave fnSum = FunctionSave::loadFromFile("fn_sum.bc"); // 从文件动态加载
    ds.manager->hostFn = &fnSum;                                // 经管理器交给客人程序
    // mprotect 演示：宿主给客人程序一块页对齐缓冲（改 rwx 属性用）
    ds.manager->rwxBuf = mmap(nullptr, 4096, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    executoring ex;
    ex.ptr = &ds;
    std::cout << std::setprecision(12);
    save prog(bc.data(), bc.size());
    ex.F8BFLRead(prog);

    // ===== LLVM JIT（v2.12 新增）：提交 save 指针 → 返回 JIT 编译后的函数指针 =====
    // 1) JIT 编译 VM 函数 fnSum 的字节码（save 指针 = &fnSum.bytecode）：
    //    调用约定与 FUNC_CALL 一致 —— fn(argPtr, retPtr, manager)。
    JadeightJIT::Fn jitSum = JadeightJIT::submit(fnSum.bytecode);
    if (jitSum) {
        uint8_t jargs[8] = {}, jret[4] = {};
        storeRaw(jargs, 123, 4); storeRaw(jargs + 4, 456, 4);
        jitSum(jargs, jret, nullptr);                   // 无 GET_ADDRS → manager 可传 nullptr
        std::cout << "JIT    fn_sum(123,456) = " << loadRaw(jret, 4)
                  << "（解释器 FUNC_CALL 期望 579）\n";
        storeRaw(jargs, 1000, 4); storeRaw(jargs + 4, 7, 4);
        jitSum(jargs, jret, nullptr);
        std::cout << "JIT    fn_sum(1000,7)  = " << loadRaw(jret, 4) << '\n';
        callFunctionSave(&fnSum, jargs, jret);          // 解释器对照
        std::cout << "解释器 fn_sum(1000,7)  = " << loadRaw(jret, 4) << '\n';
    } else {
        std::cout << "LLVM JIT 不可用（未找到 LLVM 开发包），跳过 JIT 演示\n";
    }
    // 2) JIT 编译整个主演示程序（save 指针 = &prog）：输出应与上方解释器运行逐行一致
    //    （唯一区别：JIT 版 silent，不打印"正常退出"；GET_ADDRS 需要管理器）。
    JadeightJIT::Fn jitMain = JadeightJIT::submit(prog);
    if (jitMain) {
        std::cout << "===== LLVM JIT 运行主程序（输出应与解释器一致） =====\n";
        jitMain(nullptr, nullptr, ds.manager.get());
    }

    // ===== 字节码只读冻结验证（v2.12 新增）：写能力仅限构造阶段 =====
    // save 构造完成后 mprotect 置只读。fork 一个子进程尝试写入冻结的字节码
    // （绕过 C++ 接口直写指针，等价于客人程序经 GET_ADDRS 拿到的字节码地址），
    // 子进程应死于 SIGSEGV —— 运行时写入被物理拦截，而非悄悄改坏字节码。
    {
        const pid_t pid = fork();
        if (pid == 0) {                      // 子进程：尝试写入冻结字节码
            prog.byteCode.get()[0] = OP_END; // mprotect(PROT_READ) → SIGSEGV
            _exit(0);                        // 若未冻结会走到这里
        }
        int st = 0;
        waitpid(pid, &st, 0);
        const bool frozen = WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV;
        std::cout << "字节码冻结验证：" << (frozen ? "只读生效（运行时写入触发 SIGSEGV）"
                                                   : "未冻结（运行时写入成功！）") << '\n';
    }

    // ===== JIT_SUBMIT 指令（v2.12 新增）：客人程序把 save 指针即时编译成函数指针 =====
    // 客人程序从 Manager.jitSave（偏移32）拿到宿主给的 save*（&fnSum.bytecode），
    // 存进栈槽后 JIT_SUBMIT(slot, retSlot) 编译，函数指针写回 retSlot，
    // 再经 mode1 存进 Manager.jitFn（偏移40）交还宿主对照。
    ds.manager->jitSave = &fnSum.bytecode;              // 宿主交给客人程序的 save*
    std::vector<uint8_t> jbc;
    auto jput   = [&](uint8_t b)              { jbc.push_back(b); };
    auto jput32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) jbc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto jput64 = [&](uint64_t v) { for (int i = 7; i >= 0; --i) jbc.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    jput(OP_STACK_INIT); jput32(4096); jput32(32);
    jput(OP_GET_ADDRS);                                  // [bc,size,ds,mgr]
    jput(OP_REG_POP_U64); jput(12);                      // R12 = 管理器
    jput(OP_REG_POP_U64); jput(7);
    jput(OP_REG_POP_U64); jput(8);
    jput(OP_REG_POP_U64); jput(3);
    jput(OP_REG_MOVI_U64); jput(2); jput64(32);          // Manager.jitSave 偏移
    jput(OP_REG_PUSH_U64); jput(12); jput(OP_REG_PUSH_U64); jput(2);
    jput(OP_ADD_PTR);                                    // 管理器+32 = &jitSave
    jput(OP_REG_POP_U64); jput(1);
    jput(OP_REG_STORE_U64); jput(1); jput(0); jput64(2000); // 槽2000 = &jitSave
    jput(OP_REG_LOAD_U64); jput(0); jput(1); jput64(2000);  // R0 = jitSave（save*）
    jput(OP_REG_STORE_U64); jput(0); jput(0); jput64(2008); // 槽2008 = save*
    jput(OP_JIT_SUBMIT); jput32(2008); jput32(2016);        // JIT 编译 → 函数指针@2016
    jput(OP_REG_LOAD_U64); jput(5); jput(0); jput64(2016);  // R5 = JIT 函数指针
    jput(OP_REG_MOVI_U64); jput(2); jput64(40);             // Manager.jitFn 偏移
    jput(OP_REG_PUSH_U64); jput(12); jput(OP_REG_PUSH_U64); jput(2);
    jput(OP_ADD_PTR);
    jput(OP_REG_POP_U64); jput(1);                          // R1 = &jitFn
    jput(OP_REG_STORE_U64); jput(1); jput(0); jput64(2010); // 槽2010 = &jitFn
    jput(OP_REG_STORE_U64); jput(5); jput(1); jput64(2010); // *(&jitFn) = JIT 函数指针
    jput(OP_END);
    save jprog(jbc.data(), jbc.size());
    DataSave jds;
    jds.manager = ds.manager;                              // 共享管理器（含 jitSave）
    executoring jex;
    jex.ptr = &jds;
    jex.F8BFLRead(jprog);                                  // 客人程序跑一遍
    // 宿主对照：同一 save* 应命中同一缓存项 → 指针一致；再用该指针实际调用
    JadeightJIT::Fn hostFn2 = JadeightJIT::submit(fnSum.bytecode);
    const bool samePtr = (hostFn2 != nullptr) && (reinterpret_cast<void*>(hostFn2) == ds.manager->jitFn);
    std::cout << "JIT_SUBMIT 指令：客人程序与宿主拿到同一 JIT 函数指针：" << (samePtr ? "是" : "否") << '\n';
    if (hostFn2) { // 宿主用 JIT_SUBMIT 出的函数指针实际调用（123+456）
        uint8_t a2[8] = {}, r2[4] = {};
        storeRaw(a2, 123, 4); storeRaw(a2 + 4, 456, 4);
        hostFn2(a2, r2, nullptr);
        std::cout << "JIT_SUBMIT 出的函数指针可调用：" << loadRaw(r2, 4) << "（期望 579）\n";
    }
    // JIT 编译 jprog 再跑一遍：验证 JIT 翻译器里的 JIT_SUBMIT 路径（嵌套 submit 同缓存）
    JadeightJIT::Fn jitJProg = JadeightJIT::submit(jprog);
    if (jitJProg) {
        jitJProg(nullptr, nullptr, ds.manager.get());
        const bool samePtr2 = (hostFn2 != nullptr) && (reinterpret_cast<void*>(hostFn2) == ds.manager->jitFn);
        std::cout << "JIT 版 JIT_SUBMIT 路径：同一 JIT 函数指针：" << (samePtr2 ? "是" : "否") << '\n';
    }

    // ===== 多线程演示：executoringHarness + 共享 Manager + ATOMIC_ADD =====
    // 4 个线程跑同一份字节码：每线程 25 次原子自增共享计数器（期望总和 100）
    std::vector<uint8_t> bct;
    auto tput     = [&](uint8_t b)              { bct.push_back(b); };
    auto tput32   = [&](uint32_t v) { for (int i = 3; i >= 0; --i) bct.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto tput64   = [&](uint64_t v) { for (int i = 7; i >= 0; --i) bct.push_back(static_cast<uint8_t>(v >> (8 * i))); };
    auto tpatch64 = [&](size_t pos, uint64_t v) { for (int i = 7; i >= 0; --i) bct[pos + (7 - i)] = static_cast<uint8_t>(v >> (8 * i)); };

    tput(OP_STACK_INIT); tput32(4096); tput32(64);
    // GET_ADDRS 压 [bc, size, ds, mgr]；管理器 counter 在偏移 0 → 管理器地址即 &counter
    tput(OP_GET_ADDRS);
    tput(OP_REG_POP_U64); tput(0);                           // R0 = 管理器（= &counter）
    tput(OP_REG_POP_U64); tput(1);
    tput(OP_REG_POP_U64); tput(2);
    tput(OP_REG_POP_U64); tput(3);
    tput(OP_REG_STORE_U64); tput(0); tput(0); tput64(2000);  // 栈槽2000 = &manager->counter
    tput(OP_REG_MOVI_U64); tput(6); tput64(25);              // R6 = 剩余次数
    tput(OP_REG_MOVI_U64); tput(8); tput64(1);               // R8 = 加数 1（保留副本）
    const size_t loopStart = bct.size();
    tput(OP_REG_MOV); tput(7); tput(8);                      // R7 = 1（加数）
    tput(OP_ATOMIC_ADD_U64); tput(7); tput(1); tput64(2000); // 共享计数器 += 1
    // R6 -= 1
    tput(OP_REG_MOVI_U64); tput(9); tput64(1);
    tput(OP_REG_PUSH_U64); tput(6);
    tput(OP_REG_PUSH_U64); tput(9);
    tput(OP_SUB_U64);
    tput(OP_REG_POP_U64); tput(6);
    // flag = (R6 == 0)
    tput(OP_REG_PUSH_U64); tput(6);
    tput(OP_REG_MOVI_U64); tput(9); tput64(0);
    tput(OP_REG_PUSH_U64); tput(9);
    tput(OP_CMP_EQ_U64);
    tput(OP_REG_POP_U8); tput(10);                           // R10 = flag
    // 目标 = bcBase + loopStart + flag*(exit - loopStart)：flag=0 跳回循环，flag=1 落空退出
    // （JMP_IND 吃绝对地址，必须加上 setup 时弹入 R3 的 bcBase）
    tput(OP_REG_PUSH_U64); tput(10);
    size_t diffPos = bct.size();
    tput(OP_REG_MOVI_U64); tput(11); tput64(0);              // 占位：diff
    tput(OP_REG_PUSH_U64); tput(11);
    tput(OP_MUL_U64);
    size_t loopPos = bct.size();
    tput(OP_REG_MOVI_U64); tput(12); tput64(0);              // 占位：loopStart
    tput(OP_REG_PUSH_U64); tput(12);
    tput(OP_ADD_U64);                                        // loopStart + flag*diff（偏移）
    tput(OP_REG_PUSH_U64); tput(3);                          // R3 = bcBase
    tput(OP_ADD_U64);                                        // + bcBase = 绝对地址
    tput(OP_JMP_IND);
    const size_t exitPos = bct.size();
    tpatch64(diffPos + 2, static_cast<uint64_t>(exitPos - loopStart));
    tpatch64(loopPos + 2, static_cast<uint64_t>(loopStart));
    tput(OP_END);

    save progT(bct.data(), bct.size());
    executoringHarness harness;
    harness.runThreads(progT, 4);
    harness.join();
    std::cout << "counter=" << harness.manager->counter.load()
              << "（4线程×25次原子自增，期望 100）" << '\n';
    return 0;
}
