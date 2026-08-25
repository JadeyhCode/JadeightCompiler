// j8run.cpp — Jadeight VM 宿主运行器
//
// 在真正的 Jadeight VM（Jadeight2/main.cpp 的解释器）上运行 j8c 编译出的 .bc 程序。
// 复用方式：#define main jadeight2_vm_main 后 include 整个 VM 源文件，
// 从而直接使用 save / FunctionSave / DataSave / executoring / callFunctionSave /
// externFn 等完整实现（逐字节一致的运行语义，与用户自己的 VM 完全同源）。
//
// 用法:
//   j8run <main.bc> [--externs manifest.txt] [--lib lib.so]
//
//   main.bc       编译器产出的程序（FunctionSave 文件格式：LE 头 + 字节码）
//   --externs f   外部函数清单（编译器 -emit-externs 生成），j8run 用 libffi 注册
//   --lib path    额外的共享库（缺省尝试 libc.so.6 与 RTLD_DEFAULT）

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <dlfcn.h>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#define main jadeight2_vm_main
#include "main.cpp"   // Jadeight2 VM 全量（LLVM 部分默认不启用）
#undef main

using namespace std;

// ---------------- 外部函数注册（libffi） ----------------
// manifest 格式（j8c -emit-externs 输出）：
//   每行: name:rettype(arg1,arg2,...)
//   rettype/argtype ∈ {void,u8,u16,u32,u64,i8,i16,i32,i64,f32,f64,ptr}
struct ExternSig {
    string name;
    string ret;
    vector<string> args;
};

static deque<ffi_cif> g_cifs;   // 保持 cif 存活（deque：push_back 不使已有元素引用失效）
static vector<vector<ffi_type*>> g_argTypeSets;  // 保持 arg_types 数组存活：ffi_prep_cif 只存指针不拷贝
static vector<void*> g_handles;  // dlopen 句柄保持存活

static ffi_type* ffiType(const string& t) {
    if (t == "u8" || t == "i8") return &ffi_type_uint8;
    if (t == "u16" || t == "i16") return &ffi_type_uint16;
    if (t == "u32" || t == "i32") return &ffi_type_uint32;
    if (t == "u64" || t == "i64") return &ffi_type_uint64;
    if (t == "f32") return &ffi_type_float;
    if (t == "f64") return &ffi_type_double;
    if (t == "ptr" || t == "void") return &ffi_type_pointer;
    return nullptr;
}

static vector<ExternSig> parseManifest(const string& path) {
    vector<ExternSig> sigs;
    ifstream in(path);
    if (!in) { cerr << "j8run: cannot open extern manifest: " << path << "\n"; return sigs; }
    string line;
    while (getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        // name:ret(arg1,arg2,...)
        size_t colon = line.find(':');
        if (colon == string::npos) continue;
        ExternSig s;
        s.name = line.substr(0, colon);
        size_t lparen = line.find('(', colon);
        size_t rparen = line.find(')', lparen);
        if (lparen == string::npos || rparen == string::npos) continue;
        s.ret = line.substr(colon + 1, lparen - colon - 1);
        string args = line.substr(lparen + 1, rparen - lparen - 1);
        if (!args.empty()) {
            size_t pos = 0;
            while (pos <= args.size()) {
                size_t comma = args.find(',', pos);
                string a = args.substr(pos, comma == string::npos ? string::npos : comma - pos);
                if (!a.empty()) s.args.push_back(a);
                if (comma == string::npos) break;
                pos = comma + 1;
            }
        }
        sigs.push_back(s);
    }
    return sigs;
}

static void registerExterns(const vector<ExternSig>& sigs, const vector<string>& libs) {
    for (size_t idx = 0; idx < sigs.size() && idx < 256; ++idx) {
        const ExternSig& s = sigs[idx];
        void* fn = nullptr;
        for (const string& lib : libs) {
            if (lib.empty()) { fn = dlsym(RTLD_DEFAULT, s.name.c_str()); }
            else {
                void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
                if (h) { g_handles.push_back(h); fn = dlsym(h, s.name.c_str()); }
            }
            if (fn) break;
        }
        if (!fn) {
            cerr << "j8run: extern[" << idx << "] '" << s.name << "' not found\n";
            continue;
        }
        ffi_type* rtype = ffiType(s.ret);
        vector<ffi_type*> atypes;
        bool ok = (rtype != nullptr);
        for (const string& a : s.args) {
            ffi_type* t = ffiType(a);
            if (!t) { ok = false; break; }
            atypes.push_back(t);
        }
        if (!ok) { cerr << "j8run: bad signature for '" << s.name << "'\n"; continue; }
        g_cifs.emplace_back();
        ffi_cif& cif = g_cifs.back();
        if (ffi_prep_cif(&cif, FFI_DEFAULT_ABI,
                         static_cast<unsigned>(atypes.size()), rtype, atypes.data()) != FFI_OK) {
            cerr << "j8run: ffi_prep_cif failed for '" << s.name << "'\n";
            g_cifs.pop_back();
            continue;
        }
        g_argTypeSets.push_back(std::move(atypes));   // 在 cif 之前存活（cif->arg_types 指向它）
        externFn[idx] = { &cif, fn };
        cout << "j8run: registered extern[" << idx << "] " << s.name << "\n";
    }
}

// ---------------- 主入口 ----------------
int main(int argc, char* argv[]) {
    string bcPath, manifestPath;
    vector<string> libs = { "", "libc.so.6" };

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--externs" && i + 1 < argc) manifestPath = argv[++i];
        else if (a == "--lib" && i + 1 < argc) libs.push_back(argv[++i]);
        else bcPath = a;
    }
    if (bcPath.empty()) {
        cerr << "Usage: j8run <main.bc> [--externs manifest] [--lib lib.so]\n";
        return 2;
    }

    if (!manifestPath.empty()) {
        registerExterns(parseManifest(manifestPath), libs);
    }

    FunctionSave prog = FunctionSave::loadFromFile(bcPath.c_str());
    if (prog.bytecode.size == 0) {
        cerr << "j8run: cannot load " << bcPath << "\n";
        return 1;
    }

    cout << "===== j8run: " << bcPath << " =====" << endl;
    prog.state.manager = make_shared<Manager>();   // GET_ADDRS 需要管理器
    callFunctionSave(&prog, nullptr, nullptr);

    if (prog.state.end == 2) {
        cerr << "j8run: program exited abnormally (end=" << (int)prog.state.end << ")\n";
        return 1;
    }
    return 0;
}
