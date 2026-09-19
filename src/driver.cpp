// driver.cpp — j8c 命令行入口
//
// 用法:
//   j8c input.j8 [-o out.bc] [-S] [-O0|-O1|-O2] [--no-unroll] [--unroll-limit N]
//              [--ecs-capacity N] [-emit-externs manifest.txt] [-v] [--dump-ast]
//
// 流程: 词法 → 语法 → 语义（协议/泛型/ECS 检查）→ 优化（常量折叠/循环展开/DCE…）
//       → 代码生成（Jadeight 汇编）→ 复用 jadeight_asm 汇编器 → .bc（LE 头，VM 兼容）

#include <fstream>
#include <iostream>
#include <sstream>

#include "jadeight_asm.hpp"
#include "codegen.h"
#include "lexer.h"
#include "optimize.h"
#include "parser.h"
#include "sema.h"

using namespace j8;

static void usage(const char* prog) {
    std::cerr << "用法: " << prog << " input.j8 [选项]\n"
              << "  -o out.bc          输出文件（默认 input.bc）\n"
              << "  -S                 保留生成的 Jadeight 汇编（out.jasm）\n"
              << "  -O0 / -O1 / -O2    优化级别（默认 -O2）\n"
              << "  --no-unroll        禁用循环展开\n"
              << "  --unroll-limit N   全展开迭代上限（默认 8）\n"
              << "  --no-inline        禁用内联（当前版本无内联，保留兼容）\n"
              << "  --ecs-capacity N   ECS 世界实体容量（默认 4096）\n"
              << "  --stack N          覆盖栈大小（字节，默认自动计算）\n"
              << "  -emit-externs f    输出外部函数清单（供 j8run 注册）\n"
              << "  -v / --verbose     详细输出\n"
              << "  --dump-ast         打印 AST\n";
}

// 类型名 → manifest 类型名（ptr 归一化）
static std::string manifestType(const std::string& t) {
    if (!t.empty() && t.back() == '*') return "ptr";
    if (t == "void") return "void";
    return t;
}

int main(int argc, char* argv[]) {
    std::string input, output, externsPath;
    bool verbose = false, dumpAst = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "-o" && i + 1 < argc) output = argv[++i];
        else if (a == "-S") gOpts.emitAsm = true;
        else if (a == "-O0") gOpts.optLevel = 0;
        else if (a == "-O1") gOpts.optLevel = 1;
        else if (a == "-O2") gOpts.optLevel = 2;
        else if (a == "--no-unroll") gOpts.noUnroll = true;
        else if (a == "--no-inline") gOpts.noInline = true;
        else if (a == "--unroll-limit" && i + 1 < argc) gOpts.unrollLimit = std::atoi(argv[++i]);
        else if (a == "--ecs-capacity" && i + 1 < argc) gOpts.ecsCapacity = std::atoi(argv[++i]);
        else if (a == "--stack" && i + 1 < argc) gOpts.stackSize = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (a == "-emit-externs" && i + 1 < argc) externsPath = argv[++i];
        else if (a == "-v" || a == "--verbose") verbose = true;
        else if (a == "--dump-ast") dumpAst = true;
        else if (a == "-h" || a == "--help") { usage(argv[0]); return 0; }
        else input = a;
    }
    if (input.empty()) { usage(argv[0]); return 1; }

    // 读源码
    std::ifstream in(input, std::ios::binary);
    if (!in) {
        std::cerr << "j8c: 无法打开 " << input << "\n";
        return 1;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    std::string src = ss.str();

    // 词法
    Lexer lexer(src);
    auto toks = lexer.tokenize();
    if (!lexer.ok() || Diag::errorCount) {
        std::cerr << "j8c: 词法分析失败\n";
        return 1;
    }

    // 语法
    Parser parser(std::move(toks));
    std::unique_ptr<Program> prog = parser.parseProgram();
    if (!parser.ok() || Diag::errorCount) {
        std::cerr << "j8c: 语法分析失败\n";
        return 1;
    }
    if (dumpAst) {
        for (auto& d : prog->decls) {
            std::cout << "decl: " << d->name << " (kind=" << static_cast<int>(d->kind) << ")\n";
        }
    }

    // 语义
    Sema sema(prog.get());
    if (!sema.run()) {
        std::cerr << "j8c: 语义分析失败\n";
        return 1;
    }

    // 优化
    Optimizer opt(&sema, gOpts);
    opt.run(sema.instances);

    // 代码生成
    CodeGen cg(&sema, gOpts);
    std::string asmText;
    std::vector<uint8_t> bc;
    uint32_t argSize, retSize, entry;
    if (!cg.generate(asmText, bc, argSize, retSize, entry)) {
        std::cerr << "j8c: 代码生成失败\n";
        return 1;
    }

    if (output.empty()) {
        size_t dot = input.rfind('.');
        output = (dot != std::string::npos) ? input.substr(0, dot) + ".bc" : input + ".bc";
    }

    // 写 .bc —— ISA v3 模块格式（magic "J3BC"：函数目录 + 连续码流）
    {
        jadeight::ModuleImage img;
        img.entryFunc = cg.entryFuncIndex;
        img.funcs = cg.moduleFuncs;
        img.code = bc;
        const std::vector<uint8_t> bytes = img.serialize();
        std::ofstream out(output, std::ios::binary);
        if (!out) { std::cerr << "j8c: 无法写入 " << output << "\n"; return 1; }
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        out.close();
        if (verbose)
            std::cerr << "j8c: v3 模块 " << img.funcs.size() << " 个函数，入口 #" << img.entryFunc
                      << "，码流 " << bc.size() << " 字节\n";
    }

    // -S：保留汇编
    if (gOpts.emitAsm) {
        std::string asmPath = output;
        size_t dot = asmPath.rfind('.');
        if (dot != std::string::npos) asmPath = asmPath.substr(0, dot);
        asmPath += ".jasm";
        std::ofstream out(asmPath);
        if (out) out << asmText;
        if (verbose) std::cout << "汇编文本: " << asmPath << "\n";
    }

    // extern manifest
    if (!externsPath.empty()) {
        std::ofstream out(externsPath);
        if (!out) { std::cerr << "j8c: 无法写入 " << externsPath << "\n"; return 1; }
        for (Decl* d : sema.externs) {
            out << d->name << ":" << manifestType(d->externRetType) << "(";
            for (size_t i = 0; i < d->externTypes.size(); ++i) {
                if (i) out << ",";
                out << manifestType(d->externTypes[i]);
            }
            out << ")\n";
        }
        out.close();
        if (verbose) std::cout << "外部函数清单: " << externsPath << "\n";
    }

    if (verbose) {
        std::cout << "j8c: 编译成功\n";
        std::cout << "  输出: " << output << " (" << (12 + bc.size()) << " 字节, entry=" << entry << ")\n";
        std::cout << "  函数实例: " << sema.instances.size() << "\n";
        std::cout << "  栈大小: " << cg.stackSizeForDisplay() << " 字节\n";
        std::cout << "  警告: " << Diag::warningCount << "\n";
    }
    return 0;
}
