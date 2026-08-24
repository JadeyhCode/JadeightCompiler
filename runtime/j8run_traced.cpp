#define main jadeight2_vm_main
#include "../../JadeightCompiler/tools/j8vm_traced.cpp"
#undef main
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FunctionSave prog = FunctionSave::loadFromFile(argv[1]);
    prog.state.manager = std::make_shared<Manager>();
    callFunctionSave(&prog, nullptr, nullptr);
    fprintf(stderr, "end=%d count=%llu\n", (int)prog.state.end, (unsigned long long)prog.state.count);
    return 0;
}
