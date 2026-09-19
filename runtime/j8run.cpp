// j8run.cpp — Jadeight VM 宿主运行器
//
// 在 Jadeight ISA v3 VM（Jadeight2ReWrite）上运行 j8c 编译出的 .bc 模块。
// 复用方式：#define main jadeight2_vm_main 后 include 整个 VM 源文件，
// 直接使用 save（模块 + 函数目录）/ Process / Thread / VM / Jit / externFn。
// .bc 是 v3 模块格式（magic "J3BC"：函数目录 + 连续码流）。
//
// 用法:
//   j8run <main.bc> [--externs manifest.txt] [--lib lib.so]
//
//   main.bc       编译器产出的 v3 模块（magic "J3BC"：函数目录 + 码流）
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
#include "../../Jadeight2ReWrite/Jadeight2.cpp"   // ISA v3 VM 全量（解释器 + 模板 JIT）
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

// ---------------- 多线程支持（--threads N） ----------------
// SPMD 模型：N 个线程跑同一份字节码，共享 Manager 与进程堆。
// 字节码用 tid() 区分线程、shared_buf() 拿共享内存、atomic_*_u32/u64 内建做同步。
static thread_local int g_threadId = 0;
alignas(8) static uint64_t g_sharedBuf[16];   // 128 字节共享缓冲区（所有线程同一地址）

static uint32_t thunk_tid()        { return static_cast<uint32_t>(g_threadId); }
static uint64_t thunk_shared_buf() { return reinterpret_cast<uint64_t>(g_sharedBuf); }

// 宿主 extern 查找：manifest 里出现 tid/shared_buf 时直接返回宿主函数（dlsym 找不到）
static void* hostExtern(const string& name) {
    if (name == "tid") return reinterpret_cast<void*>(&thunk_tid);
    if (name == "shared_buf") return reinterpret_cast<void*>(&thunk_shared_buf);
    return nullptr;
}

static void registerExterns(const vector<ExternSig>& sigs, const vector<string>& libs) {
    for (size_t idx = 0; idx < sigs.size() && idx < 256; ++idx) {
        const ExternSig& s = sigs[idx];
        void* fn = hostExtern(s.name);   // 宿主内建（tid/shared_buf）优先
        if (!fn) {
            for (const string& lib : libs) {
                if (lib.empty()) { fn = dlsym(RTLD_DEFAULT, s.name.c_str()); }
                else {
                    void* h = dlopen(lib.c_str(), RTLD_NOW | RTLD_GLOBAL);
                    if (h) { g_handles.push_back(h); fn = dlsym(h, s.name.c_str()); }
                }
                if (fn) break;
            }
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


// 无 manifest 时也占位注册 tid/shared_buf（单线程跑 SPMD 字节码不崩）
static void registerHostExterns() {
    static ffi_cif cifTid, cifBuf;
    ffi_prep_cif(&cifTid, FFI_DEFAULT_ABI, 0, &ffi_type_uint32, nullptr);
    ffi_prep_cif(&cifBuf, FFI_DEFAULT_ABI, 0, &ffi_type_uint64, nullptr);
    for (int i = 0; i < 256; ++i) {
        if (externFn[i].fn == nullptr) {
            externFn[i] = { &cifTid, reinterpret_cast<void*>(&thunk_tid) };
            break;
        }
    }
    for (int i = 0; i < 256; ++i) {
        if (externFn[i].fn == nullptr) {
            externFn[i] = { &cifBuf, reinterpret_cast<void*>(&thunk_shared_buf) };
            break;
        }
    }
}

// ---------------- 主入口 ----------------
int main(int argc, char* argv[]) {
    string bcPath, manifestPath;
    vector<string> libs = { "", "libc.so.6" };
    int threads = 0;     // 0/1 = 单线程
    bool useJit = false; // --jit：ISA v3 模板 JIT

    for (int i = 1; i < argc; ++i) {
        string a = argv[i];
        if (a == "--externs" && i + 1 < argc) manifestPath = argv[++i];
        else if (a == "--lib" && i + 1 < argc) libs.push_back(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) {
            threads = std::atoi(argv[++i]);
            if (threads < 1) threads = 1;
        }
        else if (a == "--jit") useJit = true;
        else bcPath = a;
    }
    if (bcPath.empty()) {
        cerr << "Usage: j8run <main.bc> [--externs manifest] [--lib lib.so] [--threads N] [--jit]\n";
        return 2;
    }

    registerHostExterns();
    if (!manifestPath.empty()) registerExterns(parseManifest(manifestPath), libs);

    VM vm;
    if (!vm.module.loadFromFile(bcPath.c_str())) {
        cerr << "j8run: cannot load v3 module: " << bcPath << "\n";
        return 1;
    }

    // 首行 banner 走 stdout：tests/run_tests.sh 用 `tail -n +2` 丢掉它
    cout << "===== j8run: " << bcPath << " =====" << endl;
    if (getenv("J8RUN_VERBOSE")) {
        cerr << "j8run: " << vm.module.funcCount() << " 个函数，"
             << (useJit ? "模板 JIT" : "解释器") << (threads > 1 ? "，SPMD" : "") << "\n";
    }

    // 入口函数下标来自模块头（j8c 的 main 不一定是 0 号函数）
    const uint16_t entryFn = static_cast<uint16_t>(vm.module.entryFunc);

    // --jit：建立 JIT 并编译入口函数；某函数编译失败会自动回退解释器
    if (useJit) {
        void* fn = vm.jitCompile(entryFn);
        if (getenv("J8RUN_VERBOSE"))
            cerr << "j8run: JIT " << (fn ? "已编译入口函数" : "入口函数不支持，回退解释器") << "\n";
    }

    Process* pr = vm.newProcess();
    const int N = (threads > 1) ? threads : 1;
    vector<Thread*> ts;
    ts.reserve(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) ts.push_back(vm.createThread(pr, entryFn, nullptr, 0));

    if (N == 1) {
        g_threadId = 0;
        vm.launch(ts[0], entryFn);
    } else {
        vector<std::thread> th;
        th.reserve(static_cast<size_t>(N));
        for (int i = 0; i < N; ++i)
            th.emplace_back([&vm, &ts, entryFn, i] { g_threadId = i; vm.launch(ts[i], entryFn); });
        for (auto& x : th) x.join();
    }

    for (Thread* t : ts)
        if (t->state.load() == 2) {
            cerr << "j8run: program exited abnormally (trap)\n";
            return 1;
        }
    return 0;
}
