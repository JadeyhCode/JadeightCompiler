# JadeightCompiler (j8c) — Jadeight 的 C 风格编译器

为 [JadeightAbstractionCode](../JadeightAbstractionCode)（Jadeight 汇编器/字节码层）封装的
**C 风格、面向协议 + ECS 的高级语言编译器**，输出 **ISA v3** 的 `.bc` 模块
（magic `"J3BC"`：函数目录 + 码流），由 [Jadeight2ReWrite](../Jadeight2ReWrite) 的虚拟机
（解释器或 `--jit`）通过宿主运行器 `j8run` 执行。支持循环展开、常量折叠/传播、DCE 等优化。

> ISA 的唯一事实源是 [`../Jadeight2ReWrite/isa.hpp`](../Jadeight2ReWrite/isa.hpp)（68 条合并式 opcode）。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target j8c j8run
```

产出：
- `build/j8c`    —— 编译器：`.j8` → `.bc`（v3 模块 `"J3BC"`；`-S` 可同时留下 `.jasm`）
- `build/j8run`  —— 宿主运行器：在 ISA v3 VM 上执行 `.bc`（复用 `../Jadeight2ReWrite/Jadeight2.cpp`）；
  `--jit` 走 copy-and-patch 模板 JIT，另有 `--threads N`（SPMD 多线程）

## 用法

```bash
j8c input.j8 [-o out.bc] [-S] [-O0|-O1|-O2] [--no-unroll] [--unroll-limit N]
             [--no-inline] [--ecs-capacity N] [--stack N]
             [-emit-externs manifest.txt] [-v] [--dump-ast]

j8run out.bc [--externs manifest.txt] [--lib lib.so] [--threads N] [--jit]
```

- `-S`：保留生成的 Jadeight 汇编文本（`out.jasm`）
- `-O0/-O1/-O2`：优化级别（默认 `-O2`）。`-O2` 启用常量折叠、常量传播、
  循环展开（全展开 + `#pragma unroll N` 部分展开）、DCE、强度削减
- `-emit-externs`：输出外部函数清单（`j8run --externs` 用 libffi 注册 C 函数）

## 语言特性

### 基础（C 风格）

```c
void main() {
    u32 x = 5;                 // u8/u16/u32/u64/i8/i16/i32/i64/f64/bool/char
    u64 big = 10000000000;
    print(x + 1);              // 输出逐行打印；u32 直接打印，u64/i64/f64 逐位打印
    print("hi");               // 字符串逐字符输出
    if (x > 3) { } else { }
    while (x < 10) { x = x + 1; }
    for (u32 i = 0; i < 5; i = i + 1) { }
    do { } while (false);
}
```

### 函数与递归

```c
u64 fib(u64 n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}
```

### 协议（面向协议）

```c
protocol Shape {
    f64 area(this);
    f64 perimeter(this) { return 0.0; }   // 默认实现
}

struct Circle { f64 r; }
impl Shape for Circle {
    f64 area(this) { return 3.14159 * this->r * this->r; }
}

void main() {
    Circle c = {2.0};
    print(c.area());        // 静态派发
    Shape s = c;            // 存在类型盒（boxing + witness 表）
    print(s.area());        // 动态派发
}
```

泛型 + 协议约束：

```c
f64 total_area<T: Shape>(T a, T b) { return a.area() + b.area(); }
print(total_area<Circle>(c, c));
```

### ECS

```c
component Pos { i32 x; i32 y; }
component Vel { i32 vx; i32 vy; }
system Physics: Pos, Vel {
    fn update(ent e) {
        get<Pos>(e)->x += get<Vel>(e)->vx;
    }
}

void main() {
    ent a = spawn();
    add<Pos>(a, 1, 2);
    add<Vel>(a, 3, 4);
    get<Pos>(a)->x;         // 读组件字段
    has<Pos>(a);            // true
    remove<Vel>(a);
    run<Physics>();         // 迭代有 Pos+Vel 的实体
    despawn(a);  alive(a);  entity_count();
}
```

### 指针 / 数组 / 结构体

```c
struct Point { i32 x; i32 y; }
Point p = {3, 4};
Point* pp = &p;
pp->x = 42;  (*pp).y = 43;
u32 arr[4];  arr[0] = 11;  u32* ap = arr;  ap[1];
```

### 外部函数

```c
extern f64 sin(f64);
extern i32 puts(u8*);
```

### 循环展开

- `-O2` 对**常量迭代**循环（`for (u32 i = 0; i < N; i = i + 1)` 且 N ≤ 展开上限）自动全展开；
  循环变量在循环后仍被使用时自动跳过展开。
- `#pragma unroll N`（函数体内可用）做部分展开（余数 + N 因子主体）。

## 测试

```bash
./tests/run_tests.sh
```

对 `tests/*.j8` 分别在 `-O0`/`-O2` 下编译运行，输出与同名
`.expected` 逐行比对。当前 9 个测试文件、`-O0`/`-O2` 共 18 个组合全部通过（t10_atomic 为 --threads 4 多线程测试）。

## 已知问题

- **`-O2` 为实验性/不稳定优化级别**：循环展开、常量折叠等优化仍在打磨，
  可能存在边界缺陷；**默认建议使用 `-O0`**（`-O0` 是可信基线，测试全绿）。
- 整数字面量默认无符号：`0 - 7` 按 u32 回绕（`i32` 用 `-7` 字面量或显式类型）。
- `x * y;` 形式的语句会被解析为指针变量声明（与 C 的 typedef 消歧同理）。

（历史遗留：`__print_u64` 的「多余 0」现象根因已查明并修复——实为 `-O0` 下
`UnaryOp::Neg` 代码生成复用 R0 寄存器导致的 `i32 neg = -7` 计算出 0，并非
`__print_u64` 或 VM 边界问题；`tests/run_tests.sh` 已移除对应豁免，当前 18/18 全绿。）
- 泛型函数当前仅支持单泛型参数。

## 项目结构

```
src/          编译器源码（词法 → 语法 → 语义 → 优化 → 代码生成）
runtime/      j8run 宿主运行器（复用 Jadeight2ReWrite 的 ISA v3 VM 全量实现）
tests/        回归测试（.j8 + .expected + run_tests.sh）
tools/        辅助工具（汇编转储、追踪解释器等）
```


## 文档

- 语言完整手册（词法/类型/语句/函数/协议/泛型/ECS/内建/已知坑）：
  [JadeightPoject/docs/07-语言参考.md](../JadeightPoject/docs/07-语言参考.md)
- j8c 使用指南（构建/用法/示例/测试/版本历史）：
  [JadeightPoject/docs/12-j8c使用指南.md](../JadeightPoject/docs/12-j8c使用指南.md)
- 全部生态文档汇总与阅读路径：**[JadeightPoject/docs/00-文档索引.md](../JadeightPoject/docs/00-文档索引.md)**
