#define main jadeight2_vm_main
#include "../../Jadeight2/main.cpp"
#undef main
#include <cstdio>
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FunctionSave prog = FunctionSave::loadFromFile(argv[1]);
    prog.state.manager = std::make_shared<Manager>();
    callFunctionSave(&prog, nullptr, nullptr);
    if (prog.state.end == 2) {
        size_t c = prog.state.count;
        fprintf(stderr, "ABNORMAL end at count=%zu (bytecode size=%zu)\n", c, prog.bytecode.size);
        for (size_t i = (c > 8 ? c - 8 : 0); i < c + 8 && i < prog.bytecode.size; ++i) {
            fprintf(stderr, "  [%zu] 0x%02x\n", i, prog.bytecode.byteCode[i]);
        }
    }
    return 0;
}
