cmake_minimum_required(VERSION 3.22)
project(JadeightCompiler)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------- j8c：Jadeight C 风格编译器（协议 / ECS / 优化） ----------
add_executable(j8c
    src/driver.cpp
    src/lexer.cpp
    src/parser.cpp
    src/sema.cpp
    src/optimize.cpp
    src/codegen.cpp
)
target_include_directories(j8c PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../JadeightAbstractionCode)
target_compile_options(j8c PRIVATE -Wall -Wextra -Wno-unused-parameter)

# ---------- j8run：在 Jadeight VM 上运行 .bc 的宿主运行器 ----------
add_executable(j8run runtime/j8run.cpp)
target_include_directories(j8run PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../Jadeight2)
target_compile_options(j8run PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(j8run PRIVATE ffi dl)

# 可选：把 j8c 安装到 build 目录，便于测试脚本引用
add_custom_target(tools DEPENDS j8c j8run)
