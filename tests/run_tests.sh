#!/usr/bin/env bash
# run_tests.sh — J8 编译器回归测试
# 用法: tests/run_tests.sh [--no-rebuild]
# 每个 tests/t*.j8在 -O0 与 -O2 下编译运行，
# 输出与同名 .expected 逐行比对；任何差异即失败。
# 注意：-O2 为实验性/不稳定优化级别（见 README「已知问题」），-O0 为可信基线。
set -u

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
J8C="$BUILD/j8c"
J8RUN="$BUILD/j8run"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$J8C" ] || [ ! -x "$J8RUN" ]; then
    echo "构建 j8c/j8run ..."
    cmake -S . -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD" --target j8c j8run >/dev/null || { echo "构建失败"; exit 2; }
fi

pass=0
fail=0
failed=()

for src in tests/*.j8; do
    [ -f "$src" ] || continue
    base="$(basename "$src" .j8)"
    exp="tests/$base.expected"
    if [ ! -f "$exp" ]; then
        echo "SKIP $base（无 $exp）"
        continue
    fi
    for opt in 0 2; do
        if ! timeout 30 "$J8C" "$src" -o "$TMP/$base.bc" -O$opt 2>"$TMP/err"; then
            echo "FAIL $base -O$opt：编译失败"
            head -3 "$TMP/err"
            fail=$((fail + 1)); failed+=("$base -O$opt(编译)")
            continue
        fi
        if ! timeout 60 "$J8RUN" "$TMP/$base.bc" 2>"$TMP/err" | tail -n +2 > "$TMP/out"; then
            echo "FAIL $base -O$opt：运行失败"
            head -3 "$TMP/err"
            fail=$((fail + 1)); failed+=("$base -O$opt(运行)")
            continue
        fi
        if diff -q "$TMP/out" "$exp" >/dev/null; then
            pass=$((pass + 1))
        else
            echo "FAIL $base -O$opt：输出不一致（diff 前 8 行）"
            diff "$TMP/out" "$exp" | head -8
            fail=$((fail + 1)); failed+=("$base -O$opt(输出)")
        fi
    done
done

echo
echo "===== 结果：$pass 通过 / $fail 失败 ====="
if [ "$fail" -gt 0 ]; then
    printf '失败项：%s\n' "${failed[@]}"
    exit 1
fi
exit 0
