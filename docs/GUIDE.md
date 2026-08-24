# JadeightCompiler (j8c) 使用指南

面向 JadeightAbstractionCode 的 C 风格编译器：`.j8` 源码 → Jadeight 汇编 → `.bc` 字节码，
由 `j8run` 在 Jadeight2 VM 上运行。支持面向协议、ECS、泛型，以及循环展开/常量折叠等优化。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target j8c j8run -j4
```

产物：`build/j8c`（编译器）、`build/j8run`（VM 运行器）。

## 用法

```bash
./build/j8c [-O0|-O2] [-S] [--dump-ast] [-o out.bc] 源文件.j8
./build/j8run out.bc
```

- 默认输出 `.bc`（LE 头 + 字节码，与 `FunctionSave`/`loadFromFile` 兼容）。
- `-S`：保留生成的 `.jasm` 汇编文本（便于调试）。
- `-O2`：实验性优化（循环展开/常量折叠/DCE），**建议日常用 `-O0`**。
- `-o out.bc` 会从 `out.bc` 推导 `.jasm` 路径。

## 快速示例

```c
void main() {
    print(1 + 2 * 3);            // 7
    u64 big = 10000000000;
    print(big);                  // 10000000000
    i32 neg = -7;                // 注意用 i32 显式类型
    print(neg);                  // -7
    print(-3.5);                 // -3.500
}
```

协议/ECS 示例见 `examples/t2_protocol.j8`、`examples/t3_ecs.j8`。

## 语言要点

- 整数字面量默认无符号：`0 - 7` 按 u32 回绕；要负数请写 `-7` 或显式 `i32` 类型。
- `x * y;` 形式会被解析为指针变量声明（与 C 的 typedef 消歧同理）。
- 支持的优化：常量折叠、常量传播、循环展开（`#pragma unroll`）、DCE、强度削减。

## 测试

```bash
tests/run_tests.sh
```

对 `tests/*.j8` 与 `examples/*.j8` 在 `-O0`/`-O2` 下编译运行并与 `.expected` 逐行比对。
当前 20/20 全绿（8 测试 + 2 示例 × 2 优化级别）。

## 版本历史

- 修复 `UnaryOp::Neg` 的 R0 复用缺陷（`i32 neg = -7` 在 `-O0` 下曾被编译为 0）。
- 修复浮点取反的常量折叠（`-3.5` 曾折成 `0.000`）。
- 移除 `run_tests.sh` 对 `t1_basic -O0` 的豁免；`-O2` 标记为实验性/不稳定。
