// dump_offsets.cpp — 逐条打印 .bc 指令偏移（调试用）
#include "jadeight_asm.hpp"
#include <cstdio>
#include <vector>
using namespace jadeight;
int main(int argc, char** argv) {
    if (argc < 2) return 1;
    FILE* f = fopen(argv[1], "rb");
    if (!f) return 1;
    uint32_t hdr[3];
    fread(hdr, 4, 3, f);
    std::vector<uint8_t> bc;
    int c;
    while ((c = fgetc(f)) != EOF) bc.push_back(static_cast<uint8_t>(c));
    size_t off = 0;
    while (off < bc.size()) {
        uint8_t op = bc[off];
        size_t len = instrLen(op);
        printf("%4zu: op=%3u len=%zu\n", off, op, len);
        off += len;
    }
    return 0;
}
